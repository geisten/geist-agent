#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#    define _DARWIN_C_SOURCE 1
#endif

#include "sporegeist/mem_executor.h"
#include "sporegeist/recommendation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int open_temp_store(struct spg_mem_store *store, char dir[static 64]) {
    memcpy(dir, "/tmp/spg_memexec_XXXXXX", 24u);
    if (mkdtemp(dir) == nullptr) {
        return 1;
    }
    return spg_mem_store_open(store, dir) == SPG_OK ? 0 : 1;
}

static enum spg_status parse_rec(const char *text,
                                 struct spg_recommendation *out) {
    struct spg_sexpr_token          tokens[256];
    struct spg_sexpr_node           nodes[256];
    struct spg_recommendation_error err = {};
    return spg_recommendation_parse(strlen(text), text, 256u, tokens, 256u,
                                    nodes, out, &err);
}

static enum spg_status run_step(const char *text, const char *slug_in,
                                enum spg_policy_decision_kind dkind,
                                struct spg_mem_store        *store,
                                struct spg_mem_executor_result *result) {
    (void)slug_in;
    struct spg_recommendation rec = {};
    if (parse_rec(text, &rec) != SPG_OK ||
        rec.state != SPG_RECOMMENDATION_VALID ||
        rec.action_kind != SPG_ACTION_MEMORY_SAVE) {
        return SPG_E_INTERNAL;
    }
    struct spg_mem_executor_state state = {.store = store, .journal = nullptr};
    const struct spg_mem_executor_config cfg = {.write_journal = false};
    char                                 payload[512];
    const struct spg_mem_executor_workspace ws = {.payload_capacity = sizeof payload,
                                                  .payload          = payload};
    const struct spg_policy_decision decision = {.kind = dkind};
    return spg_mem_executor_step(&state, &cfg, strlen(text), text, &rec,
                                 &decision, &ws, result);
}

static int test_save_writes_file(void) {
    const char text[] =
        "(recommend (kind memory_save) (capability \"mem.write\") (cost 1) "
        "(uses_network false) (confidence_bp 9000) (reason \"r\") "
        "(slug \"k1\") (description \"a hook\") (body \"the body\"))";
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp_store(&store, dir) != 0) {
        return 1;
    }
    struct spg_mem_executor_result res = {};
    if (run_step(text, "k1", SPG_POLICY_DECISION_ALLOW, &store, &res) !=
        SPG_OK) {
        return 1;
    }
    if (!res.saved || res.save_status != SPG_OK) {
        return 1;
    }
    char out[256];
    if (spg_mem_read(&store, "k1", sizeof out, out, nullptr) != SPG_OK) {
        return 1;
    }
    return (strstr(out, "description: a hook") != nullptr &&
            strstr(out, "the body") != nullptr)
               ? 0
               : 1;
}

static int test_bad_slug_recorded(void) {
    /* An invalid slug ("BAD" has uppercase) is rejected by the store; the step
     * still succeeds but records the failure. */
    const char text[] =
        "(recommend (kind memory_save) (capability \"mem.write\") (cost 1) "
        "(uses_network false) (confidence_bp 9000) (reason \"r\") "
        "(slug \"BAD\") (description \"d\") (body \"b\"))";
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp_store(&store, dir) != 0) {
        return 1;
    }
    struct spg_mem_executor_result res = {};
    if (run_step(text, "BAD", SPG_POLICY_DECISION_ALLOW, &store, &res) !=
        SPG_OK) {
        return 1;
    }
    return (!res.saved && res.save_status == SPG_E_INVALID_ARG) ? 0 : 1;
}

static int test_deny_rejected(void) {
    /* The executor only runs ALLOW'd actions; a DENY is a caller error. */
    const char text[] =
        "(recommend (kind memory_save) (capability \"mem.write\") (cost 1) "
        "(uses_network false) (confidence_bp 9000) (reason \"r\") "
        "(slug \"k2\") (description \"d\") (body \"b\"))";
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp_store(&store, dir) != 0) {
        return 1;
    }
    struct spg_mem_executor_result res = {};
    if (run_step(text, "k2", SPG_POLICY_DECISION_DENY, &store, &res) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    char out[64];
    /* nothing was written */
    return spg_mem_read(&store, "k2", sizeof out, out, nullptr) ==
                   SPG_E_NOT_FOUND
               ? 0
               : 1;
}

int main(void) {
    if (test_save_writes_file() != 0) {
        fprintf(stderr, "test_save_writes_file failed\n");
        return 1;
    }
    if (test_bad_slug_recorded() != 0) {
        fprintf(stderr, "test_bad_slug_recorded failed\n");
        return 1;
    }
    if (test_deny_rejected() != 0) {
        fprintf(stderr, "test_deny_rejected failed\n");
        return 1;
    }
    return 0;
}
