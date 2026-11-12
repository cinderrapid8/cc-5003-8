#ifndef AEC_DEMO_AEC_PROCESSOR_H
#define AEC_DEMO_AEC_PROCESSOR_H

#include <cstdint>
#include <string>
#include <vector>

#include <speex/speex_echo.h>

#include "delay_adapter.h"
#include "signal_gen.h"

struct AecConfig {
    int internal_rate = 16000;   // AEC works on a single time base
    int frame_ms = 10;           // block length sent to the AEC (10/20 ms)
    int filter_ms = 256;         // adaptive filter length, ms
    int ref_delay_ms = 0;        // configured reference delay alignment, ms
    bool bypass = false;         // passthrough (no cancellation) for A/B listen
    // Delay alignment adapter placed before the echo canceller. FIXED keeps
    // the historical one-shot static alignment; DYNAMIC tracks the echo-path
    // delay with eligibility gating. Both emit the same frame contract.
    DelayAdapterMode delay_adapter_mode = DelayAdapterMode::FIXED;
    // Dynamic-only tuning; zero means use built-in defaults at run time.
    int adapter_max_delay_ms = 0;
    int adapter_search_margin_ms = 0;
    int adapter_confirm_frames = 0;
    int adapter_cooldown_frames = 0;
    int adapter_max_step_ms = 0;
    double adapter_peak_threshold = 0.0;
    bool adapter_naive_gate_off = false;
};

enum class FrameMode {
    OK = 0,              // reference present, cancellation active
    REFERENCE_MISSING,   // reference interrupted: cancellation not possible
    BYPASS,              // user requested passthrough
    RATE_MISMATCH,       // input resampled; time base reconstructed
    TAIL_FLUSH           // partial final block, no sample dropped
};

struct InterruptionSegment {
    int64_t start_sample = 0;
    int64_t end_sample = 0;
    double start_s = 0;
    double end_s = 0;
    std::string reason;
};

struct AecRunResult {
    int rate = 0;
    std::vector<int16_t> out;       // processed near-end estimate
    std::vector<int16_t> ref_used;  // aligned reference actually fed to AEC
    std::vector<uint8_t> mode;      // FrameMode per output sample
    std::vector<InterruptionSegment> interruptions;
    int aec_frame_size = 0;
    int aec_filter_length = 0;
    int applied_ref_delay_samples = 0; // FIXED: static delay; DYNAMIC: initial
    int resampled_far = 0;          // nonzero when far rate != internal rate
    int resampled_mic = 0;
    int64_t frames_processed = 0;
    std::string library_id;
    // Delay-adapter diagnostics (empty when no adapter is active).
    DelayAdapterMode adapter_mode = DelayAdapterMode::FIXED;
    std::vector<DelayFrameRecord> adapter_records;
    DelayAdapterStats adapter_stats{};
    int adapter_final_delay_samples = 0;
};

// Processes one full recording. Block size mismatches between caller chunks
// and the AEC frame are handled with adapter buffers; samples are never
// dropped. The reference/mic time bases are aligned before cancellation.
class AecProcessor {
public:
    explicit AecProcessor(const AecConfig& cfg);
    ~AecProcessor();

    AecProcessor(const AecProcessor&) = delete;
    AecProcessor& operator=(const AecProcessor&) = delete;

    // far: reference at far_rate; mic: mix at mic_rate. ref_valid is optional
    // ground-truth outage information (may be empty -> detector only).
    bool run(const std::vector<int16_t>& far, int far_rate,
             const std::vector<int16_t>& mic, int mic_rate,
             const std::vector<bool>& ref_valid,
             AecRunResult& result, std::string& err);

private:
    // Adapter buffer: accumulates arbitrary-sized chunks and emits fixed-size
    // blocks. The final partial block is padded with zeros and flagged, so no
    // real sample is ever discarded.
    class BlockAdapter {
    public:
        explicit BlockAdapter(int block) : block_(block) {}
        void push(const int16_t* p, size_t n) {
            buf_.insert(buf_.end(), p, p + n);
        }
        bool pop(std::vector<int16_t>& frame) {
            if (static_cast<int>(buf_.size()) < block_) return false;
            frame.assign(buf_.begin(), buf_.begin() + block_);
            buf_.erase(buf_.begin(), buf_.begin() + block_);
            return true;
        }        bool flush(std::vector<int16_t>& frame, int& valid_samples) {
            if (buf_.empty()) return false;
            valid_samples = static_cast<int>(buf_.size());
            frame = buf_;
            frame.resize(block_, 0);
            buf_.clear();
            return true;
        }
        size_t buffered() const { return buf_.size(); }

    private:
        int block_;
        std::vector<int16_t> buf_;
    };

    AecConfig cfg_;
    SpeexEchoState* st_ = nullptr;
};

#endif
