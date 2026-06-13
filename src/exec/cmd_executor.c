#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#    define _DARWIN_C_SOURCE 1
#endif

#include "sporegeist/cmd_executor.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#    define PATH_MAX 4096
#endif

extern char **environ;

#define READ_CHUNK 4096u

static uint64_t now_ns(void) {
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* Copy as much of data as fits before the terminator; flag truncation when it
 * does not all fit. Maintains *len <= cap - 1 and keeps buf NUL-terminated. */
static void append_capped(char *buf, const size_t cap, size_t *len, bool *trunc,
                          const char *data, const size_t n) {
    const size_t room = cap - 1u - *len;
    const size_t take = n < room ? n : room;
    if (take > 0u) {
        memcpy(buf + *len, data, take);
        *len += take;
    }
    if (take < n) {
        *trunc = true;
    }
    buf[*len] = '\0';
}

static bool request_valid(const struct spg_cmd_request *req) {
    return req->argv != nullptr && req->argc > 0u &&
           req->argc <= SPG_CMD_MAX_ARGS && req->argv[0] != nullptr &&
           req->stdout_buf != nullptr && req->stdout_cap > 0u &&
           req->stderr_buf != nullptr && req->stderr_cap > 0u;
}

/* Drain one nonblocking pipe fully into a capped buffer. Closes *fd and
 * decrements *open_fds on EOF or a hard error. */
static void drain_fd(int *fd, char *buf, const size_t cap, size_t *len,
                     bool *trunc, size_t *open_fds) {
    for (;;) {
        char          chunk[READ_CHUNK];
        const ssize_t r = read(*fd, chunk, sizeof chunk);
        if (r > 0) {
            append_capped(buf, cap, len, trunc, chunk, (size_t)r);
            continue;
        }
        if (r == 0) {
            close(*fd);
            *fd = -1;
            *open_fds -= 1u;
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        close(*fd);
        *fd = -1;
        *open_fds -= 1u;
        return;
    }
}

/* Build a child's stdin(/dev/null) + stdout/stderr pipe redirections. */
static int build_file_actions(posix_spawn_file_actions_t *fa, const int out_w,
                              const int err_w) {
    int rc = posix_spawn_file_actions_init(fa);
    if (rc != 0) {
        return rc;
    }
    rc = posix_spawn_file_actions_addopen(fa, STDIN_FILENO, "/dev/null",
                                          O_RDONLY, 0);
    if (rc == 0) {
        rc = posix_spawn_file_actions_adddup2(fa, out_w, STDOUT_FILENO);
    }
    if (rc == 0) {
        rc = posix_spawn_file_actions_adddup2(fa, err_w, STDERR_FILENO);
    }
    return rc;
}

/* Create a pipe with both ends marked close-on-exec so siblings never inherit
 * one another's pipes (which would defer EOF). */
static bool make_cloexec_pipe(int fds[static 2]) {
    if (pipe(fds) != 0) {
        return false;
    }
    for (size_t i = 0u; i < 2u; i += 1u) {
        const int flags = fcntl(fds[i], F_GETFD);
        if (flags < 0 || fcntl(fds[i], F_SETFD, flags | FD_CLOEXEC) < 0) {
            close(fds[0]);
            close(fds[1]);
            return false;
        }
    }
    return true;
}

static void set_nonblocking(const int fd) {
    const int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

/* Spawn one request. On success records pid and the parent-side read fds and
 * returns true; on any failure fills result with the error and returns false. */
static bool spawn_one(const struct spg_cmd_request *req,
                      const char                   *saved_cwd,
                      const bool have_cwd, pid_t *out_pid, int *out_ofd,
                      int *out_efd, struct spg_cmd_result *result) {
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (!make_cloexec_pipe(out_pipe)) {
        result->status = SPG_E_IO;
        return false;
    }
    if (!make_cloexec_pipe(err_pipe)) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        result->status = SPG_E_IO;
        return false;
    }

    posix_spawn_file_actions_t fa;
    if (build_file_actions(&fa, out_pipe[1], err_pipe[1]) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        result->status = SPG_E_IO;
        return false;
    }

    const char *child_argv[SPG_CMD_MAX_ARGS + 1u];
    for (size_t k = 0u; k < req->argc; k += 1u) {
        child_argv[k] = req->argv[k];
    }
    child_argv[req->argc] = nullptr;

    char *const empty_env[] = {nullptr};
    char *const *envp       = req->clear_env ? empty_env : environ;

    /* Apply working_dir by moving the parent cwd just for this spawn, then
     * restoring it. Spawns are sequential, so siblings are unaffected. */
    bool moved = false;
    if (req->working_dir != nullptr) {
        if (!have_cwd || chdir(req->working_dir) != 0) {
            posix_spawn_file_actions_destroy(&fa);
            close(out_pipe[0]);
            close(out_pipe[1]);
            close(err_pipe[0]);
            close(err_pipe[1]);
            result->status = SPG_E_INVALID_ARG;
            return false;
        }
        moved = true;
    }

    pid_t     pid = 0;
    const int rc =
        posix_spawnp(&pid, child_argv[0], &fa, nullptr,
                     (char *const *)child_argv, envp);

    if (moved) {
        (void)chdir(saved_cwd);
    }
    posix_spawn_file_actions_destroy(&fa);
    /* Parent never writes to the children's stdout/stderr. */
    close(out_pipe[1]);
    close(err_pipe[1]);

    if (rc != 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        result->status = (rc == ENOENT) ? SPG_E_NOT_FOUND : SPG_E_IO;
        return false;
    }

    set_nonblocking(out_pipe[0]);
    set_nonblocking(err_pipe[0]);
    *out_pid        = pid;
    *out_ofd        = out_pipe[0];
    *out_efd        = err_pipe[0];
    result->started = true;
    return true;
}

static int poll_timeout_ms(const uint64_t deadline[static SPG_CMD_MAX_BATCH],
                           const int       ofd[static SPG_CMD_MAX_BATCH],
                           const int       efd[static SPG_CMD_MAX_BATCH],
                           const size_t n, const uint64_t now) {
    bool     have   = false;
    uint64_t soonest = 0u;
    for (size_t i = 0u; i < n; i += 1u) {
        if (deadline[i] == 0u || (ofd[i] < 0 && efd[i] < 0)) {
            continue;
        }
        const uint64_t at = deadline[i];
        if (!have || at < soonest) {
            soonest = at;
            have    = true;
        }
    }
    if (!have) {
        return -1;
    }
    if (now >= soonest) {
        return 0;
    }
    const uint64_t remaining_ms = (soonest - now + 999999u) / 1000000u;
    return remaining_ms > (uint64_t)INT_MAX ? INT_MAX : (int)remaining_ms;
}

static void reap_all(const size_t n, const pid_t pid[static SPG_CMD_MAX_BATCH],
                     struct spg_cmd_result results[static SPG_CMD_MAX_BATCH]) {
    for (size_t i = 0u; i < n; i += 1u) {
        if (!results[i].started) {
            continue;
        }
        int   wstatus = 0;
        pid_t w       = 0;
        do {
            w = waitpid(pid[i], &wstatus, 0);
        } while (w < 0 && errno == EINTR);
        if (w == pid[i]) {
            if (WIFEXITED(wstatus)) {
                results[i].exited    = true;
                results[i].exit_code = WEXITSTATUS(wstatus);
            } else if (WIFSIGNALED(wstatus)) {
                results[i].exited      = false;
                results[i].term_signal = WTERMSIG(wstatus);
            }
        }
        results[i].status = SPG_OK;
    }
}

enum spg_status
spg_cmd_executor_run(const size_t n, const struct spg_cmd_request reqs[],
                     struct spg_cmd_result results[]) {
    if (reqs == nullptr || results == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    if (n == 0u) {
        return SPG_OK;
    }
    if (n > SPG_CMD_MAX_BATCH) {
        return SPG_E_LIMIT;
    }

    pid_t    pid[SPG_CMD_MAX_BATCH];
    int      ofd[SPG_CMD_MAX_BATCH];
    int      efd[SPG_CMD_MAX_BATCH];
    uint64_t deadline[SPG_CMD_MAX_BATCH];
    bool     killed[SPG_CMD_MAX_BATCH];
    for (size_t i = 0u; i < n; i += 1u) {
        results[i] = (struct spg_cmd_result){.status = SPG_OK};
        pid[i]      = 0;
        ofd[i]      = -1;
        efd[i]      = -1;
        deadline[i] = 0u;
        killed[i]   = false;
        if (reqs[i].stdout_buf != nullptr && reqs[i].stdout_cap > 0u) {
            reqs[i].stdout_buf[0] = '\0';
        }
        if (reqs[i].stderr_buf != nullptr && reqs[i].stderr_cap > 0u) {
            reqs[i].stderr_buf[0] = '\0';
        }
    }

    char       saved_cwd[PATH_MAX];
    const bool have_cwd = getcwd(saved_cwd, sizeof saved_cwd) != nullptr;

    size_t open_fds = 0u;
    for (size_t i = 0u; i < n; i += 1u) {
        if (!request_valid(&reqs[i])) {
            results[i].status = SPG_E_INVALID_ARG;
            continue;
        }
        if (!spawn_one(&reqs[i], saved_cwd, have_cwd, &pid[i], &ofd[i], &efd[i],
                       &results[i])) {
            continue;
        }
        if (reqs[i].timeout_ms > 0u) {
            deadline[i] = now_ns() + reqs[i].timeout_ms * 1000000u;
        }
        open_fds += 2u;
    }

    while (open_fds > 0u) {
        struct pollfd pfds[2u * SPG_CMD_MAX_BATCH];
        size_t        owner[2u * SPG_CMD_MAX_BATCH];
        bool          is_stdout[2u * SPG_CMD_MAX_BATCH];
        size_t        np = 0u;
        for (size_t i = 0u; i < n; i += 1u) {
            if (ofd[i] >= 0) {
                pfds[np]      = (struct pollfd){.fd = ofd[i], .events = POLLIN};
                owner[np]     = i;
                is_stdout[np] = true;
                np += 1u;
            }
            if (efd[i] >= 0) {
                pfds[np]      = (struct pollfd){.fd = efd[i], .events = POLLIN};
                owner[np]     = i;
                is_stdout[np] = false;
                np += 1u;
            }
        }

        const int timeout = poll_timeout_ms(deadline, ofd, efd, n, now_ns());
        const int pr      = poll(pfds, (nfds_t)np, timeout);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (pr > 0) {
            for (size_t p = 0u; p < np; p += 1u) {
                if (pfds[p].revents == 0) {
                    continue;
                }
                const size_t i = owner[p];
                if (is_stdout[p]) {
                    drain_fd(&ofd[i], reqs[i].stdout_buf, reqs[i].stdout_cap,
                             &results[i].stdout_len,
                             &results[i].stdout_truncated, &open_fds);
                } else {
                    drain_fd(&efd[i], reqs[i].stderr_buf, reqs[i].stderr_cap,
                             &results[i].stderr_len,
                             &results[i].stderr_truncated, &open_fds);
                }
            }
        }

        const uint64_t t = now_ns();
        for (size_t i = 0u; i < n; i += 1u) {
            const bool active = ofd[i] >= 0 || efd[i] >= 0;
            if (active && deadline[i] > 0u && !killed[i] && t >= deadline[i]) {
                (void)kill(pid[i], SIGKILL);
                killed[i]              = true;
                results[i].timed_out   = true;
            }
        }
    }

    reap_all(n, pid, results);
    return SPG_OK;
}
