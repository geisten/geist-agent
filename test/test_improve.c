#include "sporegeist/improve.h"

#include <stdio.h>
#include <string.h>

/* A lesson is produced for each failing termination mode, with a valid slug. */
static int test_reflect_failure_modes(void) {
    const struct {
        enum spg_agent_loop_termination term;
        const char                     *slug;
    } cases[] = {
        {SPG_AGENT_LOOP_REJECTED, "lesson-rejected"},
        {SPG_AGENT_LOOP_DENIED, "lesson-denied"},
        {SPG_AGENT_LOOP_BUDGET, "lesson-budget"},
        {SPG_AGENT_LOOP_MAX_STEPS, "lesson-max-steps"},
        {SPG_AGENT_LOOP_ERROR, "lesson-error"},
    };
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        const struct spg_eval_case_result result = {
            .outcome     = SPG_EVAL_FAIL_TERMINATION,
            .termination = cases[i].term,
        };
        struct spg_lesson lesson = {};
        if (!spg_reflect_case(&result, &lesson)) {
            return 1;
        }
        if (strcmp(lesson.slug, cases[i].slug) != 0) {
            return 1;
        }
        /* slug is mind-palace-safe, description has no newline, body non-empty */
        if (!spg_mem_slug_valid(lesson.slug) ||
            strchr(lesson.description, '\n') != nullptr ||
            lesson.body[0] == '\0') {
            return 1;
        }
    }
    return 0;
}

/* A passing case yields no lesson. */
static int test_reflect_pass_no_lesson(void) {
    const struct spg_eval_case_result result = {
        .outcome     = SPG_EVAL_PASS,
        .termination = SPG_AGENT_LOOP_FINISHED,
    };
    struct spg_lesson lesson = {};
    return spg_reflect_case(&result, &lesson) ? 1 : 0;
}

/* Finished-but-expectation-mismatch yields no agent lesson. */
static int test_reflect_finished_no_lesson(void) {
    const struct spg_eval_case_result result = {
        .outcome     = SPG_EVAL_FAIL_OBSERVATION,
        .termination = SPG_AGENT_LOOP_FINISHED,
    };
    struct spg_lesson lesson = {};
    return spg_reflect_case(&result, &lesson) ? 1 : 0;
}

static int test_accept_gate(void) {
    /* keep on improvement and on no-change; revert on regression */
    if (!spg_improve_accept(2u, 3u) || !spg_improve_accept(2u, 2u) ||
        spg_improve_accept(2u, 1u)) {
        return 1;
    }
    return 0;
}

static int test_reflect_null_args(void) {
    struct spg_lesson lesson = {};
    const struct spg_eval_case_result result = {.outcome =
                                                    SPG_EVAL_FAIL_TERMINATION};
    return (spg_reflect_case(nullptr, &lesson) ||
            spg_reflect_case(&result, nullptr))
               ? 1
               : 0;
}

int main(void) {
    if (test_reflect_failure_modes() != 0) {
        fprintf(stderr, "test_reflect_failure_modes failed\n");
        return 1;
    }
    if (test_reflect_pass_no_lesson() != 0) {
        fprintf(stderr, "test_reflect_pass_no_lesson failed\n");
        return 1;
    }
    if (test_reflect_finished_no_lesson() != 0) {
        fprintf(stderr, "test_reflect_finished_no_lesson failed\n");
        return 1;
    }
    if (test_accept_gate() != 0) {
        fprintf(stderr, "test_accept_gate failed\n");
        return 1;
    }
    if (test_reflect_null_args() != 0) {
        fprintf(stderr, "test_reflect_null_args failed\n");
        return 1;
    }
    return 0;
}
