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
        for (size_t i = static_cast<size_t>(rate) / 2;
             i < static_cast<size_t>(rate); ++i) valid[i] = false;
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
        for (size_t i = static_cast<size_t>(rate) / 2;
             i < static_cast<size_t>(rate); ++i)
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

    // Contract 6: fixed delay adapter never moves regardless of observations.
    {
        const int rate = 16000, frame = 160, n = rate * 3;
        const int fixed_delay = rate / 10; // 100 ms
        std::vector<int16_t> far(n), mic(n, 0);
        unsigned s = 98765;
        for (int i = 0; i < n; ++i) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            far[i] = static_cast<int16_t>((s % 2001) - 1000);
        }
        // Mic carries the reference at a DIFFERENT delay; fixed must ignore it.
        for (int i = fixed_delay * 2; i < n; ++i) mic[i] = far[i - fixed_delay * 2];
        DelayAdapterConfig d;
        d.mode = DelayAdapterMode::FIXED;
        d.frame_size = frame;
        d.initial_delay_samples = fixed_delay;
        d.max_delay_samples = rate / 3;
        DelayAdapter ad(d);
        for (int f = 0; f < n / frame; ++f) {
            std::vector<int16_t> out;
            ad.align_frame(far.data() + f * frame, mic.data() + f * frame, out);
        }
        check(ad.current_delay_samples() == fixed_delay,
              "fixed adapter must hold its configured delay");
        check(ad.stats().updates == 0, "fixed adapter must never update");
    }
    std::printf("[ok] fixed delay adapter holds configuration\n");

    // Contract 7: gated dynamic adapter locks onto a clean known delay.
    {
        const int rate = 16000, frame = 160, n = rate * 4;
        const int true_delay = static_cast<int>(0.12 * rate);
        std::vector<int16_t> far(n), mic(n, 0);
        unsigned s = 12345;
        for (int i = 0; i < n; ++i) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            far[i] = static_cast<int16_t>((s % 2001) - 1000);
        }
        for (int i = true_delay; i < n; ++i) mic[i] = far[i - true_delay];
        DelayAdapterConfig d;
        d.mode = DelayAdapterMode::DYNAMIC;
        d.frame_size = frame;
        d.initial_delay_samples = 0;
        d.max_delay_samples = static_cast<int>(0.4 * rate);
        d.search_margin_samples = static_cast<int>(0.12 * rate);
        d.lag_grid_samples = rate / 200;
        d.max_step_samples = static_cast<int>(0.04 * rate);
        DelayAdapter ad(d);
        for (int f = 0; f < n / frame; ++f) {
            std::vector<int16_t> out;
            ad.align_frame(far.data() + f * frame, mic.data() + f * frame, out);
        }
        check(std::abs(ad.current_delay_samples() - true_delay) <= frame,
              "dynamic adapter must lock onto the true echo delay");
        check(ad.stats().updates > 0, "dynamic adapter must have moved");
    }
    std::printf("[ok] gated dynamic adapter tracks a clean delay\n");

    // Contract 8: a frame record is emitted per frame with a valid reason.
    {
        const int rate = 16000, frame = 160, n = rate * 2;
        std::vector<int16_t> far(n, 500), mic(n, 500);
        DelayAdapterConfig d;
        d.mode = DelayAdapterMode::DYNAMIC;
        d.frame_size = frame;
        d.max_delay_samples = rate / 4;
        DelayAdapter ad(d);
        for (int f = 0; f < n / frame; ++f) {
            std::vector<int16_t> out;
            ad.align_frame(far.data() + f * frame, mic.data() + f * frame, out);
            check(static_cast<int>(out.size()) == frame,
                  "adapter output frame size must match AEC frame");
        }
        check(static_cast<int>(ad.records().size()) == n / frame,
              "one delay record per processed frame");
    }
    std::printf("[ok] adapter frame records and output contract\n");

    std::printf("all unit tests passed\n");
    return 0;
}
