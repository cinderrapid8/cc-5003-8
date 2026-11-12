#ifndef AEC_DEMO_DELAY_ADAPTER_H
#define AEC_DEMO_DELAY_ADAPTER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Delay alignment adapter placed BEFORE the echo canceller. It owns the
// reference delay line and emits, per AEC frame, a reference sample block
// aligned to the mic block. Two switchable implementations share one output
// contract so the same Speex AEC component runs downstream of either.
enum class DelayAdapterMode {
    FIXED = 0,   // fixed reference-delay buffer, never moves after init
    DYNAMIC = 1  // constrained dynamic delay tracking with eligibility gating
};

// Why the adapter did (or did not) update its delay estimate on a frame.
enum class AdapterReason {
    FIXED_HOLD = 0,        // fixed adapter: delay is configuration, not data
    UPDATED = 1,           // dynamic adapter accepted a new delay estimate
    GATE_SILENT_REF = 2,   // reference energy too low: no correlation to measure
    GATE_SILENT_MIC = 3,   // mic energy too low: nothing to align against
    GATE_DOUBLE_TALK = 4,  // near-end dominates: cross-correlation unreliable
    GATE_LOW_PEAK = 5,     // no confident correlation peak (weak/ambiguous)
    GATE_HOLD = 6,         // candidate not confirmed for enough frames yet
    GATE_COOLDOWN = 7      // recent update: rate limiter still active
};

struct DelayAdapterConfig {
    DelayAdapterMode mode = DelayAdapterMode::FIXED;
    int initial_delay_samples = 0;
    int max_delay_samples = 0;      // size of the reference delay line
    int frame_size = 0;             // AEC frame in samples
    // Dynamic tracking constraints.
    int min_step_samples = 1;       // ignore estimates closer than this
    int max_step_samples = 0;       // clamp per-update movement (rate limit)
    int confirm_frames = 3;         // consecutive eligible votes before moving
    int cooldown_frames = 5;        // frames frozen after an accepted update
    int search_margin_samples = 0;  // search current +/- margin, not full range
    int lag_grid_samples = 80;      // lag search grid step (decimated domain)
    int corr_window_frames = 30;    // active-frame accumulation window (~300 ms);
                                    // single-frame correlation is too noisy and
                                    // longer windows get diluted by talkspurt gaps
    int dt_window_frames = 12;      // double-talk test window (~120 ms)
    int decimation = 4;             // correlation computed on decimated signals
    double ref_active_rms = 25.0;   // reference activity threshold (int16 rms)
    double mic_active_rms = 25.0;   // mic activity threshold (int16 rms)
    double peak_threshold = 0.35;   // minimum correlation to count as usable
    double move_peak_floor = 0.40;  // stronger floor required to actually MOVE
                                    // to a different lag (holding the current
                                    // one is allowed at the lower threshold)
    double hysteresis_margin = 0.15; // new lag must beat the current lag's
                                     // correlation by this much before moving,
                                     // so a spurious peak on a weak window
                                     // cannot dislodge a working alignment
    double dt_error_ratio = 0.10;   // fraction of mic energy sitting on
                                    // near-silent-reference samples; far-only
                                    // stays below ~0.05, double talk ~0.15
    bool naive_gate_off = false;    // ablation: update every frame without any
                                    // eligibility/constraint logic
};

struct DelayFrameRecord {
    int64_t frame_index = 0;
    int delay_samples = 0;       // delay used to build this frame's reference
    int raw_candidate = 0;       // best lag measured before gating
    double peak_corr = 0.0;      // normalized correlation at the best lag
    bool eligible = false;       // observation judged reliable enough to vote
    AdapterReason reason = AdapterReason::FIXED_HOLD;
};

struct DelayAdapterStats {
    int64_t frames = 0;
    int64_t updates = 0;
    int64_t gate_silent_ref = 0;
    int64_t gate_silent_mic = 0;
    int64_t gate_double_talk = 0;
    int64_t gate_low_peak = 0;
    int64_t gate_hold = 0;
    int64_t gate_cooldown = 0;
    int64_t eligible_frames = 0;
    int max_abs_step = 0;
};

// Streaming delay alignment. Frames are pushed in order for both channels;
// the adapter keeps a reference history ring so any delay within
// [0, max_delay_samples] can be emitted without re-buffering the mic.
class DelayAdapter {
public:
    explicit DelayAdapter(const DelayAdapterConfig& cfg);

    // Produce one aligned reference frame for the mic frame `mic` (frame_size
    // samples), consuming the next frame_size raw reference samples from `ref`.
    // `ref_aligned` is resized to frame_size; pre-history positions are zero.
    void align_frame(const int16_t* ref, const int16_t* mic,
                     std::vector<int16_t>& ref_aligned);

    DelayAdapterMode mode() const { return cfg_.mode; }
    int current_delay_samples() const { return delay_; }
    const std::vector<DelayFrameRecord>& records() const { return records_; }
    const DelayAdapterStats& stats() const { return stats_; }
    static const char* reason_name(AdapterReason r);

private:
    // Estimate the echo delay from cross-correlation accumulated over the
    // sliding multi-frame window. Returns the best lag and its correlation.
    void estimate_lag(int& candidate, double& peak, double& second_peak);
    // Double-talk test over a short window: energy of mic samples whose
    // aligned reference is near-silent (hence near-independent of any echo).
    bool detect_double_talk();

    DelayAdapterConfig cfg_;
    int delay_ = 0;
    std::vector<int16_t> ref_history_;   // size max_delay + frame_size
    std::vector<int16_t> mic_history_;   // recent active mic frames (corr)
    std::vector<int64_t> mic_start_;     // absolute sample position per frame
    std::vector<int16_t> ref_aligned_history_; // aligned ref for same frames
    // Short, separately maintained windows for the double-talk test so the
    // gate releases quickly after the near end stops talking instead of being
    // smeared by the longer correlation accumulation window.
    std::vector<int16_t> dt_mic_;
    std::vector<int16_t> dt_ref_;
    size_t ref_total_ = 0;               // raw reference samples ever pushed
    int64_t frame_position_ = 0;         // absolute sample index of next frame
    int pending_candidate_ = 0;
    int confirm_count_ = 0;
    int cooldown_left_ = 0;
    double corr_at_current_ = -2.0; // correlation measured at current delay
    std::vector<DelayFrameRecord> records_;
    DelayAdapterStats stats_;
};

#endif
