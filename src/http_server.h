#ifndef AEC_DEMO_HTTP_SERVER_H
#define AEC_DEMO_HTTP_SERVER_H

#include <string>

struct ServerConfig {
    int port = 8080;
    std::string web_root = "web";
};

// Blocking, single-threaded demo HTTP server. Returns on fatal error.
bool http_server_run(const ServerConfig& cfg, std::string& err);

#endif
