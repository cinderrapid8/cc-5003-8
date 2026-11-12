#include "delay_adapter.h"

#include <algorithm>
#include <cmath>

namespace {

double frame_rms(const int16_t* p, int n) {
    double acc = 0;
    for (int i = 0; i < n; ++i) {
        double v = p[i];
        acc += v * v;
    }
    return std::sqrt(acc / n);
}

// Mean-removed normalized correlation over decimated taps.
double normalized_corr(const std::vector<double>& a,
                       const std::vector<double>& b) {
    const size_t n = std::min(a.size(), b.size());
    if (n < 8) return 0.0;
    double ma = 0, mb = 0;
    for (size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= n; mb /= n;
    double cov = 0, va = 0, vb = 0;
    for (size_t i = 0; i < n; ++i) {
        double x = a[i] - ma, y = b[i] - mb;
        cov += x * y;
        va += x * x;
        vb += y * y;
    }
    if (va <= 1e-9 || vb <= 1e-9) return 0.0;
    return cov / std::sqrt(va * vb);
}

} // namespace

DelayAdapter::DelayAdapter(const DelayAdapterConfig& cfg) : cfg_(cfg) {
    delay_ = std::clamp(cfg_.initial_delay_samples, 0,
                        cfg_.max_delay_samples);
    pending_candidate_ = delay_;
    // Leading zeros represent reference samples that predate the call.
    ref_history_.assign(cfg_.max_delay_samples + cfg_.frame_size, 0);
}

const char* DelayAdapter::reason_name(AdapterReason r) {
    switch (r) {
        case AdapterReason::FIXED_HOLD: return "fixed_hold";
        case AdapterReason::UPDATED: return "updated";
        case AdapterReason::GATE_SILENT_REF: return "gate_silent_ref";
        case AdapterReason::GATE_SILENT_MIC: return "gate_silent_mic";
        case AdapterReason::GATE_DOUBLE_TALK: return "gate_double_talk";
        case AdapterReason::GATE_LOW_PEAK: return "gate_low_peak";
        case AdapterReason::GATE_HOLD: return "gate_hold";
        case AdapterReason::GATE_COOLDOWN: return "gate_cooldown";
    }
    return "unknown";
}

void DelayAdapter::estimate_lag(int& candidate, double& peak,
                                double& second_peak) {
    // Correlation over the most recent ACTIVE frames only: talkspurt pauses
    // carry no delay information and dilute the statistic, so silent frames
    // are excluded. Each active mic frame remembers its absolute sample
    // position, so reference windows are fetched at the matching time index
    // for every candidate lag despite the sparse, gap-containing window.
    int lo = std::max(0, delay_ - cfg_.search_margin_samples);
    int hi = std::min(cfg_.max_delay_samples,
                      delay_ + cfg_.search_margin_samples);
    const int hist_size = static_cast<int>(ref_history_.size());
    // Absolute position of ref_history_[0].
    const int64_t ref_origin =
        static_cast<int64_t>(ref_total_) - hist_size;
    // Need a minimum amount of active data before the statistic is meaningful.
    if (mic_start_.size() < 6) {
        candidate = delay_;
        peak = 0.0;
        second_peak = 0.0;
        return;
    }
    peak = -1.0;
    second_peak = -1.0;
    corr_at_current_ = -2.0;
    int best_lag = delay_;
    for (int lag = lo; lag <= hi; lag += cfg_.lag_grid_samples) {
        std::vector<double> mic_d, ref_d;
        for (size_t f = 0; f < mic_start_.size(); ++f) {
            for (int i = 0; i < cfg_.frame_size; i += cfg_.decimation) {
                mic_d.push_back(mic_history_[f * cfg_.frame_size + i]);
                // Mic sample at absolute time (start+i) hears reference from
                // (start+i-lag); map that absolute index into ref_history_.
                int64_t ref_abs = mic_start_[f] + i - lag;
                int local = static_cast<int>(ref_abs - ref_origin);
                ref_d.push_back((local >= 0 && local < hist_size)
                                    ? ref_history_[local]
                                    : 0.0);
            }
        }
        double c = normalized_corr(mic_d, ref_d);
        if (lag == delay_) corr_at_current_ = c;
        if (c > peak) {
            second_peak = peak;
            peak = c;
            best_lag = lag;
        } else if (c > second_peak) {
            second_peak = c;
        }
    }
    candidate = best_lag;
}

bool DelayAdapter::detect_double_talk() {
    // Physical double-talk test over a short (~120 ms) dedicated window. On
    // samples where the currently aligned reference is near silent, the
    // acoustic echo is also near zero, so appreciable mic energy there can
    // only come from the independent near-end source (or noise floor). The
    // fraction of such "unexplained" energy is near 0 in far-only speech and
    // rises sharply when the near end talks. Keeping this window short and
    // separate from the correlation window lets the gate release promptly.
    double unexplained = 0, total = 0;
    int silent_count = 0, total_count = 0;
    const double ref_silence = cfg_.ref_active_rms * 0.6;
    for (size_t i = 0; i < dt_mic_.size(); ++i) {
        double mv = dt_mic_[i];
        double rv = dt_ref_[i];
        total += mv * mv;
        ++total_count;
        if (std::fabs(rv) < ref_silence) {
            unexplained += mv * mv;
            ++silent_count;
        }
    }
    // Need at least 15% near-silent samples for the statistic to be
    // meaningful, so occasional zero crossings cannot trigger the gate.
    if (total <= 1e-6 || total_count == 0 ||
        silent_count < total_count / 7)
        return false;
    return (unexplained / total) > cfg_.dt_error_ratio;
}

void DelayAdapter::align_frame(const int16_t* ref, const int16_t* mic,
                               std::vector<int16_t>& ref_aligned) {
    ++stats_.frames;

    // Append the new raw reference frame, keeping only the needed history.
    ref_history_.insert(ref_history_.end(), ref, ref + cfg_.frame_size);
    ref_total_ += cfg_.frame_size;
    size_t cap = static_cast<size_t>(cfg_.max_delay_samples +
                                     cfg_.frame_size);
    if (ref_history_.size() > cap)
        ref_history_.erase(ref_history_.begin(),
                           ref_history_.begin() +
                               (ref_history_.size() - cap));

    double ref_rms = frame_rms(ref, cfg_.frame_size);
    double mic_rms = frame_rms(mic, cfg_.frame_size);
    const size_t max_frames = static_cast<size_t>(cfg_.corr_window_frames);

    AdapterReason reason = AdapterReason::FIXED_HOLD;
    bool observation_eligible = false;
    int candidate = delay_;
    double peak = 0.0, second_peak = 0.0;

    if (cfg_.mode == DelayAdapterMode::DYNAMIC) {
        const int hist_size = static_cast<int>(ref_history_.size());

        // Aligned reference for this frame at the current delay estimate.
        std::vector<int16_t> aligned(cfg_.frame_size);
        for (int i = 0; i < cfg_.frame_size; ++i) {
            int local = i + (cfg_.max_delay_samples - delay_);
            aligned[i] = (local >= 0 && local < hist_size)
                             ? ref_history_[local]
                             : 0;
        }

        bool ref_active = ref_rms >= cfg_.ref_active_rms;
        bool frame_mic_active = mic_rms >= cfg_.mic_active_rms;

        // Short rolling windows feeding the double-talk test (all frames,
        // independent of the active-frame correlation window).
        size_t dt_cap = static_cast<size_t>(cfg_.dt_window_frames) *
                        cfg_.frame_size;
        dt_mic_.insert(dt_mic_.end(), mic, mic + cfg_.frame_size);
        dt_ref_.insert(dt_ref_.end(), aligned.begin(), aligned.end());
        if (dt_mic_.size() > dt_cap) {
            dt_mic_.erase(dt_mic_.begin(),
                          dt_mic_.begin() + (dt_mic_.size() - dt_cap));
            dt_ref_.erase(dt_ref_.begin(),
                          dt_ref_.begin() + (dt_ref_.size() - dt_cap));
        }
        // Active-frame window: only energy-carrying frames are kept, each with
        // its absolute sample position so the estimator can index the
        // reference history across the gaps left by pauses.
        if (frame_mic_active) {
            mic_history_.insert(mic_history_.end(), mic,
                                mic + cfg_.frame_size);
            ref_aligned_history_.insert(ref_aligned_history_.end(),
                                        aligned.begin(), aligned.end());
            mic_start_.push_back(frame_position_);
            if (mic_start_.size() > max_frames) {
                size_t drop_samples =
                    static_cast<size_t>(cfg_.frame_size) *
                    (mic_start_.size() - max_frames);
                mic_history_.erase(mic_history_.begin(),
                                   mic_history_.begin() + drop_samples);
                ref_aligned_history_.erase(
                    ref_aligned_history_.begin(),
                    ref_aligned_history_.begin() + drop_samples);
                mic_start_.erase(
                    mic_start_.begin(),
                    mic_start_.begin() +
                        (mic_start_.size() - max_frames));
            }
        }
        bool double_talk = detect_double_talk();

        if (cfg_.naive_gate_off) {
            // Ablation: per-frame estimate with no windowing, no gating and no
            // constraints; it chases whatever the noisiest lag looks like.
            std::vector<double> mic_d;
            for (int i = 0; i < cfg_.frame_size; i += cfg_.decimation)
                mic_d.push_back(mic[i]);
            int lo = std::max(0, delay_ - cfg_.search_margin_samples);
            int hi = std::min(cfg_.max_delay_samples,
                              delay_ + cfg_.search_margin_samples);
            double best = -1.0;
            for (int lag = lo; lag <= hi; lag += cfg_.lag_grid_samples) {
                std::vector<double> ref_d;
                for (int i = 0; i < cfg_.frame_size;
                     i += cfg_.decimation) {
                    int local = i + (cfg_.max_delay_samples - lag);
                    ref_d.push_back(
                        (local >= 0 && local < hist_size)
                            ? ref_history_[local]
                            : 0.0);
                }
                double c = normalized_corr(mic_d, ref_d);
                if (c > best) { best = c; candidate = lag; }
            }
            peak = best;
            int step = candidate - delay_;
            delay_ = candidate;
            ++stats_.updates;
            stats_.max_abs_step =
                std::max(stats_.max_abs_step, std::abs(step));
            reason = AdapterReason::UPDATED;
        } else {
            estimate_lag(candidate, peak, second_peak);

            // Update eligibility: the time relation must be observable and
            // the observation trustworthy. Double talk injects an independent
            // source into the mic that the reference cannot explain, so its
            // correlation surface does not identify the echo delay and must
            // not be allowed to move the estimate.
            if (!ref_active) {
                ++stats_.gate_silent_ref;
                reason = AdapterReason::GATE_SILENT_REF;
            } else if (!frame_mic_active) {
                ++stats_.gate_silent_mic;
                reason = AdapterReason::GATE_SILENT_MIC;
            } else if (double_talk) {
                ++stats_.gate_double_talk;
                reason = AdapterReason::GATE_DOUBLE_TALK;
            } else if (peak < cfg_.peak_threshold) {
                ++stats_.gate_low_peak;
                reason = AdapterReason::GATE_LOW_PEAK;
            } else if (peak - second_peak < 0.10) {
                // Flat correlation surface: no dominant lag, the estimate
                // would be a coin flip rather than a measurement.
                ++stats_.gate_low_peak;
                reason = AdapterReason::GATE_LOW_PEAK;
            } else if (candidate != delay_ &&
                       (peak < cfg_.move_peak_floor ||
                        peak < corr_at_current_ + cfg_.hysteresis_margin)) {
                // Moving to a new lag is held to a stricter bar than keeping
                // the current one: the new peak must be a strong observation
                // AND clearly beat what the in-use alignment explains. This
                // rejects RIR-smearing side lobes on weak windows while still
                // allowing a genuine delay change to lock once its peak firms.
                ++stats_.gate_hold;
                reason = AdapterReason::GATE_HOLD;
            } else {
                observation_eligible = true;
                ++stats_.eligible_frames;
                if (cooldown_left_ > 0) {
                    --cooldown_left_;
                    ++stats_.gate_cooldown;
                    confirm_count_ = 0;
                    reason = AdapterReason::GATE_COOLDOWN;
                } else if (std::abs(candidate - delay_) <
                           cfg_.min_step_samples) {
                    confirm_count_ = 0;
                    ++stats_.gate_hold;
                    reason = AdapterReason::GATE_HOLD;
                } else {
                    if (candidate != pending_candidate_) {
                        pending_candidate_ = candidate;
                        confirm_count_ = 1;
                    } else {
                        ++confirm_count_;
                    }
                    if (confirm_count_ < cfg_.confirm_frames) {
                        ++stats_.gate_hold;
                        reason = AdapterReason::GATE_HOLD;
                    } else {
                        int step = candidate - delay_;
                        step = std::clamp(step, -cfg_.max_step_samples,
                                          cfg_.max_step_samples);
                        delay_ = std::clamp(delay_ + step, 0,
                                            cfg_.max_delay_samples);
                        pending_candidate_ = delay_;
                        confirm_count_ = 0;
                        cooldown_left_ = cfg_.cooldown_frames;
                        ++stats_.updates;
                        stats_.max_abs_step = std::max(
                            stats_.max_abs_step, std::abs(step));
                        reason = AdapterReason::UPDATED;
                    }
                }
            }
        }
    }

    // Emit the reference aligned with the (possibly updated) delay.
    const int hist_size = static_cast<int>(ref_history_.size());
    ref_aligned.resize(cfg_.frame_size);
    for (int i = 0; i < cfg_.frame_size; ++i) {
        int local = i + (cfg_.max_delay_samples - delay_);
        ref_aligned[i] = (local >= 0 && local < hist_size)
                             ? ref_history_[local]
                             : 0;
    }

    DelayFrameRecord rec;
    rec.frame_index = stats_.frames - 1;
    rec.delay_samples = delay_;
    rec.raw_candidate = candidate;
    rec.peak_corr = peak;
    rec.eligible = observation_eligible;
    rec.reason = reason;
    records_.push_back(rec);
    frame_position_ += cfg_.frame_size;
}
