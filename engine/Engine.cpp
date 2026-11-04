#include "Engine.h"
#include <cstdio>

namespace lb {

const char* styleName(Style s) {
    switch (s) {
    case Style::Neutral: return "标准";
    case Style::Dirty: return "脏";
    case Style::Ambient: return "氛围";
    case Style::Fragment: return "碎片";
    }
    return "?";
}

namespace {

enum class Role { Build, BuildReverse, Hit, Fill, Roll, Downlift, Pattern };

const char* roleName(Role r) {
    switch (r) {
    case Role::Build: return "铺垫（正放，最响处对齐强拍）";
    case Role::BuildReverse: return "倒放铺垫（倒放后最响处落在强拍）";
    case Role::Hit: return "强拍上的 impact";
    case Role::Fill: return "从过渡开头直接放";
    case Role::Roll: return "强拍 impact + 强拍前连击";
    case Role::Downlift: return "下降尾巴（从强拍开始往下走）";
    case Role::Pattern: return "强拍前按节奏重复几下，再落在强拍";
    }
    return "?";
}

std::string beatsLabel(float b) {
    if (std::fabs(b - 0.125f) < 1e-3f) return "1/32";
    if (std::fabs(b - 0.25f) < 1e-3f) return "1/16";
    if (std::fabs(b - 1.f / 3.f) < 1e-3f) return "1/8T";
    if (b == 0.5f) return "1/8";
    if (b == 1.f) return "1/4";
    if (b == 2.f) return "1/2";
    if (b == 4.f) return "1 bar";
    if (b == 8.f) return "2 bar";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f 拍", b);
    return buf;
}

Role pickRole(const Analysis& a, Rng& r, bool arrangeOnly) {
    const bool low = a.reg == Register::Low;
    const Role hitOrRoll = arrangeOnly ? Role::Pattern : Role::Roll;
    switch (a.shape) {
    case Shape::Rise: return std::vector<Role>{Role::Build, Role::Downlift}[(size_t)r.weighted({0.8f, 0.2f})];
    case Shape::Fall: return std::vector<Role>{Role::Downlift, Role::BuildReverse, Role::Hit}[(size_t)r.weighted({0.45f, 0.4f, 0.15f})];
    case Shape::Hit:
        if (low) return std::vector<Role>{Role::Hit, hitOrRoll, Role::BuildReverse}[(size_t)r.weighted({0.55f, 0.3f, 0.15f})];
        return std::vector<Role>{Role::Hit, Role::BuildReverse, hitOrRoll, Role::Fill}[(size_t)r.weighted({0.3f, 0.35f, 0.25f, 0.1f})];
    case Shape::Loop: return std::vector<Role>{Role::Build, Role::Fill}[(size_t)r.weighted({0.6f, 0.4f})];
    case Shape::Sustain:
    default: return std::vector<Role>{Role::Fill, Role::Build, Role::BuildReverse, Role::Hit}[(size_t)r.weighted({0.3f, 0.25f, 0.3f, 0.15f})];
    }
}

std::vector<float> fxWeights(const Analysis& a, Style st, Role role) {
    //               Reverb RevRev Delay  Eq   Dist Crystal Stutter Gran Shaper GrainDly Chroma
    std::vector<float> w{1.0f, 0.6f, 1.0f, 1.2f, 0.6f, 1.0f, 0.5f, 1.0f, 0.0f, 1.2f, 0.9f}; // Shaper off (user 11/4)
    auto mul = [&](FxType t, float m) { w[(size_t)t] *= m; };
    switch (st) {
    case Style::Dirty: mul(FxType::Distortion, 3.f); mul(FxType::Stutter, 1.5f); mul(FxType::Shaper, 1.5f); mul(FxType::Reverb, 0.5f); mul(FxType::ReverseReverb, 0.5f); break;
    case Style::Ambient: mul(FxType::Reverb, 2.f); mul(FxType::ReverseReverb, 2.f); mul(FxType::Delay, 1.5f); mul(FxType::GrainDelay, 1.5f); mul(FxType::Chroma, 1.5f); mul(FxType::Crystal, 1.5f); mul(FxType::Granular, 1.2f); mul(FxType::Distortion, 0.2f); mul(FxType::Stutter, 0.3f); break;
    case Style::Fragment: mul(FxType::Shaper, 2.f); mul(FxType::Granular, 3.f); mul(FxType::Stutter, 2.5f); mul(FxType::Crystal, 2.f); mul(FxType::Reverb, 0.6f); break;
    case Style::Neutral: break;
    }
    switch (a.kind) {
    case Kind::Tonal: mul(FxType::Crystal, 1.5f); mul(FxType::GrainDelay, 1.3f); mul(FxType::ReverseReverb, 1.5f); break;
    case Kind::FX: mul(FxType::Eq, 1.2f); mul(FxType::Granular, 1.3f); mul(FxType::Chroma, 1.6f); break;
    case Kind::Percussive: mul(FxType::Stutter, 2.f); mul(FxType::Reverb, 0.7f); break;
    case Kind::Loop: mul(FxType::Stutter, 2.f); mul(FxType::Granular, 1.5f); break;
    }
    if (a.reg == Register::Low) {
        mul(FxType::Reverb, 0.4f); mul(FxType::ReverseReverb, 0.4f); mul(FxType::Delay, 0.5f);
        mul(FxType::Granular, 0.3f); mul(FxType::Crystal, 0.3f); mul(FxType::GrainDelay, 0.3f); mul(FxType::Chroma, 0.2f); mul(FxType::Distortion, 1.5f);
    }
    if (role == Role::BuildReverse) mul(FxType::ReverseReverb, 0.3f); // already reversed
    if (role == Role::Roll) w[(size_t)FxType::Stutter] = 0.f;           // forced separately
    return w;
}

EnvelopeChoice pickEnvelope(Role role, const Analysis& a, Rng& r) {
    EnvelopeChoice e;
    if (!r.chance(0.55f)) return e;
    using K = EnvelopeChoice::Kind;
    switch (role) {
    case Role::Build:
    case Role::BuildReverse: e.kind = r.pick<K>({K::Swell, K::LowpassOpen, K::HighpassRise}); break;
    case Role::Hit:
    case Role::Roll: e.kind = a.reg == Register::Low ? r.pick<K>({K::Dive, K::TapeStop, K::FadeOut}) : r.pick<K>({K::TapeStop, K::Dive, K::FadeOut}); break;
    case Role::Fill: e.kind = r.pick<K>({K::Swell, K::LowpassOpen, K::FadeOut, K::TapeStop}); break;
    case Role::Downlift: e.kind = r.pick<K>({K::FadeOut, K::Dive, K::TapeStop}); break;
    case Role::Pattern: e.kind = r.pick<K>({K::FadeOut, K::Dive}); break;
    }
    return e;
}

// Cut leading/trailing silence below -50 dB of the loudest sample.
Buffer trimSilence(const Buffer& b) {
    float pk = 0;
    for (int i = 0; i < b.size(); ++i) pk = std::max(pk, std::max(std::fabs(b.L[(size_t)i]), std::fabs(b.R[(size_t)i])));
    const float th = pk * 0.00316f;
    int a = 0, e = b.size();
    while (a < e && std::max(std::fabs(b.L[(size_t)a]), std::fabs(b.R[(size_t)a])) < th) ++a;
    while (e > a && std::max(std::fabs(b.L[(size_t)e - 1]), std::fabs(b.R[(size_t)e - 1])) < th) --e;
    Buffer out(e - a, b.sr);
    for (int i = a; i < e; ++i) { out.L[(size_t)(i - a)] = b.L[(size_t)i]; out.R[(size_t)(i - a)] = b.R[(size_t)i]; }
    return out;
}

// Start of the loudest 10 ms: the point that should land on the downbeat for a build-up.
int loudestPos(const Buffer& b);
// A downlifter starts at its loudest point: the quiet lead-in would land after the bar line otherwise.
void cutBeforeLoudest(Buffer& b) {
    const int from = std::max(0, loudestPos(b) - b.sr / 100);
    if (from <= 0) return;
    b.L.erase(b.L.begin(), b.L.begin() + from);
    b.R.erase(b.R.begin(), b.R.begin() + from);
}

int loudestPos(const Buffer& b) {
    const int w = std::max(1, b.sr / 100);
    float m = -1.f;
    int pos = std::max(0, b.size() - 1);
    for (int s = 0; s + w <= b.size(); s += w) {
        const float v = stereoRms(b, s, s + w);
        if (v > m) { m = v; pos = s + w; }
    }
    return std::min(pos, b.size());
}

// Bus guard: no 10 ms frame away from the junction may be louder than -1 dB under the junction itself.
// Returns the deepest cut applied (dB).
float alignPeakToJunction(Buffer& b, int junction) {
    const int h = std::max(1, b.sr / 100), nf = b.size() / h;
    if (nf < 4) return 0.f;
    const int kD = junction / h;
    auto inJunction = [&](int k) { return k >= kD - 1 && k <= kD + 2; };
    float deepest = 0.f;
    for (int iter = 0; iter < 4; ++iter) {
        float ref = 0.f;
        for (int k = std::max(0, kD - 1); k <= std::min(nf - 1, kD + 2); ++k) ref = std::max(ref, stereoRms(b, k * h, (k + 1) * h));
        if (ref <= 1e-9f) return deepest;
        const float lim = ref * 0.89f;
        std::vector<float> fg((size_t)nf, 1.f);
        bool any = false;
        for (int k = 0; k < nf; ++k) {
            if (inJunction(k)) continue;
            const float e = stereoRms(b, k * h, (k + 1) * h);
            if (e > lim) { fg[(size_t)k] = 0.97f * lim / e; any = true; }
        }
        if (!any) break;
        std::vector<float> sg((size_t)nf, 1.f);
        for (int k = 0; k < nf; ++k) {
            if (inJunction(k)) continue;
            float m = 1.f;
            for (int j = std::max(0, k - 2); j <= std::min(nf - 1, k + 2); ++j) m = std::min(m, fg[(size_t)j]);
            sg[(size_t)k] = m;
            deepest = std::max(deepest, -gainToDb(m));
        }
        for (int i = 0; i < b.size(); ++i) {
            const float fpos = (float)i / (float)h - 0.5f;
            const int k0 = std::clamp((int)std::floor(fpos), 0, nf - 1), k1 = std::min(k0 + 1, nf - 1);
            const float tt = std::clamp(fpos - (float)k0, 0.f, 1.f);
            const float g = sg[(size_t)k0] * (1.f - tt) + sg[(size_t)k1] * tt;
            b.L[(size_t)i] *= g; b.R[(size_t)i] *= g;
        }
    }
    return deepest;
}

// Where the loudest 10 ms frame sits relative to the junction, in ms.
float peakOffsetMs(const Buffer& b, int junction) {
    const int h = std::max(1, b.sr / 100);
    float m = -1.f;
    int best = 0;
    for (int s = 0; s + h <= b.size(); s += h) {
        const float v = stereoRms(b, s, s + h);
        if (v > m) { m = v; best = s; }
    }
    return 1000.f * (float)(best - junction) / (float)b.sr;
}

float loudestRms(const Buffer& b) {
    const int w = std::max(1, b.sr / 10);
    float m = 0;
    for (int s = 0; s < b.size(); s += w / 2) m = std::max(m, stereoRms(b, s, s + w));
    return m;
}

struct Track {
    int input = 0;
    Role role = Role::Fill;
    float transpose = 0, gainDb = 0, pan = 0, fitRatio = 1, extraHp = 0;
    bool fit = false;
    bool stretch = false; // fit by granular stretch (pitch kept) instead of varispeed
    bool coverage = false; // changed so the rise is not empty
    bool reverse = false;
    float patternStep = 0.5f; // beats between repeats (Pattern role)
    int patternCount = 0;
    float autoDepth = 0, autoCurve = 1; // gain automation towards / from the junction
    // volume shape (stage 3): how the layer moves into / out of the junction
    enum VolShape { Swell, Steps, GateRise, PumpRise, Decay, Cut, PumpFall, GateFall } vol = Swell;
    float volStep = 0.5f;   // beats: step / gate / pump grid
    float cutAfter = -1.f;  // beats after the junction where a build-up's ring-out is cut (-1 = rings on)
    std::vector<std::unique_ptr<Effect>> fx;
    struct Link { bool on = false, inverse = false; float lo = 0; };
    std::vector<Link> links; // per effect: wet amount follows the transition curve
    EnvelopeChoice env;
    Region region;
};

// Granular time stretch (pitch kept): 80 ms Hann grains at 75% overlap, read position advances
// 1/factor as fast, small random jitter so it does not sound like a buzz.
Buffer stretchOla(const Buffer& in, float factor, uint64_t seed) {
    const int g = in.sr * 8 / 100, hop = g / 4;
    const int n = (int)((float)in.size() * factor);
    Buffer out(n + g, in.sr);
    Rng r(seed);
    std::vector<float> norm((size_t)(n + g), 0.f);
    for (int o = 0; o < n; o += hop) {
        const double src = std::clamp((double)o / factor + r.range(-0.01f, 0.01f) * in.sr, 0.0, (double)std::max(0, in.size() - g));
        for (int i = 0; i < g; ++i) {
            const float w = 0.5f - 0.5f * std::cos(2.f * kPi * (float)i / (float)(g - 1));
            out.L[(size_t)(o + i)] += readCubic(in.L, src + i) * w;
            out.R[(size_t)(o + i)] += readCubic(in.R, src + i) * w;
            norm[(size_t)(o + i)] += w;
        }
    }
    for (int i = 0; i < n; ++i) { const float k = norm[(size_t)i] > 0.3f ? 1.f / norm[(size_t)i] : 1.f; out.L[(size_t)i] *= k; out.R[(size_t)i] *= k; }
    out.resize(n);
    return out;
}

std::string fmtf(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

} // namespace

RenderResult renderTransition(const std::vector<Source>& sources, const RenderRequest& req) {
    RenderResult res;
    if (sources.empty()) return res;
    Rng rng(req.seed);
    const int sr = sources[0].audio.sr;

    Timeline tl;
    tl.sr = sr;
    tl.bpm = req.bpm;
    tl.downbeat = tl.beats(req.lengthBeats);

    // ---- which inputs take part (sometimes the dice rest one, never below 2)
    std::vector<int> used;
    for (int i = 0; i < (int)sources.size() && i < kMaxLayers; ++i) used.push_back(i);
    if (used.size() >= 3 && rng.chance(0.3f)) used.erase(used.begin() + rng.irange(0, (int)used.size() - 1));

    // ---- plan each track
    std::vector<Track> tracks;
    bool lowHitTaken = false;
    // "breath" before the downbeat; build-ups then peak right before the gap
    const bool gap = rng.chance(0.3f);
    const float gapBeats = rng.pick<float>({0.125f, 0.25f});
    const int junctionBuild = tl.downbeat - (gap ? tl.beats(gapBeats) : 0);
    for (int idx : used) {
        const Analysis& a = sources[(size_t)idx].analysis;
        Track t;
        t.input = idx;
        t.role = pickRole(a, rng, req.arrangeOnly);
        if (t.role == Role::Fill) t.role = Role::Build; // every layer is anchored to the junction
        t.reverse = t.role == Role::BuildReverse || (t.role == Role::Downlift && a.shape == Shape::Rise);
        if (t.role == Role::Pattern) {
            t.patternStep = rng.pick<float>({0.5f, 0.25f, 1.f / 3.f, 0.125f});
            const int maxCount = std::max(1, (int)std::floor(std::min(req.lengthBeats, 2.f) / t.patternStep + 1e-3f));
            t.patternCount = rng.irange(std::min(2, maxCount), std::min(8, maxCount));
        }
        const bool low = a.reg == Register::Low;
        if (a.kind == Kind::Tonal || a.kind == Kind::Percussive)
            t.transpose = low ? rng.pick<float>({0.f, 0.f, 0.f, 0.f, 12.f}) : rng.pick<float>({0.f, 0.f, 12.f, -12.f, 7.f, -5.f});
        else
            t.transpose = rng.pick<float>({0.f, 0.f, 0.f, 5.f, -5.f, 12.f, -12.f});
        t.gainDb = low ? rng.range(-3.f, 0.f) : rng.range(-6.f, -1.f);
        t.pan = low ? 0.f : rng.range(-0.4f, 0.4f);
        if (t.role == Role::Downlift) t.gainDb -= 2.f; // a tail sits under the impact
        if (t.role == Role::Build || t.role == Role::BuildReverse) { t.autoDepth = rng.range(6.f, 12.f); t.autoCurve = rng.range(1.5f, 3.f); }
        else if (t.role == Role::Downlift) { t.autoDepth = rng.range(4.f, 8.f); t.autoCurve = 0.7f; }
        else { t.autoDepth = rng.range(2.f, 5.f); t.autoCurve = 0.7f; }
        t.fit = (t.role == Role::Build) && a.rising && rng.chance(0.5f);
        if (low) {
            const bool isHit = t.role == Role::Hit || t.role == Role::Roll || t.role == Role::Pattern;
            if (isHit && !lowHitTaken) lowHitTaken = true;
            else t.extraHp = 150.f; // keep one low-end owner
        }
        tracks.push_back(std::move(t));
    }

    // ---- coverage: the user-set rise must not be mostly silence. If every build-up is much shorter than
    // the rise, slow the longest one down to fill it, or (too short even for that) repeat it on the grid.
    {
        auto prep = [&](const Track& t) {
            const Source& s = sources[(size_t)t.input];
            Buffer src = trimSilence(s.audio);
            if (src.size() == 0) src = s.audio;
            src = varispeed(src, semisToRatio(t.transpose));
            if (t.reverse) reverseInPlace(src);
            return src;
        };
        int earliest = tl.downbeat, best = -1, bestAnchor = 0;
        for (size_t i = 0; i < tracks.size(); ++i) {
            const Track& t = tracks[i];
            if (t.role == Role::Pattern) earliest = std::min(earliest, tl.downbeat - tl.beats(t.patternStep * (float)t.patternCount));
            if (t.role != Role::Build && t.role != Role::BuildReverse) continue;
            const int anchor = loudestPos(prep(t));
            earliest = std::min(earliest, junctionBuild - anchor);
            if (anchor > bestAnchor) { bestAnchor = anchor; best = (int)i; }
        }
        if (earliest > tl.downbeat * 2 / 5 && best >= 0 && bestAnchor > 0) {
            Track& t = tracks[(size_t)best];
            const float ratio = (float)bestAnchor / (float)std::max(1, junctionBuild);
            t.fit = true; t.coverage = true;
            // slowing down by more than ~1.6x muddies the sound: stretch with grains instead (pitch kept)
            t.stretch = ratio < 0.6f && !(ratio >= 0.4f && rng.chance(0.4f));
        }
    }

    // ---- place sources on the timeline
    int tail = sr * 5 / 2;
    for (auto& t : tracks) {
        const Source& s = sources[(size_t)t.input];
        Buffer src = trimSilence(s.audio);
        if (src.size() == 0) src = s.audio;
        src = varispeed(src, semisToRatio(t.transpose));
        if (t.reverse) reverseInPlace(src);
        if (t.role == Role::Downlift) cutBeforeLoudest(src);
        if (t.fit && src.size() > 0) {
            const float ratio = (float)loudestPos(src) / (float)std::max(1, junctionBuild);
            if (ratio > (t.coverage ? 0.05f : 0.5f) && ratio < 4.f) { t.fitRatio = ratio; src = t.stretch ? stretchOla(src, 1.f / ratio, 7) : varispeed(src, ratio); }
            else t.fit = false;
        }
        int start = 0;
        int len = src.size();
        switch (t.role) {
        case Role::Build:
        case Role::BuildReverse: {
            // the loudest point lands on the downbeat; whatever follows it rings on past the bar line
            const int anchor = loudestPos(src);
            start = junctionBuild - anchor;
            if (start < 0) { len += start; start = 0; }
            break;
        }
        case Role::Hit:
        case Role::Roll:
        case Role::Downlift: start = tl.downbeat; break;
        case Role::Pattern: start = tl.downbeat - tl.beats(t.patternStep * (float)t.patternCount); break;
        case Role::Fill: start = 0; break;
        }
        if (t.role == Role::Pattern) len = tl.downbeat - start + src.size();
        tail = std::max(tail, std::min(start + len - tl.downbeat + sr / 2, sr * 8));
        if (t.role != Role::Build && t.role != Role::BuildReverse) tail = std::max(tail, start + len - tl.downbeat + sr * 3 / 2);
        t.region = {start, start + len};
    }
    tail = std::min(tail, sr * 8);
    const int fallLen = req.fallBeats > 0 ? tl.beats(req.fallBeats) : 0;
    tl.length = tl.downbeat + (fallLen > 0 ? fallLen : tail);
    // the transition curve (like a single macro knob): 0 -> 1 over the rise, 1 -> 0 over the fall
    const int motionFall = fallLen > 0 ? fallLen : tl.beat();
    auto motion = [&](int i) {
        if (i < tl.downbeat) return std::pow((float)i / (float)std::max(1, tl.downbeat), 1.5f);
        return std::max(0.f, 1.f - (float)(i - tl.downbeat) / (float)motionFall);
    };

    // ---- render each track
    Buffer mix(tl.length, sr);
    std::string trackReport;
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        Track& t = tracks[ti];
        const Source& s = sources[(size_t)t.input];
        const Analysis& a = s.analysis;
        Buffer src = trimSilence(s.audio);
        if (src.size() == 0) src = s.audio;
        src = varispeed(src, semisToRatio(t.transpose));
        if (t.reverse) reverseInPlace(src);
        if (t.role == Role::Downlift) cutBeforeLoudest(src);
        if (t.fit) src = t.stretch ? stretchOla(src, 1.f / t.fitRatio, 7) : varispeed(src, t.fitRatio);
        // gain staging: loudest 100 ms at -16 dBFS, then the dice gain
        const float lr = loudestRms(src);
        const float g = lr > 1e-6f ? dbToGain(-16.f + t.gainDb) / lr : 1.f;
        const int offset = std::max(0, src.size() - (t.region.end - t.region.start));
        const bool takeTail = t.role == Role::Build || t.role == Role::BuildReverse;
        Buffer tb(tl.length, sr);
        const int fadeIn = takeTail && offset > 0 ? sr / 50 : 0;
        if (t.role == Role::Pattern) {
            // repeats on the grid before the downbeat (each cut to one step, getting louder), then the full hit
            const int step = std::max(64, tl.beats(t.patternStep)), fade = sr / 300;
            for (int k = t.patternCount; k >= 0; --k) {
                const int at = tl.downbeat - k * step;
                const int n = k == 0 ? src.size() : std::min(src.size(), step);
                const float gk = k == 0 ? 1.f : 0.35f + 0.5f * (float)(t.patternCount - k) / (float)std::max(1, t.patternCount);
                for (int i = 0; i < n && at + i < tl.length; ++i) {
                    if (at + i < 0) continue;
                    float w = 1.f;
                    if (k > 0 && i > n - fade) w = (float)(n - i) / (float)fade;
                    tb.L[(size_t)(at + i)] += src.L[(size_t)i] * g * gk * w;
                    tb.R[(size_t)(at + i)] += src.R[(size_t)i] * g * gk * w;
                }
            }
        } else {
            for (int i = 0; t.region.start + i < tl.length && i < t.region.end - t.region.start; ++i) {
                const int si = (takeTail ? offset : 0) + i;
                if (si >= src.size()) break;
                const float w = (fadeIn > 0 && i < fadeIn) ? (float)i / (float)fadeIn : 1.f;
                tb.L[(size_t)(t.region.start + i)] = src.L[(size_t)si] * g * w;
                tb.R[(size_t)(t.region.start + i)] = src.R[(size_t)si] * g * w;
            }
        }
        if (t.extraHp > 0 && !req.arrangeOnly) {
            Biquad h[2];
            h[0].set(Biquad::HP, t.extraHp, 0.707f, 0, (float)sr); h[1] = h[0];
            for (int i = 0; i < tl.length; ++i) { tb.L[(size_t)i] = h[0].process(tb.L[(size_t)i]); tb.R[(size_t)i] = h[1].process(tb.R[(size_t)i]); }
        }

        // effects: up to 3, no repeats, random order
        FxLimits lim;
        lim.lowRegister = a.reg == Register::Low;
        lim.maxReverbSec = lim.lowRegister ? 1.5f : 6.f;
        lim.maxDriveDb = lim.lowRegister ? 14.f : 24.f;
        lim.allowPitch = !lim.lowRegister;
        if (a.kind == Kind::Tonal && a.f0 > 0) lim.rootHz = a.f0;
        std::vector<FxType> chosen;
        if (t.role == Role::Roll) chosen.push_back(FxType::Stutter);
        const int count = req.arrangeOnly ? 0 : std::min(kMaxFxPerTrack, (int)chosen.size() + 1 + rng.weighted({0.3f, 0.4f, 0.3f}));
        auto w = fxWeights(a, req.style, t.role);
        w[(size_t)FxType::Shaper] = 2.5f * std::clamp(req.shaperAmount, 0.f, 1.f);
        while ((int)chosen.size() < count) {
            for (FxType c : chosen) w[(size_t)c] = 0.f;
            float sum = 0;
            for (float x : w) sum += x;
            if (sum <= 0) break;
            chosen.push_back((FxType)rng.weighted(w));
        }
        // shuffle order (Fisher-Yates); a forced roll stays first so it repeats the dry attack
        const size_t first = t.role == Role::Roll ? 1 : 0;
        for (size_t i = chosen.size(); i > first + 1; --i) std::swap(chosen[i - 1], chosen[first + (size_t)rng.irange(0, (int)(i - 1 - first))]);
        for (FxType c : chosen) t.fx.push_back(makeEffect(c, rng, lim, tl, t.region));
        Region fxRegion = t.region;
        if (t.role == Role::Roll) fxRegion.start = tl.downbeat;
        for (auto& e : t.fx) {
            Track::Link lk;
            if (rng.chance(0.6f)) { lk.on = true; lk.inverse = rng.chance(0.25f); lk.lo = rng.range(0.f, 0.4f); }
            t.links.push_back(lk);
            if (!lk.on) { e->process(tb, tl, fxRegion); continue; }
            const Buffer dry = tb;
            e->process(tb, tl, fxRegion);
            for (int i = 0; i < tl.length; ++i) {
                const float m = motion(i);
                const float w = lk.inverse ? 1.f - (1.f - lk.lo) * m : lk.lo + (1.f - lk.lo) * m;
                tb.L[(size_t)i] = dry.L[(size_t)i] + (tb.L[(size_t)i] - dry.L[(size_t)i]) * w;
                tb.R[(size_t)i] = dry.R[(size_t)i] + (tb.R[(size_t)i] - dry.R[(size_t)i]) * w;
            }
        }
        if (!req.arrangeOnly) t.env = pickEnvelope(t.role, a, rng);
        Region envRegion = t.region;
        if (t.role == Role::Build || t.role == Role::BuildReverse) envRegion = {std::max(0, std::min(t.region.start, tl.downbeat - tl.beats(req.lengthBeats))), tl.downbeat};
        if (t.role == Role::Hit || t.role == Role::Roll || t.role == Role::Pattern || t.role == Role::Downlift) envRegion = {tl.downbeat, std::min(tl.length, std::max(t.region.end, tl.downbeat + sr / 2))};
        applyEnvelope(t.env, tb, tl, envRegion);

        // volume shape: build-ups move into their peak at the junction; hits/tails move away from it.
        // Depths are big on purpose (the bus compressor would flatten small moves).
        {
            const bool build = t.role == Role::Build || t.role == Role::BuildReverse;
            if (req.arrangeOnly) {
                t.vol = build ? Track::Swell : Track::Decay;
            } else if (build) {
                t.vol = (Track::VolShape)(Track::Swell + rng.weighted({1.f, 0.f, 0.f, 0.f})); // rhythmic shapes off for now (user 11/4)
                t.autoDepth = t.vol == Track::Swell ? rng.range(24.f, 40.f) : rng.range(14.f, 24.f);
                t.volStep = req.lengthBeats >= 4.f ? rng.pick<float>({1.f, 0.5f}) : req.lengthBeats >= 1.f ? rng.pick<float>({0.5f, 0.25f}) : 0.125f;
                if (t.vol == Track::GateRise) t.volStep = std::min(t.volStep, 0.25f);
                if (rng.chance(0.5f)) t.cutAfter = rng.pick<float>({0.25f, 0.5f, 1.f});
            } else {
                const bool low = a.reg == Register::Low;
                t.vol = low ? (Track::VolShape)(Track::Decay + rng.weighted({3.f, 2.f, 0.f}))
                            : (Track::VolShape)(Track::Decay + rng.weighted({3.f, 2.f, 0.f, 0.f}));
                t.autoDepth = rng.range(14.f, 30.f);
                t.autoCurve = rng.range(0.5f, 1.2f);
                t.volStep = t.vol == Track::Cut ? rng.pick<float>({0.25f, 0.5f, 1.f}) : rng.pick<float>({0.5f, 0.25f});
                if (t.vol == Track::GateFall) t.volStep = rng.pick<float>({0.25f, 0.125f});
            }
            const int a0 = t.region.start, j = build ? junctionBuild : tl.downbeat;
            const int fall = req.fallBeats > 0 ? tl.beats(req.fallBeats) : (t.role == Role::Downlift ? tl.beats(2.f) : tl.beat());
            const int step = std::max(64, tl.beats(t.volStep));
            const float ramp = (float)(sr / 200); // 5 ms edges, no clicks
            auto edge = [&](float x, float open) { // 1 inside [0, open), smooth 5 ms edges, 0 after
                return std::clamp(std::min(x, open - x) / ramp + 0.5f, 0.f, 1.f);
            };
            const int cutAt = t.cutAfter > 0 ? tl.downbeat + tl.beats(t.cutAfter) : -1;
            for (int i = 0; i < tl.length; ++i) {
                float g = 1.f;
                if (build && i < j && j > a0) {
                    const float p = std::clamp((float)(i - a0) / (float)(j - a0), 0.f, 1.f); // 0 at the start, 1 at the junction
                    const int fromJ = j - i;                                                 // grid is counted back from the junction
                    const float ph = (float)(fromJ % step);
                    switch (t.vol) {
                    case Track::Swell: g = p < 0.02f ? p / 0.02f * dbToGain(-t.autoDepth) : dbToGain(-t.autoDepth * std::pow(1.f - p, t.autoCurve)); break;
                    case Track::Steps: {
                        const int nSteps = std::max(2, (j - a0) / step + 1), k = nSteps - 1 - fromJ / step; // 0 .. nSteps-1
                        g = dbToGain(-t.autoDepth * (1.f - (float)std::max(0, k) / (float)(nSteps - 1)));
                        break;
                    }
                    case Track::GateRise: {
                        const float duty = 0.3f + 0.7f * std::pow(p, 1.5f);
                        const float x = (float)step - ph; // position inside the step, 0 = step start
                        g = (fromJ < step ? 1.f : edge(x, duty * (float)step)) * dbToGain(-0.5f * t.autoDepth * (1.f - p));
                        break;
                    }
                    case Track::PumpRise: {
                        const float x = 1.f - ph / (float)step; // 0 at the grid line, rising through the step
                        const float dip = std::exp(-x * 5.f) * (1.f - p);                       // duck right after each grid line
                        g = (1.f - 0.92f * dip) * dbToGain(-0.5f * t.autoDepth * (1.f - p));
                        break;
                    }
                    default: break;
                    }
                }
                if (build && cutAt > 0 && i > cutAt) g *= std::clamp(1.f - (float)(i - cutAt) / ramp, 0.f, 1.f);
                if (!build && i > j) {
                    const int d = i - j;
                    const float q = std::min(1.f, (float)d / (float)fall);
                    switch (t.vol) {
                    case Track::Decay: g = dbToGain(-t.autoDepth * std::pow(q, t.autoCurve)); break;
                    case Track::Cut: g = edge((float)d + ramp, (float)tl.beats(t.volStep)); break;
                    case Track::PumpFall: {
                        const float x = (float)(d % step) / (float)step;
                        g = (d < step ? 1.f : 1.f - 0.9f * std::exp(-x * 6.f)) * dbToGain(-0.6f * t.autoDepth * q);
                        break;
                    }
                    case Track::GateFall: g = (d < step ? 1.f : edge((float)(d % step), 0.5f * (float)step)) * dbToGain(-0.6f * t.autoDepth * q); break;
                    default: break;
                    }
                }
                tb.L[(size_t)i] *= g; tb.R[(size_t)i] *= g;
            }
        }
        const float gl = std::sqrt(0.5f * (1 - t.pan)) * 1.41421f, gr = std::sqrt(0.5f * (1 + t.pan)) * 1.41421f;
        for (int i = 0; i < tl.length; ++i) { mix.L[(size_t)i] += tb.L[(size_t)i] * gl; mix.R[(size_t)i] += tb.R[(size_t)i] * gr; }

        // report
        trackReport += "- **第 " + std::to_string(ti + 1) + " 轨：" + a.name + "**（识别：" + a.kindName() + "，响度形状" + a.shapeName() + "，" + a.regName() + "音域";
        if (a.f0 > 0) trackReport += "，" + a.noteName();
        trackReport += "）\n  - 角色：" + std::string(roleName(t.role));
        if (t.reverse && t.role != Role::BuildReverse) trackReport += "，倒放";
        if (t.coverage) trackReport += "（素材比上升段短太多，" + std::string(t.stretch ? "用粒子拉伸铺满上升段，音高不变" : "放慢铺满上升段") + "）";
        if (t.role == Role::Pattern) trackReport += "（每 " + beatsLabel(t.patternStep * 1.f) + " 一下，共 " + std::to_string(t.patternCount) + " 下）";
        if (t.transpose != 0) trackReport += "，变调 " + fmtf("%+.0f", t.transpose) + " 半音";
        if (t.fit) trackReport += t.stretch ? "，粒子拉伸 " + fmtf("%.1f", 1.f / t.fitRatio) + " 倍长" : "，拉伸到过渡长度（" + fmtf("%.2f", t.fitRatio) + " 倍速）";
        trackReport += "，音量 " + fmtf("%.1f", t.gainDb) + " dB";
        if (t.pan != 0) trackReport += "，声像 " + fmtf("%+.2f", t.pan);
        if (t.extraHp > 0) trackReport += "，避让低频高通 150Hz";
        {
            const std::string st = beatsLabel(t.volStep);
            std::string v;
            switch (t.vol) {
            case Track::Swell: v = "渐强：从 -" + fmtf("%.0f", t.autoDepth) + " dB 推到衔接点 0 dB（曲线 " + fmtf("%.1f", t.autoCurve) + "）"; break;
            case Track::Steps: v = "阶梯推起：每 " + st + " 升一级，从 -" + fmtf("%.0f", t.autoDepth) + " dB 到衔接点 0 dB"; break;
            case Track::GateRise: v = "门限推起：每 " + st + " 开关一次，开的时间越来越长，最后 " + st + " 全开"; break;
            case Track::PumpRise: v = "泵感推起：每 " + st + " 压一下，越接近衔接点压得越轻"; break;
            case Track::Decay: v = "衔接点 0 dB，之后衰减 " + fmtf("%.0f", t.autoDepth) + " dB"; break;
            case Track::Cut: v = "衔接点 0 dB，" + st + " 后直接断掉"; break;
            case Track::PumpFall: v = "衔接点 0 dB，之后每 " + st + " 泵一下并往下走"; break;
            case Track::GateFall: v = "衔接点 0 dB，之后每 " + st + " 切一刀并往下走"; break;
            }
            if (t.cutAfter > 0) v += "；强拍后 " + beatsLabel(t.cutAfter) + " 切断余音";
            trackReport += "\n  - 音量包络：" + v;
        }
        if (!req.arrangeOnly) trackReport += "\n  - 效果链：";
        for (size_t k = 0; k < t.fx.size(); ++k) {
            trackReport += (k ? " → " : "") + t.fx[k]->describe();
            if (k < t.links.size() && t.links[k].on)
                trackReport += t.links[k].inverse ? "〔效果量随过渡曲线反向：衔接点最干，" + fmtf("%.0f", t.links[k].lo * 100) + "%〕"
                                                  : "〔效果量随过渡曲线：" + fmtf("%.0f", t.links[k].lo * 100) + "% → 衔接点 100%〕";
        }
        if (t.env.kind != EnvelopeChoice::None) trackReport += "\n  - 包络：" + t.env.describe();
        trackReport += "\n";
    }
    if (tracks.size() < sources.size()) {
        for (size_t i = 0; i < sources.size(); ++i) {
            bool in = false;
            for (auto& t : tracks) in = in || t.input == (int)i;
            if (!in) trackReport += "- （本次骰子让 " + sources[i].analysis.name + " 休息）\n";
        }
    }

    // ---- "breath": a short gap right before the downbeat
    if (gap) {
        const int a = tl.downbeat - tl.beats(gapBeats), f = sr / 500;
        for (int i = std::max(0, a - f); i < tl.downbeat; ++i) {
            const float g = i < a ? (float)(a - i) / (float)f : 0.f;
            mix.L[(size_t)i] *= g; mix.R[(size_t)i] *= g;
        }
    }

    // ---- bus
    BusSettings bus;
    bus.multiband = req.fallBeats > 0 ? true : rng.chance(0.6f);
    bus.mbRatio = rng.range(2.f, 4.f);
    if (req.fallBeats > 0) {
        bus.compAmount = req.compAmount ? std::clamp(*req.compAmount, 0.f, 1.f) : rng.range(0.4f, 0.9f);
        bus.mbRatio = 2.f + 6.f * bus.compAmount;
    }
    bus.tiltLowDb = rng.range(-2.f, 2.f);
    bus.tiltHighDb = rng.range(-2.f, 2.f);
    if (req.style == Style::Ambient) { bus.spaceMix = rng.range(0.15f, 0.4f); }
    else if (rng.chance(0.6f)) bus.spaceMix = rng.range(0.f, 0.25f);
    bus.spaceDecay = rng.range(0.6f, 2.5f);
    if (!req.arrangeOnly) processBus(mix, bus, tl);

    // ---- output options
    OutputOptions out;
    if (req.forcedOutput) {
        out = *req.forcedOutput;
        if (out.gate && out.gatePattern == 0xFFFFu) { // gate switched on by the user: the dice still pick the pattern
            out.gateStepBeats = rng.pick<float>({0.25f, 0.125f});
            const float dens = rng.range(0.5f, 0.8f);
            out.gatePattern = 1u;
            for (int k = 1; k < 16; ++k) if (rng.chance(dens)) out.gatePattern |= 1u << k;
        }
    }
    else {
        const float gp = req.style == Style::Dirty || req.style == Style::Fragment ? 0.6f : (req.style == Style::Ambient ? 0.15f : 0.3f);
        out.gate = rng.chance(gp) && false; // gate comes back later as a user toggle (user 11/4)
        out.gateStepBeats = rng.pick<float>({0.25f, 0.125f});
        const float dens = rng.range(0.5f, 0.8f);
        out.gatePattern = 0;
        for (int k = 0; k < 16; ++k) if (rng.chance(dens)) out.gatePattern |= 1u << k;
        out.gatePattern |= 1u; // first step always open
        out.finalReverb = rng.chance(req.style == Style::Ambient ? 0.5f : 0.25f);
        out.finalReverbMix = rng.range(0.15f, 0.35f);
        if (rng.chance(0.3f)) out.finalEq = rng.chance(0.7f) ? OutputOptions::Telephone : OutputOptions::Darken;
    }
    if (req.arrangeOnly) out = OutputOptions{};
    else processOutput(mix, out, tl);
    // bus automation: whole mix rises into the junction
    const float busDepth = rng.range(3.f, 6.f);
    for (int i = 0; i < junctionBuild && i < tl.length; ++i) {
        const float g = dbToGain(-busDepth * (1.f - (float)i / (float)std::max(1, junctionBuild)));
        mix.L[(size_t)i] *= g; mix.R[(size_t)i] *= g;
    }
    // stage 2 timing rule: hold right after the junction, then fall to silence exactly at the end
    if (fallLen > 0) {
        for (int i = tl.downbeat; i < tl.length; ++i) {
            const float p = (float)(i - tl.downbeat) / (float)fallLen;
            const float g = p < 0.25f ? 1.f : std::pow(std::max(0.f, 1.f - (p - 0.25f) / 0.75f), 1.6f);
            mix.L[(size_t)i] *= g; mix.R[(size_t)i] *= g;
        }
        const int fi = std::min(tl.downbeat, sr / 200); // 5 ms fade-in at the very start
        for (int i = 0; i < fi; ++i) { const float g = (float)i / (float)fi; mix.L[(size_t)i] *= g; mix.R[(size_t)i] *= g; }
    }
    float guardDb = alignPeakToJunction(mix, tl.downbeat);
    finalizeLoudness(mix, tl, fallLen > 0);
    guardDb = std::max(guardDb, alignPeakToJunction(mix, tl.downbeat)); // limiter may have flattened the junction
    const float offMs = peakOffsetMs(mix, tl.downbeat);

    res.audio = std::move(mix);
    res.downbeatSample = tl.downbeat;
    std::string r;
    if (fallLen > 0)
        r += "- 时值规则：上升 " + beatsLabel(req.lengthBeats) + "（" + fmtf("%.3f", (double)tl.downbeat / sr) + " 秒）+ 下降 " + beatsLabel(req.fallBeats) + "（" +
             fmtf("%.3f", (double)fallLen / sr) + " 秒），文件总长精确 " + std::to_string(res.audio.size()) + " 采样\n";
    r += "- 种子 " + std::to_string(req.seed) + (req.arrangeOnly ? std::string() : std::string("，风格骰子：") + styleName(req.style)) + "，时值 " + beatsLabel(req.lengthBeats) + " @ " +
         fmtf("%.0f", req.bpm) + " BPM（强拍在 " + fmtf("%.3f", (double)tl.downbeat / sr) + " 秒处）\n";
    r += "- 最响处离衔接点：" + fmtf("%+.0f", offMs) + " ms（总线峰值保护最多压了 " + fmtf("%.1f", guardDb) + " dB）\n";
    r += trackReport;
    r += "- 总线自动化：整体音量从 " + fmtf("-%.0f", busDepth) + " dB 推到衔接点\n";
    if (gap) r += "- 强拍前留气口：" + std::string(gapBeats == 0.125f ? "1/32" : "1/16") + " 静音\n";
    if (req.arrangeOnly) r += "- 只编排：没有效果器、包络和总线染色，只做统一响度和限幅\n";
    else {
        r += "- 总线：" + bus.describe() + "\n";
        r += "- 输出选项：" + out.describe() + "\n";
    }
    res.report = r;
    return res;
}

} // namespace lb
