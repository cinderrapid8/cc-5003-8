#include "aec_processor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <speex/speex_resampler.h>

namespace {

std::vector<int16_t> resample(const std::vector<int16_t>& in, int in_rate,
                              int out_rate) {
    if (in_rate == out_rate) return in;
    int err = 0;
    SpeexResamplerState* rs =
        speex_resampler_init(1, static_cast<spx_uint32_t>(in_rate),
                             static_cast<spx_uint32_t>(out_rate),
                             SPEEX_RESAMPLER_QUALITY_DESKTOP, &err);
    if (!rs || err != RESAMPLER_ERR_SUCCESS) {
        if (rs) speex_resampler_destroy(rs);
        return {};
    }
    size_t cap =
        static_cast<size_t>(1.0 * in.size() * out_rate / in_rate + 64);
    std::vector<int16_t> o(cap);
    spx_uint32_t il = static_cast<spx_uint32_t>(in.size());
    spx_uint32_t ol = static_cast<spx_uint32_t>(o.size());
    int rc = speex_resampler_process_int(rs, 0, in.data(), &il, o.data(), &ol);
    speex_resampler_destroy(rs);
    if (rc != RESAMPLER_ERR_SUCCESS) return {};
    o.resize(ol);
    return o;
}

// Resample the boolean validity timeline using nearest-sample mapping.
std::vector<bool> resample_validity(const std::vector<bool>& in, int in_rate,
                                    int out_rate, size_t out_len) {
    std::vector<bool> out(out_len, true);
    if (in.empty()) return out;
    for (size_t i = 0; i < out_len; ++i) {
        size_t j = static_cast<size_t>(1.0 * i * in_rate / out_rate);
        if (j >= in.size()) j = in.size() - 1;
        out[i] = in[j];
    }
    return out;
}

} // namespace

AecProcessor::AecProcessor(const AecConfig& cfg) : cfg_(cfg) {}

AecProcessor::~AecProcessor() {
    if (st_) speex_echo_state_destroy(st_);
}

bool AecProcessor::run(const std::vector<int16_t>& far, int far_rate,
                       const std::vector<int16_t>& mic, int mic_rate,
                       const std::vector<bool>& ref_valid,
                       AecRunResult& result, std::string& err) {
    const int rate = cfg_.internal_rate;
    const int frame = rate * cfg_.frame_ms / 1000;
    const int filter_len = rate * cfg_.filter_ms / 1000;
    if (frame <= 0 || filter_len <= frame) {
        err = "invalid frame/filter size";
        return false;
    }
    result.rate = rate;
    result.aec_frame_size = frame;
    result.aec_filter_length = filter_len;
#ifndef AEC_DEMO_SPEEXDSP_VERSION
#define AEC_DEMO_SPEEXDSP_VERSION "unknown"
#endif
    result.library_id =
        std::string("speexdsp ") + AEC_DEMO_SPEEXDSP_VERSION;

    // ---- Stage 1: unify both inputs onto one internal time base ----------
    std::vector<int16_t> far_i = resample(far, far_rate, rate);
    std::vector<int16_t> mic_i = resample(mic, mic_rate, rate);
    if (far_i.empty() || mic_i.empty()) {
        err = "resampler failed";
        return false;
    }
    result.resampled_far = (far_rate != rate) ? far_rate : 0;
    result.resampled_mic = (mic_rate != rate) ? mic_rate : 0;

    // Equalize lengths: the shorter channel is zero-extended, never truncated.
    size_t n = std::max(far_i.size(), mic_i.size());
    far_i.resize(n, 0);
    mic_i.resize(n, 0);

    std::vector<bool> valid =
        resample_validity(ref_valid, mic_rate, rate, n);

    // ---- Stage 2: reference delay alignment ------------------------------
    // The mic hears the loudspeaker delayed by ref_delay_ms, so sample i of
    // acoustic echo corresponds to reference sample i - delay. We feed the
    // AEC a reference delayed by the same amount (leading zeros inserted).
    int delay = rate * cfg_.ref_delay_ms / 1000;
    result.applied_ref_delay_samples = delay;
    std::vector<int16_t> ref_aligned(n, 0);
    for (size_t i = 0; i + delay < n; ++i)
        ref_aligned[i + delay] = far_i[i];
    // Explicit outage timeline shifted with the same alignment delay.
    std::vector<bool> valid_aligned(n, true);
    for (size_t i = 0; i + delay < n; ++i)
        valid_aligned[i + delay] = valid[i];
    // Pre-roll before the first aligned reference is not an interruption
    // event: no reference exists yet by construction, so the AEC simply
    // adapts through these frames with a zero reference.
    std::vector<bool> preroll(n, false);
    for (int i = 0; i < delay && i < static_cast<int>(n); ++i) {
        valid_aligned[i] = true;
        preroll[i] = true;
    }

    // ---- Stage 3: Speex AEC over fixed frames via adapter buffers --------
    if (!cfg_.bypass) {
        st_ = speex_echo_state_init(frame, filter_len);
        if (!st_) {
            err = "speex_echo_state_init failed";
            return false;
        }
        spx_int32_t sr32 = rate;
        speex_echo_ctl(st_, SPEEX_ECHO_SET_SAMPLING_RATE, &sr32);
    }

    result.out.resize(n);
    result.ref_used = ref_aligned;
    result.mode.assign(n, static_cast<uint8_t>(FrameMode::OK));
    // Extend to a whole number of frames with explicit zero padding so every
    // output sample has a defined mode and the tail is never dropped.
    size_t padded = ((n + frame - 1) / frame) * frame;
    if (padded != n) {
        mic_i.resize(padded, 0);
        ref_aligned.resize(padded, 0);
        result.out.resize(padded);
        result.mode.resize(padded, static_cast<uint8_t>(FrameMode::TAIL_FLUSH));
        result.ref_used.resize(padded, 0);
        valid_aligned.resize(padded, true);
    }

    BlockAdapter mic_adapter(frame), ref_adapter(frame);
    // Feeding both channels in one push keeps their block boundaries aligned.
    size_t pushed = 0;
    const size_t chunk = 137; // deliberately odd: not a multiple of frame
    int64_t frame_index = 0;
    bool outage_open = false;
    InterruptionSegment seg;
    // Natural conversation contains silent reference frames; those are NOT
    // outages, and the AEC keeps adapting on them. Only the explicit
    // validity timeline marks a real reference interruption. A separate
    // sustained-silence run is recorded as an informational "time relation
    // unknown" note without disabling cancellation.
    const int silence_hold_frames = std::max(1, 300 / cfg_.frame_ms);
    int silence_run = 0;
    int64_t silence_start_frame = 0;
    bool silence_note_open = false;
    InterruptionSegment note;

    auto close_seg = [&](size_t end) {
        if (!outage_open) return;
        seg.end_sample = static_cast<int64_t>(end);
        seg.end_s = static_cast<double>(end) / rate;
        result.interruptions.push_back(seg);
        outage_open = false;
    };
    auto close_note = [&](size_t end) {
        if (!silence_note_open) return;
        note.end_sample = static_cast<int64_t>(end);
        note.end_s = static_cast<double>(end) / rate;
        result.interruptions.push_back(note);
        silence_note_open = false;
    };

    const size_t total = mic_i.size(); // == padded, never truncates real data
    while (pushed < total) {
        size_t take = std::min(chunk, total - pushed);
        mic_adapter.push(mic_i.data() + pushed, take);
        ref_adapter.push(ref_aligned.data() + pushed, take);
        pushed += take;

        std::vector<int16_t> mframe, rframe;
        while (mic_adapter.pop(mframe) && ref_adapter.pop(rframe)) {
            size_t base = frame_index * frame;
            // Explicit reference outage from the stream's validity timeline.
            bool gt_missing = false;
            for (int k = 0; k < frame; ++k)
                if (!valid_aligned[base + k]) { gt_missing = true; break; }

            double rms = 0;
            for (int16_t v : rframe)
                rms += static_cast<double>(v) * v;
            rms = std::sqrt(rms / frame);
            bool silent = rms < 25.0;
            if (silent) {
                if (silence_run == 0) silence_start_frame = frame_index;
                ++silence_run;
            } else {
                silence_run = 0;
            }
            bool sustained_silence = silence_run >= silence_hold_frames;

            FrameMode mode = FrameMode::OK;
            if (result.resampled_far || result.resampled_mic)
                mode = FrameMode::RATE_MISMATCH;
            // Only an explicit outage puts the frame into limited mode.
            if (gt_missing) mode = FrameMode::REFERENCE_MISSING;

            if (gt_missing && !outage_open) {
                outage_open = true;
                seg = {};
                seg.start_sample = static_cast<int64_t>(base);
                seg.start_s = static_cast<double>(base) / rate;
                seg.reason = "reference outage (reference stream unavailable)";
            } else if (!gt_missing) {
                close_seg(base);
            }

            // Informational note for long silences: cancellation continues,
            // but downstream consumers are told the time relation is unknown.
            if (sustained_silence && !silence_note_open && !gt_missing) {
                silence_note_open = true;
                note = {};
                note.start_sample = silence_start_frame * frame;
                note.start_s =
                    static_cast<double>(note.start_sample) / rate;
                note.reason =
                    "sustained reference silence (time relation unknown; "
                    "cancellation retained)";
            } else if (!sustained_silence) {
                close_note(base);
            }

            std::vector<int16_t> of(frame);
            // Number of real (non-padding) samples in this frame.
            size_t real_in_frame =
                base + frame <= n ? frame : (n > base ? n - base : 0);
            if (cfg_.bypass) {
                // Bypass preserves every real mic sample; padding is zero.
                // Set before any other mode logic so it always wins.
                mode = FrameMode::BYPASS;
                for (int k = 0; k < frame; ++k)
                    of[k] = static_cast<size_t>(k) < real_in_frame
                                ? mframe[k]
                                : 0;
            } else if (gt_missing) {
                // No trustworthy reference: an adaptive filter cannot
                // subtract an echo it cannot observe. Pass the mic through
                // and flag the frame instead of pretending cancellation.
                of = mframe;
            } else {
                // Real library call on every aligned frame, including silent
                // ones (zero reference is a valid AEC input).
                speex_echo_cancellation(st_, mframe.data(), rframe.data(),
                                        of.data());
            }
            std::memcpy(result.out.data() + base, of.data(),
                        frame * sizeof(int16_t));
            for (int k = 0; k < frame; ++k) {
                size_t idx = base + k;
                if (static_cast<size_t>(k) >= real_in_frame) {
                    result.mode[idx] =
                        static_cast<uint8_t>(FrameMode::TAIL_FLUSH);
                } else {
                    result.mode[idx] = static_cast<uint8_t>(mode);
                }
            }
            ++frame_index;
            ++result.frames_processed;
        }
    }
    close_seg(n);
    close_note(n);

    if (st_) {
        speex_echo_state_destroy(st_);
        st_ = nullptr;
    }
    return true;
}
