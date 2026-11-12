#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "http_server.h"
#include "pipeline.h"

namespace {

void print_usage() {
    std::printf(
        "reference echo cancellation demo (real SpeexDSP AEC)\n"
        "usage:\n"
        "  aec_demo demo [--scenario full|delay_jump|rate_mismatch|"
        "ref_outage|double_talk]\n"
        "               [--frame-ms 10|20] [--ref-delay-ms N]\n"
        "               [--filter-ms N] [--far-rate N] [--mic-rate N]\n"
        "               [--duration S] [--seed N] [--bypass]\n"
        "               [--delay-adapter fixed|dynamic|dynamic_naive]\n"
        "  aec_demo server [--port 8080] [--web-root web]\n"
        "  aec_demo accept [--web-root web]   # run all acceptance scenarios\n");
}

void print_report(const RunArtifact& art) {
    const auto& r = art.run;
    const auto& m = art.metrics;
    std::printf("\n=== run %s  scenario=%s  library=%s ===\n",
                art.run_id.c_str(), art.request.scenario.c_str(),
                r.library_id.c_str());
    std::printf(
        "internal rate=%d Hz  AEC frame=%d samples (%d ms)  filter=%d samples"
        " (%d ms)\nreference delay aligned: %d samples\n",
        r.rate, r.aec_frame_size, art.request.frame_ms, r.aec_filter_length,
        art.request.filter_ms, r.applied_ref_delay_samples);
    if (r.resampled_far || r.resampled_mic)
        std::printf("rate adaptation active: far=%s mic=%s\n",
                    r.resampled_far ? std::to_string(r.resampled_far).c_str()
                                    : "native",
                    r.resampled_mic ? std::to_string(r.resampled_mic).c_str()
                                    : "native");
    std::printf("frames processed=%lld\n",
                static_cast<long long>(r.frames_processed));
    if (r.interruptions.empty()) {
        std::printf("interruptions: none\n");
    } else {
        std::printf("interruptions (processing limited, not faked):\n");
        for (const auto& s : r.interruptions)
            std::printf("  %.2fs..%.2fs  %s\n", s.start_s, s.end_s,
                        s.reason.c_str());
    }
    std::printf("\n%-22s %8s %8s %9s %9s %9s %10s %10s\n", "segment",
                "micRMS", "outRMS", "ERLE dB", "leak dB", "keep dB",
                "corrNear", "corrEcho");
    for (const auto& s : m.segments) {
        std::printf("%-22s %8.4f %8.4f %9.2f %9.2f %9.2f %10.3f %10.3f\n",
                    s.label.c_str(), s.mic_rms, s.out_rms, s.erle_db,
                    s.echo_leak_db, s.near_keep_db, s.corr_out_near,
                    s.corr_out_echo);
    }
    std::printf("\noverall far-only ERLE: %.2f dB   near-active keep: %.2f dB"
                " (0 dB = full retention)\n",
                m.overall_erle_db, m.overall_near_keep_db);
    std::printf("limited samples=%zu  rate-adapted=%zu  tail-flush=%zu\n",
                m.limited_samples, m.rate_adapted_samples, m.tail_samples);
    std::printf("outputs in: %s\n\n", art.output_dir.c_str());
}

int run_demo(const RunRequest& req) {
    RunArtifact art;
    std::string err;
    if (!pipeline_run(req, art, err)) {
        std::fprintf(stderr, "run failed: %s\n", err.c_str());
        return 2;
    }
    if (!pipeline_export(art, err)) {
        std::fprintf(stderr, "export failed: %s\n", err.c_str());
        return 2;
    }
    print_report(art);
    return 0;
}

int run_acceptance(const std::string& web_root) {
    struct Case {
        const char* name;
        RunRequest req;
    };
    std::vector<Case> cases;
    auto base = [] {
        RunRequest r;
        return r;
    };
    {
        Case c{"01_delay_jump", base()};
        c.req.scenario = "delay_jump";
        c.req.ref_delay_ms = 120;
        cases.push_back(c);
    }
    {
        Case c{"02_rate_mismatch", base()};
        c.req.scenario = "rate_mismatch";
        c.req.far_rate = 44100;
        cases.push_back(c);
    }
    {
        Case c{"03_ref_outage", base()};
        c.req.scenario = "ref_outage";
        cases.push_back(c);
    }
    {
        Case c{"04_double_talk", base()};
        c.req.scenario = "double_talk";
        cases.push_back(c);
    }
    {
        Case c{"05_full_combined", base()};
        c.req.scenario = "full";
        cases.push_back(c);
    }
    {
        Case c{"06_block_size_20ms", base()};
        c.req.scenario = "full";
        c.req.frame_ms = 20;
        cases.push_back(c);
    }
    int failures = 0;
    for (const Case& c : cases) {
        RunArtifact art;
        std::string err;
        if (!pipeline_run(c.req, art, err) ||
            !pipeline_export(art, err)) {
            std::printf("[FAIL] %s: %s\n", c.name, err.c_str());
            ++failures;
            continue;
        }
        print_report(art);
    }
    std::printf(failures == 0
                    ? "acceptance suite completed: all scenarios ran\n"
                    : "acceptance suite: %d scenario(s) failed\n",
                failures);
    (void)web_root;
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }
    std::string mode = argv[1];
    RunRequest req;
    ServerConfig scfg;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* def) -> std::string {
            if (i + 1 < argc) return argv[++i];
            return def;
        };
        if (a == "--scenario") req.scenario = next("full");
        else if (a == "--frame-ms") req.frame_ms = std::atoi(next("10").c_str());
        else if (a == "--ref-delay-ms")
            req.ref_delay_ms = std::atoi(next("120").c_str());
        else if (a == "--filter-ms")
            req.filter_ms = std::atoi(next("256").c_str());
        else if (a == "--far-rate")
            req.far_rate = std::atoi(next("16000").c_str());
        else if (a == "--mic-rate")
            req.mic_rate = std::atoi(next("16000").c_str());
        else if (a == "--duration")
            req.duration_s = std::atof(next("8").c_str());
        else if (a == "--seed")
            req.seed = static_cast<uint32_t>(std::strtoul(next("20261112").c_str(),
                                                          nullptr, 10));
        else if (a == "--bypass") req.bypass = true;
        else if (a == "--delay-adapter")
            req.delay_adapter = next("fixed");
        else if (a == "--port") scfg.port = std::atoi(next("8080").c_str());
        else if (a == "--web-root") scfg.web_root = next("web");
        else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 1;
        }
    }

    if (mode == "demo") return run_demo(req);
    if (mode == "accept") return run_acceptance(scfg.web_root);
    if (mode == "server") {
        std::string err;
        std::printf("serving web UI on http://0.0.0.0:%d (root=%s)\n",
                    scfg.port, scfg.web_root.c_str());
        if (!http_server_run(scfg, err)) {
            std::fprintf(stderr, "server error: %s\n", err.c_str());
            return 2;
        }
        return 0;
    }
    print_usage();
    return 1;
}
