#!/usr/bin/env bash
# End-to-end acceptance: every required stress scenario, generated with fixed
# seeds, then independently re-verified from the exported WAV files.
set -euo pipefail
cd "$(dirname "$0")/.."

run_case() {
    local name="$1"; shift
    echo "----------------------------------------------------------------"
    echo "acceptance case: $name"
    ./aec_demo demo "$@" > /tmp/aec_case.log 2>&1
    local rid
    rid=$(grep -o 'run-[a-f0-9-]*' /tmp/aec_case.log | head -1)
    cat /tmp/aec_case.log
    python3 -I tests/verify_run.py "run_data/${rid}"
}

# 1. Abrupt echo-path delay change (uses a longer horizon for reconvergence).
run_case "delay jump"        --scenario delay_jump --duration 12
# 2. Input sample-rate mismatch between far reference and mic.
run_case "rate mismatch"     --scenario rate_mismatch --far-rate 44100
# 3. Reference interruption: must be flagged, not silently faked.
run_case "reference outage"  --scenario ref_outage
# 4. Double talk: near-end retention while echo is still present.
run_case "double talk"       --scenario double_talk
# 5. Combined stress and an alternate block length (adapter buffer path).
run_case "full combined"     --scenario full --duration 12
run_case "20 ms blocks"      --scenario full --frame-ms 20 --duration 12

echo
echo "all acceptance cases passed"
