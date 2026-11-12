#include "wav_io.h"

#include <cstdio>
#include <cstring>

namespace {

uint16_t rd16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t rd32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) |
                                  (p[3] << 24));
}
void wr16(std::vector<unsigned char>& v, uint16_t x) {
    v.push_back(static_cast<unsigned char>(x & 0xff));
    v.push_back(static_cast<unsigned char>((x >> 8) & 0xff));
}
void wr32(std::vector<unsigned char>& v, uint32_t x) {
    v.push_back(static_cast<unsigned char>(x & 0xff));
    v.push_back(static_cast<unsigned char>((x >> 8) & 0xff));
    v.push_back(static_cast<unsigned char>((x >> 16) & 0xff));
    v.push_back(static_cast<unsigned char>((x >> 24) & 0xff));
}

} // namespace

bool wav_read(const std::string& path, WavData& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    unsigned char hdr[44];
    size_t got = std::fread(hdr, 1, sizeof(hdr), f);
    if (got < 44 || std::memcmp(hdr, "RIFF", 4) != 0 ||
        std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        std::fclose(f);
        err = "not a RIFF/WAVE file: " + path;
        return false;
    }
    // Chunks beyond the canonical 44-byte header: walk the chunk list.
    uint32_t sample_rate = rd32(hdr + 24);
    uint16_t channels = rd16(hdr + 22);
    uint16_t bits = rd16(hdr + 34);
    std::vector<unsigned char> data;
    if (std::memcmp(hdr + 12, "fmt ", 4) == 0 && rd32(hdr + 16) >= 16) {
        // header layout is canonical; data chunk may still move if fmt has ext.
    }
    long data_pos = -1;
    uint32_t data_len = 0;
    // Scan from end of fmt chunk to be robust against fmt extensions / LIST.
    long scan_pos = 12;
    std::fseek(f, 0, SEEK_END);
    long file_size = std::ftell(f);
    std::fseek(f, scan_pos, SEEK_SET);
    while (scan_pos + 8 <= file_size) {
        unsigned char ch[8];
        if (std::fread(ch, 1, 8, f) != 8) break;
        uint32_t chunk_size = rd32(ch + 4);
        if (std::memcmp(ch, "fmt ", 4) == 0) {
            unsigned char fmt[40];
            uint32_t want = chunk_size < sizeof(fmt) ? chunk_size : sizeof(fmt);
            if (std::fread(fmt, 1, want, f) != want) break;
            channels = rd16(fmt + 2);
            sample_rate = rd32(fmt + 4);
            bits = rd16(fmt + 14);
            if (chunk_size > want)
                std::fseek(f, chunk_size - want, SEEK_CUR);
        } else if (std::memcmp(ch, "data", 4) == 0) {
            data_pos = scan_pos + 8;
            data_len = chunk_size;
            break;
        } else {
            std::fseek(f, chunk_size, SEEK_CUR);
        }
        scan_pos = std::ftell(f);
        // chunks are word-aligned
        if (chunk_size & 1u) {
            std::fseek(f, 1, SEEK_CUR);
            scan_pos += 1;
        }
    }
    if (data_pos < 0) {
        std::fclose(f);
        err = "no data chunk in " + path;
        return false;
    }
    if (bits != 16 && bits != 8) {
        std::fclose(f);
        err = "unsupported bit depth " + std::to_string(bits) + " in " + path;
        return false;
    }
    data.resize(data_len);
    std::fseek(f, data_pos, SEEK_SET);
    size_t n = std::fread(data.data(), 1, data_len, f);
    data.resize(n);
    std::fclose(f);

    out.sample_rate = sample_rate;
    out.channels = channels;
    out.samples.clear();
    if (bits == 16) {
        size_t frames = n / (2u * channels);
        out.samples.resize(frames * channels);
        for (size_t i = 0; i < out.samples.size(); ++i) {
            out.samples[i] = static_cast<int16_t>(
                data[2 * i] | (static_cast<unsigned>(data[2 * i + 1]) << 8));
        }
    } else {
        size_t frames = n / channels;
        out.samples.resize(frames * channels);
        for (size_t i = 0; i < out.samples.size(); ++i)
            out.samples[i] = static_cast<int16_t>((data[i] - 128) * 256);
    }
    return true;
}

bool wav_write(const std::string& path, uint32_t sample_rate,
               const std::vector<int16_t>& samples, uint16_t channels,
               std::string& err) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        err = "cannot create " + path;
        return false;
    }
    uint32_t data_bytes = static_cast<uint32_t>(samples.size() * 2);
    uint32_t byte_rate = sample_rate * channels * 2;
    std::vector<unsigned char> h;
    h.reserve(44);
    h.insert(h.end(), {'R', 'I', 'F', 'F'});
    wr32(h, 36 + data_bytes);
    h.insert(h.end(), {'W', 'A', 'V', 'E'});
    h.insert(h.end(), {'f', 'm', 't', ' '});
    wr32(h, 16);
    wr16(h, 1); // PCM
    wr16(h, channels);
    wr32(h, sample_rate);
    wr32(h, byte_rate);
    wr16(h, static_cast<uint16_t>(channels * 2));
    wr16(h, 16);
    h.insert(h.end(), {'d', 'a', 't', 'a'});
    wr32(h, data_bytes);
    bool ok = std::fwrite(h.data(), 1, h.size(), f) == h.size() &&
              std::fwrite(samples.data(), 1, data_bytes, f) == data_bytes;
    std::fclose(f);
    if (!ok) err = "short write to " + path;
    return ok;
}
