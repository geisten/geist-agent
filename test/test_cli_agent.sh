#!/bin/sh
# System test: the governed multi-step agent loop. A scripted fake model drives
# local_shell + finish through policy-gating, journaling, and observation
# feedback; the loop terminates on `finish` and is run-to-run deterministic.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/sporegeist}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

cat > "$T/run.spg" <<EOF
(run
 (model "fake.gguf")
 (policy "examples/policy.spg")
 (scenario "examples/scenario.spg")
 (corpus "examples/corpus.spg")
 (journal "$T/j.sgj")
 (seed 42)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8) (wall_ms 10000) (journal_bytes 1048576) (risk_bp 10000)))
EOF

# Script: one governed shell step, then finish.
cat > "$T/script.txt" <<'EOF'
(recommend (kind local_shell) (capability "build.run") (cost 1) (uses_network false) (confidence_bp 6000) (reason "probe") (command "echo AGENT-LOOP-PROOF"))
(recommend (kind finish) (reason "task complete"))
EOF

# --- governed run with exec enabled: loop runs the shell, observes, finishes ---
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script.txt" \
    --allow-exec --max-steps 5 > "$T/agent.out" 2>&1
grep -q "termination=finished" "$T/agent.out"
grep -q "steps=2" "$T/agent.out"
# The shell stdout was fed back into the observation channel.
grep -q "AGENT-LOOP-PROOF" "$T/agent.out"

# --- the journal records the governed steps (policy decision + action) ---
V=$("$SPG_BIN" verify-journal "$T/j.sgj")
echo "$V" | grep -q "verified=true"
echo "$V" | grep -q "status_failures=0"
echo "$V" | grep -q "event.action=1"

# --- two runs produce byte-identical journals (logical timestamps) ---
cp "$T/j.sgj" "$T/j_first.sgj"
rm "$T/j.sgj"
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script.txt" \
    --allow-exec --max-steps 5 >/dev/null 2>&1
cmp "$T/j_first.sgj" "$T/j.sgj"

# --- without --allow-exec the boundary denies, but the loop still finishes ---
rm "$T/j.sgj"
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script.txt" \
    --max-steps 5 > "$T/agent_denied.out" 2>&1
grep -q "termination=finished" "$T/agent_denied.out"
grep -q "denied" "$T/agent_denied.out"

echo "test_cli_agent: PASS"
