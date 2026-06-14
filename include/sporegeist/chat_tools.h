#ifndef SPOREGEIST_CHAT_TOOLS_H
#define SPOREGEIST_CHAT_TOOLS_H

#include "sporegeist/mem_store.h"
#include "sporegeist/status.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* In-turn agentic tool calls for the chat REPL. When the model's reply is a
 * (tool <name> ...) s-expression, the chat executes it and feeds the result
 * back for another generation, rather than showing it to the user.
 *
 * If input parses as a tool call, run it against the memory store, write a
 * human/agent-readable result into out (NUL-terminated), and set *was_tool.
 * Otherwise *was_tool is false and out is empty (the reply is a normal answer).
 *
 * Supported tools:
 *   (tool memory_list)
 *   (tool memory_read (slug "<slug>"))
 *   (tool memory_save (slug "<slug>") (description "<d>") (body "<b>"))
 *   (tool memory_delete (slug "<slug>"))
 *
 * Returns SPG_E_INVALID_ARG on null arguments; otherwise SPG_OK (a tool's own
 * failure is reported in the result text, not the return value). */
[[nodiscard]] enum spg_status
spg_chat_tool_dispatch(struct spg_mem_store *store, size_t input_n,
                       const char *input, size_t out_cap, char out[],
                       bool *was_tool);

#ifdef __cplusplus
}
#endif

#endif
