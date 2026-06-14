#include "sporegeist/eval.h"

#include "sporegeist/policy_config.h"
#include "sporegeist/run_config.h"
#include "sporegeist/sim_config.h"

#include <stdio.h>
#include <string.h>

static const char policy_text[] =
    "(policy (network_default deny)"
    " (budgets (inference_steps 100) (tokens 4096) (shell_actions 8)"
    "  (sim_actions 8) (wall_ms 60000) (journal_bytes 1048576) (risk_bp 10000))"
    " (capability ((name sim.act) (kind simulator) (enabled true) (budget 8))))";

static const char scenario_text[] =
    "(scenario (host (id web) (criticality_bp 8000))"
    " (service (id s) (host web) (name ssh) (port 22) (exposure_bp 5000))"
    " (vulnerability (id v) (service s) (severity_bp 8000) (patched false)))";

static const char run_text[] =
    "(run (model \"fake\") (policy \"p\") (scenario \"s\") (corpus \"c\")"
    " (journal \"j\") (seed 42)"
    " (budgets (inference_steps 100) (tokens 4096) (shell_actions 8)"
    "  (sim_actions 8) (wall_ms 60000) (journal_bytes 1048576) (risk_bp 10000)))";

#define FR(s) ((struct spg_fake_response){.n = sizeof(s) - 1u, .text = (s)})

struct fixture {
    struct spg_policy_config policy;
    struct spg_sim_config    sim;
    struct spg_run_config    run;

    struct spg_context_graph_ref   graph_refs[8];
    struct spg_context_memory_ref  memory_refs[8];
    struct spg_context_journal_ref journal_refs[8];
    char                           context[8192];
    char                           model_output[1024];
    struct spg_sexpr_token         tokens[128];
    struct spg_sexpr_node          nodes[128];
    char                           policy_payload[1024];
    char                           sim_payload[1024];
    char                           observation[4096];
    char                           shell_stdout[4096];
    char                           shell_stderr[1024];
    struct spg_journal_record_header trajectory[256];
};

static int load_fixture(struct fixture *fx) {
    *fx = (struct fixture){};
    struct spg_sexpr_token tok[256];
    struct spg_sexpr_node  nod[256];
    struct spg_policy_config_error pe = {};
    struct spg_sim_config_error    se = {};
    struct spg_run_config_error    re = {};
    if (spg_policy_config_load(strlen(policy_text), policy_text, 256u, tok, 256u,
                               nod, &fx->policy, &pe) != SPG_OK) {
        return 1;
    }
    if (spg_sim_config_load(strlen(scenario_text), scenario_text, 256u, tok,
                            256u, nod, &fx->sim, &se) != SPG_OK) {
        return 1;
    }
    if (spg_run_config_load(strlen(run_text), run_text, 256u, tok, 256u, nod,
                            &fx->run, &re) != SPG_OK) {
        return 1;
    }
    return 0;
}

static struct spg_agent_run_workspace ws_for(struct fixture *fx) {
    return (struct spg_agent_run_workspace){
        .context_capacity        = sizeof fx->context,
        .context                 = fx->context,
        .model_output_capacity   = sizeof fx->model_output,
        .model_output            = fx->model_output,
        .graph_ref_capacity      = 8u,
        .graph_refs              = fx->graph_refs,
        .memory_ref_capacity     = 8u,
        .memory_refs             = fx->memory_refs,
        .journal_ref_capacity    = 8u,
        .journal_refs            = fx->journal_refs,
        .token_capacity          = 128u,
        .tokens                  = fx->tokens,
        .node_capacity           = 128u,
        .nodes                   = fx->nodes,
        .policy_payload_capacity = sizeof fx->policy_payload,
        .policy_payload          = fx->policy_payload,
        .sim_payload_capacity    = sizeof fx->sim_payload,
        .sim_payload             = fx->sim_payload,
        .observation_capacity    = sizeof fx->observation,
        .observation             = fx->observation,
        .shell_stdout_capacity   = sizeof fx->shell_stdout,
        .shell_stdout            = fx->shell_stdout,
        .shell_stderr_capacity   = sizeof fx->shell_stderr,
        .shell_stderr            = fx->shell_stderr,
        .trajectory_capacity     = 256u,
        .trajectory              = fx->trajectory,
    };
}

static const struct spg_fake_response sim_finish[] = {
    FR("(recommend (kind simulator) (capability \"sim.act\") (cost 1) "
       "(uses_network false) (confidence_bp 7000) (reason \"r\"))"),
    FR("(recommend (kind finish) (reason \"done\"))"),
};

static int test_case_pass(void) {
    struct fixture fx;
    if (load_fixture(&fx) != 0) {
        return 1;
    }
    const struct spg_agent_run_inputs inputs = {
        .policy        = &fx.policy,
        .policy_text_n = strlen(policy_text),
        .policy_text   = policy_text,
        .run           = &fx.run,
        .sim           = &fx.sim,
    };
    const struct spg_agent_run_config config = {.max_steps = 5u};
    const struct spg_agent_run_workspace ws = ws_for(&fx);
    const struct spg_eval_expect expect = {.check_termination = true,
                                           .termination = SPG_AGENT_LOOP_FINISHED,
                                           .min_steps   = 2u,
                                           .max_steps   = 2u};
    struct spg_eval_case_result res = {};
    if (spg_eval_run_case(sim_finish, 2u, &inputs, &config, &ws, &expect,
                          &res) != SPG_OK) {
        return 1;
    }
    return (res.outcome == SPG_EVAL_PASS &&
            res.termination == SPG_AGENT_LOOP_FINISHED && res.steps_taken == 2u)
               ? 0
               : 1;
}

static int test_case_fail_termination(void) {
    struct fixture fx;
    if (load_fixture(&fx) != 0) {
        return 1;
    }
    const struct spg_agent_run_inputs inputs = {
        .policy        = &fx.policy,
        .policy_text_n = strlen(policy_text),
        .policy_text   = policy_text,
        .run           = &fx.run,
        .sim           = &fx.sim,
    };
    /* cap the loop at 1 step: it will not reach finish -> MAX_STEPS */
    const struct spg_agent_run_config config = {.max_steps = 1u};
    const struct spg_agent_run_workspace ws = ws_for(&fx);
    const struct spg_eval_expect expect = {.check_termination = true,
                                           .termination =
                                               SPG_AGENT_LOOP_FINISHED};
    struct spg_eval_case_result res = {};
    if (spg_eval_run_case(sim_finish, 2u, &inputs, &config, &ws, &expect,
                          &res) != SPG_OK) {
        return 1;
    }
    return (res.outcome == SPG_EVAL_FAIL_TERMINATION &&
            res.termination == SPG_AGENT_LOOP_MAX_STEPS)
               ? 0
               : 1;
}

int main(void) {
    if (test_case_pass() != 0) {
        fprintf(stderr, "test_case_pass failed\n");
        return 1;
    }
    if (test_case_fail_termination() != 0) {
        fprintf(stderr, "test_case_fail_termination failed\n");
        return 1;
    }
    return 0;
}
