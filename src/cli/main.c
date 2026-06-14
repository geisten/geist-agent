#include "sporegeist/sporegeist.h"

#include "sporegeist/exec_command.h"
#include "sporegeist/mem_command.h"
#include "sporegeist/mem_store.h"

#include <geist.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CLI_TOKEN_CAPACITY 1024u
#define CLI_NODE_CAPACITY 1024u
#define CLI_CONTEXT_BYTES 32768u
#define CLI_MODEL_OUTPUT_BYTES 8192u
#define CLI_PAYLOAD_BYTES 8192u
#define CLI_CONTEXT_REFS 64u
#define CLI_JOURNAL_VERIFY_PAYLOAD_BYTES 8192u
#define CLI_REPLAY_PAYLOAD_BYTES CLI_CONTEXT_BYTES
#define CLI_REPLAY_PREVIEW_BYTES 96u

struct file_buffer {
    size_t n;
    char  *data;
};

static void free_file_buffer(struct file_buffer *buffer);
static enum spg_status load_policy_file(const char *path,
                                        struct file_buffer *policy_text,
                                        struct spg_policy_config *policy);
static enum spg_status load_scenario_file(const char *path,
                                          struct file_buffer *scenario_text,
                                          struct spg_sim_config *sim);

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s <command> [args]\n"
            "\n"
            "commands:\n"
            "  version          print sporegeist and libgeist versions\n"
            "  exec             run a guarded local command and capture output\n"
            "  memory           store/recall Markdown long-term memories\n"
            "  tick             run one fake-model orchestrator tick\n"
            "  run              run fake-model orchestrator ticks\n"
            "  replay           print a journal timeline as JSONL\n"
            "  verify-journal   verify and summarize a journal\n"
            "  policy-check     validate and summarize a policy file\n"
            "  sim-validate     validate and summarize a scenario file\n",
            argv0);
}

static void print_tick_usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s tick --run <run.spg> --fake '<recommendation>'\n"
            "\n"
            "Runs exactly one orchestrator tick with a fake model output.\n"
            "Run config supplies policy, scenario and journal paths.\n",
            argv0);
}

static void print_run_usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s run --config <run.spg> [--fake '<recommendation>'] "
            "[--ticks <n>] [--write-sim-state <build/final.spg>] "
            "[--write-run-state <build/run-state.json>]\n"
            "\n"
            "Runs orchestrator ticks with one mutable in-process "
            "simulator state.\n"
            "Without --fake, the model path from the run config is loaded via "
            "libgeist.\n"
            "Shell and network recommendations stop after recommendation and "
            "policy gating.\n"
            "If requested, writes the final simulator state as a scenario "
            "S-expression.\n",
            argv0);
}

static void print_verify_journal_usage(const char *argv0) {
    fprintf(stderr, "usage: %s verify-journal <journal.sgj>\n", argv0);
}

static void print_replay_usage(const char *argv0) {
    fprintf(stderr, "usage: %s replay <journal.sgj>\n", argv0);
}

static void print_policy_check_usage(const char *argv0) {
    fprintf(stderr, "usage: %s policy-check <policy.spg>\n", argv0);
}

static void print_sim_validate_usage(const char *argv0) {
    fprintf(stderr, "usage: %s sim-validate <scenario.spg>\n", argv0);
}

static const char *policy_capability_kind_name(
    const enum spg_policy_capability_kind kind) {
    switch (kind) {
    case SPG_POLICY_CAP_LOCAL_SHELL:
        return "local_shell";
    case SPG_POLICY_CAP_SSH_AUTH_PROBE:
        return "ssh_auth_probe";
    case SPG_POLICY_CAP_SIMULATOR:
        return "simulator";
    case SPG_POLICY_CAP_MEMORY:
        return "memory";
    }
    return "unknown";
}

static const char *policy_network_default_name(
    const enum spg_policy_network_default value) {
    switch (value) {
    case SPG_POLICY_NETWORK_DENY:
        return "deny";
    case SPG_POLICY_NETWORK_ALLOW:
        return "allow";
    }
    return "unknown";
}

static void print_span_text(const size_t input_n, const char input[],
                            const struct spg_text_span span) {
    if (input == nullptr || span.offset > input_n ||
        span.length > input_n - span.offset) {
        printf("<invalid>");
        return;
    }
    printf("%.*s", (int)span.length, input + span.offset);
}

static const char *journal_event_kind_name(const uint32_t kind) {
    switch ((enum spg_journal_event_kind)kind) {
    case SPG_JOURNAL_EVENT_RUN_START:
        return "run_start";
    case SPG_JOURNAL_EVENT_POLICY_DECISION:
        return "policy_decision";
    case SPG_JOURNAL_EVENT_MODEL_INPUT:
        return "model_input";
    case SPG_JOURNAL_EVENT_MODEL_OUTPUT:
        return "model_output";
    case SPG_JOURNAL_EVENT_ACTION:
        return "action";
    case SPG_JOURNAL_EVENT_RESULT:
        return "result";
    case SPG_JOURNAL_EVENT_GRAPH:
        return "graph";
    case SPG_JOURNAL_EVENT_MEMORY:
        return "memory";
    case SPG_JOURNAL_EVENT_SIM:
        return "sim";
    case SPG_JOURNAL_EVENT_ERROR:
        return "error";
    }
    return "unknown";
}

static uint32_t replay_first_child(
    const struct spg_sexpr_node nodes[static 1], const uint32_t node) {
    return nodes[node].first_child;
}

static bool replay_span_eq_cstr(const size_t input_n, const char input[],
                                const struct spg_text_span span,
                                const char *expected) {
    if (input == nullptr || expected == nullptr || span.offset > input_n ||
        span.length > input_n - span.offset) {
        return false;
    }
    const size_t expected_n = strlen(expected);
    return span.length == expected_n &&
           memcmp(input + span.offset, expected, expected_n) == 0;
}

static bool replay_field_value(const size_t input_n, const char input[],
                               const struct spg_sexpr_node nodes[static 1],
                               const size_t node_count, const char *form_name,
                               const char *field_name,
                               struct spg_text_span *out) {
    if (input == nullptr || nodes == nullptr || node_count == 0u ||
        form_name == nullptr || field_name == nullptr || out == nullptr) {
        return false;
    }
    const uint32_t form = 0u;
    const uint32_t name = replay_first_child(nodes, form);
    if (name == SPG_SEXPR_INVALID_INDEX ||
        !replay_span_eq_cstr(input_n, input, nodes[name].span, form_name)) {
        return false;
    }
    uint32_t field = nodes[name].next_sibling;
    while (field != SPG_SEXPR_INVALID_INDEX) {
        const uint32_t field_name_node = replay_first_child(nodes, field);
        if (field_name_node != SPG_SEXPR_INVALID_INDEX &&
            replay_span_eq_cstr(input_n, input, nodes[field_name_node].span,
                                field_name)) {
            const uint32_t value = nodes[field_name_node].next_sibling;
            if (value != SPG_SEXPR_INVALID_INDEX &&
                nodes[value].next_sibling == SPG_SEXPR_INVALID_INDEX) {
                *out = nodes[value].span;
                return true;
            }
            return false;
        }
        field = nodes[field].next_sibling;
    }
    return false;
}

static bool replay_payload_field(const size_t payload_n, const uint8_t payload[],
                                 const char *form_name, const char *field_name,
                                 struct spg_text_span *out) {
    if (payload == nullptr || out == nullptr) {
        return false;
    }
    struct spg_sexpr_token tokens[CLI_TOKEN_CAPACITY];
    struct spg_sexpr_node  nodes[CLI_NODE_CAPACITY];
    struct spg_sexpr_error error = {};
    size_t token_count = 0u;
    size_t node_count = 0u;
    const char *text = (const char *)payload;
    const enum spg_status status = spg_sexpr_parse_text(
        payload_n, text, CLI_TOKEN_CAPACITY, tokens, CLI_NODE_CAPACITY, nodes,
        &token_count, &node_count, &error);
    if (status != SPG_OK) {
        return false;
    }
    (void)token_count;
    return replay_field_value(payload_n, text, nodes, node_count, form_name,
                              field_name, out);
}

static bool payload_span_valid(const size_t payload_n,
                               const struct spg_text_span span) {
    return span.offset <= payload_n && span.length <= payload_n - span.offset;
}

static bool payload_span_is_uint(const size_t payload_n,
                                 const uint8_t payload[],
                                 const struct spg_text_span span) {
    if (payload == nullptr || !payload_span_valid(payload_n, span) ||
        span.length == 0u) {
        return false;
    }
    for (size_t i = 0u; i < span.length; i += 1u) {
        const unsigned char ch = payload[span.offset + i];
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    return true;
}

static bool payload_span_is_bool(const size_t payload_n,
                                 const uint8_t payload[],
                                 const struct spg_text_span span) {
    return replay_span_eq_cstr(payload_n, (const char *)payload, span, "true") ||
           replay_span_eq_cstr(payload_n, (const char *)payload, span, "false");
}

static void print_json_bytes(const size_t bytes_n, const uint8_t bytes[]) {
    printf("\"");
    if (bytes != nullptr) {
        for (size_t i = 0u; i < bytes_n; i += 1u) {
            const unsigned char ch = bytes[i];
            if (ch == '"' || ch == '\\') {
                printf("\\%c", ch);
            } else if (ch == '\n') {
                printf("\\n");
            } else if (ch == '\r') {
                printf("\\r");
            } else if (ch == '\t') {
                printf("\\t");
            } else if (ch >= 32u && ch <= 126u) {
                printf("%c", ch);
            } else {
                printf("\\u%04x", (unsigned int)ch);
            }
        }
    }
    printf("\"");
}

static void print_json_span_string(const size_t payload_n,
                                   const uint8_t payload[],
                                   const struct spg_text_span span) {
    if (payload == nullptr || !payload_span_valid(payload_n, span)) {
        printf("null");
        return;
    }
    print_json_bytes(span.length, payload + span.offset);
}

static void print_json_span_value(const size_t payload_n,
                                  const uint8_t payload[],
                                  const struct spg_text_span span) {
    if (payload == nullptr || !payload_span_valid(payload_n, span)) {
        printf("null");
        return;
    }
    if (payload_span_is_uint(payload_n, payload, span) ||
        payload_span_is_bool(payload_n, payload, span)) {
        printf("%.*s", (int)span.length, (const char *)payload + span.offset);
        return;
    }
    print_json_span_string(payload_n, payload, span);
}

static void print_json_preview(const size_t payload_n, const uint8_t payload[]) {
    const size_t n =
        payload_n < CLI_REPLAY_PREVIEW_BYTES ? payload_n : CLI_REPLAY_PREVIEW_BYTES;
    print_json_bytes(n, payload);
}

static int verify_journal_command(const char *path) {
    if (path == nullptr) {
        return 2;
    }

    struct spg_journal_reader reader = {};
    enum spg_status status = spg_journal_reader_open(&reader, path);
    if (status != SPG_OK) {
        fprintf(stderr, "verify-journal: open failed: %s\n",
                spg_status_to_string(status));
        return 1;
    }

    uint8_t payload[CLI_JOURNAL_VERIFY_PAYLOAD_BYTES];
    uint64_t counts[SPG_JOURNAL_EVENT_ERROR + 1u] = {};
    uint64_t total = 0u;
    uint64_t truncated_payloads = 0u;
    uint64_t status_failures = 0u;
    uint64_t payload_bytes = 0u;
    uint64_t last_sequence = 0u;
    struct spg_journal_record record = {};

    for (;;) {
        status = spg_journal_reader_next(&reader, sizeof payload, payload,
                                         &record);
        if (status == SPG_E_NOT_FOUND) {
            break;
        }
        if (status != SPG_OK && status != SPG_E_LIMIT) {
            fprintf(stderr, "verify-journal: corrupt at next record: %s\n",
                    spg_status_to_string(status));
            (void)spg_journal_reader_close(&reader);
            return 1;
        }

        total += 1u;
        last_sequence = record.header.sequence;
        if (record.header.event_kind <= SPG_JOURNAL_EVENT_ERROR) {
            counts[record.header.event_kind] += 1u;
        }
        if (record.header.status != (uint32_t)SPG_OK) {
            status_failures += 1u;
        }
        if (record.header.payload_bytes > UINT64_MAX - payload_bytes) {
            payload_bytes = UINT64_MAX;
        } else {
            payload_bytes += record.header.payload_bytes;
        }
        if (status == SPG_E_LIMIT) {
            truncated_payloads += 1u;
        }
    }

    const enum spg_status close_status = spg_journal_reader_close(&reader);
    if (close_status != SPG_OK) {
        fprintf(stderr, "verify-journal: close failed: %s\n",
                spg_status_to_string(close_status));
        return 1;
    }

    printf("journal=%s\n", path);
    printf("verified=true\n");
    printf("records=%llu\n", (unsigned long long)total);
    printf("last_sequence=%llu\n", (unsigned long long)last_sequence);
    printf("payload_bytes=%llu\n", (unsigned long long)payload_bytes);
    printf("status_failures=%llu\n", (unsigned long long)status_failures);
    printf("truncated_payloads=%llu\n",
           (unsigned long long)truncated_payloads);
    for (uint32_t i = 0u; i <= (uint32_t)SPG_JOURNAL_EVENT_ERROR; i += 1u) {
        if (counts[i] == 0u) {
            continue;
        }
        printf("event.%s=%llu\n", journal_event_kind_name(i),
               (unsigned long long)counts[i]);
    }
    return 0;
}

static void print_replay_common_json(const struct spg_journal_record *record,
                                     const bool truncated) {
    printf("{\"seq\":%llu", (unsigned long long)record->header.sequence);
    printf(",\"parent\":%llu",
           (unsigned long long)record->header.parent_sequence);
    printf(",\"ts\":%llu", (unsigned long long)record->header.timestamp_ns);
    printf(",\"event\":");
    print_json_bytes(strlen(journal_event_kind_name(record->header.event_kind)),
                     (const uint8_t *)journal_event_kind_name(
                         record->header.event_kind));
    printf(",\"status\":");
    print_json_bytes(
        strlen(spg_status_to_string((enum spg_status)record->header.status)),
        (const uint8_t *)spg_status_to_string(
            (enum spg_status)record->header.status));
    printf(",\"payload_bytes\":%llu",
           (unsigned long long)record->header.payload_bytes);
    printf(",\"truncated\":%s", truncated ? "true" : "false");
}

static void print_replay_field_json(const size_t payload_n,
                                    const uint8_t payload[],
                                    const char *form_name,
                                    const char *field_name) {
    struct spg_text_span value = {};
    if (!replay_payload_field(payload_n, payload, form_name, field_name,
                              &value)) {
        return;
    }
    printf(",\"%s\":", field_name);
    print_json_span_value(payload_n, payload, value);
}

static void print_replay_policy_json(const size_t payload_n,
                                     const uint8_t payload[]) {
    const char *fields[] = {"decision", "deny_reason", "action_kind",
                            "cost", "uses_network", "confidence_bp"};
    for (size_t i = 0u; i < sizeof fields / sizeof fields[0]; i += 1u) {
        print_replay_field_json(payload_n, payload, "policy_decision",
                                fields[i]);
    }
}

static void print_replay_sim_json(const size_t payload_n,
                                  const uint8_t payload[]) {
    const char *fields[] = {"action", "selected_index", "mutated",
                            "risk_before", "risk_after"};
    for (size_t i = 0u; i < sizeof fields / sizeof fields[0]; i += 1u) {
        print_replay_field_json(payload_n, payload, "sim_result", fields[i]);
    }
}

static int replay_command(const char *path) {
    if (path == nullptr) {
        return 2;
    }

    struct spg_journal_reader reader = {};
    enum spg_status status = spg_journal_reader_open(&reader, path);
    if (status != SPG_OK) {
        fprintf(stderr, "replay: open failed: %s\n",
                spg_status_to_string(status));
        return 1;
    }

    uint8_t payload[CLI_REPLAY_PAYLOAD_BYTES];
    struct spg_journal_record record = {};

    for (;;) {
        status = spg_journal_reader_next(&reader, sizeof payload, payload,
                                         &record);
        if (status == SPG_E_NOT_FOUND) {
            break;
        }
        if (status != SPG_OK && status != SPG_E_LIMIT) {
            fprintf(stderr, "replay: corrupt at next record: %s\n",
                    spg_status_to_string(status));
            (void)spg_journal_reader_close(&reader);
            return 1;
        }

        const bool truncated = status == SPG_E_LIMIT;
        print_replay_common_json(&record, truncated);
        const size_t available_payload =
            truncated ? sizeof payload : record.payload_used;
        switch ((enum spg_journal_event_kind)record.header.event_kind) {
        case SPG_JOURNAL_EVENT_MODEL_OUTPUT:
            printf(",\"preview\":");
            print_json_preview(available_payload, payload);
            printf(",\"preview_truncated\":%s",
                   available_payload > CLI_REPLAY_PREVIEW_BYTES ? "true"
                                                                 : "false");
            break;
        case SPG_JOURNAL_EVENT_POLICY_DECISION:
            if (!truncated) {
                print_replay_policy_json(available_payload, payload);
            }
            break;
        case SPG_JOURNAL_EVENT_SIM:
            if (!truncated) {
                print_replay_sim_json(available_payload, payload);
            }
            break;
        case SPG_JOURNAL_EVENT_RUN_START:
        case SPG_JOURNAL_EVENT_MODEL_INPUT:
        case SPG_JOURNAL_EVENT_ACTION:
        case SPG_JOURNAL_EVENT_RESULT:
        case SPG_JOURNAL_EVENT_GRAPH:
        case SPG_JOURNAL_EVENT_MEMORY:
        case SPG_JOURNAL_EVENT_ERROR:
            break;
        }
        printf("}\n");
    }

    const enum spg_status close_status = spg_journal_reader_close(&reader);
    if (close_status != SPG_OK) {
        fprintf(stderr, "replay: close failed: %s\n",
                spg_status_to_string(close_status));
        return 1;
    }

    return 0;
}

static void print_budget_summary(const struct spg_run_budgets *budgets) {
    printf("budget.inference_steps=%llu\n",
           (unsigned long long)budgets->inference_steps);
    printf("budget.tokens=%llu\n", (unsigned long long)budgets->tokens);
    printf("budget.shell_actions=%llu\n",
           (unsigned long long)budgets->shell_actions);
    printf("budget.sim_actions=%llu\n",
           (unsigned long long)budgets->sim_actions);
    printf("budget.wall_ms=%llu\n", (unsigned long long)budgets->wall_ms);
    printf("budget.journal_bytes=%llu\n",
           (unsigned long long)budgets->journal_bytes);
    printf("budget.risk_bp=%llu\n", (unsigned long long)budgets->risk_bp);
}

static int policy_check_command(const char *path) {
    if (path == nullptr) {
        return 2;
    }
    struct file_buffer policy_text = {};
    struct spg_policy_config policy = {};
    const enum spg_status status = load_policy_file(path, &policy_text, &policy);
    if (status != SPG_OK) {
        fprintf(stderr, "policy-check: load failed: %s\n",
                spg_status_to_string(status));
        free_file_buffer(&policy_text);
        return 1;
    }

    printf("policy=%s\n", path);
    printf("valid=true\n");
    printf("network_default=%s\n",
           policy_network_default_name(policy.network_default));
    print_budget_summary(&policy.budgets);
    printf("capabilities=%zu\n", policy.capability_count);
    for (size_t i = 0u; i < policy.capability_count; i += 1u) {
        const struct spg_policy_capability *cap = &policy.capabilities[i];
        printf("capability.%zu.name=", i);
        print_span_text(policy_text.n, policy_text.data, cap->name);
        printf("\n");
        printf("capability.%zu.kind=%s\n", i,
               policy_capability_kind_name(cap->kind));
        printf("capability.%zu.enabled=%s\n", i,
               cap->enabled ? "true" : "false");
        printf("capability.%zu.budget=%llu\n", i,
               (unsigned long long)cap->budget);
    }

    free_file_buffer(&policy_text);
    return 0;
}

static void print_risk_summary(const struct spg_risk_score *risk) {
    printf("risk.asset=%llu\n", (unsigned long long)risk->asset_component);
    printf("risk.exposure=%llu\n",
           (unsigned long long)risk->exposure_component);
    printf("risk.vulnerability=%llu\n",
           (unsigned long long)risk->vulnerability_component);
    printf("risk.credential=%llu\n",
           (unsigned long long)risk->credential_component);
    printf("risk.reachability=%llu\n",
           (unsigned long long)risk->reachability_component);
    printf("risk.total=%llu\n", (unsigned long long)risk->total);
}

static const char *action_kind_name(const enum spg_action_kind kind) {
    switch (kind) {
    case SPG_ACTION_LOCAL_SHELL:
        return "local_shell";
    case SPG_ACTION_SSH_AUTH_PROBE:
        return "ssh_auth_probe";
    case SPG_ACTION_SIMULATOR:
        return "simulator";
    case SPG_ACTION_MEMORY_SAVE:
        return "memory_save";
    }
    return "unknown";
}

static bool parse_positive_size(const char *text, size_t *out) {
    if (text == nullptr || out == nullptr || text[0] == '\0') {
        return false;
    }
    errno     = 0;
    char *end = nullptr;
    const unsigned long long value = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || value == 0u ||
        value > (unsigned long long)SIZE_MAX) {
        return false;
    }
    *out = (size_t)value;
    return true;
}

static void add_budget_u64(uint64_t *value, const uint64_t delta) {
    if (value == nullptr) {
        return;
    }
    if (delta > UINT64_MAX - *value) {
        *value = UINT64_MAX;
        return;
    }
    *value += delta;
}

static uint64_t latest_result_sequence(
    const struct spg_orchestrator_result *result,
    const uint64_t fallback_sequence) {
    if (result == nullptr) {
        return fallback_sequence;
    }
    if (result->sim.memory_sequence != 0u) {
        return result->sim.memory_sequence;
    }
    if (result->sim.graph_sequence != 0u) {
        return result->sim.graph_sequence;
    }
    if (result->sim.sim_sequence != 0u) {
        return result->sim.sim_sequence;
    }
    if (result->policy_gate.policy_sequence != 0u) {
        return result->policy_gate.policy_sequence;
    }
    if (result->actor.memory_sequence != 0u) {
        return result->actor.memory_sequence;
    }
    if (result->actor.graph_sequence != 0u) {
        return result->actor.graph_sequence;
    }
    if (result->actor.model_output_sequence != 0u) {
        return result->actor.model_output_sequence;
    }
    if (result->actor.model_input_sequence != 0u) {
        return result->actor.model_input_sequence;
    }
    return fallback_sequence;
}

static void update_run_usage(struct spg_policy_usage *usage,
                             const struct spg_orchestrator_result *result) {
    if (usage == nullptr || result == nullptr) {
        return;
    }
    add_budget_u64(&usage->consumed.inference_steps, 1u);
    add_budget_u64(&usage->consumed.tokens,
                   (uint64_t)result->actor.tokens_decoded);
    if (result->sim_executed) {
        add_budget_u64(&usage->consumed.sim_actions,
                       result->recommendation.action.cost);
    }
}

static void print_run_tick_summary(
    const size_t tick_index, const struct spg_orchestrator_result *result,
    const struct spg_policy_usage *usage) {
    printf("tick=%zu", tick_index);
    printf(" stage=%s", spg_orchestrator_stage_to_string(result->stage));
    printf(" recommendation=%s",
           result->recommendation_valid ? "valid" : "rejected");
    if (!result->recommendation_valid) {
        printf(" reject_reason=%s",
               spg_recommendation_reject_reason_to_string(
                   result->recommendation.reject_reason));
    } else {
        printf(" action=%s",
               action_kind_name(result->recommendation.action_kind));
    }
    if (result->policy_evaluated) {
        printf(" policy=%s",
               result->policy_gate.decision.kind == SPG_POLICY_DECISION_ALLOW
                   ? "allow"
                   : "deny");
        printf(" deny_reason=%s",
               spg_policy_deny_reason_to_string(
                   result->policy_gate.decision.deny_reason));
    }
    if (result->sim_executed) {
        printf(" sim_action=%s",
               spg_sim_exec_action_to_string(result->sim.action));
        printf(" risk_before=%llu",
               (unsigned long long)result->sim.risk_before.total);
        printf(" risk_after=%llu",
               (unsigned long long)result->sim.risk_after.total);
    }
    printf(" consumed.inference_steps=%llu",
           (unsigned long long)usage->consumed.inference_steps);
    printf(" consumed.tokens=%llu",
           (unsigned long long)usage->consumed.tokens);
    printf(" consumed.sim_actions=%llu",
           (unsigned long long)usage->consumed.sim_actions);
    printf("\n");
}

static bool file_span_valid(const size_t text_n,
                            const struct spg_text_span span) {
    return span.offset <= text_n && span.length <= text_n - span.offset;
}

static enum spg_status write_file_text(FILE *file, const char *text) {
    if (file == nullptr || text == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    return fputs(text, file) >= 0 ? SPG_OK : SPG_E_IO;
}

static enum spg_status write_file_span(FILE *file, const size_t text_n,
                                       const char text[],
                                       const struct spg_text_span span) {
    if (file == nullptr || text == nullptr || !file_span_valid(text_n, span)) {
        return SPG_E_INVALID_ARG;
    }
    if (span.length == 0u) {
        return SPG_OK;
    }
    return fwrite(text + span.offset, 1u, span.length, file) == span.length
               ? SPG_OK
               : SPG_E_IO;
}

static enum spg_status write_file_u32(FILE *file, const uint32_t value) {
    if (file == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    return fprintf(file, "%u", value) >= 0 ? SPG_OK : SPG_E_IO;
}

static enum spg_status write_file_u64(FILE *file, const uint64_t value) {
    if (file == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    return fprintf(file, "%llu", (unsigned long long)value) >= 0 ? SPG_OK
                                                                 : SPG_E_IO;
}

static enum spg_status write_file_bool(FILE *file, const bool value) {
    return write_file_text(file, value ? "true" : "false");
}

static enum spg_status write_sim_state_file(const char *path,
                                            const size_t source_n,
                                            const char source[],
                                            const struct spg_sim_config *sim) {
    if (path == nullptr || source == nullptr || sim == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    FILE *file = fopen(path, "wb");
    if (file == nullptr) {
        return SPG_E_IO;
    }

#define WRITE_TEXT(value_)                                                     \
    do {                                                                       \
        status = write_file_text(file, (value_));                              \
        if (status != SPG_OK) {                                                \
            goto done;                                                         \
        }                                                                      \
    } while (0)
#define WRITE_SPAN(span_)                                                      \
    do {                                                                       \
        status = write_file_span(file, source_n, source, (span_));             \
        if (status != SPG_OK) {                                                \
            goto done;                                                         \
        }                                                                      \
    } while (0)
#define WRITE_U32(value_)                                                      \
    do {                                                                       \
        status = write_file_u32(file, (value_));                               \
        if (status != SPG_OK) {                                                \
            goto done;                                                         \
        }                                                                      \
    } while (0)
#define WRITE_BOOL(value_)                                                     \
    do {                                                                       \
        status = write_file_bool(file, (value_));                              \
        if (status != SPG_OK) {                                                \
            goto done;                                                         \
        }                                                                      \
    } while (0)

    enum spg_status status = SPG_OK;
    WRITE_TEXT("(scenario\n");
    for (size_t i = 0u; i < sim->host_count; i += 1u) {
        WRITE_TEXT(" (host (id ");
        WRITE_SPAN(sim->hosts[i].id);
        WRITE_TEXT(") (criticality_bp ");
        WRITE_U32(sim->hosts[i].criticality_bp);
        WRITE_TEXT("))\n");
    }
    for (size_t i = 0u; i < sim->service_count; i += 1u) {
        const struct spg_sim_service *service = &sim->services[i];
        if (service->host_index >= sim->host_count) {
            status = SPG_E_SCHEMA;
            goto done;
        }
        WRITE_TEXT(" (service (id ");
        WRITE_SPAN(service->id);
        WRITE_TEXT(") (host ");
        WRITE_SPAN(sim->hosts[service->host_index].id);
        WRITE_TEXT(") (name ");
        WRITE_SPAN(service->name);
        WRITE_TEXT(") (port ");
        WRITE_U32(service->port);
        WRITE_TEXT(") (exposure_bp ");
        WRITE_U32(service->exposure_bp);
        WRITE_TEXT("))\n");
    }
    for (size_t i = 0u; i < sim->account_count; i += 1u) {
        const struct spg_sim_account *account = &sim->accounts[i];
        if (account->host_index >= sim->host_count) {
            status = SPG_E_SCHEMA;
            goto done;
        }
        WRITE_TEXT(" (account (id ");
        WRITE_SPAN(account->id);
        WRITE_TEXT(") (host ");
        WRITE_SPAN(sim->hosts[account->host_index].id);
        WRITE_TEXT(") (username ");
        WRITE_SPAN(account->username);
        WRITE_TEXT(") (enabled ");
        WRITE_BOOL(account->enabled);
        WRITE_TEXT("))\n");
    }
    for (size_t i = 0u; i < sim->credential_count; i += 1u) {
        const struct spg_sim_credential *credential = &sim->credentials[i];
        if (credential->account_index >= sim->account_count) {
            status = SPG_E_SCHEMA;
            goto done;
        }
        WRITE_TEXT(" (credential (id ");
        WRITE_SPAN(credential->id);
        WRITE_TEXT(") (account ");
        WRITE_SPAN(sim->accounts[credential->account_index].id);
        WRITE_TEXT(") (strength_bp ");
        WRITE_U32(credential->strength_bp);
        WRITE_TEXT("))\n");
    }
    for (size_t i = 0u; i < sim->vulnerability_count; i += 1u) {
        const struct spg_sim_vulnerability *vuln = &sim->vulnerabilities[i];
        if (vuln->service_index >= sim->service_count) {
            status = SPG_E_SCHEMA;
            goto done;
        }
        WRITE_TEXT(" (vulnerability (id ");
        WRITE_SPAN(vuln->id);
        WRITE_TEXT(") (service ");
        WRITE_SPAN(sim->services[vuln->service_index].id);
        WRITE_TEXT(") (severity_bp ");
        WRITE_U32(vuln->severity_bp);
        WRITE_TEXT(") (patched ");
        WRITE_BOOL(vuln->patched);
        WRITE_TEXT("))\n");
    }
    for (size_t i = 0u; i < sim->network_edge_count; i += 1u) {
        const struct spg_sim_network_edge *edge = &sim->network_edges[i];
        if (edge->from_host_index >= sim->host_count ||
            edge->to_host_index >= sim->host_count) {
            status = SPG_E_SCHEMA;
            goto done;
        }
        WRITE_TEXT(" (network_edge (from ");
        WRITE_SPAN(sim->hosts[edge->from_host_index].id);
        WRITE_TEXT(") (to ");
        WRITE_SPAN(sim->hosts[edge->to_host_index].id);
        WRITE_TEXT(") (reachability_bp ");
        WRITE_U32(edge->reachability_bp);
        WRITE_TEXT("))\n");
    }
    WRITE_TEXT(")\n");

done:
#undef WRITE_BOOL
#undef WRITE_U32
#undef WRITE_SPAN
#undef WRITE_TEXT
    if (fclose(file) != 0 && status == SPG_OK) {
        status = SPG_E_IO;
    }
    return status;
}

static enum spg_status write_run_state_file(
    const char *path, const size_t ticks, const struct spg_graph *graph,
    const struct spg_memory *memory, const struct spg_policy_usage *usage,
    const struct spg_risk_score *risk) {
    if (path == nullptr || graph == nullptr || memory == nullptr ||
        usage == nullptr || risk == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    FILE *file = fopen(path, "wb");
    if (file == nullptr) {
        return SPG_E_IO;
    }

#define WRITE_STATE_TEXT(value_)                                               \
    do {                                                                       \
        status = write_file_text(file, (value_));                              \
        if (status != SPG_OK) {                                                \
            goto done;                                                         \
        }                                                                      \
    } while (0)
#define WRITE_STATE_U64(value_)                                                \
    do {                                                                       \
        status = write_file_u64(file, (value_));                               \
        if (status != SPG_OK) {                                                \
            goto done;                                                         \
        }                                                                      \
    } while (0)

    enum spg_status status = SPG_OK;
    WRITE_STATE_TEXT("{\"version\":1");
    WRITE_STATE_TEXT(",\"ticks\":");
    WRITE_STATE_U64((uint64_t)ticks);
    WRITE_STATE_TEXT(",\"consumed\":{\"inference_steps\":");
    WRITE_STATE_U64(usage->consumed.inference_steps);
    WRITE_STATE_TEXT(",\"tokens\":");
    WRITE_STATE_U64(usage->consumed.tokens);
    WRITE_STATE_TEXT(",\"shell_actions\":");
    WRITE_STATE_U64(usage->consumed.shell_actions);
    WRITE_STATE_TEXT(",\"sim_actions\":");
    WRITE_STATE_U64(usage->consumed.sim_actions);
    WRITE_STATE_TEXT("}");
    WRITE_STATE_TEXT(",\"graph\":{\"nodes\":");
    WRITE_STATE_U64((uint64_t)graph->node_count);
    WRITE_STATE_TEXT(",\"edges\":");
    WRITE_STATE_U64((uint64_t)graph->edge_count);
    WRITE_STATE_TEXT("}");
    WRITE_STATE_TEXT(",\"memory\":{\"facts\":");
    WRITE_STATE_U64((uint64_t)memory->fact_count);
    WRITE_STATE_TEXT("}");
    WRITE_STATE_TEXT(",\"risk\":{\"total\":");
    WRITE_STATE_U64(risk->total);
    WRITE_STATE_TEXT("}}\n");

done:
#undef WRITE_STATE_U64
#undef WRITE_STATE_TEXT
    if (fclose(file) != 0 && status == SPG_OK) {
        status = SPG_E_IO;
    }
    return status;
}

static int sim_validate_command(const char *path) {
    if (path == nullptr) {
        return 2;
    }
    struct file_buffer scenario_text = {};
    struct spg_sim_config sim = {};
    enum spg_status status = load_scenario_file(path, &scenario_text, &sim);
    if (status != SPG_OK) {
        fprintf(stderr, "sim-validate: load failed: %s\n",
                spg_status_to_string(status));
        free_file_buffer(&scenario_text);
        return 1;
    }
    struct spg_risk_score risk = {};
    status = spg_risk_evaluate(&sim, &risk);
    if (status != SPG_OK) {
        fprintf(stderr, "sim-validate: risk failed: %s\n",
                spg_status_to_string(status));
        free_file_buffer(&scenario_text);
        return 1;
    }

    printf("scenario=%s\n", path);
    printf("valid=true\n");
    printf("hosts=%zu\n", sim.host_count);
    printf("services=%zu\n", sim.service_count);
    printf("accounts=%zu\n", sim.account_count);
    printf("credentials=%zu\n", sim.credential_count);
    printf("vulnerabilities=%zu\n", sim.vulnerability_count);
    printf("network_edges=%zu\n", sim.network_edge_count);
    print_risk_summary(&risk);

    free_file_buffer(&scenario_text);
    return 0;
}

static void free_file_buffer(struct file_buffer *buffer) {
    if (buffer == nullptr) {
        return;
    }
    safe_free((void **)&buffer->data);
    buffer->n = 0u;
}

static enum spg_status read_file(const char *path, struct file_buffer *out) {
    if (path == nullptr || out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct file_buffer){};
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        return SPG_E_IO;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        (void)fclose(file);
        return SPG_E_IO;
    }
    const long end = ftell(file);
    if (end < 0) {
        (void)fclose(file);
        return SPG_E_IO;
    }
    if ((unsigned long)end > SIZE_MAX - 1u) {
        (void)fclose(file);
        return SPG_E_OVERFLOW;
    }
    if (fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return SPG_E_IO;
    }
    const size_t n = (size_t)end;
    char *data = heap_alloc_aligned(n + 1u, alignof(char));
    if (data == nullptr) {
        (void)fclose(file);
        return SPG_E_OOM;
    }
    if (n > 0u && fread(data, 1u, n, file) != n) {
        safe_free((void **)&data);
        (void)fclose(file);
        return SPG_E_IO;
    }
    if (fclose(file) != 0) {
        safe_free((void **)&data);
        return SPG_E_IO;
    }
    data[n] = '\0';
    out->n = n;
    out->data = data;
    return SPG_OK;
}

static enum spg_status span_to_cstr(const size_t input_n, const char input[],
                                    const struct spg_text_span span,
                                    char **out) {
    if (input == nullptr || out == nullptr || span.offset > input_n ||
        span.length > input_n - span.offset) {
        return SPG_E_INVALID_ARG;
    }
    *out = nullptr;
    char *text = heap_alloc_aligned(span.length + 1u, alignof(char));
    if (text == nullptr) {
        return SPG_E_OOM;
    }
    memcpy(text, input + span.offset, span.length);
    text[span.length] = '\0';
    *out = text;
    return SPG_OK;
}

static enum spg_status load_run_file(const char *path,
                                     struct file_buffer *run_text,
                                     struct spg_run_config *run) {
    enum spg_status status = read_file(path, run_text);
    if (status != SPG_OK) {
        return status;
    }
    struct spg_sexpr_token tokens[CLI_TOKEN_CAPACITY];
    struct spg_sexpr_node  nodes[CLI_NODE_CAPACITY];
    struct spg_run_config_error error = {};
    status = spg_run_config_load(run_text->n, run_text->data,
                                 CLI_TOKEN_CAPACITY, tokens,
                                 CLI_NODE_CAPACITY, nodes, run, &error);
    return status;
}

static enum spg_status load_policy_file(const char *path,
                                        struct file_buffer *policy_text,
                                        struct spg_policy_config *policy) {
    enum spg_status status = read_file(path, policy_text);
    if (status != SPG_OK) {
        return status;
    }
    struct spg_sexpr_token tokens[CLI_TOKEN_CAPACITY];
    struct spg_sexpr_node  nodes[CLI_NODE_CAPACITY];
    struct spg_policy_config_error error = {};
    status = spg_policy_config_load(policy_text->n, policy_text->data,
                                    CLI_TOKEN_CAPACITY, tokens,
                                    CLI_NODE_CAPACITY, nodes, policy, &error);
    return status;
}

static enum spg_status load_scenario_file(const char *path,
                                          struct file_buffer *scenario_text,
                                          struct spg_sim_config *sim) {
    enum spg_status status = read_file(path, scenario_text);
    if (status != SPG_OK) {
        return status;
    }
    struct spg_sexpr_token tokens[CLI_TOKEN_CAPACITY];
    struct spg_sexpr_node  nodes[CLI_NODE_CAPACITY];
    struct spg_sim_config_error error = {};
    status = spg_sim_config_load(scenario_text->n, scenario_text->data,
                                 CLI_TOKEN_CAPACITY, tokens,
                                 CLI_NODE_CAPACITY, nodes, sim, &error);
    return status;
}

static int run_tick_fake(const char *run_path, const char *fake_output) {
    if (run_path == nullptr || fake_output == nullptr) {
        return 2;
    }

    int rc = 1;
    struct file_buffer run_text = {};
    struct file_buffer policy_text = {};
    struct file_buffer scenario_text = {};
    char *policy_path = nullptr;
    char *scenario_path = nullptr;
    char *journal_path = nullptr;
    struct spg_journal_writer journal = {};
    bool journal_open = false;
    struct spg_model_adapter model = {};
    bool model_open = false;

    struct spg_run_config run = {};
    enum spg_status status = load_run_file(run_path, &run_text, &run);
    if (status != SPG_OK) {
        fprintf(stderr, "tick: load run failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    status = span_to_cstr(run_text.n, run_text.data, run.policy_path,
                          &policy_path);
    if (status != SPG_OK) {
        fprintf(stderr, "tick: policy path failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    status = span_to_cstr(run_text.n, run_text.data, run.scenario_path,
                          &scenario_path);
    if (status != SPG_OK) {
        fprintf(stderr, "tick: scenario path failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    status = span_to_cstr(run_text.n, run_text.data, run.journal_path,
                          &journal_path);
    if (status != SPG_OK) {
        fprintf(stderr, "tick: journal path failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }

    struct spg_policy_config policy = {};
    status = load_policy_file(policy_path, &policy_text, &policy);
    if (status != SPG_OK) {
        fprintf(stderr, "tick: load policy failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    struct spg_sim_config sim = {};
    status = load_scenario_file(scenario_path, &scenario_text, &sim);
    if (status != SPG_OK) {
        fprintf(stderr, "tick: load scenario failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }

    status = spg_journal_writer_open(&journal, journal_path);
    if (status != SPG_OK) {
        fprintf(stderr, "tick: open journal failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    journal_open = true;

    const struct spg_model_adapter_config model_config = {
        .kind            = SPG_MODEL_ADAPTER_FAKE,
        .sampling        = {.top_p = 1.0f, .random_seed = run.seed},
        .fake_response_n = strlen(fake_output),
        .fake_response   = fake_output,
    };
    status = spg_model_adapter_init(&model, &model_config);
    if (status != SPG_OK) {
        fprintf(stderr, "tick: fake model init failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    model_open = true;

    struct spg_graph graph = {};
    struct spg_memory memory = {};
    spg_graph_init(&graph);
    spg_memory_init(&memory);

    struct spg_context_graph_ref graph_refs[CLI_CONTEXT_REFS];
    struct spg_context_memory_ref memory_refs[CLI_CONTEXT_REFS];
    struct spg_context_journal_ref journal_refs[CLI_CONTEXT_REFS];
    char context[CLI_CONTEXT_BYTES];
    char model_output[CLI_MODEL_OUTPUT_BYTES];
    struct spg_sexpr_token rec_tokens[CLI_TOKEN_CAPACITY];
    struct spg_sexpr_node rec_nodes[CLI_NODE_CAPACITY];
    char policy_payload[CLI_PAYLOAD_BYTES];
    char sim_payload[CLI_PAYLOAD_BYTES];

    const struct spg_orchestrator_workspace workspace = {
        .actor = {
            .context_capacity      = sizeof context,
            .context               = context,
            .model_output_capacity = sizeof model_output,
            .model_output          = model_output,
            .graph_ref_capacity    = CLI_CONTEXT_REFS,
            .graph_refs            = graph_refs,
            .memory_ref_capacity   = CLI_CONTEXT_REFS,
            .memory_refs           = memory_refs,
            .journal_ref_capacity  = CLI_CONTEXT_REFS,
            .journal_refs          = journal_refs,
        },
        .recommendation_token_capacity = CLI_TOKEN_CAPACITY,
        .recommendation_tokens         = rec_tokens,
        .recommendation_node_capacity  = CLI_NODE_CAPACITY,
        .recommendation_nodes          = rec_nodes,
        .policy_payload_capacity       = sizeof policy_payload,
        .policy_payload                = policy_payload,
        .sim_payload_capacity          = sizeof sim_payload,
        .sim_payload                   = sim_payload,
    };

    struct spg_policy_usage usage = {};
    struct spg_orchestrator_state state = {
        .graph         = &graph,
        .memory        = &memory,
        .journal       = &journal,
        .model         = &model,
        .sim           = &sim,
        .run           = &run,
        .usage         = &usage,
        .policy        = &policy,
        .policy_text_n = policy_text.n,
        .policy_text   = policy_text.data,
        .graph_text_n  = CLI_CONTEXT_BYTES,
        .graph_text    = context,
        .memory_text_n = CLI_MODEL_OUTPUT_BYTES,
        .memory_text   = model_output,
    };
    const struct spg_orchestrator_config config = {
        .actor_id            = 1u,
        .timestamp_ns        = 1u,
        .context_limits      = {.graph_nodes = CLI_CONTEXT_REFS,
                                .memory_facts = CLI_CONTEXT_REFS,
                                .journal_events = CLI_CONTEXT_REFS},
        .max_decode_tokens   = run.budgets.tokens > 0u ? 1u : 0u,
        .reset_model_session = true,
        .write_journal       = true,
        .update_graph        = true,
        .update_memory       = true,
    };
    struct spg_orchestrator_result result = {};
    status = spg_orchestrator_tick(&state, &config, &workspace, &result);
    if (status != SPG_OK) {
        fprintf(stderr, "tick: orchestrator failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }

    printf("stage=%s\n", spg_orchestrator_stage_to_string(result.stage));
    printf("recommendation=%s\n",
           result.recommendation_valid ? "valid" : "rejected");
    if (!result.recommendation_valid) {
        printf("reject_reason=%s\n",
               spg_recommendation_reject_reason_to_string(
                   result.recommendation.reject_reason));
    }
    if (result.policy_evaluated) {
        printf("policy=%s\n",
               result.policy_gate.decision.kind == SPG_POLICY_DECISION_ALLOW
                   ? "allow"
                   : "deny");
        printf("deny_reason=%s\n",
               spg_policy_deny_reason_to_string(
                   result.policy_gate.decision.deny_reason));
    }
    if (result.sim_executed) {
        printf("sim_action=%s\n",
               spg_sim_exec_action_to_string(result.sim.action));
        printf("risk_before=%llu\n",
               (unsigned long long)result.sim.risk_before.total);
        printf("risk_after=%llu\n",
               (unsigned long long)result.sim.risk_after.total);
    }
    printf("journal=%s\n", journal_path);
    printf("graph_nodes=%zu\n", graph.node_count);
    printf("memory_facts=%zu\n", memory.fact_count);
    rc = 0;

done:
    if (journal_open) {
        const enum spg_status close_status = spg_journal_writer_close(&journal);
        if (close_status != SPG_OK && rc == 0) {
            fprintf(stderr, "tick: close journal failed: %s\n",
                    spg_status_to_string(close_status));
            rc = 1;
        }
    }
    if (model_open) {
        spg_model_adapter_destroy(&model);
    }
    safe_free((void **)&policy_path);
    safe_free((void **)&scenario_path);
    safe_free((void **)&journal_path);
    free_file_buffer(&run_text);
    free_file_buffer(&policy_text);
    free_file_buffer(&scenario_text);
    return rc;
}

static int run_loop(const char *run_path, const char *fake_output,
                    const size_t ticks, const char *sim_state_path,
                    const char *run_state_path, const char *memory_dir) {
    if (run_path == nullptr || ticks == 0u) {
        return 2;
    }

    /* Memory store is opt-in: only opened when a directory is configured, so a
     * plain run never creates one. */
    struct spg_mem_store mem_store_obj;
    bool                 have_store = false;
    if (memory_dir != nullptr && memory_dir[0] != '\0') {
        have_store = spg_mem_store_open(&mem_store_obj, memory_dir) == SPG_OK;
        if (!have_store) {
            fprintf(stderr, "run: cannot open memory dir %s\n", memory_dir);
            return 1;
        }
    }

    int rc = 1;
    struct file_buffer run_text = {};
    struct file_buffer policy_text = {};
    struct file_buffer scenario_text = {};
    char *policy_path = nullptr;
    char *scenario_path = nullptr;
    char *journal_path = nullptr;
    char *model_path = nullptr;
    struct spg_journal_writer journal = {};
    bool journal_open = false;
    struct spg_model_adapter model = {};
    bool model_open = false;

    struct spg_run_config run = {};
    enum spg_status status = load_run_file(run_path, &run_text, &run);
    if (status != SPG_OK) {
        fprintf(stderr, "run: load run failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    if (run.budgets.inference_steps > 0u &&
        ticks > (size_t)run.budgets.inference_steps) {
        fprintf(stderr,
                "run: ticks exceed budget.inference_steps "
                "(ticks=%zu budget=%llu)\n",
                ticks, (unsigned long long)run.budgets.inference_steps);
        goto done;
    }

    status = span_to_cstr(run_text.n, run_text.data, run.policy_path,
                          &policy_path);
    if (status != SPG_OK) {
        fprintf(stderr, "run: policy path failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    status = span_to_cstr(run_text.n, run_text.data, run.scenario_path,
                          &scenario_path);
    if (status != SPG_OK) {
        fprintf(stderr, "run: scenario path failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    status = span_to_cstr(run_text.n, run_text.data, run.journal_path,
                          &journal_path);
    if (status != SPG_OK) {
        fprintf(stderr, "run: journal path failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    status = span_to_cstr(run_text.n, run_text.data, run.model_path,
                          &model_path);
    if (status != SPG_OK) {
        fprintf(stderr, "run: model path failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }

    struct spg_policy_config policy = {};
    status = load_policy_file(policy_path, &policy_text, &policy);
    if (status != SPG_OK) {
        fprintf(stderr, "run: load policy failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    struct spg_sim_config sim = {};
    status = load_scenario_file(scenario_path, &scenario_text, &sim);
    if (status != SPG_OK) {
        fprintf(stderr, "run: load scenario failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }

    status = spg_journal_writer_open(&journal, journal_path);
    if (status != SPG_OK) {
        fprintf(stderr, "run: open journal failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    journal_open = true;

    const bool use_fake = fake_output != nullptr;
    const struct spg_model_adapter_config model_config = {
        .kind            = use_fake ? SPG_MODEL_ADAPTER_FAKE
                                    : SPG_MODEL_ADAPTER_GEIST,
        .model_path      = use_fake ? nullptr : model_path,
        .sampling        = {.max_seq_len = 4096u,
                            .temperature = 0.0f,
                            .top_p = 1.0f,
                            .top_k = 0,
                            .random_seed = run.seed},
        .fake_response_n = use_fake ? strlen(fake_output) : 0u,
        .fake_response   = fake_output,
    };
    status = spg_model_adapter_init(&model, &model_config);
    if (status != SPG_OK) {
        fprintf(stderr, "run: model init failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    model_open = true;

    struct spg_graph graph = {};
    struct spg_memory memory = {};
    spg_graph_init(&graph);
    spg_memory_init(&memory);

    struct spg_context_graph_ref graph_refs[CLI_CONTEXT_REFS];
    struct spg_context_memory_ref memory_refs[CLI_CONTEXT_REFS];
    struct spg_context_journal_ref journal_refs[CLI_CONTEXT_REFS];
    char context[CLI_CONTEXT_BYTES];
    char model_output[CLI_MODEL_OUTPUT_BYTES];
    struct spg_sexpr_token rec_tokens[CLI_TOKEN_CAPACITY];
    struct spg_sexpr_node rec_nodes[CLI_NODE_CAPACITY];
    char policy_payload[CLI_PAYLOAD_BYTES];
    char sim_payload[CLI_PAYLOAD_BYTES];

    const struct spg_orchestrator_workspace workspace = {
        .actor = {
            .context_capacity      = sizeof context,
            .context               = context,
            .model_output_capacity = sizeof model_output,
            .model_output          = model_output,
            .graph_ref_capacity    = CLI_CONTEXT_REFS,
            .graph_refs            = graph_refs,
            .memory_ref_capacity   = CLI_CONTEXT_REFS,
            .memory_refs           = memory_refs,
            .journal_ref_capacity  = CLI_CONTEXT_REFS,
            .journal_refs          = journal_refs,
        },
        .recommendation_token_capacity = CLI_TOKEN_CAPACITY,
        .recommendation_tokens         = rec_tokens,
        .recommendation_node_capacity  = CLI_NODE_CAPACITY,
        .recommendation_nodes          = rec_nodes,
        .policy_payload_capacity       = sizeof policy_payload,
        .policy_payload                = policy_payload,
        .sim_payload_capacity          = sizeof sim_payload,
        .sim_payload                   = sim_payload,
    };

    struct spg_policy_usage usage = {};
    char                    mem_index_buf[4096] = {0};
    struct spg_orchestrator_state state = {
        .graph         = &graph,
        .memory        = &memory,
        .journal       = &journal,
        .model         = &model,
        .sim           = &sim,
        .store         = have_store ? &mem_store_obj : nullptr,
        .run           = &run,
        .usage         = &usage,
        .policy        = &policy,
        .policy_text_n = policy_text.n,
        .policy_text   = policy_text.data,
        .graph_text_n  = 0u,
        .graph_text    = nullptr,
        .memory_text_n = 0u,
        .memory_text   = nullptr,
        .memory_index  = have_store ? mem_index_buf : nullptr,
    };

    uint64_t parent_sequence = 0u;
    for (size_t i = 0u; i < ticks; i += 1u) {
        /* Refresh the memory index so the context reflects saves from prior
         * ticks. */
        if (have_store) {
            (void)spg_mem_index(&mem_store_obj, sizeof mem_index_buf,
                                mem_index_buf, nullptr, nullptr);
        }
        if (run.budgets.tokens > 0u &&
            usage.consumed.tokens >= run.budgets.tokens) {
            fprintf(stderr, "run: token budget exhausted before tick %zu\n",
                    i + 1u);
            goto done;
        }

        const struct spg_orchestrator_config config = {
            .actor_id            = 1u,
            .timestamp_ns        = i + 1u,
            .parent_sequence     = parent_sequence,
            .context_limits      = {.graph_nodes = CLI_CONTEXT_REFS,
                                    .memory_facts = CLI_CONTEXT_REFS,
                                    .journal_events = CLI_CONTEXT_REFS},
            .max_decode_tokens   = run.budgets.tokens > 0u ? 1u : 0u,
            .reset_model_session = true,
            .write_journal       = true,
            .update_graph        = true,
            .update_memory       = true,
        };
        struct spg_orchestrator_result result = {};
        status = spg_orchestrator_tick(&state, &config, &workspace, &result);
        if (status != SPG_OK) {
            fprintf(stderr, "run: orchestrator failed at tick %zu: %s\n",
                    i + 1u, spg_status_to_string(status));
            goto done;
        }

        update_run_usage(&usage, &result);
        parent_sequence = latest_result_sequence(&result, parent_sequence);
        print_run_tick_summary(i + 1u, &result, &usage);
    }

    struct spg_risk_score final_risk = {};
    status = spg_risk_evaluate(&sim, &final_risk);
    if (status != SPG_OK) {
        fprintf(stderr, "run: final risk failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }

    printf("journal=%s\n", journal_path);
    if (sim_state_path != nullptr) {
        status = write_sim_state_file(sim_state_path, scenario_text.n,
                                      scenario_text.data, &sim);
        if (status != SPG_OK) {
            fprintf(stderr, "run: write sim state failed: %s\n",
                    spg_status_to_string(status));
            goto done;
        }
        printf("sim_state=%s\n", sim_state_path);
    }
    if (run_state_path != nullptr) {
        status = write_run_state_file(run_state_path, ticks, &graph, &memory,
                                      &usage, &final_risk);
        if (status != SPG_OK) {
            fprintf(stderr, "run: write run state failed: %s\n",
                    spg_status_to_string(status));
            goto done;
        }
        printf("run_state=%s\n", run_state_path);
    }
    printf("ticks=%zu\n", ticks);
    printf("graph_nodes=%zu\n", graph.node_count);
    printf("memory_facts=%zu\n", memory.fact_count);
    printf("risk_final=%llu\n", (unsigned long long)final_risk.total);
    rc = 0;

done:
    if (journal_open) {
        const enum spg_status close_status = spg_journal_writer_close(&journal);
        if (close_status != SPG_OK && rc == 0) {
            fprintf(stderr, "run: close journal failed: %s\n",
                    spg_status_to_string(close_status));
            rc = 1;
        }
    }
    if (model_open) {
        spg_model_adapter_destroy(&model);
    }
    safe_free((void **)&policy_path);
    safe_free((void **)&scenario_path);
    safe_free((void **)&journal_path);
    safe_free((void **)&model_path);
    free_file_buffer(&run_text);
    free_file_buffer(&policy_text);
    free_file_buffer(&scenario_text);
    return rc;
}

static int tick_command(int argc, char **argv) {
    const char *run_path = nullptr;
    const char *fake_output = nullptr;
    for (int i = 2; i < argc; i += 1) {
        if (strcmp(argv[i], "--run") == 0 && i + 1 < argc) {
            run_path = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--fake") == 0 && i + 1 < argc) {
            fake_output = argv[i + 1];
            i += 1;
            continue;
        }
        fprintf(stderr, "tick: unknown or incomplete argument: %s\n", argv[i]);
        return 2;
    }
    if (run_path == nullptr || fake_output == nullptr) {
        return 2;
    }
    return run_tick_fake(run_path, fake_output);
}

static int run_command(int argc, char **argv) {
    const char *run_path = nullptr;
    const char *fake_output = nullptr;
    const char *sim_state_path = nullptr;
    const char *run_state_path = nullptr;
    const char *memory_dir     = getenv("SPOREGEIST_MEMORY_DIR");
    size_t ticks = 3u;
    for (int i = 2; i < argc; i += 1) {
        if ((strcmp(argv[i], "--config") == 0 ||
             strcmp(argv[i], "--run") == 0) &&
            i + 1 < argc) {
            run_path = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--fake") == 0 && i + 1 < argc) {
            fake_output = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
            if (!parse_positive_size(argv[i + 1], &ticks)) {
                fprintf(stderr, "run: invalid --ticks value: %s\n",
                        argv[i + 1]);
                return 2;
            }
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--write-sim-state") == 0 && i + 1 < argc) {
            sim_state_path = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--write-run-state") == 0 && i + 1 < argc) {
            run_state_path = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--memory-dir") == 0 && i + 1 < argc) {
            memory_dir = argv[i + 1];
            i += 1;
            continue;
        }
        fprintf(stderr, "run: unknown or incomplete argument: %s\n", argv[i]);
        return 2;
    }
    if (run_path == nullptr) {
        return 2;
    }
    return run_loop(run_path, fake_output, ticks, sim_state_path,
                    run_state_path, memory_dir);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "version") == 0) {
        printf("sporegeist %s\n", SPG_VERSION_STRING);
        printf("libgeist %s\n", geist_version_string());
        return 0;
    }

    if (strcmp(argv[1], "exec") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: %s exec <command> [args...]\n", argv[0]);
            return 2;
        }
        return spg_exec_command(argc - 2, argv + 2);
    }

    if (strcmp(argv[1], "memory") == 0) {
        return spg_memory_command(argc - 2, argv + 2);
    }

    if (strcmp(argv[1], "tick") == 0) {
        const int rc = tick_command(argc, argv);
        if (rc == 2) {
            print_tick_usage(argv[0]);
        }
        return rc;
    }

    if (strcmp(argv[1], "run") == 0) {
        const int rc = run_command(argc, argv);
        if (rc == 2) {
            print_run_usage(argv[0]);
        }
        return rc;
    }

    if (strcmp(argv[1], "verify-journal") == 0) {
        if (argc != 3) {
            print_verify_journal_usage(argv[0]);
            return 2;
        }
        return verify_journal_command(argv[2]);
    }

    if (strcmp(argv[1], "replay") == 0) {
        if (argc != 3) {
            print_replay_usage(argv[0]);
            return 2;
        }
        return replay_command(argv[2]);
    }

    if (strcmp(argv[1], "policy-check") == 0) {
        if (argc != 3) {
            print_policy_check_usage(argv[0]);
            return 2;
        }
        return policy_check_command(argv[2]);
    }

    if (strcmp(argv[1], "sim-validate") == 0) {
        if (argc != 3) {
            print_sim_validate_usage(argv[0]);
            return 2;
        }
        return sim_validate_command(argv[2]);
    }

    fprintf(stderr, "%s: unknown command\n", argv[1]);
    print_usage(argv[0]);
    return 2;
}
