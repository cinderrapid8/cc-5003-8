#include "metrics.h"

#include <cmath>

namespace {

double rms(const int16_t* p, size_t n) {
    if (n == 0) return 0;
    double acc = 0;
    for (size_t i = 0; i < n; ++i) {
        double v = p[i] / 32768.0;
        acc += v * v;
    }
    return std::sqrt(acc / n);
}

double corr(const int16_t* a, const int16_t* b, size_t n) {
    if (n == 0) return 0;
    double sa = 0, sb = 0, sab = 0, sqa = 0, sqb = 0;
    for (size_t i = 0; i < n; ++i) {
        double x = a[i] / 32768.0;
        double y = b[i] / 32768.0;
        sa += x;
        sb += y;
        sab += x * y;
        sqa += x * x;
        sqb += y * y;
    }
    double va = sqa - sa * sa / n;
    double vb = sqb - sb * sb / n;
    if (va <= 1e-12 || vb <= 1e-12) return 0;
    return (sab - sa * sb / n) / std::sqrt(va * vb);
}

} // namespace

RunMetrics evaluate(const GeneratedSignals& gen, const AecRunResult& run) {
    RunMetrics m;
    const int sr = run.rate;
    m.total_samples = run.out.size();
    for (size_t i = 0; i < run.mode.size(); ++i) {
        FrameMode fm = static_cast<FrameMode>(run.mode[i]);
        if (fm == FrameMode::REFERENCE_MISSING) ++m.limited_samples;
        if (fm == FrameMode::RATE_MISMATCH) ++m.rate_adapted_samples;
        if (fm == FrameMode::TAIL_FLUSH) ++m.tail_samples;
    }

    double far_mic_p = 0, far_out_p = 0;
    double near_in_p = 0, near_out_p = 0;

    for (const ScenarioEvent& ev : gen.events) {
        size_t i0 = static_cast<size_t>(ev.start_s * sr);
        size_t i1 = static_cast<size_t>(ev.end_s * sr);
        if (i1 > run.out.size()) i1 = run.out.size();
        if (i0 >= i1) continue;
        size_t n = i1 - i0;
        SegmentMetrics s;
        s.label = ev.label;
        s.start_s = ev.start_s;
        s.end_s = ev.end_s;
        s.samples = n;
        s.mic_rms = rms(gen.mic.data() + i0, n);
        s.out_rms = rms(run.out.data() + i0, n);
        s.echo_rms = rms(gen.echo.data() + i0, n);
        s.near_rms = rms(gen.near.data() + i0, n);
        s.corr_out_near = corr(run.out.data() + i0, gen.near.data() + i0, n);
        s.corr_out_echo = corr(run.out.data() + i0, gen.echo.data() + i0, n);

        // Power accumulations in linear scale.
        double mp = 0, op = 0, ep = 0, np_ = 0;
        for (size_t i = i0; i < i1; ++i) {
            double mv = gen.mic[i] / 32768.0;
            double ov = run.out[i] / 32768.0;
            double ev_ = gen.echo[i] / 32768.0;
            double nv = gen.near[i] / 32768.0;
            mp += mv * mv;
            op += ov * ov;
            ep += ev_ * ev_;
            np_ += nv * nv;
        }
        s.erle_db = (mp > 1e-10 && op > 1e-10)
                        ? 10.0 * std::log10(mp / op)
                        : 0;
        s.echo_leak_db = (ep > 1e-10 && op > 1e-10)
                             ? 10.0 * std::log10(op / ep)
                             : 0;
        s.near_keep_db = (np_ > 1e-10 && op > 1e-10)
                             ? 10.0 * std::log10(op / np_)
                             : 0;
        m.segments.push_back(s);

        if (ev.label == "far_only" || ev.label == "delay_jump_recovery") {
            far_mic_p += mp;
            far_out_p += op;
        }
        if (ev.label == "double_talk" || ev.label == "near_only") {
            near_in_p += np_;
            near_out_p += op;
        }
    }

    m.overall_erle_db = (far_mic_p > 1e-10 && far_out_p > 1e-10)
                            ? 10.0 * std::log10(far_mic_p / far_out_p)
                            : 0;
    m.overall_near_keep_db = (near_in_p > 1e-10 && near_out_p > 1e-10)
                                 ? 10.0 * std::log10(near_out_p / near_in_p)
                                 : 0;

    m.verdict = "see segment table; ERLE measures residual echo, near_keep "
                "and corr_out_near measure near-end retention, output energy "
                "alone is not a pass criterion";
    return m;
}
