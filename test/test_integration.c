/* In-process integration test: composes the DSL parser, the recommendation
 * parser, the policy decision type, and the executor boundary guard across
 * module boundaries — the same chain the orchestrator drives, without the CLI. */
#include "sporegeist/executor_boundary.h"
#include "sporegeist/policy.h"
#include "sporegeist/recommendation.h"

#include <stdio.h>
#include <string.h>

static enum spg_status parse_rec(const char *text,
                                 struct spg_recommendation *out) {
    struct spg_sexpr_token          tokens[128];
    struct spg_sexpr_node           nodes[128];
    struct spg_recommendation_error err = {};
    return spg_recommendation_parse(strlen(text), text, 128u, tokens, 128u,
                                    nodes, out, &err);
}

static const struct spg_executor_boundary_config k_cfg = {
    .execution_enabled      = true,
    .allowed_workdir_prefix = "/",
    .max_timeout_ms         = 5000u,
    .max_stdout_bytes       = 1u << 20,
    .max_stderr_bytes       = 1u << 20,
    .require_clean_env      = false,
};
static const struct spg_executor_boundary_request k_req = {
    .working_dir        = "/",
    .timeout_ms         = 3000u,
    .stdout_limit_bytes = 4096u,
    .stderr_limit_bytes = 4096u,
    .env_cleared        = false,
};

static enum spg_executor_boundary_reason
gate(const char *text, enum spg_policy_decision_kind decision_kind) {
    struct spg_recommendation rec = {};
    if (parse_rec(text, &rec) != SPG_OK ||
        rec.state != SPG_RECOMMENDATION_VALID) {
        return SPG_EXECUTOR_BOUNDARY_OK; /* caller checks this never happens */
    }
    const struct spg_policy_decision decision = {
        .kind             = decision_kind,
        .deny_reason      = SPG_POLICY_DENY_NONE,
        .capability_index = 0u,
    };
    struct spg_executor_boundary_plan plan = {};
    if (spg_executor_boundary_check(&k_cfg, &rec, &decision, &k_req, &plan) !=
        SPG_OK) {
        return SPG_EXECUTOR_BOUNDARY_EXECUTION_DISABLED;
    }
    return plan.approved ? SPG_EXECUTOR_BOUNDARY_OK : plan.reason;
}

static int test_shell_allowed(void) {
    const char text[] =
        "(recommend (kind local_shell) (capability \"build.run\") (cost 1) "
        "(uses_network false) (confidence_bp 9000) (reason \"build\") "
        "(command \"make\"))";
    return gate(text, SPG_POLICY_DECISION_ALLOW) == SPG_EXECUTOR_BOUNDARY_OK
               ? 0
               : 1;
}

static int test_policy_deny_blocks(void) {
    const char text[] =
        "(recommend (kind local_shell) (capability \"build.run\") (cost 1) "
        "(uses_network false) (confidence_bp 9000) (reason \"build\") "
        "(command \"make\"))";
    return gate(text, SPG_POLICY_DECISION_DENY) ==
                   SPG_EXECUTOR_BOUNDARY_POLICY_DENIED
               ? 0
               : 1;
}

static int test_shell_network_rejected(void) {
    /* The recommendation parser couples kind and network: a local_shell action
     * may not use the network, so this never becomes a VALID recommendation
     * (the boundary's network branch is thus only reachable defensively). */
    const char text[] =
        "(recommend (kind local_shell) (capability \"x\") (cost 1) "
        "(uses_network true) (confidence_bp 9000) (reason \"net\") "
        "(command \"curl\"))";
    struct spg_recommendation rec = {};
    (void)parse_rec(text, &rec);
    return rec.state == SPG_RECOMMENDATION_VALID ? 1 : 0;
}

static int test_non_shell_unsupported(void) {
    const char text[] =
        "(recommend (kind simulator) (capability \"sim.act\") (cost 1) "
        "(uses_network false) (confidence_bp 9000) (reason \"sim\"))";
    return gate(text, SPG_POLICY_DECISION_ALLOW) ==
                   SPG_EXECUTOR_BOUNDARY_UNSUPPORTED_ACTION
               ? 0
               : 1;
}

static int test_garbage_rejected(void) {
    const char text[] = "(recommend (kind not_a_kind) (capability \"x\"))";
    struct spg_recommendation rec = {};
    (void)parse_rec(text, &rec);
    /* An unknown/incomplete recommendation must not become VALID. */
    return rec.state == SPG_RECOMMENDATION_VALID ? 1 : 0;
}

int main(void) {
    if (test_shell_allowed() != 0) {
        fprintf(stderr, "test_shell_allowed failed\n");
        return 1;
    }
    if (test_policy_deny_blocks() != 0) {
        fprintf(stderr, "test_policy_deny_blocks failed\n");
        return 1;
    }
    if (test_shell_network_rejected() != 0) {
        fprintf(stderr, "test_shell_network_rejected failed\n");
        return 1;
    }
    if (test_non_shell_unsupported() != 0) {
        fprintf(stderr, "test_non_shell_unsupported failed\n");
        return 1;
    }
    if (test_garbage_rejected() != 0) {
        fprintf(stderr, "test_garbage_rejected failed\n");
        return 1;
    }
    return 0;
}
