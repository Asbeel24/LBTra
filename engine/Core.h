// LBTra engine - shared basics: stereo buffer, deterministic RNG, biquad, FFT.
// Pure C++17, no dependencies, so the same code drops into the JUCE plugin later.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace lb {

constexpr float kPi = 3.14159265358979f;

struct Buffer {
    int sr = 48000;
    std::vector<float> L, R;
    Buffer() = default;
    Buffer(int n, int sampleRate) : sr(sampleRate), L((size_t)n, 0.f), R((size_t)n, 0.f) {}
    int size() const { return (int)L.size(); }
    void resize(int n) { L.resize((size_t)n, 0.f); R.resize((size_t)n, 0.f); }
    float seconds() const { return (float)size() / (float)sr; }
};

inline float dbToGain(float db) { return std::pow(10.f, db / 20.f); }
inline float gainToDb(float g) { return 20.f * std::log10(std::max(g, 1e-9f)); }
inline float semisToRatio(float s) { return std::pow(2.f, s / 12.f); }

// splitmix64: identical results on every platform for a given seed.
class Rng {
public:
    explicit Rng(uint64_t seed) : state(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float uni() { return (float)(next() >> 40) * (1.0f / 16777216.0f); }
    float range(float a, float b) { return a + (b - a) * uni(); }
    float logRange(float a, float b) { return a * std::pow(b / a, uni()); }
    int irange(int a, int b) { return std::min(b, a + (int)(uni() * (float)(b - a + 1))); }
    bool chance(float p) { return uni() < p; }
    template <class T> T pick(const std::vector<T>& v) {
        return v[std::min((size_t)(uni() * (float)v.size()), v.size() - 1)];
    }
    int weighted(const std::vector<float>& w) {
        float s = 0.f;
        for (float x : w) s += std::max(0.f, x);
        if (s <= 0.f) return 0;
        float r = uni() * s;
        for (size_t i = 0; i < w.size(); ++i) {
            float x = std::max(0.f, w[i]);
            if (r < x) return (int)i;
            r -= x;
        }
        return (int)w.size() - 1;
    }

private:
    uint64_t state;
};

struct Biquad {
    enum Type { LP, HP, BP, Peak, LowShelf, HighShelf };
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    void set(Type t, float f, float q, float gainDb, float sr) {
        f = std::clamp(f, 10.f, sr * 0.45f);
        q = std::max(q, 0.05f);
        const float w = 2.f * kPi * f / sr, c = std::cos(w), s = std::sin(w), al = s / (2.f * q);
        const float A = std::pow(10.f, gainDb / 40.f);
        float B0 = 1, B1 = 0, B2 = 0, A0 = 1, A1 = 0, A2 = 0;
        switch (t) {
        case LP: B0 = (1 - c) / 2; B1 = 1 - c; B2 = (1 - c) / 2; A0 = 1 + al; A1 = -2 * c; A2 = 1 - al; break;
        case HP: B0 = (1 + c) / 2; B1 = -(1 + c); B2 = (1 + c) / 2; A0 = 1 + al; A1 = -2 * c; A2 = 1 - al; break;
        case BP: B0 = al; B1 = 0; B2 = -al; A0 = 1 + al; A1 = -2 * c; A2 = 1 - al; break;
        case Peak: B0 = 1 + al * A; B1 = -2 * c; B2 = 1 - al * A; A0 = 1 + al / A; A1 = -2 * c; A2 = 1 - al / A; break;
        case LowShelf: {
            float sq = 2 * std::sqrt(A) * al;
            B0 = A * ((A + 1) - (A - 1) * c + sq); B1 = 2 * A * ((A - 1) - (A + 1) * c); B2 = A * ((A + 1) - (A - 1) * c - sq);
            A0 = (A + 1) + (A - 1) * c + sq; A1 = -2 * ((A - 1) + (A + 1) * c); A2 = (A + 1) + (A - 1) * c - sq;
            break;
        }
        case HighShelf: {
            float sq = 2 * std::sqrt(A) * al;
            B0 = A * ((A + 1) + (A - 1) * c + sq); B1 = -2 * A * ((A - 1) + (A + 1) * c); B2 = A * ((A + 1) + (A - 1) * c - sq);
            A0 = (A + 1) - (A - 1) * c + sq; A1 = 2 * ((A - 1) - (A + 1) * c); A2 = (A + 1) - (A - 1) * c - sq;
            break;
        }
        }
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    float process(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
};

// In-place iterative radix-2 FFT. n must be a power of two.
inline void fft(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.f * kPi / (float)len;
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.f, 0.f);
            for (size_t k = 0; k < len / 2; ++k) {
                auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Cubic (Hermite) read with a fractional index; out-of-range reads return 0.
inline float readCubic(const std::vector<float>& x, double pos) {
    const long n = (long)x.size();
    const long i = (long)std::floor(pos);
    const float t = (float)(pos - (double)i);
    auto at = [&](long k) { return (k >= 0 && k < n) ? x[(size_t)k] : 0.f; };
    const float xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
    const float c0 = x0, c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2,
                c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + c0;
}

// Varispeed: plays the buffer at `ratio` times speed (pitch and length both change).
inline Buffer varispeed(const Buffer& in, float ratio) {
    if (std::fabs(ratio - 1.f) < 1e-4f) return in;
    const int n = std::max(1, (int)std::floor((double)in.size() / ratio));
    Buffer out(n, in.sr);
    for (int i = 0; i < n; ++i) {
        const double p = (double)i * ratio;
        out.L[(size_t)i] = readCubic(in.L, p);
        out.R[(size_t)i] = readCubic(in.R, p);
    }
    return out;
}

inline Buffer resampleTo(const Buffer& in, int sr) {
    if (in.sr == sr) return in;
    Buffer out = varispeed(in, (float)in.sr / (float)sr);
    out.sr = sr;
    return out;
}

inline void reverseInPlace(Buffer& b) {
    std::reverse(b.L.begin(), b.L.end());
    std::reverse(b.R.begin(), b.R.end());
}

inline float rms(const std::vector<float>& x, int a, int b) {
    a = std::max(0, a); b = std::min((int)x.size(), b);
    if (b <= a) return 0.f;
    double s = 0;
    for (int i = a; i < b; ++i) s += (double)x[(size_t)i] * x[(size_t)i];
    return (float)std::sqrt(s / (double)(b - a));
}

inline float stereoRms(const Buffer& b, int a, int e) {
    float l = rms(b.L, a, e), r = rms(b.R, a, e);
    return std::sqrt(0.5f * (l * l + r * r));
}

} // namespace lb
