#ifndef AEC_DEMO_SIGNAL_GEN_H
#define AEC_DEMO_SIGNAL_GEN_H

#include <cstdint>
#include <string>
#include <vector>

// A span of the timeline, labeled for segment-level evaluation.
struct ScenarioEvent {
    double start_s = 0;
    double end_s = 0;
    std::string label; // far_only, near_only, double_talk, ref_outage, ...
};

// Configuration of the synthetic recording / echo path.
struct ScenarioConfig {
    int mic_rate = 16000;          // microphone (near/mix) sample rate, Hz
    int far_rate = 16000;          // far-end reference sample rate, Hz
    double duration_s = 8.0;
    uint32_t seed = 20261112;      // fixed seed => reproducible signals

    double echo_delay_ms = 120.0;  // loudspeaker->mic acoustic delay
    double echo_gain = 0.55;       // attenuation of the echo path
    double rir_tail_ms = 180.0;    // room impulse response length
    double rir_decay_ms = 70.0;    // exponential decay constant of the RIR

    bool ref_outage = false;       // reference interruption segment
    bool delay_jump = false;       // abrupt echo delay change mid-call
    double delay_jump_at_s = 6.6;
    double delay_jump_ms = 200.0;  // delay after the jump
};

struct GeneratedSignals {
    int mic_rate = 0;
    int far_rate = 0;
    std::vector<int16_t> far;      // reference at far_rate (zeros in outage)
    std::vector<int16_t> mic;      // mixed mic signal at mic_rate
    std::vector<int16_t> near;     // clean near-end at mic_rate (ground truth)
    std::vector<int16_t> echo;     // echo-only component at mic_rate
    std::vector<ScenarioEvent> events;
    // Actual reference validity per mic-rate sample: false where far is
    // interrupted / unavailable. Used to flag limited processing honestly.
    std::vector<bool> ref_valid;
    // Echo path delay in samples at mic_rate, per sample (delay jump visible).
    std::vector<int> echo_delay_samples;
};

// Build the whole scenario. All randomness derives from cfg.seed.
bool generate_scenario(const ScenarioConfig& cfg, GeneratedSignals& out,
                       std::string& err);

#endif
