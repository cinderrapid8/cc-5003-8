#ifndef AEC_DEMO_PIPELINE_H
#define AEC_DEMO_PIPELINE_H

#include <string>

#include "aec_processor.h"
#include "metrics.h"
#include "signal_gen.h"

struct RunRequest {
    std::string scenario = "full"; // full|delay_jump|rate_mismatch|ref_outage|double_talk
    int frame_ms = 10;
    int ref_delay_ms = 120;
    int filter_ms = 256;
    int far_rate = 16000;
    int mic_rate = 16000;
    double duration_s = 8.0;
    bool bypass = false;
    uint32_t seed = 20261112;
};

struct RunArtifact {
    RunRequest request;
    ScenarioConfig scenario_cfg;
    GeneratedSignals gen;
    AecRunResult run;
    RunMetrics metrics;
    std::string run_id;
    std::string output_dir;
    std::string manifest_json;
    std::string service_version;
};

// Validates the request, applies scenario presets and runs the full chain.
bool pipeline_run(const RunRequest& req, RunArtifact& art, std::string& err);

// Writes WAVs (mic/far/near/echo/out/bypass) and the JSON manifest.
bool pipeline_export(RunArtifact& art, std::string& err);

#endif
