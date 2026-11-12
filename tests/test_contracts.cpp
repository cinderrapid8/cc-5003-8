// Unit tests for the processing contracts:
//  1. no samples are dropped for arbitrary input lengths (adapter + tail flush)
//  2. bypass mode preserves the mic signal sample-for-sample
//  3. outage frames are flagged REFERENCE_MISSING and passed through
//  4. resampling path keeps the two time bases aligned (lengths equalized)
//  5. deterministic generation: same seed -> byte-identical signals
#include <cassert>
#include <cstdio>
#include <cmath>

#include "aec_processor.h"
#include "signal_gen.h"

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::printf("FAIL: %s\n", msg);
        std::exit(1);
    }
}

int main() {
    // Contract 1 & 2: odd lengths and bypass passthrough.
    for (int extra : {0, 1, 7, 137, 159, 160, 161, 333}) {
        int rate = 16000;
        size_t n = static_cast<size_t>(rate * 2) + extra;
        std::vector<int16_t> far(n), mic(n);
        for (size_t i = 0; i < n; ++i) {
            far[i] = static_cast<int16_t>((i * 37) % 4000 - 2000);
            mic[i] = static_cast<int16_t>((i * 53) % 6000 - 3000);
        }
        AecConfig cfg;
        cfg.internal_rate = rate;
        cfg.frame_ms = 10;
        cfg.filter_ms = 100;
        cfg.ref_delay_ms = 0;
        cfg.bypass = true;
        AecProcessor proc(cfg);
        AecRunResult r;
        std::string err;
        int frame = rate / 100; // 10 ms frame
        bool ok = proc.run(far, rate, mic, rate, {}, r, err);
        check(ok, "processor run failed");
        // Output is zero-padded up to a whole frame; real samples are kept.
        check(r.out.size() >= n && r.out.size() < n + static_cast<size_t>(frame),
              "bypass output must cover all input samples, padded to a frame");
        check(r.ref_used.size() == r.out.size(),
              "aligned reference length mismatch");
        bool identical = true;
        for (size_t i = 0; i < n; ++i)
            if (r.out[i] != mic[i]) { identical = false; break; }
        check(identical, "bypass must pass mic through unchanged");
        bool pad_zero = true;
        for (size_t i = n; i < r.out.size(); ++i)
            if (r.out[i] != 0) pad_zero = false;
        check(pad_zero, "padded tail samples must be zero");
        if (n % frame != 0) {
            bool tail = false;
            for (uint8_t m : r.mode)
                if (static_cast<FrameMode>(m) == FrameMode::TAIL_FLUSH)
                    tail = true;
            check(tail, "non-frame-aligned input must produce TAIL_FLUSH");
        }
    }
    std::printf("[ok] no-sample-drop and bypass passthrough\n");

    // Contract 3: outage flagging and passthrough on missing reference.
    {
        int rate = 16000;
        size_t n = rate * 2;
        std::vector<int16_t> far(n, 1000), mic(n, 2000);
        std::vector<bool> valid(n, true);
        for (size_t i = rate / 2; i < rate; ++i) valid[i] = false;
        AecConfig cfg;
        cfg.internal_rate = rate;
        cfg.frame_ms = 10;
        cfg.filter_ms = 100;
        AecProcessor proc(cfg);
        AecRunResult r;
        std::string err;
        check(proc.run(far, rate, mic, rate, valid, r, err), "run failed");
        bool found = false;
        for (const auto& s : r.interruptions) {
            if (s.reason.find("outage") != std::string::npos) found = true;
        }
        check(found, "outage must be recorded");
        size_t limited = 0;
        for (uint8_t m : r.mode)
            if (static_cast<FrameMode>(m) == FrameMode::REFERENCE_MISSING)
                ++limited;
        check(limited > 0, "outage frames must be flagged");
        // Inside the outage the mic is passed through (no faked cancellation).
        bool passthrough = true;
        for (size_t i = rate / 2; i < rate; ++i)
            if (r.out[i] != mic[i]) { passthrough = false; break; }
        check(passthrough, "missing-reference frames must pass mic through");
    }
    std::printf("[ok] reference outage flagged and not faked\n");

    // Contract 4: mismatched rates are reconciled onto one time base.
    {
        ScenarioConfig sc;
        sc.mic_rate = 16000;
        sc.far_rate = 44100;
        sc.duration_s = 2.0;
        GeneratedSignals g;
        std::string err;
        check(generate_scenario(sc, g, err), "scenario generation failed");
        AecConfig cfg;
        cfg.internal_rate = 16000;
        cfg.frame_ms = 10;
        cfg.filter_ms = 100;
        AecProcessor proc(cfg);
        AecRunResult r;
        check(proc.run(g.far, 44100, g.mic, 16000, g.ref_valid, r, err),
              "rate-mismatch run failed");
        check(r.resampled_far == 44100, "far resampling must be recorded");
        check(r.out.size() >= g.mic.size(),
              "output must stay on the mic time base");
    }
    std::printf("[ok] sample-rate mismatch reconciled\n");

    // Contract 5: reproducibility.
    {
        ScenarioConfig a, b;
        a.duration_s = b.duration_s = 3.0;
        GeneratedSignals ga, gb;
        std::string err;
        check(generate_scenario(a, ga, err) && generate_scenario(b, gb, err),
              "generation failed");
        check(ga.mic == gb.mic && ga.far == gb.far && ga.near == gb.near &&
                  ga.echo == gb.echo,
              "same seed must yield byte-identical signals");
        ScenarioConfig c;
        c.seed = 999;
        c.duration_s = 3.0;
        GeneratedSignals gc;
        check(generate_scenario(c, gc, err), "generation failed");
        check(gc.mic != ga.mic, "different seed must change the signal");
    }
    std::printf("[ok] deterministic, reproducible signal generation\n");

    std::printf("all unit tests passed\n");
    return 0;
}
