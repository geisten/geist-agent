#include "sporegeist/chat_tools.h"

#include "sporegeist/cmd_executor.h"
#include "sporegeist/sexpr.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TOOL_TOKENS 256u
#define TOOL_NODES  256u

/* Find a (argname "value") child under the tool form and return the value's
 * string-payload span (quotes stripped). */
static bool arg_string(size_t input_n, const char *input,
                       const struct spg_sexpr_node *nodes, uint32_t root,
                       const char *argname, struct spg_text_span *out) {
    for (uint32_t c = spg_sexpr_first_child(nodes, root);
         c != SPG_SEXPR_INVALID_INDEX; c = nodes[c].next_sibling) {
        if (nodes[c].kind != SPG_SEXPR_NODE_LIST) {
            continue;
        }
        const uint32_t name = spg_sexpr_first_child(nodes, c);
        const uint32_t val  = spg_sexpr_second_child(nodes, c);
        if (name == SPG_SEXPR_INVALID_INDEX || val == SPG_SEXPR_INVALID_INDEX ||
            nodes[name].kind != SPG_SEXPR_NODE_SYMBOL ||
            nodes[val].kind != SPG_SEXPR_NODE_STRING ||
            nodes[val].span.length < 2u) {
            continue;
        }
        if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name].span, argname)) {
            *out = (struct spg_text_span){.offset = nodes[val].span.offset + 1u,
                                          .length = nodes[val].span.length - 2u};
            return true;
        }
    }
    return false;
}

static void span_to_buf(const char *input, struct spg_text_span span, char *buf,
                        size_t cap) {
    const size_t take = span.length + 1u > cap ? cap - 1u : span.length;
    memcpy(buf, input + span.offset, take);
    buf[take] = '\0';
}

/* Split s on whitespace in place; argv[i] point into s. No quoting. */
static size_t split_ws(char *s, const char *argv[], const size_t max) {
    size_t n = 0u;
    char  *p = s;
    while (*p != '\0' && n < max) {
        while (*p == ' ' || *p == '\t') {
            p += 1u;
        }
        if (*p == '\0') {
            break;
        }
        argv[n] = p;
        n += 1u;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p += 1u;
        }
        if (*p != '\0') {
            *p = '\0';
            p += 1u;
        }
    }
    return n;
}

static void run_exec(const char *input, const struct spg_sexpr_node *nodes,
                     bool allow_exec, char out[], const size_t out_cap,
                     size_t input_n) {
    if (!allow_exec) {
        (void)snprintf(out, out_cap,
                       "error: exec is disabled (start sporegeist-chat with "
                       "--allow-exec)");
        return;
    }
    struct spg_text_span cspan;
    if (!arg_string(input_n, input, nodes, 0u, "command", &cspan)) {
        (void)snprintf(out, out_cap, "error: exec needs (command \"...\")");
        return;
    }
    char cmd[1024];
    span_to_buf(input, cspan, cmd, sizeof cmd);
    const char *argv[SPG_CMD_MAX_ARGS];
    const size_t argc = split_ws(cmd, argv, SPG_CMD_MAX_ARGS);
    if (argc == 0u) {
        (void)snprintf(out, out_cap, "error: empty command");
        return;
    }
    static char ob[4096];
    static char eb[1024];
    ob[0] = '\0';
    eb[0] = '\0';
    const struct spg_cmd_request creq = {
        .argc       = argc,
        .argv       = argv,
        .timeout_ms = 5000u,
        .stdout_cap = sizeof ob,
        .stdout_buf = ob,
        .stderr_cap = sizeof eb,
        .stderr_buf = eb,
    };
    struct spg_cmd_result cres = {};
    (void)spg_cmd_executor_run(1u, &creq, &cres);
    if (!cres.started) {
        (void)snprintf(out, out_cap, "error: cannot run %s", argv[0]);
        return;
    }
    (void)snprintf(out, out_cap, "exit %d%s%s%s%s", cres.exit_code,
                   ob[0] != '\0' ? "\n" : "", ob,
                   eb[0] != '\0' ? "\n[stderr] " : "", eb);
}

enum spg_status spg_chat_tool_dispatch(struct spg_mem_store *store,
                                       const bool allow_exec,
                                       const size_t input_n, const char *input,
                                       const size_t out_cap, char out[],
                                       bool *was_tool) {
    if (store == nullptr || input == nullptr || out == nullptr ||
        out_cap == 0u || was_tool == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *was_tool = false;
    out[0]    = '\0';

    struct spg_sexpr_token tokens[TOOL_TOKENS];
    struct spg_sexpr_node  nodes[TOOL_NODES];
    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;
    struct spg_sexpr_error error       = {};
    if (spg_sexpr_parse_text(input_n, input, TOOL_TOKENS, tokens, TOOL_NODES,
                             nodes, &token_count, &node_count, &error) !=
            SPG_OK ||
        node_count == 0u || nodes[0].kind != SPG_SEXPR_NODE_LIST) {
        return SPG_OK; /* not a tool call */
    }

    const uint32_t head = spg_sexpr_first_child(nodes, 0u);
    const uint32_t name = spg_sexpr_second_child(nodes, 0u);
    if (head == SPG_SEXPR_INVALID_INDEX || name == SPG_SEXPR_INVALID_INDEX ||
        nodes[head].kind != SPG_SEXPR_NODE_SYMBOL ||
        nodes[name].kind != SPG_SEXPR_NODE_SYMBOL ||
        !spg_sexpr_span_eq_cstr(input_n, input, nodes[head].span, "tool")) {
        return SPG_OK; /* not a tool call */
    }
    *was_tool = true;

    char slug[SPG_MEM_SLUG_MAX + 1u] = {0};
    struct spg_text_span slug_span;
    const bool has_slug =
        arg_string(input_n, input, nodes, 0u, "slug", &slug_span);
    if (has_slug) {
        span_to_buf(input, slug_span, slug, sizeof slug);
    }

    if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name].span,
                               "memory_list")) {
        (void)spg_mem_index(store, out_cap, out, nullptr, nullptr);
        if (out[0] == '\0') {
            (void)snprintf(out, out_cap, "(no memories)");
        }
        return SPG_OK;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name].span,
                               "memory_read")) {
        if (!has_slug) {
            (void)snprintf(out, out_cap, "error: memory_read needs a slug");
            return SPG_OK;
        }
        const enum spg_status s =
            spg_mem_read(store, slug, out_cap, out, nullptr);
        if (s != SPG_OK) {
            (void)snprintf(out, out_cap, "error: cannot read %s (%s)", slug,
                           spg_status_to_string(s));
        }
        return SPG_OK;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name].span,
                               "memory_delete")) {
        const enum spg_status s = has_slug ? spg_mem_delete(store, slug)
                                           : SPG_E_INVALID_ARG;
        (void)snprintf(out, out_cap,
                       s == SPG_OK ? "deleted %s" : "error: cannot delete %s",
                       slug);
        return SPG_OK;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name].span,
                               "memory_save")) {
        char desc[SPG_MEM_DESC_MAX + 1u] = {0};
        static char           body[SPG_MEM_BODY_MAX + 1u];
        struct spg_text_span  dspan;
        struct spg_text_span  bspan;
        body[0] = '\0';
        const bool ok = has_slug &&
                        arg_string(input_n, input, nodes, 0u, "description",
                                   &dspan) &&
                        arg_string(input_n, input, nodes, 0u, "body", &bspan);
        if (ok) {
            span_to_buf(input, dspan, desc, sizeof desc);
            span_to_buf(input, bspan, body, sizeof body);
        }
        const enum spg_status s =
            ok ? spg_mem_save(store, slug, desc, body) : SPG_E_INVALID_ARG;
        (void)snprintf(out, out_cap,
                       s == SPG_OK ? "saved %s"
                                   : "error: cannot save (need slug, "
                                     "description, body)",
                       slug);
        return SPG_OK;
    }

    if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name].span, "exec")) {
        run_exec(input, nodes, allow_exec, out, out_cap, input_n);
        return SPG_OK;
    }

    (void)snprintf(out, out_cap, "error: unknown tool");
    return SPG_OK;
}
