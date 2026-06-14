#include "sporegeist/mem_executor.h"

#include "sporegeist/sexpr.h"

#include <string.h>

static bool workspace_valid(const struct spg_mem_executor_workspace *workspace) {
    return workspace != nullptr && workspace->payload != nullptr &&
           workspace->payload_capacity > 0u;
}

/* Copy a span out of text into a NUL-terminated, bounded buffer. Returns false
 * when the span is out of bounds or does not fit (out is left empty). */
static bool copy_span(size_t text_n, const char *text,
                      const struct spg_text_span span, char *out,
                      const size_t cap) {
    out[0] = '\0';
    if (!spg_sexpr_span_valid(text_n, span) || span.length + 1u > cap) {
        return false;
    }
    memcpy(out, text + span.offset, span.length);
    out[span.length] = '\0';
    return true;
}

static enum spg_status journal_memory(
    struct spg_mem_executor_state *state,
    const struct spg_mem_executor_config *config, const char *slug,
    const char *description, size_t body_n,
    const struct spg_mem_executor_workspace *workspace,
    struct spg_mem_executor_result *result) {
    struct spg_sexpr_writer writer;
    spg_sexpr_writer_init(&writer, workspace->payload_capacity,
                          workspace->payload);
    enum spg_status s = spg_sexpr_writer_append_text(&writer, "(memory_save");
    if (s == SPG_OK) {
        s = spg_sexpr_writer_append_text(&writer, " (slug ");
    }
    if (s == SPG_OK) {
        s = spg_sexpr_writer_append_text(&writer, slug);
    }
    if (s == SPG_OK) {
        s = spg_sexpr_writer_append_text(&writer, ") (bytes ");
    }
    if (s == SPG_OK) {
        s = spg_sexpr_writer_append_size(&writer, body_n);
    }
    if (s == SPG_OK) {
        s = spg_sexpr_writer_append_text(
            &writer, result->save_status == SPG_OK ? ") (status ok))"
                                                   : ") (status error))");
    }
    (void)description;
    result->payload_used      = writer.used;
    result->payload_truncated = writer.truncated;

    if (!config->write_journal || state->journal == nullptr) {
        return SPG_OK;
    }
    return spg_journal_writer_append(
        state->journal, config->timestamp_ns, config->parent_sequence,
        SPG_JOURNAL_EVENT_MEMORY, result->save_status, writer.used,
        (const uint8_t *)workspace->payload, &result->memory_sequence);
}

enum spg_status spg_mem_executor_step(
    struct spg_mem_executor_state *state,
    const struct spg_mem_executor_config *config, const size_t text_n,
    const char text[], const struct spg_recommendation *recommendation,
    const struct spg_policy_decision *policy_decision,
    const struct spg_mem_executor_workspace *workspace,
    struct spg_mem_executor_result *result) {
    if (state == nullptr || state->store == nullptr || config == nullptr ||
        text == nullptr || recommendation == nullptr ||
        policy_decision == nullptr || !workspace_valid(workspace) ||
        result == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *result = (struct spg_mem_executor_result){.save_status = SPG_OK};

    if (policy_decision->kind != SPG_POLICY_DECISION_ALLOW ||
        recommendation->action_kind != SPG_ACTION_MEMORY_SAVE) {
        return SPG_E_INVALID_ARG;
    }

    char slug[SPG_MEM_SLUG_MAX + 1u];
    char description[SPG_MEM_DESC_MAX + 1u];
    static char body[SPG_MEM_BODY_MAX + 1u]; /* not on the hot path */
    body[0] = '\0';
    if (!copy_span(text_n, text, recommendation->mem_slug, slug, sizeof slug) ||
        !copy_span(text_n, text, recommendation->mem_description, description,
                   sizeof description) ||
        !copy_span(text_n, text, recommendation->mem_body, body, sizeof body)) {
        result->save_status = SPG_E_INVALID_ARG;
    } else {
        result->save_status =
            spg_mem_save(state->store, slug, description, body);
    }
    result->saved = result->save_status == SPG_OK;

    return journal_memory(state, config, slug, description, strlen(body),
                          workspace, result);
}
