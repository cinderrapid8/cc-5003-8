#include "signal_gen.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <speex/speex_resampler.h>

namespace {

// Deterministic PRNG (xorshift32) so every run with the same seed is identical.
class Rng {
public:
    explicit Rng(uint32_t s) : state_(s ? s : 0x9e3779b9u) {}
    uint32_t next_u32() {
        uint32_t x = state_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state_ = x;
        return x;
    }
    float uniform(float lo, float hi) {
        float u = (next_u32() >> 8) * (1.0f / 16777216.0f);
        return lo + u * (hi - lo);
    }

private:
    uint32_t state_;
};

// One formant resonance: 2-pole resonator with impulse-excited (voiced) or
// noise-excited (unvoiced) input. This produces clearly speech-shaped,
// pitch-varying signals without any trained model or external assets.
struct Formant {
    float a1 = 0, a2 = 0;
    float s1 = 0, s2 = 0;
    void set(float freq, float bandwidth, int sr) {
        float r = std::exp(-static_cast<float>(M_PI) * bandwidth / sr);
        float theta = 2.0f * static_cast<float>(M_PI) * freq / sr;
        a1 = 2.0f * r * std::cos(theta);
        a2 = -r * r;
    }
    float step(float x) {
        float y = x + a1 * s1 + a2 * s2;
        s2 = s1;
        s1 = y;
        return y;
    }
    void reset() { s1 = s2 = 0; }
};

// speech_flag selects independent parameter streams for far vs near speech.
std::vector<float> synth_speech_like(int sr, double dur_s, Rng& rng,
                                     int speech_flag) {
    const size_t n = static_cast<size_t>(sr * dur_s);
    std::vector<float> x(n, 0.0f);
    // Independent sub-streams: derive a distinct, fixed RNG offset.
    Rng local(rng.next_u32() ^ (0x9e3779b9u * (speech_flag + 1)));

    Formant f1, f2, f3;
    f1.set(700, 90, sr);
    f2.set(1200, 110, sr);
    f3.set(2600, 160, sr);

    double t = 0.0;
    double next_segment = 0.0;
    bool voiced = true;
    double f0 = 120.0;
    double phase = 0.0;
    double segment_dur = 0.0;
    double amp = 0.0;
    double amp_target = 0.35;
    size_t noise_hold = 0;
    float noise_val = 0.0f;

    for (size_t i = 0; i < n; ++i, t += 1.0 / sr) {
        if (t >= next_segment) {
            segment_dur = local.uniform(0.18f, 0.45f);
            next_segment = t + segment_dur;
            voiced = local.next_u32() % 100 < 72;
            f0 = (speech_flag == 0 ? local.uniform(100, 160)
                                   : local.uniform(130, 210));
            amp_target = voiced ? local.uniform(0.30f, 0.5f)
                                : local.uniform(0.10f, 0.22f);
            // Slow formant drift gives a time-varying spectral envelope.
            f1.set(local.uniform(500, 850), 90, sr);
            f2.set(local.uniform(1000, 1500), 110, sr);
            f3.set(local.uniform(2300, 3000), 160, sr);
        }
        amp += (amp_target - amp) * 0.002;

        float exc;
        if (voiced) {
            // Glottal pulse train; pitch glides gently within a segment.
            double glide = 1.0 + 0.04 * std::sin(t * 2.0 * M_PI / segment_dur);
            phase += f0 * glide / sr;
            if (phase >= 1.0) {
                phase -= 1.0;
                exc = 1.0f;
            } else {
                exc = 0.0f;
            }
        } else {
            if (noise_hold == 0) {
                noise_val = local.uniform(-1.0f, 1.0f);
                noise_hold = 7;
            }
            --noise_hold;
            exc = noise_val * 0.5f;
        }
        float y = f1.step(exc) * 0.55f + f2.step(exc) * 0.3f +
                  f3.step(exc) * 0.15f;
        x[i] = static_cast<float>(y * amp);
    }

    // Envelope: talkspurts with pauses, independently scheduled.
    Rng env_rng(rng.next_u32() ^ (0x85ebca6bu * (speech_flag + 7)));
    double tt = 0.0;
    bool on = false;
    double switch_at = env_rng.uniform(0.05f, 0.3f);
    for (size_t i = 0; i < n; ++i, tt += 1.0 / sr) {
        if (tt >= switch_at) {
            on = !on;
            switch_at = tt + (on ? env_rng.uniform(0.6f, 1.4f)
                                 : env_rng.uniform(0.25f, 0.7f));
        }
        if (!on) x[i] = 0.0f;
    }
    // Peak-normalize to a safe level, leaving headroom for mixing.
    float peak = 1e-9f;
    for (float v : x) peak = std::max(peak, std::fabs(v));
    float scale = 0.8f / peak;
    for (float& v : x) v *= scale;
    return x;
}

// Exponentially decaying room impulse response with sparse early reflections.
std::vector<float> make_rir(int sr, double delay_samp, double tail_ms,
                            double decay_ms, double gain, Rng& rng) {
    int direct = std::max(0, static_cast<int>(std::round(delay_samp)));
    int tail = static_cast<int>(tail_ms * sr / 1000.0);
    std::vector<float> h(direct + tail, 0.0f);
    h[direct] = static_cast<float>(gain);
    for (int k = 1; k < tail; ++k) {
        double env = std::exp(-k * 1000.0 / (decay_ms * sr));
        // Sparse reflections + dense stochastic tail, deterministic per seed.
        float noise = rng.uniform(-1.0f, 1.0f);
        float reflection = (k % 137 == 0) ? 0.6f : 0.0f;
        h[direct + k] = static_cast<float>(gain * env * (0.35f * noise + reflection));
    }
    return h;
}

// Direct-form I convolution. Lengths are short (<= a few hundred ms @16k).
std::vector<float> convolve(const std::vector<float>& x,
                            const std::vector<float>& h) {
    std::vector<float> y(x.size(), 0.0f);
    for (size_t i = 0; i < x.size(); ++i) {
        float acc = 0.0f;
        size_t k0 = (i + 1 < h.size()) ? 0 : (i + 1 - h.size());
        for (size_t k = k0; k <= i; ++k) acc += x[k] * h[i - k];
        y[i] = acc;
    }
    return y;
}

// Resample with the real Speex resampler; ratio = out_rate / in_rate.
std::vector<int16_t> resample_int(const std::vector<int16_t>& in,
                                  int in_rate, int out_rate) {
    if (in_rate == out_rate) return in;
    int err = 0;
    SpeexResamplerState* st =
        speex_resampler_init(1, static_cast<spx_uint32_t>(in_rate),
                             static_cast<spx_uint32_t>(out_rate),
                             SPEEX_RESAMPLER_QUALITY_DESKTOP, &err);
    if (!st || err != RESAMPLER_ERR_SUCCESS) {
        if (st) speex_resampler_destroy(st);
        return {};
    }
    size_t cap = static_cast<size_t>(
        static_cast<double>(in.size()) * out_rate / in_rate + 64);
    std::vector<int16_t> out(cap);
    spx_uint32_t in_len = static_cast<spx_uint32_t>(in.size());
    spx_uint32_t out_len = static_cast<spx_uint32_t>(out.size());
    speex_resampler_process_int(st, 0, in.data(), &in_len, out.data(),
                                &out_len);
    out.resize(out_len);
    speex_resampler_destroy(st);
    return out;
}

std::vector<float> resample_float(const std::vector<float>& in, int in_rate,
                                  int out_rate) {
    std::vector<int16_t> q(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        float v = std::max(-1.0f, std::min(1.0f, in[i]));
        q[i] = static_cast<int16_t>(std::lround(v * 32767.0f));
    }
    std::vector<int16_t> o = resample_int(q, in_rate, out_rate);
    std::vector<float> f(o.size());
    for (size_t i = 0; i < o.size(); ++i) f[i] = o[i] / 32768.0f;
    return f;
}

int16_t to_int16(float v) {
    v = std::max(-1.0f, std::min(1.0f, v));
    return static_cast<int16_t>(std::lround(v * 32767.0f));
}

} // namespace

bool generate_scenario(const ScenarioConfig& cfg, GeneratedSignals& out,
                       std::string& err) {
    if (cfg.mic_rate <= 0 || cfg.far_rate <= 0 || cfg.duration_s <= 0) {
        err = "invalid sample rate or duration";
        return false;
    }
    Rng rng(cfg.seed);
    const int sr = cfg.mic_rate;
    const size_t n = static_cast<size_t>(sr * cfg.duration_s);

    // 1. Two independent speech-like sources at their native rates.
    std::vector<float> far_native =
        synth_speech_like(cfg.far_rate, cfg.duration_s, rng, 0);
    std::vector<float> near =
        synth_speech_like(sr, cfg.duration_s, rng, 1);

    // 2. Far reference at its own rate (clipped later during an outage).
    std::vector<int16_t> far(far_native.size());
    for (size_t i = 0; i < far_native.size(); ++i)
        far[i] = to_int16(far_native[i]);

    // 3. Far signal as heard at the mic rate via a sample-rate conversion.
    std::vector<float> far_at_mic =
        resample_float(far_native, cfg.far_rate, sr);
    far_at_mic.resize(n, 0.0f);

    // 4. Echo path: known delay, attenuation, room response. With a delay jump
    //    the second half uses a different RIR; the two responses are summed.
    std::vector<float> echo(n, 0.0f);
    std::vector<int> delay_per_sample(n);
    int jump = cfg.delay_jump
                   ? static_cast<int>(cfg.delay_jump_at_s * sr)
                   : static_cast<int>(n + 1);
    int d1 = static_cast<int>(cfg.echo_delay_ms * sr / 1000.0);
    int d2 = static_cast<int>(cfg.delay_jump_ms * sr / 1000.0);
    Rng rir_rng(cfg.seed ^ 0x5bd1e995u);
    std::vector<float> h1 = make_rir(sr, d1, cfg.rir_tail_ms,
                                     cfg.rir_decay_ms, cfg.echo_gain, rir_rng);
    std::vector<float> h2 = make_rir(sr, d2, cfg.rir_tail_ms,
                                     cfg.rir_decay_ms, cfg.echo_gain, rir_rng);
    if (cfg.delay_jump) {
        std::vector<float> part1(far_at_mic.begin(),
                                 far_at_mic.begin() + std::min<size_t>(jump, n));
        std::vector<float> part2(far_at_mic.begin() + std::min<size_t>(jump, n),
                                 far_at_mic.end());
        std::vector<float> e1 = convolve(part1, h1);
        std::vector<float> e2 = convolve(part2, h2);
        for (size_t i = 0; i < e1.size(); ++i) echo[i] += e1[i];
        for (size_t i = 0; i < e2.size(); ++i)
            if (jump + i < n) echo[jump + i] += e2[i];
        for (size_t i = 0; i < n; ++i)
            delay_per_sample[i] = (i < static_cast<size_t>(jump)) ? d1 : d2;
    } else {
        echo = convolve(far_at_mic, h1);
        for (size_t i = 0; i < n; ++i) delay_per_sample[i] = d1;
    }

    // 5. Reference outage: the far stream is missing for a window at BOTH
    //    rates, but the acoustic echo physically keeps arriving.
    std::vector<bool> ref_valid(n, true);
    if (cfg.ref_outage) {
        size_t o0 = static_cast<size_t>(2.5 * sr);
        size_t o1 = static_cast<size_t>(3.5 * sr);
        for (size_t i = o0; i < o1 && i < n; ++i) ref_valid[i] = false;
        size_t f0 = static_cast<size_t>(2.5 * cfg.far_rate);
        size_t f1 = static_cast<size_t>(3.5 * cfg.far_rate);
        for (size_t i = f0; i < f1 && i < far.size(); ++i) far[i] = 0;
    }

    // 6. Near-end activity schedule: near-only then double-talk windows.
    std::vector<bool> near_on(n, false);
    auto mark_on = [&](double a, double b) {
        size_t i0 = static_cast<size_t>(a * sr);
        size_t i1 = static_cast<size_t>(b * sr);
        for (size_t i = i0; i < i1 && i < n; ++i) near_on[i] = true;
    };
    mark_on(0.6, 1.8);   // near-only window
    mark_on(5.0, 6.4);   // double-talk window (far stays active)
    std::vector<float> near_gated(n, 0.0f);
    for (size_t i = 0; i < n; ++i)
        if (near_on[i]) near_gated[i] = near[i];

    // 7. Mic mix = echo + near (+ faint sensor floor).
    std::vector<int16_t> mic(n), near_out(n), echo_out(n);
    Rng floor_rng(cfg.seed ^ 0x1234567u);
    for (size_t i = 0; i < n; ++i) {
        float floor_noise = floor_rng.uniform(-1.0f, 1.0f) * 0.0008f;
        float mix = echo[i] + near_gated[i] + floor_noise;
        mic[i] = to_int16(mix);
        near_out[i] = to_int16(near_gated[i]);
        echo_out[i] = to_int16(echo[i]);
    }

    // 8. Ground-truth event table (used by segment-level metrics).
    std::vector<ScenarioEvent> ev;
    double dur = cfg.duration_s;
    auto add = [&](double a, double b, const std::string& label) {
        if (b > a && a < dur)
            ev.push_back({a, std::min(b, dur), label});
    };
    add(0.0, 0.6, "far_only");
    add(0.6, 1.8, "near_only");
    add(1.8, 2.5, "far_only");
    if (cfg.ref_outage) {
        add(2.5, 3.5, "ref_outage");
        add(3.5, 5.0, "far_only");
    } else {
        add(1.8, 5.0, "far_only");
    }
    add(5.0, 6.4, "double_talk");
    add(6.4, dur, "far_only");
    if (cfg.delay_jump)
        add(cfg.delay_jump_at_s,
            cfg.delay_jump_at_s + 1.2, "delay_jump_recovery");

    out.mic_rate = sr;
    out.far_rate = cfg.far_rate;
    out.far = std::move(far);
    out.mic = std::move(mic);
    out.near = std::move(near_out);
    out.echo = std::move(echo_out);
    out.events = std::move(ev);
    out.ref_valid = std::move(ref_valid);
    out.echo_delay_samples = std::move(delay_per_sample);
    return true;
}
