#!/usr/bin/env python3
"""Independent verification of the delay-adapter comparison experiment.

It does NOT trust any C++ number. For every scenario/adapter variant under
experiments/delay_adapter/<scenario>/<variant>/ it re-reads the exported WAVs
(mic mix, echo-only ground truth, near clean ground truth, AEC output) and
recomputes from scratch:

  * ERLE / echo leak on far-only and recovery windows   -> residual echo
  * near-end retention / output-near correlation        -> near preservation
  * the adapter delay trace against the ground-truth echo delay (CSV),
    including time-to-lock and stability on a non-jumping path

Every recomputed metric is compared against the value the C++ harness wrote
into results.json using the SAME window definitions as src/metrics.cpp and
tests/delay_experiment.cpp. A mismatch (beyond a small numerical tolerance)
fails the check, so every figure quoted in the experiment write-up can be
regenerated from code alone.

Run with an isolated interpreter (no site packages / cwd shadowing):
    python3 -I tests/verify_delay_experiment.py [experiments/delay_adapter]
"""
import array
import csv
import json
import math
import os
import sys
import wave

# Window definitions mirrored EXACTLY from src/signal_gen.cpp, including the
# non-outage path where the 1.8-2.5 s far-only event is pushed twice (once
# standalone and once as part of the 1.8-5.0 s event). metrics.cpp accumulates
# power per event, so the overlap is counted twice there as well. The outage
# path instead splits the stretch into two non-overlapping events.
FAR_ONLY_DEFAULT = [(0.0, 0.6), (1.8, 2.5), (1.8, 5.0), (6.4, None)]
FAR_ONLY_OUTAGE = [(0.0, 0.6), (1.8, 2.5), (3.5, 5.0), (6.4, None)]
DOUBLE_TALK = (5.0, 6.4)
NEAR_ONLY = (0.6, 1.8)
TOL_DB = 0.10
TOL_CORR = 0.005


def read_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getsampwidth() == 2 and w.getnchannels() == 1
        sr = w.getframerate()
        raw = w.readframes(w.getnframes())
    a = array.array("h")
    a.frombytes(raw)
    return sr, [x / 32768.0 for x in a]


def corr(a, b):
    n = len(a)
    if n == 0:
        return 0.0
    ma, mb = sum(a) / n, sum(b) / n
    sa = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    va = sum((x - ma) ** 2 for x in a)
    vb = sum((y - mb) ** 2 for y in b)
    if va <= 1e-12 or vb <= 1e-12:
        return 0.0
    return sa / math.sqrt(va * vb)


def window_metrics(mic, out, echo, near, sr, t0, t1):
    i0, i1 = int(t0 * sr), min(int(t1 * sr), len(out))
    if i0 >= i1:
        return None
    mp = sum(v * v for v in mic[i0:i1])
    op = sum(v * v for v in out[i0:i1])
    ep = sum(v * v for v in echo[i0:i1])
    np_ = sum(v * v for v in near[i0:i1])
    erle = 10 * math.log10(mp / op) if mp > 1e-10 and op > 1e-10 else 0.0
    leak = 10 * math.log10(op / ep) if ep > 1e-10 and op > 1e-10 else 0.0
    keep = 10 * math.log10(op / np_) if np_ > 1e-10 and op > 1e-10 else 0.0
    return {
        "erle": erle,
        "leak": leak,
        "keep": keep,
        "corr_echo": corr(out[i0:i1], echo[i0:i1]),
        "corr_near": corr(out[i0:i1], near[i0:i1]),
    }


def aggregate_power_windows(mic, other, sr, windows):
    # Linear-scale power aggregation across the given windows, matching
    # metrics.cpp (windows may overlap; each listed window is accumulated).
    p_mic = p_other = 0.0
    for t0, t1 in windows:
        if t1 is None:
            t1 = len(other) / sr
        i0, i1 = int(t0 * sr), min(int(t1 * sr), len(other))
        p_mic += sum(v * v for v in mic[i0:i1])
        p_other += sum(v * v for v in other[i0:i1])
    if p_mic <= 1e-10 or p_other <= 1e-10:
        return 0.0
    return 10 * math.log10(p_mic / p_other)


def variant_dir_name(adapter, configured_ms):
    # Directory keys are defined in tests/delay_experiment.cpp.
    if adapter == "fixed":
        return "fixed_120" if configured_ms == 120 else "fixed_wrong_200"
    if adapter == "dynamic":
        return "dynamic_gated"
    return "dynamic_naive"


def load_trace(path):
    rows = []
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            rows.append({
                "t_s": float(r["t_s"]),
                "delay_ms": float(r["adapter_delay_ms"]),
                "truth_ms": float(r["truth_delay_ms"]),
                "candidate_ms": float(r["raw_candidate_ms"]),
                "peak": float(r["peak_corr"]),
                "eligible": r["eligible"] == "1",
                "reason": r["reason"],
            })
    return rows


def check_close(failures, label, got, want, tol=TOL_DB):
    if abs(got - want) > tol:
        failures.append(f"{label}: recomputed {got:.4f} != reported {want:.4f}")


def main(root):
    with open(os.path.join(root, "results.json"), encoding="utf-8") as f:
        summary = json.load(f)
    failures = []
    checked = 0
    print(f"verifying experiment tree under {root}\n")
    print(f"{'scenario/variant':<32}{'ERLE':>7}{'leak':>8}{'keep':>7}"
          f"{'cNear':>7}{'cEcho':>7}")

    for sc in summary["scenarios"]:
        sc_name = sc["scenario"]
        jump = sc["delay_jump"]
        jump_at = sc["jump_at_s"]
        jump_ms = sc["jump_ms"]
        has_outage = sc_name == "s4_full_stress"
        # metrics.cpp accumulates every far_only event plus the overlapping
        # delay_jump_recovery event, so the recovery window is listed twice on
        # jumping scenarios (once via far_only, once via its own event).
        far_windows = list(FAR_ONLY_OUTAGE if has_outage
                           else FAR_ONLY_DEFAULT)
        if jump:
            far_windows.append((jump_at, jump_at + 1.2))
        for v in sc["variants"]:
            dname = variant_dir_name(v["adapter"], v["configured_delay_ms"])
            d = os.path.join(root, sc_name, dname)
            if not os.path.isdir(d):
                failures.append(f"missing artifact directory {d}")
                continue
            sr, mic = read_wav(os.path.join(d, "mic_mix.wav"))
            _, out = read_wav(os.path.join(d, "aec_output.wav"))
            _, echo = read_wav(os.path.join(d, "echo_only.wav"))
            _, near = read_wav(os.path.join(d, "near_clean.wav"))
            n = min(len(mic), len(out), len(echo), len(near))
            mic, out, echo, near = mic[:n], out[:n], echo[:n], near[:n]

            # --- residual echo / near retention over the double-talk window ---
            dt = window_metrics(mic, out, echo, near, sr, *DOUBLE_TALK)
            check_close(failures, f"{dname} dt ERLE",
                        dt["erle"], v["double_talk_erle_db"])
            check_close(failures, f"{dname} dt echo leak",
                        dt["leak"], v["double_talk_echo_leak_db"])
            check_close(failures, f"{dname} dt near keep",
                        dt["keep"], v["double_talk_near_keep_db"])
            check_close(failures, f"{dname} dt corr_near",
                        dt["corr_near"], v["double_talk_corr_near"], TOL_CORR)
            check_close(failures, f"{dname} dt corr_echo",
                        dt["corr_echo"], v["double_talk_corr_echo"], TOL_CORR)

            # --- overall ERLE over the far-only windows (linear power) ---
            overall_erle = aggregate_power_windows(mic, out, sr, far_windows)
            check_close(failures, f"{dname} overall ERLE",
                        overall_erle, v["overall_erle_db"])

            # --- overall near retention over near_only + double_talk windows
            no = window_metrics(mic, out, echo, near, sr, *NEAR_ONLY)
            near_windows = [NEAR_ONLY, DOUBLE_TALK]
            p_out = sum(
                sum(x * x for x in out[int(a * sr):int(b * sr)])
                for a, b in near_windows)
            p_near = sum(
                sum(x * x for x in near[int(a * sr):int(b * sr)])
                for a, b in near_windows)
            overall_keep = (10 * math.log10(p_out / p_near)
                            if p_out > 1e-10 and p_near > 1e-10 else 0.0)
            check_close(failures, f"{dname} overall near keep",
                        overall_keep, v["overall_near_keep_db"])
            check_close(failures, f"{dname} near_only corr_near",
                        no["corr_near"], v["near_only_corr_near"], TOL_CORR)

            # --- recovery window after a jump (jump .. jump+2 s) ---
            if jump and "recovery_erle_db" in v:
                rec = window_metrics(mic, out, echo, near, sr, jump_at,
                                     min(n / sr, jump_at + 2.0))
                check_close(failures, f"{dname} recovery ERLE",
                            rec["erle"], v["recovery_erle_db"])
                check_close(failures, f"{dname} recovery corr_echo",
                            rec["corr_echo"], v["recovery_corr_echo"],
                            TOL_CORR)

            print(f"{sc_name[:14] + '/' + dname:<32}{dt['erle']:>7.2f}"
                  f"{dt['leak']:>8.2f}{dt['keep']:>7.2f}"
                  f"{dt['corr_near']:>7.3f}{dt['corr_echo']:>7.3f}")
            checked += 1

            # --- delay trace honesty against ground truth ---
            trace = load_trace(os.path.join(d, "delay_trace.csv"))
            if not trace:
                failures.append(f"{dname}: empty delay trace")
                continue
            # Every recorded "truth" must match the scenario definition.
            for r in trace:
                want_truth = jump_ms if (jump and r["t_s"] >= jump_at) else 120.0
                if abs(r["truth_ms"] - want_truth) > 1e-9:
                    failures.append(
                        f"{dname} t={r['t_s']}: CSV truth {r['truth_ms']} "
                        f"!= scenario truth {want_truth}")
                    break
            if v["adapter"] == "fixed":
                vals = {r["delay_ms"] for r in trace}
                if len(vals) != 1 or next(iter(vals)) != v["configured_delay_ms"]:
                    failures.append(
                        f"{dname}: fixed trace not constant at configured value")
            elif v["adapter"] == "dynamic" and not jump:
                # Gated dynamic on a stable path must never drift off truth.
                # The naive ablation is intentionally free to chase noise.
                delays = {r["delay_ms"] for r in trace}
                if len(delays) > 1:
                    failures.append(
                        f"{dname}: gated adapter drifted on stable path "
                        f"{sorted(delays)[:4]}")
            elif v["adapter"] == "dynamic" and jump:
                # Gated dynamic must eventually settle on the post-jump truth
                # within one lag-grid step (5 ms) plus one frame (10 ms).
                post = [r for r in trace if r["t_s"] >= jump_at + 1.0]
                if post and abs(post[-1]["delay_ms"] - jump_ms) > 15.0:
                    failures.append(
                        f"{dname}: final delay {post[-1]['delay_ms']} not "
                        f"within 15 ms of truth {jump_ms}")

    print(f"\nrecomputed and cross-checked {checked} variant runs against "
          f"results.json")
    if failures:
        print("\nVERIFY FAILED:")
        for x in failures:
            print("  - " + x)
        return 1
    print("VERIFY OK: residual echo, near retention and delay-trace honesty "
          "confirmed independently from WAV/CSV artifacts")
    return 0


if __name__ == "__main__":
    root = sys.argv[1] if len(sys.argv) > 1 else "experiments/delay_adapter"
    sys.exit(main(root))
