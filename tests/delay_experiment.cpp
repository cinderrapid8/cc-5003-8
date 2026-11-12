// Delay-adapter comparison experiment.
//
// Two delay alignment adapters are run as switchable front ends in front of
// the SAME Speex MDF echo canceller on the SAME generated signals:
//   fixed          : static reference-delay buffer, never moves
//   dynamic        : constrained tracking, updates gated by observation
//                    eligibility (double talk / silence / weak peak)
//   dynamic_naive  : ablation - tracking with all gates and constraints off
//
// Scenarios cover a stable baseline, an abrupt delay jump on far-only speech,
// and a delay jump that lands inside double talk. Every run reuses the normal
// pipeline output contract (WAVs + manifest) and additionally records the
// adapter's per-frame delay trace against the ground-truth echo delay.
//
// Outputs land under experiments/delay_adapter/<scenario>/<adapter>/ and are
// fully reproducible from a fixed seed; tests/verify_delay_experiment.py
// recomputes every residual-echo / near-retention number from the WAVs.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "aec_processor.h"
#include "delay_adapter.h"
#include "json.h"
#include "metrics.h"
#include "pipeline.h"
#include "signal_gen.h"
#include "wav_io.h"

namespace fs = std::filesystem;

namespace {

struct Variant {
    const char* key;
    std::string adapter;
    int configured_delay_ms; // what the adapter is initialized with
};

struct Scenario {
    const char* key;
    bool delay_jump;
    double jump_at_s;
    double jump_ms;
    bool ref_outage;
    double duration_s;
};

JsonValue num(double v) { return JsonValue::make_num(v); }
JsonValue str(std::string v) { return JsonValue::make_str(std::move(v)); }

// Ground-truth delay at time t, in samples (mic rate).
int truth_delay_samples(const Scenario& sc, const ScenarioConfig& cfg,
                        double t) {
    if (sc.delay_jump && t >= sc.jump_at_s)
        return static_cast<int>(sc.jump_ms * cfg.mic_rate / 1000.0);
    return static_cast<int>(cfg.echo_delay_ms * cfg.mic_rate / 1000.0);
}

const SegmentMetrics* find_segment(const RunMetrics& m,
                                   const std::string& label) {
    for (const auto& s : m.segments)
        if (s.label == label) return &s;
    return nullptr;
}

void write_wav(const fs::path& dir, const std::string& name, int rate,
               const std::vector<int16_t>& data) {
    std::string err;
    wav_write((dir / name).string(), rate, data, 1, err);
}

// Aggregate ERLE / retention over a custom time window using ground-truth
// decomposition, matching the definitions in metrics.cpp / verify_run.py.
void window_metrics(const GeneratedSignals& gen,
                    const AecRunResult& run, double t0, double t1,
                    double& erle_db, double& leak_db, double& keep_db,
                    double& corr_echo, double& corr_near) {
    const int sr = run.rate;
    size_t i0 = static_cast<size_t>(t0 * sr);
    size_t i1 = std::min(static_cast<size_t>(t1 * sr), run.out.size());
    if (i0 >= i1) { erle_db = leak_db = keep_db = 0; corr_echo = corr_near = 0; return; }
    double mp = 0, op = 0, ep = 0, np_ = 0;
    double sa = 0, sb = 0, sab = 0, sqa = 0, sqb = 0;       // out vs echo
    double sc = 0, sd = 0, scd = 0, sqc = 0, sqd = 0;       // out vs near
    size_t n = i1 - i0;
    for (size_t i = i0; i < i1; ++i) {
        double mv = gen.mic[i] / 32768.0, ov = run.out[i] / 32768.0;
        double ev = gen.echo[i] / 32768.0, nv = gen.near[i] / 32768.0;
        mp += mv * mv; op += ov * ov; ep += ev * ev; np_ += nv * nv;
        sa += ov; sb += ev; sab += ov * ev; sqa += ov * ov; sqb += ev * ev;
        sc += ov; sd += nv; scd += ov * nv; sqc += ov * ov; sqd += nv * nv;
    }
    erle_db = (mp > 1e-10 && op > 1e-10) ? 10 * std::log10(mp / op) : 0;
    leak_db = (ep > 1e-10 && op > 1e-10) ? 10 * std::log10(op / ep) : 0;
    keep_db = (np_ > 1e-10 && op > 1e-10) ? 10 * std::log10(op / np_) : 0;
    double ve = sqa - sa * sa / n, vbe = sqb - sb * sb / n;
    corr_echo = (ve > 1e-12 && vbe > 1e-12)
                    ? (sab - sa * sb / n) / std::sqrt(ve * vbe) : 0;
    double vn = sqc - sc * sc / n, vbn = sqd - sd * sd / n;
    corr_near = (vn > 1e-12 && vbn > 1e-12)
                    ? (scd - sc * sd / n) / std::sqrt(vn * vbn) : 0;
}

} // namespace

int main() {
    const int seed = 20261112;
    const std::vector<Scenario> scenarios = {
        // key, jump, jump_at, jump_ms, outage, duration
        {"s1_stable_baseline",     false, 0.0,   0.0,   false, 10.0},
        {"s2_jump_far_only",       true,  4.0, 200.0,  false, 12.0},
        {"s3_jump_in_double_talk", true,  5.6, 200.0,  false, 12.0},
        {"s4_full_stress",         true,  6.6, 200.0,  true,  12.0},
    };
    // Each variant uses the same initial delay (120 ms) so differences come
    // from adapter behavior, not initialization. fixed_wrong is the static
    // buffer configured to the post-jump value on purpose.
    const std::vector<Variant> variants = {
        {"fixed_120",      "fixed",         120},
        {"fixed_wrong_200","fixed",         200},
        {"dynamic_gated",  "dynamic",       120},
        {"dynamic_naive",  "dynamic_naive", 120},
    };

    fs::path root = "experiments/delay_adapter";
    fs::remove_all(root);
    fs::create_directories(root);

    JsonValue all;
    all.type = JsonValue::Type::Object;
    all.obj["seed"] = num(seed);
    all.obj["library"] = str("speexdsp 1.2.1 MDF, shared by every variant");
    all.obj["note"] = str(
        "all variants run the same generated signals and the same echo "
        "canceller; only the delay adapter in front differs");
    JsonValue scenario_arr;
    scenario_arr.type = JsonValue::Type::Array;

    for (const Scenario& sc : scenarios) {
        // Build the scenario once; every variant consumes identical signals.
        ScenarioConfig cfg;
        cfg.seed = seed;
        cfg.duration_s = sc.duration_s;
        cfg.mic_rate = 16000;
        cfg.far_rate = 16000;
        cfg.echo_delay_ms = 120.0;
        cfg.delay_jump = sc.delay_jump;
        cfg.delay_jump_at_s = sc.jump_at_s;
        cfg.delay_jump_ms = sc.jump_ms;
        cfg.ref_outage = sc.ref_outage;
        GeneratedSignals gen;
        std::string err;
        if (!generate_scenario(cfg, gen, err)) {
            std::fprintf(stderr, "generation failed for %s: %s\n",
                         sc.key, err.c_str());
            return 1;
        }

        JsonValue scj;
        scj.type = JsonValue::Type::Object;
        scj.obj["scenario"] = str(sc.key);
        scj.obj["delay_jump"] = JsonValue::make_bool(sc.delay_jump);
        scj.obj["jump_at_s"] = num(sc.jump_at_s);
        scj.obj["jump_ms"] = num(sc.jump_ms);
        JsonValue varr;
        varr.type = JsonValue::Type::Array;

        for (const Variant& v : variants) {
            AecConfig ac;
            ac.internal_rate = cfg.mic_rate;
            ac.frame_ms = 10;
            ac.filter_ms = 256;
            ac.ref_delay_ms = v.configured_delay_ms;
            ac.delay_adapter_mode =
                (v.adapter == "fixed") ? DelayAdapterMode::FIXED
                                       : DelayAdapterMode::DYNAMIC;
            ac.adapter_naive_gate_off = (v.adapter == "dynamic_naive");

            AecProcessor proc(ac);
            AecRunResult run;
            if (!proc.run(gen.far, cfg.far_rate, gen.mic, cfg.mic_rate,
                          gen.ref_valid, run, err)) {
                std::fprintf(stderr, "run failed %s/%s: %s\n", sc.key,
                             v.key, err.c_str());
                return 1;
            }
            RunMetrics m = evaluate(gen, run);

            fs::path dir = root / sc.key / v.key;
            fs::create_directories(dir);
            write_wav(dir, "mic_mix.wav", gen.mic_rate, gen.mic);
            write_wav(dir, "far_reference.wav", gen.far_rate, gen.far);
            write_wav(dir, "near_clean.wav", gen.mic_rate, gen.near);
            write_wav(dir, "echo_only.wav", gen.mic_rate, gen.echo);
            write_wav(dir, "aec_output.wav", run.rate, run.out);

            // Delay trace vs ground truth (one entry per AEC frame).
            std::ofstream tf(dir / "delay_trace.csv");
            tf << "frame,t_s,adapter_delay_ms,truth_delay_ms,"
                  "raw_candidate_ms,peak_corr,eligible,reason\n";
            for (const auto& r : run.adapter_records) {
                double t = (r.frame_index * run.aec_frame_size) /
                           static_cast<double>(run.rate);
                int truth = truth_delay_samples(sc, cfg, t);
                tf << r.frame_index << "," << t << ","
                   << 1000.0 * r.delay_samples / run.rate << ","
                   << 1000.0 * truth / run.rate << ","
                   << 1000.0 * r.raw_candidate / run.rate << ","
                   << r.peak_corr << "," << (r.eligible ? 1 : 0) << ","
                   << DelayAdapter::reason_name(r.reason) << "\n";
            }
            // Fixed adapter has no records: emit the configured delay so the
            // same CSV contract is available for every variant.
            if (run.adapter_records.empty()) {
                int64_t frames =
                    static_cast<int64_t>(run.out.size()) / run.aec_frame_size;
                for (int64_t f = 0; f < frames; ++f) {
                    double t = (f * run.aec_frame_size) /
                               static_cast<double>(run.rate);
                    int truth = truth_delay_samples(sc, cfg, t);
                    tf << f << "," << t << "," << v.configured_delay_ms << ","
                       << 1000.0 * truth / run.rate << ","
                       << v.configured_delay_ms << ",0.0,0,fixed_hold\n";
                }
            }
            tf.close();

            // Time-to-lock: first frame after the jump whose delay is within
            // one frame of the new ground truth and stays there for 100 ms.
            double lock_s = -1.0;
            int target = static_cast<int>(sc.jump_ms * run.rate / 1000.0);
            int tol = run.aec_frame_size;
            int hold_need = std::max(1, 100 / ac.frame_ms);
            if (sc.delay_jump) {
                int hold = 0;
                const auto& recs = run.adapter_records;
                for (size_t k = 0; k < recs.size(); ++k) {
                    double t = (recs[k].frame_index * run.aec_frame_size) /
                               static_cast<double>(run.rate);
                    if (t < sc.jump_at_s) continue;
                    if (std::abs(recs[k].delay_samples - target) <= tol) {
                        ++hold;
                        if (hold >= hold_need) {
                            lock_s = t - hold_need * ac.frame_ms / 1000.0;
                            break;
                        }
                    } else {
                        hold = 0;
                    }
                }
            }

            // Recovery-window ERLE: jump .. jump+2 s, far-only ground truth.
            double r_erle, r_leak, r_keep, r_ce, r_cn;
            if (sc.delay_jump) {
                window_metrics(gen, run, sc.jump_at_s,
                               std::min(sc.duration_s, sc.jump_at_s + 2.0),
                               r_erle, r_leak, r_keep, r_ce, r_cn);
            } else {
                r_erle = r_leak = r_keep = r_ce = r_cn = 0;
            }

            JsonValue vj;
            vj.type = JsonValue::Type::Object;
            vj.obj["adapter"] = str(v.adapter);
            vj.obj["configured_delay_ms"] = num(v.configured_delay_ms);
            vj.obj["overall_erle_db"] = num(m.overall_erle_db);
            vj.obj["overall_near_keep_db"] = num(m.overall_near_keep_db);
            if (const auto* s = find_segment(m, "double_talk")) {
                vj.obj["double_talk_erle_db"] = num(s->erle_db);
                vj.obj["double_talk_echo_leak_db"] = num(s->echo_leak_db);
                vj.obj["double_talk_near_keep_db"] = num(s->near_keep_db);
                vj.obj["double_talk_corr_near"] = num(s->corr_out_near);
                vj.obj["double_talk_corr_echo"] = num(s->corr_out_echo);
            }
            if (const auto* s = find_segment(m, "near_only")) {
                vj.obj["near_only_corr_near"] = num(s->corr_out_near);
            }
            if (sc.delay_jump) {
                vj.obj["recovery_erle_db"] = num(r_erle);
                vj.obj["recovery_echo_leak_db"] = num(r_leak);
                vj.obj["recovery_corr_echo"] = num(r_ce);
                vj.obj["time_to_lock_s"] = num(lock_s);
                vj.obj["final_delay_ms"] =
                    num(1000.0 * run.adapter_final_delay_samples / run.rate);
            }
            if (run.adapter_mode == DelayAdapterMode::DYNAMIC) {
                const auto& st = run.adapter_stats;
                vj.obj["updates"] = num(static_cast<double>(st.updates));
                vj.obj["eligible_frames"] =
                    num(static_cast<double>(st.eligible_frames));
                vj.obj["gate_double_talk"] =
                    num(static_cast<double>(st.gate_double_talk));
                vj.obj["gate_silent_ref"] =
                    num(static_cast<double>(st.gate_silent_ref));
                vj.obj["gate_silent_mic"] =
                    num(static_cast<double>(st.gate_silent_mic));
                vj.obj["gate_low_peak"] =
                    num(static_cast<double>(st.gate_low_peak));
                vj.obj["gate_hold"] =
                    num(static_cast<double>(st.gate_hold));
                vj.obj["gate_cooldown"] =
                    num(static_cast<double>(st.gate_cooldown));
                vj.obj["max_abs_step_ms"] =
                    num(1000.0 * st.max_abs_step / run.rate);
            }
            varr.arr.push_back(std::move(vj));

            std::printf("[%s/%s] ERLE=%.2f dB near_keep=%.2f dB",
                        sc.key, v.key, m.overall_erle_db,
                        m.overall_near_keep_db);
            if (sc.delay_jump)
                std::printf(" recovery_ERLE=%.2f dB lock=%.2f s",
                            r_erle, lock_s);
            std::printf("\n");
            std::fflush(stdout);
        }
        scj.obj["variants"] = std::move(varr);
        scenario_arr.arr.push_back(std::move(scj));
    }

    all.obj["scenarios"] = std::move(scenario_arr);
    std::ofstream mf(root / "results.json");
    mf << json_dump(all);
    mf.close();
    std::printf("\nwrote %s/results.json and per-variant WAV/CSV artifacts\n",
                root.string().c_str());
    return 0;
}
