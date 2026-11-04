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
    //               Reverb RevRev Delay  Eq   Dist Crystal Stutter Gran
    std::vector<float> w{1.0f, 0.7f, 1.0f, 1.0f, 0.6f, 0.6f, 0.5f, 0.5f};
    auto mul = [&](FxType t, float m) { w[(size_t)t] *= m; };
    switch (st) {
    case Style::Dirty: mul(FxType::Distortion, 3.f); mul(FxType::Stutter, 1.5f); mul(FxType::Reverb, 0.5f); mul(FxType::ReverseReverb, 0.5f); break;
    case Style::Ambient: mul(FxType::Reverb, 2.f); mul(FxType::ReverseReverb, 2.f); mul(FxType::Delay, 1.5f); mul(FxType::Granular, 1.2f); mul(FxType::Distortion, 0.2f); mul(FxType::Stutter, 0.3f); break;
    case Style::Fragment: mul(FxType::Granular, 3.f); mul(FxType::Stutter, 2.5f); mul(FxType::Crystal, 2.f); mul(FxType::Reverb, 0.6f); break;
    case Style::Neutral: break;
    }
    switch (a.kind) {
    case Kind::Tonal: mul(FxType::Crystal, 1.5f); mul(FxType::ReverseReverb, 1.5f); break;
    case Kind::FX: mul(FxType::Eq, 1.5f); mul(FxType::Granular, 1.3f); mul(FxType::Crystal, 0.6f); break;
    case Kind::Percussive: mul(FxType::Stutter, 2.f); mul(FxType::Reverb, 0.7f); break;
    case Kind::Loop: mul(FxType::Stutter, 2.f); mul(FxType::Granular, 1.5f); break;
    }
    if (a.reg == Register::Low) {
        mul(FxType::Reverb, 0.4f); mul(FxType::ReverseReverb, 0.4f); mul(FxType::Delay, 0.5f);
        mul(FxType::Granular, 0.3f); mul(FxType::Crystal, 0.3f); mul(FxType::Distortion, 1.5f);
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
    case Role::BuildReverse: e.kind = r.pick<K>({K::Swell, K::LowpassOpen, K::HighpassRise, K::Pump}); break;
    case Role::Hit:
    case Role::Roll: e.kind = a.reg == Register::Low ? r.pick<K>({K::Dive, K::TapeStop, K::FadeOut}) : r.pick<K>({K::TapeStop, K::Dive, K::FadeOut, K::Pump}); break;
    case Role::Fill: e.kind = r.pick<K>({K::Swell, K::LowpassOpen, K::Pump, K::FadeOut, K::TapeStop}); break;
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
    bool reverse = false;
    float patternStep = 0.5f; // beats between repeats (Pattern role)
    int patternCount = 0;
    std::vector<std::unique_ptr<Effect>> fx;
    EnvelopeChoice env;
    Region region;
};

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
    for (int idx : used) {
        const Analysis& a = sources[(size_t)idx].analysis;
        Track t;
        t.input = idx;
        t.role = pickRole(a, rng, req.arrangeOnly);
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
        t.fit = (t.role == Role::Build) && a.rising && rng.chance(0.5f);
        if (low) {
            const bool isHit = t.role == Role::Hit || t.role == Role::Roll || t.role == Role::Pattern;
            if (isHit && !lowHitTaken) lowHitTaken = true;
            else t.extraHp = 150.f; // keep one low-end owner
        }
        tracks.push_back(std::move(t));
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
            const float ratio = (float)loudestPos(src) / (float)std::max(1, tl.downbeat);
            if (ratio > 0.5f && ratio < 4.f) { t.fitRatio = ratio; src = varispeed(src, ratio); }
            else t.fit = false;
        }
        int start = 0;
        int len = src.size();
        switch (t.role) {
        case Role::Build:
        case Role::BuildReverse: {
            // the loudest point lands on the downbeat; whatever follows it rings on past the bar line
            const int anchor = loudestPos(src);
            start = tl.downbeat - anchor;
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
    tl.length = tl.downbeat + tail;

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
        if (t.fit) src = varispeed(src, t.fitRatio);
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
        std::vector<FxType> chosen;
        if (t.role == Role::Roll) chosen.push_back(FxType::Stutter);
        const int count = req.arrangeOnly ? 0 : std::min(kMaxFxPerTrack, (int)chosen.size() + 1 + rng.weighted({0.3f, 0.4f, 0.3f}));
        auto w = fxWeights(a, req.style, t.role);
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
        for (auto& e : t.fx) e->process(tb, tl, fxRegion);
        if (!req.arrangeOnly) t.env = pickEnvelope(t.role, a, rng);
        Region envRegion = t.region;
        if (t.role == Role::Build || t.role == Role::BuildReverse) envRegion = {std::max(0, std::min(t.region.start, tl.downbeat - tl.beats(req.lengthBeats))), tl.downbeat};
        if (t.role == Role::Hit || t.role == Role::Roll || t.role == Role::Pattern || t.role == Role::Downlift) envRegion = {tl.downbeat, std::min(tl.length, std::max(t.region.end, tl.downbeat + sr / 2))};
        applyEnvelope(t.env, tb, tl, envRegion);

        const float gl = std::sqrt(0.5f * (1 - t.pan)) * 1.41421f, gr = std::sqrt(0.5f * (1 + t.pan)) * 1.41421f;
        for (int i = 0; i < tl.length; ++i) { mix.L[(size_t)i] += tb.L[(size_t)i] * gl; mix.R[(size_t)i] += tb.R[(size_t)i] * gr; }

        // report
        trackReport += "- **第 " + std::to_string(ti + 1) + " 轨：" + a.name + "**（识别：" + a.kindName() + "，响度形状" + a.shapeName() + "，" + a.regName() + "音域";
        if (a.f0 > 0) trackReport += "，" + a.noteName();
        trackReport += "）\n  - 角色：" + std::string(roleName(t.role));
        if (t.reverse && t.role != Role::BuildReverse) trackReport += "，倒放";
        if (t.role == Role::Pattern) trackReport += "（每 " + beatsLabel(t.patternStep * 1.f) + " 一下，共 " + std::to_string(t.patternCount) + " 下）";
        if (t.transpose != 0) trackReport += "，变调 " + fmtf("%+.0f", t.transpose) + " 半音";
        if (t.fit) trackReport += "，拉伸到过渡长度（" + fmtf("%.2f", t.fitRatio) + " 倍速）";
        trackReport += "，音量 " + fmtf("%.1f", t.gainDb) + " dB";
        if (t.pan != 0) trackReport += "，声像 " + fmtf("%+.2f", t.pan);
        if (t.extraHp > 0) trackReport += "，避让低频高通 150Hz";
        if (!req.arrangeOnly) trackReport += "\n  - 效果链：";
        for (size_t k = 0; k < t.fx.size(); ++k) trackReport += (k ? " → " : "") + t.fx[k]->describe();
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
    const bool gap = rng.chance(0.3f);
    const float gapBeats = rng.pick<float>({0.125f, 0.25f});
    if (gap) {
        const int a = tl.downbeat - tl.beats(gapBeats), f = sr / 500;
        for (int i = std::max(0, a - f); i < tl.downbeat; ++i) {
            const float g = i < a ? (float)(a - i) / (float)f : 0.f;
            mix.L[(size_t)i] *= g; mix.R[(size_t)i] *= g;
        }
    }

    // ---- bus
    BusSettings bus;
    bus.multiband = rng.chance(0.6f);
    bus.mbRatio = rng.range(2.f, 4.f);
    bus.tiltLowDb = rng.range(-2.f, 2.f);
    bus.tiltHighDb = rng.range(-2.f, 2.f);
    if (req.style == Style::Ambient) { bus.spaceMix = rng.range(0.15f, 0.4f); }
    else if (rng.chance(0.6f)) bus.spaceMix = rng.range(0.f, 0.25f);
    bus.spaceDecay = rng.range(0.6f, 2.5f);
    if (!req.arrangeOnly) processBus(mix, bus, tl);

    // ---- output options
    OutputOptions out;
    if (req.forcedOutput) out = *req.forcedOutput;
    else {
        const float gp = req.style == Style::Dirty || req.style == Style::Fragment ? 0.6f : (req.style == Style::Ambient ? 0.15f : 0.3f);
        out.gate = rng.chance(gp);
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
    finalizeLoudness(mix, tl);

    res.audio = std::move(mix);
    res.downbeatSample = tl.downbeat;
    std::string r;
    r += "- 种子 " + std::to_string(req.seed) + (req.arrangeOnly ? std::string() : std::string("，风格骰子：") + styleName(req.style)) + "，时值 " + beatsLabel(req.lengthBeats) + " @ " +
         fmtf("%.0f", req.bpm) + " BPM（强拍在 " + fmtf("%.3f", (double)tl.downbeat / sr) + " 秒处）\n";
    r += trackReport;
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
