#include "Wav.h"
#include <cstring>
#include <fstream>

namespace lb {

namespace {
uint32_t u32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
} // namespace

bool readWav(const std::string& path, Buffer& out, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "cannot open " + path; return false; }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0) {
        error = "not a RIFF/WAVE file: " + path;
        return false;
    }
    int format = 0, channels = 0, sr = 0, bits = 0;
    const uint8_t* data = nullptr;
    size_t dataLen = 0;
    size_t pos = 12;
    while (pos + 8 <= d.size()) {
        const uint8_t* ck = d.data() + pos;
        const size_t len = u32(ck + 4);
        const size_t body = pos + 8;
        if (std::memcmp(ck, "fmt ", 4) == 0 && body + 16 <= d.size()) {
            format = u16(d.data() + body);
            channels = u16(d.data() + body + 2);
            sr = (int)u32(d.data() + body + 4);
            bits = u16(d.data() + body + 14);
            if (format == 0xFFFE && body + 26 <= d.size()) format = u16(d.data() + body + 24);
        } else if (std::memcmp(ck, "data", 4) == 0) {
            data = d.data() + body;
            dataLen = std::min(len, d.size() - body);
        }
        pos = body + len + (len & 1);
    }
    if (!data || channels <= 0 || sr <= 0) { error = "missing fmt/data chunk: " + path; return false; }
    const int bytes = bits / 8;
    const size_t frames = dataLen / (size_t)(bytes * channels);
    out = Buffer((int)frames, sr);
    auto sample = [&](const uint8_t* p) -> float {
        if (format == 3 && bits == 32) { float v; std::memcpy(&v, p, 4); return v; }
        if (format == 3 && bits == 64) { double v; std::memcpy(&v, p, 8); return (float)v; }
        if (format == 1 && bits == 16) return (float)(int16_t)u16(p) / 32768.f;
        if (format == 1 && bits == 24) {
            int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
            return (float)v / 8388608.f;
        }
        if (format == 1 && bits == 32) return (float)(int32_t)u32(p) / 2147483648.f;
        return 0.f;
    };
    if (!((format == 3 && (bits == 32 || bits == 64)) || (format == 1 && (bits == 16 || bits == 24 || bits == 32)))) {
        error = "unsupported WAV encoding (format " + std::to_string(format) + ", " + std::to_string(bits) + " bit): " + path;
        return false;
    }
    for (size_t i = 0; i < frames; ++i) {
        const uint8_t* p = data + i * (size_t)(bytes * channels);
        const float l = sample(p);
        const float r = channels > 1 ? sample(p + bytes) : l;
        out.L[i] = std::isfinite(l) ? l : 0.f;
        out.R[i] = std::isfinite(r) ? r : 0.f;
    }
    return true;
}

bool writeWav24(const std::string& path, const Buffer& in, std::string& error) {
    std::ofstream f(path, std::ios::binary);
    if (!f) { error = "cannot write " + path; return false; }
    const uint32_t frames = (uint32_t)in.size(), dataLen = frames * 6;
    auto w32 = [&](uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; f.write((char*)b, 4); };
    auto w16 = [&](uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; f.write((char*)b, 2); };
    f.write("RIFF", 4); w32(36 + dataLen); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(1); w16(2); w32((uint32_t)in.sr); w32((uint32_t)in.sr * 6); w16(6); w16(24);
    f.write("data", 4); w32(dataLen);
    std::vector<uint8_t> buf((size_t)dataLen);
    size_t k = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
            float x = ch == 0 ? in.L[i] : in.R[i];
            x = std::clamp(x, -1.f, 1.f);
            int32_t v = (int32_t)std::lrint(x * 8388607.f);
            buf[k++] = (uint8_t)v; buf[k++] = (uint8_t)(v >> 8); buf[k++] = (uint8_t)(v >> 16);
        }
    }
    f.write((char*)buf.data(), (std::streamsize)buf.size());
    return (bool)f;
}

} // namespace lb
