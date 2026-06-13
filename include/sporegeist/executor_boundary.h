#ifndef SPOREGEIST_EXECUTOR_BOUNDARY_H
#define SPOREGEIST_EXECUTOR_BOUNDARY_H

#include "sporegeist/policy.h"
#include "sporegeist/recommendation.h"
#include "sporegeist/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum spg_executor_boundary_reason {
    SPG_EXECUTOR_BOUNDARY_OK = 0,
    SPG_EXECUTOR_BOUNDARY_EXECUTION_DISABLED,
    SPG_EXECUTOR_BOUNDARY_POLICY_DENIED,
    SPG_EXECUTOR_BOUNDARY_UNSUPPORTED_ACTION,
    SPG_EXECUTOR_BOUNDARY_NETWORK_FORBIDDEN,
    SPG_EXECUTOR_BOUNDARY_MISSING_COMMAND,
    SPG_EXECUTOR_BOUNDARY_BAD_WORKDIR,
    SPG_EXECUTOR_BOUNDARY_BAD_TIMEOUT,
    SPG_EXECUTOR_BOUNDARY_BAD_OUTPUT_LIMIT,
    SPG_EXECUTOR_BOUNDARY_ENV_NOT_CLEARED,
};

struct spg_executor_boundary_config {
    bool execution_enabled;

    const char *allowed_workdir_prefix;
    uint64_t    max_timeout_ms;
    size_t      max_stdout_bytes;
    size_t      max_stderr_bytes;

    bool require_clean_env;
};

struct spg_executor_boundary_request {
    const char *working_dir;
    uint64_t    timeout_ms;
    size_t      stdout_limit_bytes;
    size_t      stderr_limit_bytes;
    bool        env_cleared;
};

struct spg_executor_boundary_plan {
    bool approved;
    enum spg_executor_boundary_reason reason;
};

[[nodiscard]] enum spg_status spg_executor_boundary_check(
    const struct spg_executor_boundary_config *config,
    const struct spg_recommendation *recommendation,
    const struct spg_policy_decision *policy_decision,
    const struct spg_executor_boundary_request *request,
    struct spg_executor_boundary_plan *plan);

[[nodiscard]] const char *spg_executor_boundary_reason_to_string(
    enum spg_executor_boundary_reason reason);

#ifdef __cplusplus
}
#endif

#endif
