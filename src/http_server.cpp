#include "http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "json.h"
#include "pipeline.h"

namespace fs = std::filesystem;

namespace {

std::string content_type(const std::string& path) {
    auto ends = [&](const char* suf) {
        size_t n = std::strlen(suf);
        return path.size() >= n && path.compare(path.size() - n, n, suf) == 0;
    };
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".js")) return "application/javascript; charset=utf-8";
    if (ends(".css")) return "text/css; charset=utf-8";
    if (ends(".json")) return "application/json; charset=utf-8";
    if (ends(".wav")) return "audio/wav";
    if (ends(".svg")) return "image/svg+xml";
    return "application/octet-stream";
}

bool read_file(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

void send_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

void send_response(int fd, int status, const std::string& type,
                   const std::string& body) {
    const char* reason = status == 200 ? "OK"
                         : status == 400 ? "Bad Request"
                         : status == 404 ? "Not Found"
                         : status == 500 ? "Internal Server Error"
                                         : "OK";
    std::ostringstream h;
    h << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
      << "Content-Type: " << type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Cache-Control: no-store\r\n\r\n";
    send_all(fd, h.str());
    send_all(fd, body);
}

void send_json(int fd, int status, const std::string& body) {
    send_response(fd, status, "application/json; charset=utf-8", body);
}

void handle_run(int fd, const std::string& payload) {
    JsonValue req_json;
    std::string err;
    if (!json_parse(payload, req_json, err) ||
        req_json.type != JsonValue::Type::Object) {
        send_json(fd, 400,
                  "{\"error\":\"invalid JSON request body\"}");
        return;
    }
    RunRequest req;
    req.scenario = json_get_string(req_json, "scenario", "full");
    req.frame_ms = static_cast<int>(json_get_number(req_json, "frame_ms", 10));
    req.ref_delay_ms =
        static_cast<int>(json_get_number(req_json, "ref_delay_ms", 120));
    req.filter_ms =
        static_cast<int>(json_get_number(req_json, "filter_ms", 256));
    req.far_rate = static_cast<int>(json_get_number(req_json, "far_rate", 16000));
    req.mic_rate = static_cast<int>(json_get_number(req_json, "mic_rate", 16000));
    req.duration_s = json_get_number(req_json, "duration_s", 8.0);
    req.bypass = json_get_bool(req_json, "bypass", false);
    req.seed = static_cast<uint32_t>(
        json_get_number(req_json, "seed", 20261112));

    RunArtifact art;
    if (!pipeline_run(req, art, err)) {
        JsonValue e;
        e.type = JsonValue::Type::Object;
        e.obj["error"] = JsonValue::make_str(err);
        send_json(fd, 400, json_dump(e));
        return;
    }
    if (!pipeline_export(art, err)) {
        JsonValue e;
        e.type = JsonValue::Type::Object;
        e.obj["error"] = JsonValue::make_str(err);
        send_json(fd, 500, json_dump(e));
        return;
    }
    JsonValue resp;
    resp.type = JsonValue::Type::Object;
    resp.obj["run_id"] = JsonValue::make_str(art.run_id);
    resp.obj["output_dir"] = JsonValue::make_str(art.output_dir);
    JsonValue manifest;
    json_parse(art.manifest_json, manifest, err);
    resp.obj["manifest"] = manifest;

    JsonValue files;
    files.type = JsonValue::Type::Array;
    for (const char* name :
         {"mic_mix.wav", "far_reference.wav", "near_clean.wav",
          "echo_only.wav", "aec_output.wav", "bypass_mic.wav",
          "manifest.json"}) {
        files.arr.push_back(
            JsonValue::make_str("/files/" + art.run_id + "/" + name));
    }
    resp.obj["files"] = std::move(files);
    send_json(fd, 200, json_dump(resp));
}

void handle_static(int fd, const fs::path& web_root, const std::string& uri) {
    std::string rel = uri.substr(1); // strip leading '/'
    if (rel.empty() || rel.back() == '/') rel += "index.html";
    // Prevent path traversal.
    fs::path target = fs::weakly_canonical(web_root / rel);
    fs::path base = fs::weakly_canonical(web_root);
    if (target.string().compare(0, base.string().size(), base.string()) != 0 ||
        !fs::is_regular_file(target)) {
        send_response(fd, 404, "text/plain", "not found\n");
        return;
    }
    std::string body;
    if (!read_file(target, body)) {
        send_response(fd, 404, "text/plain", "not found\n");
        return;
    }
    send_response(fd, 200, content_type(target.string()), body);
}

void handle_file(int fd, const std::string& uri) {
    // /files/<run_id>/<name> -> run_data/<run_id>/<name>
    fs::path target = fs::path("run_data") / uri.substr(std::strlen("/files/"));
    fs::path base = fs::weakly_canonical("run_data");
    target = fs::weakly_canonical(target);
    if (target.string().compare(0, base.string().size(), base.string()) != 0 ||
        !fs::is_regular_file(target)) {
        send_response(fd, 404, "text/plain", "not found\n");
        return;
    }
    std::string body;
    if (!read_file(target, body)) {
        send_response(fd, 404, "text/plain", "not found\n");
        return;
    }
    send_response(fd, 200, content_type(target.string()), body);
}

} // namespace

bool http_server_run(const ServerConfig& cfg, std::string& err) {
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        err = "socket() failed";
        return false;
    }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(cfg.port));
    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        err = "bind() failed on port " + std::to_string(cfg.port);
        ::close(srv);
        return false;
    }
    ::listen(srv, 16);

    for (;;) {
        sockaddr_in cli{};
        socklen_t clen = sizeof(cli);
        int cfd = ::accept(srv, reinterpret_cast<sockaddr*>(&cli), &clen);
        if (cfd < 0) continue;
        char buf[65536];
        ssize_t n = ::recv(cfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            ::close(cfd);
            continue;
        }
        buf[n] = '\0';
        std::string request(buf, static_cast<size_t>(n));
        std::istringstream line(request);
        std::string method, uri, version;
        line >> method >> uri >> version;
        size_t q = uri.find('?');
        if (q != std::string::npos) uri.resize(q);

        if (method == "POST" && uri == "/api/run") {
            size_t body_pos = request.find("\r\n\r\n");
            std::string body =
                body_pos == std::string::npos ? "" : request.substr(body_pos + 4);
            handle_run(cfd, body);
        } else if (method == "GET" && uri.rfind("/files/", 0) == 0) {
            handle_file(cfd, uri);
        } else if (method == "GET") {
            handle_static(cfd, fs::path(cfg.web_root), uri);
        } else {
            send_response(cfd, 404, "text/plain", "not found\n");
        }
        ::close(cfd);
    }
}
