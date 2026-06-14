#include "sporegeist/orchestrator.h"

static bool workspace_valid(const struct spg_orchestrator_workspace *workspace) {
    if (workspace == nullptr || workspace->policy_payload == nullptr ||
        workspace->policy_payload_capacity == 0u ||
        workspace->sim_payload == nullptr ||
        workspace->sim_payload_capacity == 0u ||
        workspace->recommendation_tokens == nullptr ||
        workspace->recommendation_token_capacity == 0u ||
        workspace->recommendation_nodes == nullptr ||
        workspace->recommendation_node_capacity == 0u) {
        return false;
    }
    return true;
}

static enum spg_status journal_recommendation_rejected(
    struct spg_orchestrator_state *state,
    const struct spg_orchestrator_config *config,
    const struct spg_orchestrator_workspace *workspace,
    const struct spg_orchestrator_result *result) {
    if (!config->write_journal) {
        return SPG_OK;
    }
    if (state == nullptr || state->journal == nullptr || config == nullptr ||
        workspace == nullptr || result == nullptr ||
        workspace->policy_payload == nullptr ||
        workspace->policy_payload_capacity == 0u) {
        return SPG_E_INVALID_ARG;
    }
    struct spg_sexpr_writer writer;
    spg_sexpr_writer_init(&writer, workspace->policy_payload_capacity,
                          workspace->policy_payload);
    (void)spg_sexpr_writer_append_text(&writer,
                                       "(recommendation_rejected (reason ");
    (void)spg_sexpr_writer_append_text(
        &writer, spg_recommendation_reject_reason_to_string(
                     result->recommendation.reject_reason));
    (void)spg_sexpr_writer_append_text(&writer, "))");
    const size_t          payload_n    = writer.used;
    const enum spg_status event_status = writer.truncated ? SPG_E_LIMIT
                                                          : SPG_E_SCHEMA;
    uint64_t sequence = 0u;
    const uint64_t parent_sequence =
        result->actor.model_output_sequence != 0u
            ? result->actor.model_output_sequence
            : config->parent_sequence;
    return spg_journal_writer_append(
        state->journal, config->timestamp_ns, parent_sequence,
        SPG_JOURNAL_EVENT_ERROR, event_status, payload_n,
        (const uint8_t *)workspace->policy_payload, &sequence);
}

const char *spg_orchestrator_stage_to_string(
    const enum spg_orchestrator_stage stage) {
    switch (stage) {
    case SPG_ORCHESTRATOR_STAGE_NOT_STARTED:
        return "not_started";
    case SPG_ORCHESTRATOR_STAGE_ACTOR_DONE:
        return "actor_done";
    case SPG_ORCHESTRATOR_STAGE_RECOMMENDATION_REJECTED:
        return "recommendation_rejected";
    case SPG_ORCHESTRATOR_STAGE_POLICY_GATED:
        return "policy_gated";
    case SPG_ORCHESTRATOR_STAGE_SIM_EXECUTED:
        return "sim_executed";
    case SPG_ORCHESTRATOR_STAGE_MEMORY_EXECUTED:
        return "memory_executed";
    }
    return "unknown";
}

/* The derived predicates below treat `stage` as a progress counter: a tick is
 * "valid"/"gated" once it reaches POLICY_GATED, so every pre-gate or rejected
 * stage must sort below it and every executed stage at or above it. Pin that
 * ordering so a future reorder cannot silently flip the predicates. */
static_assert(SPG_ORCHESTRATOR_STAGE_NOT_STARTED <
                      SPG_ORCHESTRATOR_STAGE_POLICY_GATED &&
                  SPG_ORCHESTRATOR_STAGE_ACTOR_DONE <
                      SPG_ORCHESTRATOR_STAGE_POLICY_GATED &&
                  SPG_ORCHESTRATOR_STAGE_RECOMMENDATION_REJECTED <
                      SPG_ORCHESTRATOR_STAGE_POLICY_GATED &&
                  SPG_ORCHESTRATOR_STAGE_POLICY_GATED <=
                      SPG_ORCHESTRATOR_STAGE_SIM_EXECUTED &&
                  SPG_ORCHESTRATOR_STAGE_POLICY_GATED <=
                      SPG_ORCHESTRATOR_STAGE_MEMORY_EXECUTED,
              "orchestrator stage ordering: pre-gate/rejected stages must sort "
              "below POLICY_GATED and executed stages at or above it");

bool spg_orchestrator_recommendation_valid(
    const struct spg_orchestrator_result *result) {
    return result->stage >= SPG_ORCHESTRATOR_STAGE_POLICY_GATED;
}

bool spg_orchestrator_policy_evaluated(
    const struct spg_orchestrator_result *result) {
    return result->stage >= SPG_ORCHESTRATOR_STAGE_POLICY_GATED;
}

bool spg_orchestrator_sim_executed(
    const struct spg_orchestrator_result *result) {
    return result->stage == SPG_ORCHESTRATOR_STAGE_SIM_EXECUTED;
}

bool spg_orchestrator_memory_executed(
    const struct spg_orchestrator_result *result) {
    return result->stage == SPG_ORCHESTRATOR_STAGE_MEMORY_EXECUTED;
}

enum spg_status spg_orchestrator_tick(
    struct spg_orchestrator_state *state,
    const struct spg_orchestrator_config *config,
    const struct spg_orchestrator_workspace *workspace,
    struct spg_orchestrator_result *result) {
    if (state == nullptr || config == nullptr || !workspace_valid(workspace) ||
        result == nullptr || state->model == nullptr ||
        state->policy == nullptr || state->usage == nullptr ||
        state->policy_text == nullptr || state->policy_text_n == 0u ||
        (config->write_journal && state->journal == nullptr) ||
        (config->update_graph && state->graph == nullptr) ||
        (config->update_memory && state->memory == nullptr)) {
        return SPG_E_INVALID_ARG;
    }

    *result = (struct spg_orchestrator_result){
        .stage = SPG_ORCHESTRATOR_STAGE_NOT_STARTED,
    };

    struct spg_actor_state actor_state = {
        .graph                = state->graph,
        .memory               = state->memory,
        .journal              = state->journal,
        .model                = state->model,
        .run                  = state->run,
        .usage                = state->usage,
        .journal_header_count = state->journal_header_count,
        .journal_headers      = state->journal_headers,
        .graph_text_n         = state->graph_text_n,
        .graph_text           = state->graph_text,
        .memory_text_n        = state->memory_text_n,
        .memory_text          = state->memory_text,
        .memory_index         = state->memory_index,
        .memory_recall        = state->memory_recall,
    };
    const struct spg_actor_step_config actor_config = {
        .actor_id            = config->actor_id,
        .timestamp_ns        = config->timestamp_ns,
        .parent_sequence     = config->parent_sequence,
        .context_limits      = config->context_limits,
        .max_decode_tokens   = config->max_decode_tokens,
        .reset_model_session = config->reset_model_session,
        .write_journal       = config->write_journal,
        .update_graph        = config->update_graph,
        .update_memory       = config->update_memory,
    };
    enum spg_status status = spg_actor_step(
        &actor_state, &actor_config, &workspace->actor, &result->actor);
    if (status != SPG_OK) {
        return status;
    }
    result->stage = SPG_ORCHESTRATOR_STAGE_ACTOR_DONE;

    status = spg_recommendation_parse(
        result->actor.model_output_n, workspace->actor.model_output,
        workspace->recommendation_token_capacity,
        workspace->recommendation_tokens,
        workspace->recommendation_node_capacity,
        workspace->recommendation_nodes, &result->recommendation,
        &result->recommendation_error);
    if (status != SPG_OK) {
        return status;
    }
    if (result->recommendation.state != SPG_RECOMMENDATION_VALID) {
        result->stage = SPG_ORCHESTRATOR_STAGE_RECOMMENDATION_REJECTED;
        return journal_recommendation_rejected(state, config, workspace, result);
    }

    const struct spg_policy_gate_workspace policy_workspace = {
        .payload_capacity = workspace->policy_payload_capacity,
        .payload          = workspace->policy_payload,
    };
    const struct spg_policy_gate_state policy_state = {
        .policy_text_n         = state->policy_text_n,
        .policy_text           = state->policy_text,
        .recommendation_text_n = result->actor.model_output_n,
        .recommendation_text   = workspace->actor.model_output,
        .policy                = state->policy,
        .usage                 = state->usage,
        .journal               = state->journal,
        .graph                 = state->graph,
    };
    const struct spg_policy_gate_config policy_config = {
        .actor_id                = config->actor_id,
        .timestamp_ns            = config->timestamp_ns,
        .parent_sequence         = result->actor.model_output_sequence != 0u
                                       ? result->actor.model_output_sequence
                                       : config->parent_sequence,
        .write_journal           = config->write_journal,
        .update_graph_on_deny    = config->update_graph,
        .has_recommendation_node = result->actor.has_recommendation_node,
        .recommendation_node     = result->actor.recommendation_node,
    };
    status = spg_policy_gate_step(&policy_state, &policy_config,
                                  &result->recommendation, &policy_workspace,
                                  &result->policy_gate);
    if (status != SPG_OK) {
        return status;
    }
    result->stage = SPG_ORCHESTRATOR_STAGE_POLICY_GATED;

    if (result->policy_gate.decision.kind != SPG_POLICY_DECISION_ALLOW) {
        return SPG_OK;
    }

    if (result->recommendation.action_kind == SPG_ACTION_MEMORY_SAVE ||
        result->recommendation.action_kind == SPG_ACTION_MEMORY_DELETE ||
        result->recommendation.action_kind == SPG_ACTION_MEMORY_READ) {
        if (state->store == nullptr) {
            return SPG_E_INVALID_ARG;
        }
        struct spg_mem_executor_state mem_state = {
            .store   = state->store,
            .journal = state->journal,
        };
        const struct spg_mem_executor_config mem_config = {
            .actor_id        = config->actor_id,
            .timestamp_ns    = config->timestamp_ns,
            .parent_sequence = result->policy_gate.policy_sequence != 0u
                                   ? result->policy_gate.policy_sequence
                                   : policy_config.parent_sequence,
            .write_journal   = config->write_journal,
        };
        const struct spg_mem_executor_workspace mem_workspace = {
            .payload_capacity = workspace->sim_payload_capacity,
            .payload          = workspace->sim_payload,
            .recall_capacity  = workspace->memory_recall_capacity,
            .recall           = workspace->memory_recall_buf,
        };
        status = spg_mem_executor_step(
            &mem_state, &mem_config, result->actor.model_output_n,
            workspace->actor.model_output, &result->recommendation,
            &result->policy_gate.decision, &mem_workspace, &result->memory);
        if (status != SPG_OK) {
            return status;
        }
        result->stage = SPG_ORCHESTRATOR_STAGE_MEMORY_EXECUTED;
        return SPG_OK;
    }

    if (result->recommendation.action_kind != SPG_ACTION_SIMULATOR) {
        return SPG_OK;
    }
    if (state->sim == nullptr) {
        return SPG_E_INVALID_ARG;
    }

    const struct spg_sim_executor_workspace sim_workspace = {
        .payload_capacity = workspace->sim_payload_capacity,
        .payload          = workspace->sim_payload,
    };
    struct spg_sim_executor_state sim_state = {
        .sim     = state->sim,
        .journal = state->journal,
        .graph   = state->graph,
        .memory  = state->memory,
    };
    const struct spg_sim_executor_config sim_config = {
        .actor_id                = config->actor_id,
        .timestamp_ns            = config->timestamp_ns,
        .parent_sequence         = result->policy_gate.policy_sequence != 0u
                                       ? result->policy_gate.policy_sequence
                                       : policy_config.parent_sequence,
        .write_journal           = config->write_journal,
        .update_graph            = config->update_graph,
        .update_memory           = config->update_memory,
        .has_recommendation_node = result->actor.has_recommendation_node,
        .recommendation_node     = result->actor.recommendation_node,
    };
    status = spg_sim_executor_step(&sim_state, &sim_config,
                                   &result->recommendation,
                                   &result->policy_gate.decision,
                                   &sim_workspace, &result->sim);
    if (status != SPG_OK) {
        return status;
    }
    result->stage = SPG_ORCHESTRATOR_STAGE_SIM_EXECUTED;
    return SPG_OK;
}
