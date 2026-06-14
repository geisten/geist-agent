#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#    define _DARWIN_C_SOURCE 1
#endif

#include "sporegeist/mem_store.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define SLUGBUF (SPG_MEM_SLUG_MAX + 1u)
#define LINEBUF 1024u

bool spg_mem_slug_valid(const char *slug) {
    if (slug == nullptr) {
        return false;
    }
    size_t n = 0u;
    for (; slug[n] != '\0'; n += 1u) {
        const char c = slug[n];
        const bool ok =
            (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) {
            return false;
        }
    }
    return n >= 1u && n <= SPG_MEM_SLUG_MAX;
}

enum spg_status spg_mem_store_open(struct spg_mem_store *store,
                                   const char           *dir) {
    if (store == nullptr || dir == nullptr || dir[0] == '\0') {
        return SPG_E_INVALID_ARG;
    }
    const size_t n = strlen(dir);
    if (n + 1u > sizeof store->dir) {
        return SPG_E_LIMIT;
    }
    memcpy(store->dir, dir, n + 1u);
    if (mkdir(store->dir, 0755) != 0 && errno != EEXIST) {
        return SPG_E_IO;
    }
    return SPG_OK;
}

static enum spg_status build_path(const struct spg_mem_store *store,
                                  const char *slug, const char *suffix,
                                  char *out, const size_t cap) {
    const int n = snprintf(out, cap, "%s/%s%s", store->dir, slug, suffix);
    if (n < 0 || (size_t)n >= cap) {
        return SPG_E_LIMIT;
    }
    return SPG_OK;
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* A memory file is "<slug>.md" but not the generated "MEMORY.md" index. */
static bool is_memory_file(const char *name) {
    const size_t n = strlen(name);
    if (n < 4u || strcmp(name + n - 3u, ".md") != 0) {
        return false;
    }
    return strcmp(name, "MEMORY.md") != 0;
}

/* Read "description:" from a file's frontmatter into out (always terminated). */
static void read_description(const char *path, char *out, const size_t cap) {
    out[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        return;
    }
    char line[LINEBUF];
    bool in_fm = false;
    while (fgets(line, sizeof line, f) != nullptr) {
        const size_t ln = strlen(line);
        if (ln > 0u && line[ln - 1u] == '\n') {
            line[ln - 1u] = '\0';
        }
        if (strcmp(line, "---") == 0) {
            if (!in_fm) {
                in_fm = true;
                continue;
            }
            break;
        }
        if (in_fm && strncmp(line, "description:", 12u) == 0) {
            const char *v = line + 12u;
            while (*v == ' ') {
                v += 1u;
            }
            const size_t vn = strlen(v);
            const size_t take = vn + 1u > cap ? cap - 1u : vn;
            memcpy(out, v, take);
            out[take] = '\0';
            break;
        }
    }
    (void)fclose(f);
}

/* Collect memory slugs (without ".md"), slug-sorted, into slugs[0..return). */
static size_t collect_sorted(const struct spg_mem_store *store,
                             char slugs[][SLUGBUF], const size_t max) {
    DIR *d = opendir(store->dir);
    if (d == nullptr) {
        return 0u;
    }
    size_t         count = 0u;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr && count < max) {
        if (!is_memory_file(e->d_name)) {
            continue;
        }
        const size_t nm = strlen(e->d_name) - 3u; /* drop ".md" */
        if (nm + 1u > SLUGBUF) {
            continue;
        }
        char slug[SLUGBUF];
        memcpy(slug, e->d_name, nm);
        slug[nm] = '\0';
        /* insertion sort into slugs[] */
        size_t pos = count;
        while (pos > 0u && strcmp(slugs[pos - 1u], slug) > 0) {
            memcpy(slugs[pos], slugs[pos - 1u], SLUGBUF);
            pos -= 1u;
        }
        memcpy(slugs[pos], slug, nm + 1u);
        count += 1u;
    }
    (void)closedir(d);
    return count;
}

static size_t count_memories(const struct spg_mem_store *store) {
    DIR *d = opendir(store->dir);
    if (d == nullptr) {
        return 0u;
    }
    size_t         count = 0u;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (is_memory_file(e->d_name)) {
            count += 1u;
        }
    }
    (void)closedir(d);
    return count;
}

/* Rewrite <dir>/MEMORY.md with the full index, atomically. Best-effort. */
static void regenerate_index(const struct spg_mem_store *store) {
    char tmp[SPG_MEM_PATH_MAX];
    char dst[SPG_MEM_PATH_MAX];
    if (build_path(store, "MEMORY", ".md.tmp", tmp, sizeof tmp) != SPG_OK ||
        build_path(store, "MEMORY", ".md", dst, sizeof dst) != SPG_OK) {
        return;
    }
    FILE *f = fopen(tmp, "wb");
    if (f == nullptr) {
        return;
    }
    static char slugs[SPG_MEM_MAX_FILES][SLUGBUF];
    const size_t count = collect_sorted(store, slugs, SPG_MEM_MAX_FILES);
    for (size_t i = 0u; i < count; i += 1u) {
        char path[SPG_MEM_PATH_MAX];
        char desc[SPG_MEM_DESC_MAX + 1u];
        if (build_path(store, slugs[i], ".md", path, sizeof path) != SPG_OK) {
            continue;
        }
        read_description(path, desc, sizeof desc);
        (void)fprintf(f, "- %s: %s\n", slugs[i], desc);
    }
    if (fclose(f) == 0) {
        (void)rename(tmp, dst);
    } else {
        (void)remove(tmp);
    }
}

enum spg_status spg_mem_save(struct spg_mem_store *store, const char *slug,
                             const char *description, const char *body) {
    if (store == nullptr || !spg_mem_slug_valid(slug) ||
        description == nullptr || body == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    if (strchr(description, '\n') != nullptr) {
        return SPG_E_INVALID_ARG; /* would break the frontmatter */
    }
    if (strlen(description) > SPG_MEM_DESC_MAX || strlen(body) > SPG_MEM_BODY_MAX) {
        return SPG_E_LIMIT;
    }

    char path[SPG_MEM_PATH_MAX];
    char tmp[SPG_MEM_PATH_MAX];
    if (build_path(store, slug, ".md", path, sizeof path) != SPG_OK ||
        build_path(store, slug, ".md.tmp", tmp, sizeof tmp) != SPG_OK) {
        return SPG_E_LIMIT;
    }
    if (!file_exists(path) && count_memories(store) >= SPG_MEM_MAX_FILES) {
        return SPG_E_LIMIT;
    }

    FILE *f = fopen(tmp, "wb");
    if (f == nullptr) {
        return SPG_E_IO;
    }
    const int w = fprintf(f, "---\nname: %s\ndescription: %s\n---\n%s", slug,
                          description, body);
    const bool body_nl = body[0] == '\0' || body[strlen(body) - 1u] == '\n';
    if (w >= 0 && !body_nl) {
        (void)fputc('\n', f);
    }
    if (w < 0 || fclose(f) != 0) {
        (void)remove(tmp);
        return SPG_E_IO;
    }
    if (rename(tmp, path) != 0) {
        (void)remove(tmp);
        return SPG_E_IO;
    }
    regenerate_index(store);
    return SPG_OK;
}

enum spg_status spg_mem_delete(struct spg_mem_store *store, const char *slug) {
    if (store == nullptr || !spg_mem_slug_valid(slug)) {
        return SPG_E_INVALID_ARG;
    }
    char path[SPG_MEM_PATH_MAX];
    if (build_path(store, slug, ".md", path, sizeof path) != SPG_OK) {
        return SPG_E_LIMIT;
    }
    if (!file_exists(path)) {
        return SPG_E_NOT_FOUND;
    }
    if (remove(path) != 0) {
        return SPG_E_IO;
    }
    regenerate_index(store);
    return SPG_OK;
}

enum spg_status spg_mem_read(struct spg_mem_store *store, const char *slug,
                             const size_t dst_cap, char dst[],
                             size_t *out_required) {
    if (store == nullptr || !spg_mem_slug_valid(slug) || dst == nullptr ||
        dst_cap == 0u) {
        return SPG_E_INVALID_ARG;
    }
    char path[SPG_MEM_PATH_MAX];
    if (build_path(store, slug, ".md", path, sizeof path) != SPG_OK) {
        return SPG_E_LIMIT;
    }
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        return SPG_E_NOT_FOUND;
    }
    size_t total = 0u;
    size_t used  = 0u;
    bool   over  = false;
    char   chunk[4096];
    size_t r;
    while ((r = fread(chunk, 1u, sizeof chunk, f)) > 0u) {
        total += r;
        const size_t room = used < dst_cap - 1u ? dst_cap - 1u - used : 0u;
        const size_t take = r < room ? r : room;
        if (take > 0u) {
            memcpy(dst + used, chunk, take);
            used += take;
        }
        if (take < r) {
            over = true;
        }
    }
    dst[used] = '\0';
    (void)fclose(f);
    if (out_required != nullptr) {
        *out_required = total;
    }
    return over ? SPG_E_LIMIT : SPG_OK;
}

enum spg_status spg_mem_index(struct spg_mem_store *store, const size_t dst_cap,
                              char dst[], size_t *out_required,
                              bool *truncated) {
    if (store == nullptr || dst == nullptr || dst_cap == 0u) {
        return SPG_E_INVALID_ARG;
    }
    static char  slugs[SPG_MEM_MAX_FILES][SLUGBUF];
    const size_t count = collect_sorted(store, slugs, SPG_MEM_MAX_FILES);

    dst[0]              = '\0';
    size_t used         = 0u;
    size_t required     = 0u;
    bool   over         = false;
    bool   trunc        = false;

    const size_t shown = count < SPG_MEM_INDEX_TOPK ? count : SPG_MEM_INDEX_TOPK;
    for (size_t i = 0u; i < shown; i += 1u) {
        char path[SPG_MEM_PATH_MAX];
        char desc[SPG_MEM_DESC_MAX + 1u];
        char line[SPG_MEM_SLUG_MAX + SPG_MEM_DESC_MAX + 8u];
        if (build_path(store, slugs[i], ".md", path, sizeof path) != SPG_OK) {
            continue;
        }
        read_description(path, desc, sizeof desc);
        const int ln =
            snprintf(line, sizeof line, "- %s: %s\n", slugs[i], desc);
        if (ln < 0) {
            continue;
        }
        const size_t n    = (size_t)ln;
        required         += n;
        const size_t room = used < dst_cap - 1u ? dst_cap - 1u - used : 0u;
        const size_t take = n < room ? n : room;
        if (take > 0u) {
            memcpy(dst + used, line, take);
            used += take;
        }
        if (take < n) {
            over = true;
        }
    }
    if (count > SPG_MEM_INDEX_TOPK) {
        trunc = true;
        char ptr[64];
        const int ln = snprintf(ptr, sizeof ptr, "- ... %zu more (memory list)\n",
                                count - SPG_MEM_INDEX_TOPK);
        if (ln > 0) {
            const size_t n    = (size_t)ln;
            required         += n;
            const size_t room = used < dst_cap - 1u ? dst_cap - 1u - used : 0u;
            const size_t take = n < room ? n : room;
            if (take > 0u) {
                memcpy(dst + used, ptr, take);
                used += take;
            }
            if (take < n) {
                over = true;
            }
        }
    }
    dst[used] = '\0';
    if (out_required != nullptr) {
        *out_required = required;
    }
    if (truncated != nullptr) {
        *truncated = trunc;
    }
    return over ? SPG_E_LIMIT : SPG_OK;
}

enum spg_status spg_mem_list(struct spg_mem_store *store, const size_t cap,
                             char slugs[][SPG_MEM_SLUG_MAX + 1u],
                             size_t *count) {
    if (store == nullptr || slugs == nullptr || count == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    static char  all[SPG_MEM_MAX_FILES][SLUGBUF];
    const size_t total = collect_sorted(store, all, SPG_MEM_MAX_FILES);
    const size_t n     = total < cap ? total : cap;
    for (size_t i = 0u; i < n; i += 1u) {
        memcpy(slugs[i], all[i], SLUGBUF);
    }
    *count = n;
    return total > cap ? SPG_E_LIMIT : SPG_OK;
}
