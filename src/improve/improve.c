#include "sporegeist/improve.h"

#include <stdio.h>
#include <string.h>

static void set_lesson(struct spg_lesson *out, const char *slug,
                       const char *description, const char *body) {
    (void)snprintf(out->slug, sizeof out->slug, "%s", slug);
    (void)snprintf(out->description, sizeof out->description, "%s", description);
    (void)snprintf(out->body, sizeof out->body, "%s", body);
}

enum spg_status spg_improve_commit(struct spg_mem_store *store,
                                   const struct spg_lesson *lesson,
                                   const bool accepted, bool *kept) {
    if (store == nullptr || lesson == nullptr || kept == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    if (accepted) {
        *kept = true;
        return SPG_OK;
    }
    *kept                 = false;
    const enum spg_status s = spg_mem_delete(store, lesson->slug);
    /* A revert of a never-saved lesson is fine. */
    return (s == SPG_OK || s == SPG_E_NOT_FOUND) ? SPG_OK : SPG_E_IO;
}

bool spg_reflect_case(const struct spg_eval_case_result *result,
                      struct spg_lesson *out) {
    if (result == nullptr || out == nullptr ||
        result->outcome == SPG_EVAL_PASS) {
        return false;
    }
    switch (result->termination) {
    case SPG_AGENT_LOOP_REJECTED:
        set_lesson(
            out, "lesson-rejected",
            "Emit exactly one valid recommendation s-expression.",
            "A previous reply was rejected as malformed. Reply with exactly one "
            "valid (recommend (kind <action>) (capability \"...\") (cost 1) "
            "(uses_network false) (confidence_bp <n>) (reason \"...\")) form, or "
            "(recommend (kind finish) (reason \"...\")) when the task is done. "
            "Output only the s-expression and nothing else.");
        return true;
    case SPG_AGENT_LOOP_DENIED:
        set_lesson(
            out, "lesson-denied",
            "Only recommend actions whose capability the policy allows.",
            "A previous action was denied by policy. Recommend only actions "
            "whose capability is enabled; when unsure, prefer a simulator or a "
            "finish action over one that gets denied.");
        return true;
    case SPG_AGENT_LOOP_BUDGET:
        set_lesson(
            out, "lesson-budget",
            "Finish before the step or token budget runs out.",
            "A previous run exhausted its budget before finishing. Take the most "
            "direct path to the goal and emit (recommend (kind finish) ...) as "
            "soon as the task is complete, instead of spending steps on "
            "redundant actions.");
        return true;
    case SPG_AGENT_LOOP_MAX_STEPS:
        set_lesson(out, "lesson-max-steps",
                   "Reach the finish action in fewer steps.",
                   "A previous run hit the step cap without finishing. Plan the "
                   "shortest sequence of actions and finish promptly; do not "
                   "repeat an action that already succeeded.");
        return true;
    case SPG_AGENT_LOOP_ERROR:
        set_lesson(out, "lesson-error",
                   "Re-check an action's required fields to avoid errors.",
                   "A previous run ended with an internal error. Verify each "
                   "action's required fields and prefer a simpler, well-formed "
                   "action.");
        return true;
    case SPG_AGENT_LOOP_FINISHED:
        /* Finished cleanly but missed an eval expectation: no agent-level
         * lesson to learn from the failure mode. */
        return false;
    }
    return false;
}
