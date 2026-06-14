#ifndef SPOREGEIST_ORCHESTRATOR_H
#define SPOREGEIST_ORCHESTRATOR_H

#include "sporegeist/actor.h"
#include "sporegeist/mem_executor.h"
#include "sporegeist/policy_gate.h"
#include "sporegeist/recommendation.h"
#include "sporegeist/sim_executor.h"
#include "sporegeist/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum spg_orchestrator_stage {
    SPG_ORCHESTRATOR_STAGE_NOT_STARTED = 0,
    SPG_ORCHESTRATOR_STAGE_ACTOR_DONE,
    SPG_ORCHESTRATOR_STAGE_RECOMMENDATION_REJECTED,
    SPG_ORCHESTRATOR_STAGE_POLICY_GATED,
    SPG_ORCHESTRATOR_STAGE_SIM_EXECUTED,
    SPG_ORCHESTRATOR_STAGE_MEMORY_EXECUTED,
};

struct spg_orchestrator_state {
    struct spg_graph          *graph;
    struct spg_memory         *memory;
    struct spg_journal_writer *journal;
    struct spg_model_adapter  *model;
    struct spg_sim_config     *sim;
    struct spg_mem_store      *store;

    const struct spg_run_config    *run;
    const struct spg_policy_usage  *usage;
    const struct spg_policy_config *policy;

    size_t      policy_text_n;
    const char *policy_text;

    size_t                                  journal_header_count;
    const struct spg_journal_record_header *journal_headers;

    size_t      graph_text_n;
    const char *graph_text;
    size_t      memory_text_n;
    const char *memory_text;
};

struct spg_orchestrator_config {
    uint32_t actor_id;
    uint64_t timestamp_ns;
    uint64_t parent_sequence;

    struct spg_context_limits context_limits;
    size_t                    max_decode_tokens;

    bool reset_model_session;
    bool write_journal;
    bool update_graph;
    bool update_memory;
};

struct spg_orchestrator_workspace {
    struct spg_actor_step_workspace actor;

    size_t                  recommendation_token_capacity;
    struct spg_sexpr_token *recommendation_tokens;
    size_t                  recommendation_node_capacity;
    struct spg_sexpr_node  *recommendation_nodes;

    size_t policy_payload_capacity;
    char  *policy_payload;

    size_t sim_payload_capacity;
    char  *sim_payload;
};

struct spg_orchestrator_result {
    enum spg_orchestrator_stage stage;

    struct spg_actor_step_result      actor;
    struct spg_recommendation        recommendation;
    struct spg_recommendation_error  recommendation_error;
    struct spg_policy_gate_result    policy_gate;
    struct spg_sim_executor_result   sim;
    struct spg_mem_executor_result   memory;

    bool recommendation_valid;
    bool policy_evaluated;
    bool sim_executed;
    bool memory_executed;
};

[[nodiscard]] enum spg_status
spg_orchestrator_tick(struct spg_orchestrator_state *state,
                      const struct spg_orchestrator_config *config,
                      const struct spg_orchestrator_workspace *workspace,
                      struct spg_orchestrator_result *result);

[[nodiscard]] const char *
spg_orchestrator_stage_to_string(enum spg_orchestrator_stage stage);

#ifdef __cplusplus
}
#endif

#endif
