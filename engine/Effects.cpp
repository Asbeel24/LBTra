#include "Effects.h"
#include <cstdarg>
#include <cstdio>

namespace lb {

namespace {

#if defined(__GNUC__)
std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
#endif
std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string beatsName(float b) {
    if (std::fabs(b - 0.125f) < 1e-3f) return "1/32";
    if (std::fabs(b - 0.25f) < 1e-3f) return "1/16";
    if (std::fabs(b - 1.f / 3.f) < 1e-3f) return "1/8T";
    if (std::fabs(b - 0.5f) < 1e-3f) return "1/8";
    if (std::fabs(b - 0.75f) < 1e-3f) return "3/16";
    if (std::fabs(b - 1.f) < 1e-3f) return "1/4";
    if (std::fabs(b - 2.f) < 1e-3f) return "1/2";
    if (std::fabs(b - 4.f) < 1e-3f) return "1 bar";
    if (std::fabs(b - 8.f) < 1e-3f) return "2 bar";
    return fmt("%.2f 拍", b);
}

void mixInto(Buffer& dst, const Buffer& src, float g) {
    const int n = std::min(dst.size(), src.size());
    for (int i = 0; i < n; ++i) { dst.L[(size_t)i] += g * src.L[(size_t)i]; dst.R[(size_t)i] += g * src.R[(size_t)i]; }
}

// Pitch shifter: two crossfaded taps over a rotating delay (sin^2 windows sum to 1).
struct PitchShifter {
    std::vector<float> buf;
    int w = 0, W = 1;
    float phase = 0, inc = 0;
    void init(int windowSamples, float ratio) {
        W = std::max(16, windowSamples);
        buf.assign((size_t)(2 * W + 8), 0.f);
        inc = (1.f - ratio) / (float)W;
    }
    float tap(float d) const {
        float pos = (float)w - d;
        while (pos < 0) pos += (float)buf.size();
        const int i0 = (int)pos % (int)buf.size(), i1 = (i0 + 1) % (int)buf.size();
        const float fr = pos - std::floor(pos);
        return buf[(size_t)i0] * (1 - fr) + buf[(size_t)i1] * fr;
    }
    float process(float x) {
        buf[(size_t)w] = x;
        float p2 = phase + 0.5f;
        if (p2 >= 1.f) p2 -= 1.f;
        const float g1 = std::sin(kPi * phase), g2 = std::sin(kPi * p2);
        const float y = tap(1.f + phase * (float)W) * g1 * g1 + tap(1.f + p2 * (float)W) * g2 * g2;
        phase += inc;
        if (phase >= 1.f) phase -= 1.f;
        if (phase < 0.f) phase += 1.f;
        if (++w >= (int)buf.size()) w = 0;
        return y;
    }
};

float hann(int i, int n) { return 0.5f - 0.5f * std::cos(2.f * kPi * (float)i / (float)std::max(1, n - 1)); }

// ------------------------------------------------------------------ Reverb
class Reverb : public Effect {
public:
    float decay, damp, mix, width, wetHp;
    int predelayMs;
    Reverb(Rng& r, const FxLimits& lim) {
        decay = r.logRange(0.3f, std::max(0.31f, lim.maxReverbSec));
        damp = r.range(0.15f, 0.6f);
        mix = r.range(0.1f, 0.6f);
        width = r.range(0.6f, 1.f);
        predelayMs = r.irange(0, 40);
        wetHp = lim.lowRegister ? r.range(250.f, 500.f) : r.range(80.f, 300.f);
    }
    void process(Buffer& b, const Timeline&, const Region&) override {
        Buffer wet;
        freeverb(b, wet, decay, damp, width, predelayMs * b.sr / 1000);
        Biquad hl, hr;
        hl.set(Biquad::HP, wetHp, 0.707f, 0, (float)b.sr);
        hr = hl;
        for (int i = 0; i < b.size(); ++i) {
            b.L[(size_t)i] = b.L[(size_t)i] * (1.f - 0.5f * mix) + mix * hl.process(wet.L[(size_t)i]);
            b.R[(size_t)i] = b.R[(size_t)i] * (1.f - 0.5f * mix) + mix * hr.process(wet.R[(size_t)i]);
        }
    }
    std::string describe() const override {
        return fmt("混响（衰减 %.1fs，湿 %d%%，预延迟 %dms，湿声高通 %.0fHz）", decay, (int)(mix * 100), predelayMs, wetHp);
    }
};

// ------------------------------------------------------------------ Reverse reverb
class ReverseReverb : public Effect {
public:
    float decay, level;
    ReverseReverb(Rng& r, const FxLimits& lim) {
        decay = r.logRange(0.8f, std::max(0.81f, std::min(lim.maxReverbSec, 4.f)));
        level = r.range(0.5f, 1.0f);
    }
    void process(Buffer& b, const Timeline&, const Region&) override {
        Buffer rev = b;
        reverseInPlace(rev);
        Buffer wet;
        freeverb(rev, wet, decay, 0.3f, 1.f, 0);
        reverseInPlace(wet);
        mixInto(b, wet, level);
    }
    std::string describe() const override { return fmt("倒放混响（衰减 %.1fs，量 %d%%）", decay, (int)(level * 100)); }
};

// ------------------------------------------------------------------ Delay / grain delay
class Delay : public Effect {
public:
    float beats, fb, mix, lpHz, hpHz, semis;
    bool pingpong;
    int grainMs;
    Delay(Rng& r, const FxLimits& lim) {
        beats = r.pick<float>({0.125f, 0.25f, 0.5f, 0.75f, 1.f / 3.f});
        fb = r.range(0.f, 0.7f);
        mix = r.range(0.2f, 0.6f);
        lpHz = r.logRange(2000.f, 12000.f);
        hpHz = r.logRange(100.f, 600.f);
        pingpong = r.chance(0.5f);
        semis = (lim.allowPitch && r.chance(0.45f)) ? r.pick<float>({12.f, -12.f, 7.f, 5.f, -5.f}) : 0.f;
        grainMs = r.irange(40, 120);
    }
    void process(Buffer& b, const Timeline& t, const Region&) override {
        const int d = std::max(1, t.beats(beats));
        std::vector<float> rl((size_t)d, 0.f), rr((size_t)d, 0.f);
        Biquad lpl, lpr, hpl, hpr;
        lpl.set(Biquad::LP, lpHz, 0.707f, 0, (float)b.sr); lpr = lpl;
        hpl.set(Biquad::HP, hpHz, 0.707f, 0, (float)b.sr); hpr = hpl;
        PitchShifter psl, psr;
        const bool ps = std::fabs(semis) > 0.01f;
        if (ps) { psl.init(grainMs * b.sr / 1000, semisToRatio(semis)); psr.init(grainMs * b.sr / 1000, semisToRatio(semis)); }
        int idx = 0;
        for (int i = 0; i < b.size(); ++i) {
            const float dl = rl[(size_t)idx], dr = rr[(size_t)idx];
            float fl = hpl.process(lpl.process(dl)), fr = hpr.process(lpr.process(dr));
            if (ps) { fl = psl.process(fl); fr = psr.process(fr); }
            const float inL = b.L[(size_t)i], inR = b.R[(size_t)i];
            if (pingpong) {
                rl[(size_t)idx] = 0.5f * (inL + inR) + fb * fr;
                rr[(size_t)idx] = fb * fl;
            } else {
                rl[(size_t)idx] = inL + fb * fl;
                rr[(size_t)idx] = inR + fb * fr;
            }
            b.L[(size_t)i] = inL + mix * fl;
            b.R[(size_t)i] = inR + mix * fr;
            if (++idx >= d) idx = 0;
        }
    }
    std::string describe() const override {
        std::string s = fmt("%s（%s，反馈 %d%%，湿 %d%%%s", std::fabs(semis) > 0.01f ? "粒子延迟" : "延迟", beatsName(beats).c_str(),
                            (int)(fb * 100), (int)(mix * 100), pingpong ? "，乒乓" : "");
        if (std::fabs(semis) > 0.01f) s += fmt("，每次回声变调 %+d 半音，颗粒 %dms", (int)semis, grainMs);
        return s + "）";
    }
};

// ------------------------------------------------------------------ EQ / filter sweep
class Eq : public Effect {
public:
    bool sweep;
    Biquad::Type type = Biquad::LP;
    float f0 = 0, f1 = 0, q = 0.707f;
    float lowDb = 0, midDb = 0, midHz = 1000, highDb = 0;
    Eq(Rng& r, const FxLimits& lim) {
        sweep = r.chance(0.6f);
        if (sweep) {
            const int k = r.irange(0, 2);
            type = k == 0 ? Biquad::LP : (k == 1 ? Biquad::HP : Biquad::BP);
            const bool up = r.chance(0.7f);
            float lo = type == Biquad::HP ? 30.f : 200.f, hi = type == Biquad::HP ? 2500.f : 16000.f;
            if (lim.lowRegister && type == Biquad::HP) hi = 400.f;
            f0 = up ? lo * r.range(1.f, 1.5f) : hi * r.range(0.6f, 1.f);
            f1 = up ? hi * r.range(0.6f, 1.f) : lo * r.range(1.f, 1.5f);
            q = r.range(0.7f, type == Biquad::BP ? 4.f : 6.f);
        } else {
            const float mag = r.range(3.f, 9.f);
            lowDb = r.range(-mag, mag) * (lim.lowRegister ? 0.5f : 1.f);
            midDb = r.range(-mag, mag);
            midHz = r.logRange(300.f, 5000.f);
            highDb = r.range(-mag, mag);
            q = r.range(0.5f, 2.f);
        }
    }
    void process(Buffer& b, const Timeline&, const Region& r) override {
        if (!sweep) {
            Biquad s[2][3];
            for (auto& ch : s) {
                ch[0].set(Biquad::LowShelf, 150.f, 0.707f, lowDb, (float)b.sr);
                ch[1].set(Biquad::Peak, midHz, q, midDb, (float)b.sr);
                ch[2].set(Biquad::HighShelf, 7000.f, 0.707f, highDb, (float)b.sr);
            }
            for (int i = 0; i < b.size(); ++i) {
                b.L[(size_t)i] = s[0][2].process(s[0][1].process(s[0][0].process(b.L[(size_t)i])));
                b.R[(size_t)i] = s[1][2].process(s[1][1].process(s[1][0].process(b.R[(size_t)i])));
            }
            return;
        }
        Biquad l, rr;
        const int a = r.start, e = std::max(r.start + 1, r.end);
        for (int i = 0; i < b.size(); ++i) {
            if ((i & 31) == 0) {
                const float t = std::clamp((float)(i - a) / (float)(e - a), 0.f, 1.f);
                const float f = f0 * std::pow(f1 / f0, t);
                l.set(type, f, q, 0, (float)b.sr);
                rr.b0 = l.b0; rr.b1 = l.b1; rr.b2 = l.b2; rr.a1 = l.a1; rr.a2 = l.a2;
            }
            float gl = l.process(b.L[(size_t)i]), gr = rr.process(b.R[(size_t)i]);
            if (type == Biquad::BP) { gl *= 1.5f; gr *= 1.5f; }
            b.L[(size_t)i] = gl;
            b.R[(size_t)i] = gr;
        }
    }
    std::string describe() const override {
        if (!sweep) return fmt("EQ（低架 %+.1fdB，%.0fHz %+.1fdB，高架 %+.1fdB）", lowDb, midHz, midDb, highDb);
        const char* n = type == Biquad::LP ? "低通" : (type == Biquad::HP ? "高通" : "带通");
        return fmt("%s扫频（%.0fHz → %.0fHz，Q %.1f）", n, f0, f1, q);
    }
};

// ------------------------------------------------------------------ Distortion
class Distortion : public Effect {
public:
    float driveDb, mix, toneHz;
    int mode, bits, decim;
    Distortion(Rng& r, const FxLimits& lim) {
        driveDb = r.range(3.f, lim.maxDriveDb);
        mode = r.irange(0, 2); // 0 tanh, 1 fold, 2 crush
        bits = r.irange(4, 10);
        decim = r.irange(1, 6);
        mix = r.range(0.5f, 1.f);
        toneHz = r.logRange(4000.f, 16000.f);
    }
    void process(Buffer& b, const Timeline&, const Region& r) override {
        const float inRms = stereoRms(b, r.start, r.end) + 1e-9f;
        Buffer w = b;
        const float g = dbToGain(driveDb);
        const float q = std::pow(2.f, (float)bits - 1.f);
        float holdL = 0, holdR = 0;
        Biquad tl, tr;
        tl.set(Biquad::LP, toneHz, 0.707f, 0, (float)b.sr); tr = tl;
        for (int i = 0; i < w.size(); ++i) {
            for (int ch = 0; ch < 2; ++ch) {
                float& x = ch ? w.R[(size_t)i] : w.L[(size_t)i];
                float y = x * g;
                if (mode == 0) y = std::tanh(y);
                else if (mode == 1) y = std::sin(std::clamp(y, -8.f, 8.f) * 0.5f * kPi);
                else {
                    float& hold = ch ? holdR : holdL;
                    if (i % decim == 0) hold = std::round(std::tanh(y) * q) / q;
                    y = hold;
                }
                x = ch ? tr.process(y) : tl.process(y);
            }
        }
        const float outRms = stereoRms(w, r.start, r.end) + 1e-9f;
        const float comp = std::min(4.f, inRms / outRms);
        for (int i = 0; i < b.size(); ++i) {
            b.L[(size_t)i] = b.L[(size_t)i] * (1 - mix) + mix * comp * w.L[(size_t)i];
            b.R[(size_t)i] = b.R[(size_t)i] * (1 - mix) + mix * comp * w.R[(size_t)i];
        }
    }
    std::string describe() const override {
        if (mode == 2) return fmt("压碎（%d bit，降采样 1/%d，驱动 %.0fdB，湿 %d%%）", bits, decim, driveDb, (int)(mix * 100));
        return fmt("%s（驱动 %.0fdB，湿 %d%%，音色 %.0fHz）", mode == 0 ? "饱和失真" : "折叠失真", driveDb, (int)(mix * 100), toneHz);
    }
};

// ------------------------------------------------------------------ Crystal (reverse pitched slice echoes)
class Crystal : public Effect {
public:
    float chunkBeats, semis, decay, mix;
    int repeats;
    bool reverse;
    Crystal(Rng& r, const FxLimits& lim) {
        chunkBeats = r.pick<float>({0.125f, 0.25f, 0.5f});
        semis = lim.allowPitch ? r.pick<float>({12.f, 7.f, -12.f, 19.f, 5.f, 0.f}) : 0.f;
        repeats = r.irange(1, 4);
        decay = r.range(0.4f, 0.8f);
        mix = r.range(0.4f, 0.8f);
        reverse = r.chance(0.7f);
    }
    void process(Buffer& b, const Timeline& t, const Region&) override {
        const int chunk = std::max(64, t.beats(chunkBeats));
        const float ratio = semisToRatio(semis);
        Buffer out(b.size(), b.sr);
        const int fade = std::min(chunk / 4, b.sr / 300);
        for (int k = 0; k + chunk <= b.size(); k += chunk) {
            if (stereoRms(b, k, k + chunk) < 1e-4f) continue;
            Buffer seg(chunk, b.sr);
            for (int i = 0; i < chunk; ++i) { seg.L[(size_t)i] = b.L[(size_t)(k + i)]; seg.R[(size_t)i] = b.R[(size_t)(k + i)]; }
            if (reverse) reverseInPlace(seg);
            seg = varispeed(seg, ratio);
            const int n = seg.size();
            for (int i = 0; i < n; ++i) {
                float w = 1.f;
                if (i < fade) w = (float)i / (float)fade;
                if (i > n - fade) w = (float)(n - i) / (float)fade;
                seg.L[(size_t)i] *= w; seg.R[(size_t)i] *= w;
            }
            float g = 1.f;
            for (int j = 1; j <= repeats; ++j) {
                g *= decay;
                const int at = k + j * chunk;
                const float pl = (j & 1) ? 0.3f : 1.f, pr = (j & 1) ? 1.f : 0.3f;
                for (int i = 0; i < n && at + i < out.size(); ++i) {
                    out.L[(size_t)(at + i)] += g * pl * seg.L[(size_t)i];
                    out.R[(size_t)(at + i)] += g * pr * seg.R[(size_t)i];
                }
            }
        }
        mixInto(b, out, mix);
    }
    std::string describe() const override {
        return fmt("结晶回声（切片 %s，%s%+d 半音，重复 %d 次，衰减 %d%%，湿 %d%%）", beatsName(chunkBeats).c_str(),
                   reverse ? "倒放，" : "", (int)semis, repeats, (int)(decay * 100), (int)(mix * 100));
    }
};

// ------------------------------------------------------------------ Stutter / roll into the downbeat
class Stutter : public Effect {
public:
    float regionBeats, sliceBeats, pitchStep;
    bool accelerate, crescendo;
    Stutter(Rng& r, const FxLimits& lim, const Timeline& tl) {
        const float maxBeats = (float)tl.downbeat / (float)tl.beat();
        std::vector<float> opts;
        for (float v : {0.5f, 1.f, 2.f, 4.f}) if (v <= maxBeats + 1e-3f) opts.push_back(v);
        if (opts.empty()) opts.push_back(maxBeats);
        regionBeats = r.pick(opts);
        sliceBeats = r.pick<float>({0.125f, 0.25f, 1.f / 3.f, 0.5f});
        if (sliceBeats > regionBeats / 2) sliceBeats = std::max(0.125f, regionBeats / 4);
        accelerate = r.chance(0.5f);
        crescendo = r.chance(0.7f);
        pitchStep = (lim.allowPitch && r.chance(0.4f)) ? r.range(0.25f, 1.f) : 0.f;
    }
    void process(Buffer& b, const Timeline& t, const Region& r) override {
        const int regionLen = std::min(t.downbeat, t.beats(regionBeats));
        const int a = t.downbeat - regionLen;
        if (regionLen <= 0) return;
        // slice source: the sound's own start (its attack)
        int slice = std::max(64, t.beats(sliceBeats));
        // slice source: the attack of a hit, or the loudest slice of a build-up
        int src = std::clamp(r.start, 0, std::max(0, b.size() - 1));
        if (r.start < t.downbeat) {
            float best = -1.f;
            for (int p = r.start; p + slice <= std::min(r.end, t.downbeat); p += slice / 2) {
                const float v = stereoRms(b, p, p + slice);
                if (v > best) { best = v; src = p; }
            }
        }
        Buffer roll(regionLen, b.sr);
        int pos = 0, rep = 0;
        while (pos < regionLen) {
            if (accelerate && pos > regionLen * 3 / 4) slice = std::max(64, t.beats(sliceBeats) / 2);
            Buffer seg(slice, b.sr);
            for (int i = 0; i < slice && src + i < b.size(); ++i) { seg.L[(size_t)i] = b.L[(size_t)(src + i)]; seg.R[(size_t)i] = b.R[(size_t)(src + i)]; }
            if (pitchStep > 0) seg = varispeed(seg, semisToRatio(pitchStep * (float)rep));
            const int n = std::min(seg.size(), slice);
            const int fade = std::min(n / 4, b.sr / 400);
            for (int i = 0; i < n && pos + i < regionLen; ++i) {
                float w = 1.f;
                if (i < fade) w = (float)i / (float)std::max(1, fade);
                if (i > n - fade) w = (float)(n - i) / (float)std::max(1, fade);
                const float c = crescendo ? 0.25f + 0.75f * (float)(pos + i) / (float)regionLen : 1.f;
                roll.L[(size_t)(pos + i)] = seg.L[(size_t)i] * w * c;
                roll.R[(size_t)(pos + i)] = seg.R[(size_t)i] * w * c;
            }
            pos += slice;
            ++rep;
        }
        for (int i = 0; i < regionLen; ++i) {
            b.L[(size_t)(a + i)] = 0.3f * b.L[(size_t)(a + i)] + roll.L[(size_t)i];
            b.R[(size_t)(a + i)] = 0.3f * b.R[(size_t)(a + i)] + roll.R[(size_t)i];
        }
    }
    std::string describe() const override {
        std::string s = fmt("卡顿连击（强拍前 %s，切片 %s", beatsName(regionBeats).c_str(), beatsName(sliceBeats).c_str());
        if (accelerate) s += "，末段加速";
        if (crescendo) s += "，渐强";
        if (pitchStep > 0) s += fmt("，每下升 %.2f 半音", pitchStep);
        return s + "）";
    }
};

// ------------------------------------------------------------------ Granular cloud
class Granular : public Effect {
public:
    float density, sprayBeats, reverseProb, mix;
    int grainMs;
    bool freeze;
    std::vector<float> pitches;
    uint64_t seed;
    Granular(Rng& r, const FxLimits& lim) {
        density = r.range(15.f, 80.f);
        grainMs = r.irange(20, 160);
        sprayBeats = r.range(0.f, 0.5f);
        reverseProb = r.range(0.f, 0.6f);
        mix = r.range(0.3f, 0.9f);
        freeze = r.chance(0.3f);
        pitches = lim.allowPitch ? std::vector<float>{0.f, 12.f, -12.f, 7.f} : std::vector<float>{0.f};
        if (lim.allowPitch && r.chance(0.5f)) pitches = {0.f, 12.f};
        seed = r.next();
    }
    void process(Buffer& b, const Timeline& t, const Region& reg) override {
        Rng r(seed);
        Buffer out(b.size(), b.sr);
        const int g = grainMs * b.sr / 1000;
        const int spray = t.beats(sprayBeats);
        int a = reg.start, e = reg.end;
        int frozen = reg.start;
        if (freeze) {
            float best = -1;
            for (int p = reg.start; p + g < reg.end; p += g / 2) {
                const float v = stereoRms(b, p, p + g);
                if (v > best) { best = v; frozen = p; }
            }
            a = std::min(reg.start, std::max(0, t.downbeat - t.beats(4))); // texture can run ahead of the sound
            e = std::max(e, t.downbeat);
        }
        const double step = (double)b.sr / density;
        for (double tpos = a; tpos < e; tpos += step * r.range(0.5f, 1.5f)) {
            const int at = (int)tpos;
            int src = freeze ? frozen + (int)r.range(-0.02f * b.sr, 0.02f * b.sr) : at - (int)(r.uni() * (float)spray);
            src = std::clamp(src, 0, std::max(0, b.size() - g - 1));
            const float ratio = semisToRatio(r.pick(pitches));
            const bool rev = r.chance(reverseProb);
            const float pan = r.range(-0.8f, 0.8f);
            const float gl = std::sqrt(0.5f * (1 - pan)), gr = std::sqrt(0.5f * (1 + pan));
            const int len = (int)((float)g / ratio);
            for (int i = 0; i < len && at + i < out.size(); ++i) {
                const double sp = rev ? (double)src + (double)(len - 1 - i) * ratio : (double)src + (double)i * ratio;
                const float w = hann(i, len) * 0.7f;
                const float m = 0.5f * (readCubic(b.L, sp) + readCubic(b.R, sp));
                out.L[(size_t)(at + i)] += m * w * gl;
                out.R[(size_t)(at + i)] += m * w * gr;
            }
        }
        // normalise the cloud to the dry level of the region
        const float dry = stereoRms(b, reg.start, reg.end) + 1e-9f, wet = stereoRms(out, a, e) + 1e-9f;
        const float k = std::min(6.f, dry / wet);
        for (int i = 0; i < b.size(); ++i) {
            b.L[(size_t)i] = b.L[(size_t)i] * (1.f - 0.6f * mix) + mix * k * out.L[(size_t)i];
            b.R[(size_t)i] = b.R[(size_t)i] * (1.f - 0.6f * mix) + mix * k * out.R[(size_t)i];
        }
    }
    std::string describe() const override {
        std::string p;
        for (float v : pitches) p += (p.empty() ? "" : "/") + fmt("%+d", (int)v);
        return fmt("粒子云（%s密度 %.0f 粒/秒，颗粒 %dms，散布 %.2f 拍，倒放 %d%%，音高 %s，湿 %d%%）", freeze ? "冻结，" : "", density,
                   grainMs, sprayBeats, (int)(reverseProb * 100), p.c_str(), (int)(mix * 100));
    }
};

} // namespace

const char* fxName(FxType t) {
    switch (t) {
    case FxType::Reverb: return "混响";
    case FxType::ReverseReverb: return "倒放混响";
    case FxType::Delay: return "延迟";
    case FxType::Eq: return "EQ/扫频";
    case FxType::Distortion: return "失真";
    case FxType::Crystal: return "结晶回声";
    case FxType::Stutter: return "卡顿连击";
    case FxType::Granular: return "粒子云";
    default: return "?";
    }
}

std::unique_ptr<Effect> makeEffect(FxType t, Rng& rng, const FxLimits& lim, const Timeline& tl, const Region&) {
    switch (t) {
    case FxType::Reverb: return std::make_unique<Reverb>(rng, lim);
    case FxType::ReverseReverb: return std::make_unique<ReverseReverb>(rng, lim);
    case FxType::Delay: return std::make_unique<Delay>(rng, lim);
    case FxType::Eq: return std::make_unique<Eq>(rng, lim);
    case FxType::Distortion: return std::make_unique<Distortion>(rng, lim);
    case FxType::Crystal: return std::make_unique<Crystal>(rng, lim);
    case FxType::Stutter: return std::make_unique<Stutter>(rng, lim, tl);
    case FxType::Granular: return std::make_unique<Granular>(rng, lim);
    default: return nullptr;
    }
}

// ------------------------------------------------------------------ Freeverb
void freeverb(const Buffer& in, Buffer& wet, float decaySec, float damp, float width, int predelay) {
    static const int combT[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static const int apT[4] = {556, 441, 341, 225};
    const float scale = (float)in.sr / 44100.f;
    struct Comb { std::vector<float> b; int i = 0; float fb = 0, st = 0; };
    struct Ap { std::vector<float> b; int i = 0; };
    Comb cl[8], cr[8];
    Ap al[4], ar[4];
    for (int k = 0; k < 8; ++k) {
        const int nl = (int)(combT[k] * scale), nr = (int)((combT[k] + 23) * scale);
        cl[k].b.assign((size_t)nl, 0.f); cr[k].b.assign((size_t)nr, 0.f);
        cl[k].fb = std::pow(10.f, -3.f * (float)nl / (decaySec * (float)in.sr));
        cr[k].fb = std::pow(10.f, -3.f * (float)nr / (decaySec * (float)in.sr));
    }
    for (int k = 0; k < 4; ++k) {
        al[k].b.assign((size_t)(apT[k] * scale), 0.f);
        ar[k].b.assign((size_t)((apT[k] + 23) * scale), 0.f);
    }
    wet = Buffer(in.size(), in.sr);
    auto comb = [damp](Comb& c, float x) {
        float y = c.b[(size_t)c.i];
        c.st = y * (1 - damp) + c.st * damp;
        c.b[(size_t)c.i] = x + c.st * c.fb;
        if (++c.i >= (int)c.b.size()) c.i = 0;
        return y;
    };
    auto ap = [](Ap& a, float x) {
        float bo = a.b[(size_t)a.i];
        float y = -x + bo;
        a.b[(size_t)a.i] = x + bo * 0.5f;
        if (++a.i >= (int)a.b.size()) a.i = 0;
        return y;
    };
    const float w1 = 0.5f * (1 + width), w2 = 0.5f * (1 - width);
    for (int n = 0; n < in.size(); ++n) {
        const int s = n - predelay;
        const float x = s >= 0 ? 0.015f * (in.L[(size_t)s] + in.R[(size_t)s]) : 0.f;
        float ol = 0, orr = 0;
        for (int k = 0; k < 8; ++k) { ol += comb(cl[k], x); orr += comb(cr[k], x); }
        for (int k = 0; k < 4; ++k) { ol = ap(al[k], ol); orr = ap(ar[k], orr); }
        wet.L[(size_t)n] = 3.f * (ol * w1 + orr * w2);
        wet.R[(size_t)n] = 3.f * (orr * w1 + ol * w2);
    }
}

// ------------------------------------------------------------------ Envelopes
std::string EnvelopeChoice::describe() const {
    switch (kind) {
    case None: return "";
    case Swell: return "音量渐强包络（指数上升到强拍）";
    case Pump: return "1/8 泵感包络";
    case LowpassOpen: return "低通打开包络（300Hz → 18kHz）";
    case HighpassRise: return "高通上升包络（30Hz → 1.2kHz，强拍复位）";
    case TapeStop: return "磁带停止包络（尾段减速到 0）";
    case Dive: return "音高下潜包络（降到 0.6 倍速）";
    case FadeOut: return "尾部淡出包络";
    }
    return "";
}

void applyEnvelope(const EnvelopeChoice& e, Buffer& b, const Timeline& t, const Region& r) {
    const int a = r.start, z = std::max(r.start + 1, r.end);
    auto prog = [&](int i) { return std::clamp((float)(i - a) / (float)(z - a), 0.f, 1.f); };
    switch (e.kind) {
    case EnvelopeChoice::None: return;
    case EnvelopeChoice::Swell:
        for (int i = 0; i < b.size(); ++i) {
            const float g = i < z ? std::pow(prog(i), 2.2f) : 1.f;
            b.L[(size_t)i] *= g; b.R[(size_t)i] *= g;
        }
        return;
    case EnvelopeChoice::Pump: {
        const int p = t.beats(0.5f);
        for (int i = 0; i < b.size(); ++i) {
            const float ph = (float)(((i - t.downbeat) % p + p) % p) / (float)p;
            const float g = 0.25f + 0.75f * std::min(1.f, ph * 3.f);
            b.L[(size_t)i] *= g; b.R[(size_t)i] *= g;
        }
        return;
    }
    case EnvelopeChoice::LowpassOpen:
    case EnvelopeChoice::HighpassRise: {
        const bool lp = e.kind == EnvelopeChoice::LowpassOpen;
        Biquad l, rr;
        for (int i = 0; i < b.size(); ++i) {
            if ((i & 31) == 0) {
                const float pr = prog(i);
                float f = lp ? 300.f * std::pow(60.f, pr) : 30.f * std::pow(40.f, pr);
                if (!lp && i >= t.downbeat) f = 20.f;
                l.set(lp ? Biquad::LP : Biquad::HP, f, 0.9f, 0, (float)b.sr);
                rr.b0 = l.b0; rr.b1 = l.b1; rr.b2 = l.b2; rr.a1 = l.a1; rr.a2 = l.a2;
            }
            b.L[(size_t)i] = l.process(b.L[(size_t)i]);
            b.R[(size_t)i] = rr.process(b.R[(size_t)i]);
        }
        return;
    }
    case EnvelopeChoice::TapeStop:
    case EnvelopeChoice::Dive: {
        const bool stop = e.kind == EnvelopeChoice::TapeStop;
        const int s0 = stop ? a + (int)((float)(z - a) * 0.55f) : a;
        Buffer src = b;
        double ph = s0;
        for (int i = s0; i < b.size(); ++i) {
            const float p = std::clamp((float)(i - s0) / (float)std::max(1, z - s0), 0.f, 1.f);
            const float rate = stop ? std::max(0.f, 1.f - p) : 1.f - 0.4f * p;
            // a stopping tape also loses level; without this the frozen read position leaves DC + a click
            const float amp = stop ? std::sqrt(std::max(0.f, 1.f - p)) : 1.f;
            b.L[(size_t)i] = readCubic(src.L, ph) * amp;
            b.R[(size_t)i] = readCubic(src.R, ph) * amp;
            ph += rate;
        }
        return;
    }
    case EnvelopeChoice::FadeOut:
        for (int i = a; i < b.size(); ++i) {
            const float g = 1.f - prog(i);
            b.L[(size_t)i] *= g * g; b.R[(size_t)i] *= g * g;
        }
        return;
    }
}

// ------------------------------------------------------------------ Bus
std::string BusSettings::describe() const {
    std::string s = multiband ? fmt("多段压缩（3 段，180Hz / 3.5kHz 分频，比率 %.1f:1）", mbRatio) : std::string("整体 EQ（25Hz 低切）");
    s += fmt("；总 EQ（低架 %+.1fdB，高架 %+.1fdB）", tiltLowDb, tiltHighDb);
    if (spaceMix > 0.01f) s += fmt("；总混响（衰减 %.1fs，湿 %d%%）", spaceDecay, (int)(spaceMix * 100));
    else s += "；总混响关";
    return s;
}

namespace {
void compressBand(std::vector<float>& l, std::vector<float>& r, float ratio, int sr) {
    // threshold sits 6 dB under the band's loudest 50 ms
    float pk = 0;
    const int w = sr / 20;
    for (int s = 0; s + w < (int)l.size(); s += w / 2) pk = std::max(pk, std::sqrt(0.5f * (rms(l, s, s + w) * rms(l, s, s + w) + rms(r, s, s + w) * rms(r, s, s + w))));
    if (pk < 1e-6f) return;
    const float thr = gainToDb(pk) - 6.f;
    const float at = std::exp(-1.f / (0.005f * sr)), rl = std::exp(-1.f / (0.08f * sr));
    float env = 0, makeup = (thr + 6.f - (thr + 6.f / ratio)) * 0.5f;
    for (size_t i = 0; i < l.size(); ++i) {
        const float x = std::max(std::fabs(l[i]), std::fabs(r[i]));
        env = x > env ? at * env + (1 - at) * x : rl * env + (1 - rl) * x;
        const float db = gainToDb(env);
        const float over = db - thr;
        const float gr = over > 0 ? -over * (1.f - 1.f / ratio) : 0.f;
        const float g = dbToGain(gr + makeup);
        l[i] *= g; r[i] *= g;
    }
}
} // namespace

void processBus(Buffer& b, const BusSettings& s, const Timeline&) {
    const float sr = (float)b.sr;
    if (s.multiband) {
        // Linkwitz-Riley 4th order splits (two cascaded Butterworth biquads)
        auto split = [&](const std::vector<float>& x, float f, std::vector<float>& lo, std::vector<float>& hi) {
            Biquad l1, l2, h1, h2;
            l1.set(Biquad::LP, f, 0.7071f, 0, sr); l2 = l1;
            h1.set(Biquad::HP, f, 0.7071f, 0, sr); h2 = h1;
            lo.resize(x.size()); hi.resize(x.size());
            for (size_t i = 0; i < x.size(); ++i) { lo[i] = l2.process(l1.process(x[i])); hi[i] = h2.process(h1.process(x[i])); }
        };
        std::vector<float> lowL, restL, midL, highL, lowR, restR, midR, highR;
        split(b.L, 180.f, lowL, restL); split(restL, 3500.f, midL, highL);
        split(b.R, 180.f, lowR, restR); split(restR, 3500.f, midR, highR);
        compressBand(lowL, lowR, s.mbRatio, b.sr);
        compressBand(midL, midR, s.mbRatio, b.sr);
        compressBand(highL, highR, s.mbRatio, b.sr);
        for (int i = 0; i < b.size(); ++i) {
            b.L[(size_t)i] = lowL[(size_t)i] + midL[(size_t)i] + highL[(size_t)i];
            b.R[(size_t)i] = lowR[(size_t)i] + midR[(size_t)i] + highR[(size_t)i];
        }
    }
    Biquad eq[2][3];
    for (auto& ch : eq) {
        ch[0].set(Biquad::HP, 25.f, 0.707f, 0, sr);
        ch[1].set(Biquad::LowShelf, 120.f, 0.707f, s.tiltLowDb, sr);
        ch[2].set(Biquad::HighShelf, 8000.f, 0.707f, s.tiltHighDb, sr);
    }
    for (int i = 0; i < b.size(); ++i) {
        b.L[(size_t)i] = eq[0][2].process(eq[0][1].process(eq[0][0].process(b.L[(size_t)i])));
        b.R[(size_t)i] = eq[1][2].process(eq[1][1].process(eq[1][0].process(b.R[(size_t)i])));
    }
    if (s.spaceMix > 0.01f) {
        Buffer wet;
        freeverb(b, wet, s.spaceDecay, 0.4f, 1.f, b.sr / 100);
        Biquad h[2];
        h[0].set(Biquad::HP, 300.f, 0.707f, 0, sr); h[1] = h[0];
        for (int i = 0; i < b.size(); ++i) {
            b.L[(size_t)i] += s.spaceMix * h[0].process(wet.L[(size_t)i]);
            b.R[(size_t)i] += s.spaceMix * h[1].process(wet.R[(size_t)i]);
        }
    }
}

// ------------------------------------------------------------------ Output options
std::string OutputOptions::describe() const {
    std::string s;
    if (gate) {
        std::string pat;
        for (int k = 0; k < 16; ++k) pat += (gatePattern >> k & 1u) ? "x" : ".";
        s += fmt("门限切片 开（%s 步，图案 %s，只作用在强拍前）", beatsName(gateStepBeats).c_str(), pat.c_str());
    } else s += "门限切片 关";
    s += finalReverb ? fmt("；最终混响 开（湿 %d%%）", (int)(finalReverbMix * 100)) : std::string("；最终混响 关");
    if (finalEq == Telephone) s += "；最终 EQ：电话音色（强拍前变窄，强拍恢复全频）";
    else if (finalEq == Darken) s += "；最终 EQ：压暗（4kHz 低通）";
    else s += "；最终 EQ 关";
    return s;
}

void processOutput(Buffer& b, const OutputOptions& o, const Timeline& t) {
    const float sr = (float)b.sr;
    if (o.finalEq != OutputOptions::EqOff) {
        Biquad f[2][2];
        for (auto& ch : f) {
            if (o.finalEq == OutputOptions::Telephone) { ch[0].set(Biquad::HP, 400.f, 0.9f, 0, sr); ch[1].set(Biquad::LP, 3200.f, 0.9f, 0, sr); }
            else { ch[0].set(Biquad::HP, 20.f, 0.707f, 0, sr); ch[1].set(Biquad::LP, 4000.f, 0.707f, 0, sr); }
        }
        const int xf = b.sr / 200;
        for (int i = 0; i < b.size(); ++i) {
            const float yl = f[0][1].process(f[0][0].process(b.L[(size_t)i]));
            const float yr = f[1][1].process(f[1][0].process(b.R[(size_t)i]));
            float m = 1.f;
            if (o.finalEq == OutputOptions::Telephone) m = std::clamp((float)(t.downbeat - i) / (float)xf, 0.f, 1.f);
            const float boost = o.finalEq == OutputOptions::Telephone ? 1.6f : 1.f;
            b.L[(size_t)i] = b.L[(size_t)i] * (1 - m) + m * yl * boost;
            b.R[(size_t)i] = b.R[(size_t)i] * (1 - m) + m * yr * boost;
        }
    }
    if (o.gate) {
        const int step = std::max(1, t.beats(o.gateStepBeats));
        const int sm = b.sr / 400; // 2.5 ms edges
        float g = 1.f;
        const float coef = 1.f / (float)sm;
        for (int i = 0; i < b.size(); ++i) {
            float target = 1.f;
            if (i < t.downbeat) {
                const int k = ((t.downbeat - 1 - i) / step); // count steps backwards from the downbeat
                const int idx = 15 - (k % 16);
                target = (o.gatePattern >> idx & 1u) ? 1.f : 0.f;
            }
            g += (target - g) * std::min(1.f, coef * 2.f);
            b.L[(size_t)i] *= g; b.R[(size_t)i] *= g;
        }
    }
    if (o.finalReverb) {
        Buffer wet;
        freeverb(b, wet, 2.2f, 0.35f, 1.f, b.sr / 50);
        for (int i = 0; i < b.size(); ++i) { b.L[(size_t)i] += o.finalReverbMix * wet.L[(size_t)i]; b.R[(size_t)i] += o.finalReverbMix * wet.R[(size_t)i]; }
    }
}

void finalizeLoudness(Buffer& b, const Timeline& t) {
    // level: loudest 300 ms window around the transition brought to -10 dBFS RMS
    const int w = b.sr * 3 / 10;
    float loud = 0;
    for (int s = 0; s + w <= b.size(); s += w / 3) loud = std::max(loud, stereoRms(b, s, s + w));
    if (loud > 1e-6f) {
        const float g = dbToGain(-10.f) / loud;
        for (int i = 0; i < b.size(); ++i) { b.L[(size_t)i] *= g; b.R[(size_t)i] *= g; }
    }
    // limiter: 1.5 ms lookahead, -1 dBFS ceiling, 80 ms release
    const float ceil = dbToGain(-1.f);
    const int la = b.sr * 3 / 2000;
    std::vector<float> need((size_t)b.size(), 1.f);
    for (int i = 0; i < b.size(); ++i) {
        const float x = std::max(std::fabs(b.L[(size_t)i]), std::fabs(b.R[(size_t)i]));
        if (x > ceil) {
            const float g = ceil / x;
            for (int j = std::max(0, i - la); j <= i; ++j) need[(size_t)j] = std::min(need[(size_t)j], g);
        }
    }
    const float rel = std::exp(-1.f / (0.08f * b.sr));
    float g = 1.f;
    for (int i = 0; i < b.size(); ++i) {
        g = std::min(need[(size_t)i], rel * g + (1 - rel));
        b.L[(size_t)i] *= g; b.R[(size_t)i] *= g;
    }
    // trim trailing silence (keep at least 300 ms after the downbeat), then fade the last 150 ms
    int end = b.size();
    while (end > t.downbeat + b.sr * 3 / 10 && std::max(std::fabs(b.L[(size_t)end - 1]), std::fabs(b.R[(size_t)end - 1])) < dbToGain(-70.f)) --end;
    end = std::min(b.size(), end + b.sr / 20);
    b.resize(end);
    const int f = std::min(end / 4, b.sr * 15 / 100);
    for (int i = 0; i < f; ++i) {
        const float gg = (float)i / (float)f;
        b.L[(size_t)(end - 1 - i)] *= gg; b.R[(size_t)(end - 1 - i)] *= gg;
    }
}

} // namespace lb
