#include <cstdio>
#include <vector>

#include "aec_processor.h"
#include "pipeline.h"
#include "signal_gen.h"

int main() {
    for (int d : {0, 40, 80, 100, 120, 140, 160, 200}) {
        RunRequest req;
        req.scenario = "double_talk";
        req.ref_delay_ms = d;
        req.duration_s = 8.0;
        RunArtifact art;
        std::string err;
        if (!pipeline_run(req, art, err)) {
            std::printf("delay=%d failed: %s\n", d, err.c_str());
            continue;
        }
        // ERLE over the first far-only window only (0..0.6 s is short);
        // use overall instead.
        std::printf("delay=%3d ms  overall_erle=%6.2f dB  near_keep=%6.2f dB\n",
                    d, art.metrics.overall_erle_db,
                    art.metrics.overall_near_keep_db);
    }
}
