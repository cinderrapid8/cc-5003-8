#ifndef AEC_DEMO_WAV_IO_H
#define AEC_DEMO_WAV_IO_H

#include <cstdint>
#include <string>
#include <vector>

struct WavData {
    uint32_t sample_rate = 0;
    uint16_t channels = 1;
    std::vector<int16_t> samples; // interleaved when multi-channel
};

// Reads a PCM WAV file (16-bit or 8-bit PCM, any standard channel count).
bool wav_read(const std::string& path, WavData& out, std::string& err);

// Writes a 16-bit PCM mono/multi-channel WAV file.
bool wav_write(const std::string& path, uint32_t sample_rate,
               const std::vector<int16_t>& samples, uint16_t channels,
               std::string& err);

#endif
