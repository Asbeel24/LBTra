#include "Analysis.h"
#include <numeric>

namespace lb {

std::string Analysis::kindName() const {
    switch (kind) {
    case Kind::Tonal: return sharpAttack ? "有调 one-shot（带打击起音）" : "有调 one-shot";
    case Kind::Percussive: return "打击";
    case Kind::FX: return rising ? "音效（上升型）" : "音效";
    case Kind::Loop: return "loop";
    }
    return "?";
}

std::string Analysis::regName() const {
    switch (reg) {
    case Register::Low: return "低";
    case Register::Mid: return "中";
    case Register::High: return "高";
    }
    return "?";
}

std::string Analysis::noteName() const {
    if (f0 <= 0) return "-";
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const float midi = 69.f + 12.f * std::log2(f0 / 440.f);
    const int m = (int)std::lround(midi);
    const int cents = (int)std::lround((midi - (float)m) * 100.f);
    std::string s = std::string(names[((m % 12) + 12) % 12]) + std::to_string(m / 12 - 1);
    if (cents != 0) s += (cents > 0 ? " +" : " ") + std::to_string(cents) + "c";
    return s;
}

namespace {

// YIN on a mono signal already decimated to `sr`. Returns f0 per frame (0 = unvoiced).
std::vector<float> yinTrack(const std::vector<float>& x, int sr, int W, int hop, float minHz, float maxHz,
                            const std::vector<bool>& loud) {
    const int maxTau = (int)((float)sr / minHz), minTau = std::max(2, (int)((float)sr / maxHz));
    std::vector<float> f0s;
    std::vector<float> diff((size_t)maxTau + 1);
    int frame = 0;
    for (int start = 0; start + W + maxTau < (int)x.size(); start += hop, ++frame) {
        if (frame >= (int)loud.size() || !loud[(size_t)frame]) { f0s.push_back(0.f); continue; }
        for (int tau = 1; tau <= maxTau; ++tau) {
            double s = 0;
            for (int j = 0; j < W; ++j) {
                const float dlt = x[(size_t)(start + j)] - x[(size_t)(start + j + tau)];
                s += (double)dlt * dlt;
            }
            diff[(size_t)tau] = (float)s;
        }
        // cumulative mean normalised difference
        double run = 0;
        std::vector<float> cm((size_t)maxTau + 1, 1.f);
        for (int tau = 1; tau <= maxTau; ++tau) {
            run += diff[(size_t)tau];
            cm[(size_t)tau] = run > 0 ? (float)(diff[(size_t)tau] * tau / run) : 1.f;
        }
        int best = -1;
        for (int tau = minTau; tau < maxTau; ++tau) {
            if (cm[(size_t)tau] < 0.25f) {
                while (tau + 1 < maxTau && cm[(size_t)tau + 1] < cm[(size_t)tau]) ++tau;
                best = tau;
                break;
            }
        }
        if (best < 0) { f0s.push_back(0.f); continue; }
        const float a = cm[(size_t)best - 1], b = cm[(size_t)best], c = cm[(size_t)best + 1];
        const float den = a - 2 * b + c;
        const float shift = std::fabs(den) > 1e-9f ? 0.5f * (a - c) / den : 0.f;
        f0s.push_back((float)sr / ((float)best + std::clamp(shift, -1.f, 1.f)));
    }
    return f0s;
}

} // namespace

Analysis analyze(const Buffer& b, const std::string& name) {
    Analysis a;
    a.name = name;
    const int sr = b.sr, n = b.size();
    a.duration = (float)n / (float)sr;
    std::vector<float> mono((size_t)n);
    for (int i = 0; i < n; ++i) mono[(size_t)i] = 0.5f * (b.L[(size_t)i] + b.R[(size_t)i]);

    // --- 10 ms RMS envelope
    const int hop = sr / 100;
    std::vector<float> env;
    for (int s = 0; s < n; s += hop) env.push_back(rms(mono, s, s + 2 * hop));
    if (env.empty()) return a;
    const size_t peakIdx = (size_t)(std::max_element(env.begin(), env.end()) - env.begin());
    const float peak = std::max(env[peakIdx], 1e-9f);
    size_t first = 0, last = env.size() - 1;
    while (first < env.size() && env[first] < peak * 0.0056f) ++first; // -45 dB
    while (last > first && env[last] < peak * 0.0056f) --last;
    a.firstAudible = (int)first * hop;
    a.lastAudible = std::min(n, (int)(last + 1) * hop);
    a.loudestSample = (int)peakIdx * hop;
    a.attackMs = (float)(peakIdx - std::min(first, peakIdx)) * 10.f;
    {
        size_t half = first;
        while (half < peakIdx && env[half] < peak * 0.5f) ++half;
        a.sharpAttack = (float)(half - first) * 10.f <= 20.f; // reaches -6 dB within 20 ms
    }
    const float audibleLen = std::max(1.f, (float)(last - first));
    a.peakPos = (float)(peakIdx - std::min(first, peakIdx)) / audibleLen;

    // rising: second half clearly louder than first half and loudest point late
    {
        const size_t mid = first + (last - first) / 2;
        double e1 = 0, e2 = 0;
        for (size_t i = first; i < mid; ++i) e1 += env[i] * env[i];
        for (size_t i = mid; i <= last; ++i) e2 += env[i] * env[i];
        a.rising = a.peakPos > 0.6f && e2 > e1 * 2.0;
    }

    // --- spectrum: flatness and centroid averaged over loud frames
    {
        const int N = 2048;
        std::vector<std::complex<float>> buf((size_t)N);
        double flatSum = 0, centSum = 0, wSum = 0;
        for (int s = a.firstAudible; s + N < a.lastAudible; s += N / 2) {
            const float fr = rms(mono, s, s + N);
            if (fr < peak * 0.1f) continue;
            for (int j = 0; j < N; ++j) {
                const float w = 0.5f - 0.5f * std::cos(2 * kPi * (float)j / (float)(N - 1));
                buf[(size_t)j] = {mono[(size_t)(s + j)] * w, 0.f};
            }
            fft(buf);
            double logSum = 0, linSum = 0, num = 0;
            int cnt = 0;
            const int lo = (int)(50.f * N / sr), hi = (int)(16000.f * N / sr);
            for (int k = std::max(1, lo); k < hi && k < N / 2; ++k) {
                const double p = (double)std::norm(buf[(size_t)k]) + 1e-12;
                logSum += std::log(p);
                linSum += p;
                num += p * ((double)k * sr / N);
                ++cnt;
            }
            if (cnt == 0 || linSum <= 0) continue;
            const double flat = std::exp(logSum / cnt) / (linSum / cnt);
            flatSum += flat * fr;
            centSum += (num / linSum) * fr;
            wSum += fr;
        }
        if (wSum > 0) { a.flatness = (float)(flatSum / wSum); a.centroid = (float)(centSum / wSum); }
    }

    // --- pitch (YIN on a 12 kHz decimated copy)
    {
        const int dec = std::max(1, sr / 12000), dsr = sr / dec;
        Biquad lp1, lp2;
        lp1.set(Biquad::LP, 2500.f, 0.707f, 0, (float)sr);
        lp2.set(Biquad::LP, 2500.f, 0.707f, 0, (float)sr);
        std::vector<float> d;
        for (int i = 0; i < n; ++i) {
            const float y = lp2.process(lp1.process(mono[(size_t)i]));
            if (i % dec == 0) d.push_back(y);
        }
        const int W = 1024, dhop = 256;
        std::vector<bool> loud;
        for (int s = 0; s + W < (int)d.size(); s += dhop) loud.push_back(rms(d, s, s + W) > 0.0f);
        // loudness gate relative to the decimated signal's own peak frame
        float dpk = 0;
        std::vector<float> fr;
        for (int s = 0; s + W < (int)d.size(); s += dhop) { fr.push_back(rms(d, s, s + W)); dpk = std::max(dpk, fr.back()); }
        for (size_t i = 0; i < loud.size() && i < fr.size(); ++i) loud[i] = fr[i] > dpk * 0.1f;
        const auto f0s = yinTrack(d, dsr, W, dhop, 30.f, 1000.f, loud);
        std::vector<float> voiced;
        int loudCount = 0;
        for (size_t i = 0; i < f0s.size(); ++i) {
            if (i < loud.size() && loud[i]) ++loudCount;
            if (f0s[i] > 0) voiced.push_back(f0s[i]);
        }
        a.pitchConfidence = loudCount ? (float)voiced.size() / (float)loudCount : 0.f;
        if (!voiced.empty()) {
            std::vector<float> s = voiced;
            std::nth_element(s.begin(), s.begin() + (long)s.size() / 2, s.end());
            const float med = s[s.size() / 2];
            double var = 0;
            for (float v : voiced) { const double st = 12.0 * std::log2(v / med); var += st * st; }
            a.pitchSpread = (float)std::sqrt(var / (double)voiced.size());
            a.f0 = med;
        }
    }

    // --- onsets via positive energy flux on the envelope
    {
        std::vector<float> flux(env.size(), 0.f);
        for (size_t i = 1; i < env.size(); ++i) flux[i] = std::max(0.f, env[i] - env[i - 1]);
        std::vector<int> on;
        for (size_t i = 2; i + 2 < flux.size(); ++i) {
            float local = 0;
            for (size_t j = (i > 10 ? i - 10 : 0); j < std::min(flux.size(), i + 10); ++j) local += flux[j];
            local /= 20.f;
            if (flux[i] > peak * 0.08f && flux[i] > 3.f * local && flux[i] >= flux[i - 1] && flux[i] > flux[i + 1] &&
                (on.empty() || (int)i - on.back() > 8))
                on.push_back((int)i);
        }
        a.onsets = (int)on.size();
        if (on.size() >= 3) {
            std::vector<float> ioi;
            for (size_t i = 1; i < on.size(); ++i) ioi.push_back((float)(on[i] - on[i - 1]));
            const float mean = std::accumulate(ioi.begin(), ioi.end(), 0.f) / (float)ioi.size();
            float var = 0;
            for (float v : ioi) var += (v - mean) * (v - mean);
            const float cv = std::sqrt(var / (float)ioi.size()) / std::max(mean, 1.f);
            a.onsetRegularity = std::clamp(1.f - cv, 0.f, 1.f);
        }
    }

    // --- decision
    const bool stablePitch = a.pitchConfidence > 0.5f && a.pitchSpread < 1.5f && a.flatness < 0.25f;
    if (!a.rising && a.duration >= 1.8f && a.onsets >= 4 && a.onsetRegularity > 0.6f) a.kind = Kind::Loop;
    else if (stablePitch) a.kind = Kind::Tonal;
    else if (a.sharpAttack && (a.lastAudible - a.firstAudible) < sr * 0.8f) a.kind = Kind::Percussive;
    else a.kind = Kind::FX;

    const float refHz = (a.kind == Kind::Tonal && a.f0 > 0) ? a.f0 : a.centroid * 0.25f;
    a.reg = refHz < 150.f ? Register::Low : (refHz < 600.f ? Register::Mid : Register::High);
    if (a.kind != Kind::Tonal) a.f0 = a.pitchConfidence > 0.5f && a.pitchSpread < 1.5f ? a.f0 : 0.f;
    return a;
}

} // namespace lb
