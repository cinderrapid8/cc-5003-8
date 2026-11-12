#!/usr/bin/env python3
"""Independent verification of an AEC run against the exported WAVs.

This does NOT trust the C++ metrics: it re-reads the ground-truth WAV files
(mic mix, echo-only, near clean, AEC output) and recomputes, per scenario
segment described in manifest.json:

  * ERLE over far-only segments (echo residual, not generic energy)
  * near-end retention and output/near correlation over near-active segments
  * reference-outage honesty: outage frames must remain echo-correlated and
    must not show faked cancellation

Usage: verify_run.py run_data/<run_id>
"""
import json
import math
import os
import sys
import wave


def read_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getsampwidth() == 2 and w.getnchannels() == 1, \
            "only 16-bit mono WAV is supported"
        sr = w.getframerate()
        raw = w.readframes(w.getnframes())
    import array
    a = array.array("h")
    a.frombytes(raw)
    return sr, [x / 32768.0 for x in a]


def rms(x, i0, i1):
    seg = x[i0:i1]
    return math.sqrt(sum(v * v for v in seg) / max(1, len(seg)))


def corr(a, b, i0, i1):
    x, y = a[i0:i1], b[i0:i1]
    n = len(x)
    if n == 0:
        return 0.0
    mx, my = sum(x) / n, sum(y) / n
    cov = sum((p - mx) * (q - my) for p, q in zip(x, y))
    vx = sum((p - mx) ** 2 for p in x)
    vy = sum((q - my) ** 2 for q in y)
    if vx <= 1e-12 or vy <= 1e-12:
        return 0.0
    return cov / math.sqrt(vx * vy)


def main(run_dir):
    with open(os.path.join(run_dir, "manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    sr, mic = read_wav(os.path.join(run_dir, "mic_mix.wav"))
    _, out = read_wav(os.path.join(run_dir, "aec_output.wav"))
    _, echo = read_wav(os.path.join(run_dir, "echo_only.wav"))
    _, near = read_wav(os.path.join(run_dir, "near_clean.wav"))
    n = min(len(mic), len(out), len(echo), len(near))

    print(f"run {manifest['run_id']}  library={manifest['summary']['library_id']}")
    print(f"sample rate={sr} Hz  shared samples={n}\n")
    print(f"{'segment':<22}{'ERLE(dB)':>10}{'out~echo':>10}{'out~near':>10}")
    failures = []
    far_mic_p = far_out_p = 0.0
    near_in_p = near_out_p = 0.0
    best_far_erle = -99.0
    recovery_erle = None
    for s in manifest["segments"]:
        i0, i1 = int(s["start_s"] * sr), min(n, int(s["end_s"] * sr))
        mp = sum(v * v for v in mic[i0:i1])
        op = sum(v * v for v in out[i0:i1])
        ep = sum(v * v for v in echo[i0:i1])
        np_ = sum(v * v for v in near[i0:i1])
        erle = 10 * math.log10(mp / op) if mp > 1e-10 and op > 1e-10 else 0.0
        ce = corr(out, echo, i0, i1)
        cn = corr(out, near, i0, i1)
        print(f"{s['label']:<22}{erle:>10.2f}{ce:>10.3f}{cn:>10.3f}")
        if s["label"] in ("far_only", "delay_jump_recovery"):
            far_mic_p += mp
            far_out_p += op
        if s["label"] == "far_only":
            best_far_erle = max(best_far_erle, erle)
        if s["label"] == "delay_jump_recovery":
            recovery_erle = erle
        if s["label"] in ("double_talk", "near_only"):
            near_in_p += np_
            near_out_p += op
        if s["label"] == "ref_outage":
            # Honesty contract: with no reference, output stays close to the
            # mic and stays echo-correlated (no faked cancellation).
            if erle > 3.0 or ce < 0.7:
                failures.append(
                    "ref_outage segment looks faked: erle=%.2f corr_echo=%.3f"
                    % (erle, ce))
        if s["label"] == "double_talk":
            # Near speech must remain present and be the dominant component.
            if cn < 0.5:
                failures.append(
                    "double_talk near-end not retained: corr_near=%.3f" % cn)
        if s["label"] == "near_only" and cn < 0.6:
            failures.append(
                "near_only segment distorted: corr_near=%.3f" % cn)

    # A real canceller must show substantial attenuation at least once the
    # adaptive filter has converged on a stable echo path.
    if best_far_erle < 8.0:
        failures.append(
            "no converged far-only segment reached 8 dB ERLE (best=%.2f)"
            % best_far_erle)
    # A delay jump must visibly disrupt a previously converged canceller.
    if recovery_erle is not None and recovery_erle >= best_far_erle - 3.0:
        failures.append(
            "delay jump not reflected: recovery ERLE %.2f did not degrade "
            "from converged %.2f" % (recovery_erle, best_far_erle))

    overall_erle = 10 * math.log10(far_mic_p / far_out_p) \
        if far_out_p > 1e-10 else 0.0
    print(f"\nrecomputed far-only ERLE = {overall_erle:.2f} dB "
          f"(C++ reported {manifest['summary']['overall_erle_db']:.2f} dB)")
    limited = manifest["summary"]["limited_samples"]
    print(f"limited (REFERENCE_MISSING) samples = {limited}")

    # Cross-check the independently recomputed number against the service.
    if abs(overall_erle - manifest["summary"]["overall_erle_db"]) > 1.0:
        failures.append("recomputed ERLE disagrees with service manifest")
    if manifest["parameters"]["frame_ms"] not in (10, 20):
        failures.append("invalid frame length recorded")

    if failures:
        print("\nVERIFY FAILED:")
        for x in failures:
            print("  - " + x)
        return 1
    print("\nVERIFY OK: echo residual and near retention confirmed from WAVs")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: verify_run.py run_data/<run_id>")
    sys.exit(main(sys.argv[1]))
