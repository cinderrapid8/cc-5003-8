#include "pipeline.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <sys/stat.h>

#include "json.h"
#include "wav_io.h"

#ifndef AEC_DEMO_SERVICE_VERSION
#define AEC_DEMO_SERVICE_VERSION "dev"
#endif
#ifndef AEC_DEMO_SPEEXDSP_VERSION
#define AEC_DEMO_SPEEXDSP_VERSION "1.2.1"
#endif

namespace fs = std::filesystem;

namespace {

std::atomic<uint64_t> g_run_counter{0};

std::string make_run_id() {
    using namespace std::chrono;
    auto now = system_clock::now().time_since_epoch();
    uint64_t ms = duration_cast<milliseconds>(now).count();
    char buf[48];
    std::snprintf(buf, sizeof(buf), "run-%llx-%04llx",
                  static_cast<unsigned long long>(ms),
                  static_cast<unsigned long long>(++g_run_counter));
    return buf;
}

bool is_known_scenario(const std::string& s) {
    return s == "full" || s == "delay_jump" || s == "rate_mismatch" ||
           s == "ref_outage" || s == "double_talk";
}

JsonValue segment_to_json(const SegmentMetrics& s) {
    JsonValue o;
    o.type = JsonValue::Type::Object;
    o.obj["label"] = JsonValue::make_str(s.label);
    o.obj["start_s"] = JsonValue::make_num(s.start_s);
    o.obj["end_s"] = JsonValue::make_num(s.end_s);
    o.obj["samples"] = JsonValue::make_num(static_cast<double>(s.samples));
    o.obj["mic_rms"] = JsonValue::make_num(s.mic_rms);
    o.obj["out_rms"] = JsonValue::make_num(s.out_rms);
    o.obj["echo_rms"] = JsonValue::make_num(s.echo_rms);
    o.obj["near_rms"] = JsonValue::make_num(s.near_rms);
    o.obj["erle_db"] = JsonValue::make_num(s.erle_db);
    o.obj["echo_leak_db"] = JsonValue::make_num(s.echo_leak_db);
    o.obj["near_keep_db"] = JsonValue::make_num(s.near_keep_db);
    o.obj["corr_out_near"] = JsonValue::make_num(s.corr_out_near);
    o.obj["corr_out_echo"] = JsonValue::make_num(s.corr_out_echo);
    return o;
}

} // namespace

bool pipeline_run(const RunRequest& req, RunArtifact& art,
                  std::string& err) {
    if (!is_known_scenario(req.scenario)) {
        err = "unknown scenario: " + req.scenario;
        return false;
    }
    if (req.frame_ms != 10 && req.frame_ms != 20) {
        err = "frame_ms must be 10 or 20";
        return false;
    }
    if (req.filter_ms <= req.frame_ms || req.filter_ms > 1000) {
        err = "filter_ms out of range";
        return false;
    }
    if (req.ref_delay_ms < 0 || req.ref_delay_ms > 800) {
        err = "ref_delay_ms out of range";
        return false;
    }
    if (req.mic_rate < 8000 || req.mic_rate > 48000 ||
        req.far_rate < 8000 || req.far_rate > 48000) {
        err = "sample rates must be within 8k..48k";
        return false;
    }

    art.request = req;
    ScenarioConfig sc;
    sc.seed = req.seed;
    sc.duration_s = req.duration_s;
    sc.mic_rate = req.mic_rate;
    sc.far_rate = req.far_rate;
    sc.echo_delay_ms = 120.0;
    sc.echo_gain = 0.55;
    sc.rir_tail_ms = 180.0;
    sc.rir_decay_ms = 70.0;

    if (req.scenario == "delay_jump" || req.scenario == "full") {
        sc.delay_jump = true;
        sc.delay_jump_at_s = 6.6;
        sc.delay_jump_ms = 200.0;
    }
    if (req.scenario == "ref_outage" || req.scenario == "full")
        sc.ref_outage = true;
    if (req.scenario == "rate_mismatch") {
        // The two channels genuinely arrive on different time bases.
        sc.far_rate = req.far_rate == 16000 ? 44100 : req.far_rate;
        sc.mic_rate = req.mic_rate;
    }
    art.scenario_cfg = sc;

    if (!generate_scenario(sc, art.gen, err)) return false;

    AecConfig ac;
    ac.internal_rate = sc.mic_rate;
    ac.frame_ms = req.frame_ms;
    ac.filter_ms = req.filter_ms;
    ac.ref_delay_ms = req.ref_delay_ms;
    ac.bypass = req.bypass;
    AecProcessor proc(ac);
    if (!proc.run(art.gen.far, sc.far_rate, art.gen.mic, sc.mic_rate,
                  art.gen.ref_valid, art.run, err))
        return false;

    art.metrics = evaluate(art.gen, art.run);
    art.run_id = make_run_id();
    art.service_version = AEC_DEMO_SERVICE_VERSION;

    // Manifest: input versions, parameters, interruptions, segment results.
    JsonValue root;
    root.type = JsonValue::Type::Object;
    root.obj["run_id"] = JsonValue::make_str(art.run_id);
    root.obj["service_version"] = JsonValue::make_str(art.service_version);
    root.obj["speexdsp_version"] =
        JsonValue::make_str(AEC_DEMO_SPEEXDSP_VERSION);
    root.obj["scenario"] = JsonValue::make_str(req.scenario);
    root.obj["seed"] = JsonValue::make_num(req.seed);
    root.obj["bypass"] = JsonValue::make_bool(req.bypass);

    JsonValue p;
    p.type = JsonValue::Type::Object;
    p.obj["frame_ms"] = JsonValue::make_num(req.frame_ms);
    p.obj["filter_ms"] = JsonValue::make_num(req.filter_ms);
    p.obj["ref_delay_ms"] = JsonValue::make_num(req.ref_delay_ms);
    p.obj["mic_rate"] = JsonValue::make_num(sc.mic_rate);
    p.obj["far_rate"] = JsonValue::make_num(sc.far_rate);
    p.obj["aec_frame_size"] = JsonValue::make_num(art.run.aec_frame_size);
    p.obj["aec_filter_length"] =
        JsonValue::make_num(art.run.aec_filter_length);
    p.obj["applied_ref_delay_samples"] =
        JsonValue::make_num(art.run.applied_ref_delay_samples);
    root.obj["parameters"] = p;

    JsonValue iv;
    iv.type = JsonValue::Type::Object;
    iv.obj["generator"] = JsonValue::make_str("synthetic-speech-like-v1");
    iv.obj["rir_model"] = JsonValue::make_str("exp-decay-sparse-v1");
    iv.obj["echo_delay_ms"] = JsonValue::make_num(sc.echo_delay_ms);
    iv.obj["echo_gain"] = JsonValue::make_num(sc.echo_gain);
    iv.obj["rir_tail_ms"] = JsonValue::make_num(sc.rir_tail_ms);
    iv.obj["rir_decay_ms"] = JsonValue::make_num(sc.rir_decay_ms);
    iv.obj["delay_jump"] = JsonValue::make_bool(sc.delay_jump);
    iv.obj["delay_jump_ms"] = JsonValue::make_num(sc.delay_jump_ms);
    iv.obj["ref_outage"] = JsonValue::make_bool(sc.ref_outage);
    root.obj["input_version"] = iv;

    JsonValue ints;
    ints.type = JsonValue::Type::Array;
    for (const InterruptionSegment& s : art.run.interruptions) {
        JsonValue e;
        e.type = JsonValue::Type::Object;
        e.obj["start_s"] = JsonValue::make_num(s.start_s);
        e.obj["end_s"] = JsonValue::make_num(s.end_s);
        e.obj["start_sample"] = JsonValue::make_num(s.start_sample);
        e.obj["end_sample"] = JsonValue::make_num(s.end_sample);
        e.obj["reason"] = JsonValue::make_str(s.reason);
        ints.arr.push_back(std::move(e));
    }
    root.obj["interruptions"] = std::move(ints);

    JsonValue segs;
    segs.type = JsonValue::Type::Array;
    for (const SegmentMetrics& s : art.metrics.segments)
        segs.arr.push_back(segment_to_json(s));
    root.obj["segments"] = std::move(segs);

    JsonValue summ;
    summ.type = JsonValue::Type::Object;
    summ.obj["overall_erle_db"] =
        JsonValue::make_num(art.metrics.overall_erle_db);
    summ.obj["overall_near_keep_db"] =
        JsonValue::make_num(art.metrics.overall_near_keep_db);
    summ.obj["limited_samples"] =
        JsonValue::make_num(static_cast<double>(art.metrics.limited_samples));
    summ.obj["rate_adapted_samples"] =
        JsonValue::make_num(static_cast<double>(art.metrics.rate_adapted_samples));
    summ.obj["tail_samples"] =
        JsonValue::make_num(static_cast<double>(art.metrics.tail_samples));
    summ.obj["frames_processed"] =
        JsonValue::make_num(static_cast<double>(art.run.frames_processed));
    summ.obj["library_id"] = JsonValue::make_str(art.run.library_id);
    root.obj["summary"] = std::move(summ);
    art.manifest_json = json_dump(root);
    return true;
}

bool pipeline_export(RunArtifact& art, std::string& err) {
    fs::path dir = fs::path("run_data") / art.run_id;
    fs::create_directories(dir);
    art.output_dir = dir.string();
    const int sr = art.gen.mic_rate;
    struct W {
        const char* name;
        const std::vector<int16_t>* data;
        int rate;
    };
    std::vector<W> files = {
        {"mic_mix.wav", &art.gen.mic, sr},
        {"near_clean.wav", &art.gen.near, sr},
        {"echo_only.wav", &art.gen.echo, sr},
        {"aec_output.wav", &art.run.out, art.run.rate},
    };
    for (const W& w : files)
        if (!wav_write((dir / w.name).string(), w.rate, *w.data, 1, err))
            return false;
    if (!wav_write((dir / "far_reference.wav").string(), art.gen.far_rate,
                   art.gen.far, 1, err))
        return false;

    // Bypass render for A/B listening: same run config, no cancellation.
    RunRequest breq = art.request;
    breq.bypass = true;
    RunArtifact bart;
    if (pipeline_run(breq, bart, err)) {
        wav_write((dir / "bypass_mic.wav").string(), bart.run.rate,
                  bart.run.out, 1, err);
    }

    FILE* f = std::fopen((dir / "manifest.json").string().c_str(), "wb");
    if (!f) {
        err = "cannot write manifest in " + dir.string();
        return false;
    }
    std::fwrite(art.manifest_json.data(), 1, art.manifest_json.size(), f);
    std::fclose(f);
    return true;
}
