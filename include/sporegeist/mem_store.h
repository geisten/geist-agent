#ifndef SPOREGEIST_MEM_STORE_H
#define SPOREGEIST_MEM_STORE_H

#include "sporegeist/status.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Persistent, human-readable long-term memory ("mind palace"): one Markdown
 * file per memory in a directory, plus a generated one-line-hook index. This
 * is the durable, cross-run store, distinct from the ephemeral structured
 * spg_memory fact store. Operations write atomically and stay bounded. */

#define SPG_MEM_MAX_FILES  128u
#define SPG_MEM_SLUG_MAX   64u
#define SPG_MEM_DESC_MAX   256u
#define SPG_MEM_BODY_MAX   65536u
#define SPG_MEM_INDEX_TOPK 24u /* index lines injected before the "N more" pointer */

#ifndef SPG_MEM_PATH_MAX
#    define SPG_MEM_PATH_MAX 4096
#endif

struct spg_mem_store {
    char dir[SPG_MEM_PATH_MAX];
};

/* Bind store to a directory, creating it if missing. Returns SPG_E_INVALID_ARG
 * on null/empty, SPG_E_LIMIT if the path is too long, SPG_E_IO if the directory
 * cannot be created. */
[[nodiscard]] enum spg_status spg_mem_store_open(struct spg_mem_store *store,
                                                 const char           *dir);

/* Resolve a memory directory: an explicit flag (when non-null/non-empty) wins,
 * else $SPOREGEIST_MEMORY_DIR, else the "memory" default. The shared policy for
 * CLI/chat surfaces that default a store; never returns null. */
[[nodiscard]] const char *spg_mem_resolve_dir(const char *flag);

/* True when slug is a safe identifier: 1..SPG_MEM_SLUG_MAX bytes of [a-z0-9-]
 * only (no path separators, dots, uppercase). All file paths are built from a
 * validated slug, never from raw model input. */
[[nodiscard]] bool spg_mem_slug_valid(const char *slug);

/* Upsert a memory: write <dir>/<slug>.md with frontmatter (name, description)
 * and the Markdown body, then regenerate the index. Overwrites an existing slug
 * atomically. Returns SPG_E_INVALID_ARG (bad slug/null/description with a
 * newline) or SPG_E_LIMIT (description/body over cap, or a new slug beyond
 * SPG_MEM_MAX_FILES). */
[[nodiscard]] enum spg_status spg_mem_save(struct spg_mem_store *store,
                                           const char           *slug,
                                           const char           *description,
                                           const char           *body);

/* Delete a memory and regenerate the index. SPG_E_NOT_FOUND if absent. */
[[nodiscard]] enum spg_status spg_mem_delete(struct spg_mem_store *store,
                                             const char           *slug);

/* Read the full file content of a memory into dst. SPG_E_NOT_FOUND if absent,
 * SPG_E_LIMIT if dst is too small (dst gets the truncated, NUL-terminated
 * prefix); *out_required (may be null) receives the full byte length. */
[[nodiscard]] enum spg_status spg_mem_read(struct spg_mem_store *store,
                                           const char *slug, size_t dst_cap,
                                           char dst[], size_t *out_required);

/* Render the index for context injection: "- <slug>: <description>\n" per
 * memory (slug-sorted), capped at SPG_MEM_INDEX_TOPK lines followed by a
 * "- ... N more (memory list)\n" pointer. *truncated (may be null) is set when
 * the pointer is emitted; *out_required (may be null) receives the full length.
 * SPG_E_LIMIT if dst is too small. */
[[nodiscard]] enum spg_status spg_mem_index(struct spg_mem_store *store,
                                            size_t dst_cap, char dst[],
                                            size_t *out_required,
                                            bool   *truncated);

/* Enumerate memory slugs (slug-sorted) into slugs[0..*count). Returns
 * SPG_E_LIMIT if there are more than cap (the first cap are written). */
[[nodiscard]] enum spg_status
spg_mem_list(struct spg_mem_store *store, size_t cap,
             char slugs[][SPG_MEM_SLUG_MAX + 1u], size_t *count);

#ifdef __cplusplus
}
#endif

#endif
