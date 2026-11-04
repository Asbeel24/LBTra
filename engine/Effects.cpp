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
    if (std::fabs(b - 1.f / 6.f) < 1e-3f) return "1/16T";
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

// ------------------------------------------------------------------ Character delay
// Tape (wow/flutter, saturating, darker each repeat), Dub (resonant band in the loop, can build up),
// Digital (clean ping-pong) and Diffuse (allpass smear in the loop). L/R can run different times.
// The wet signal ducks under the dry sound so the echoes bloom in the gaps.
class Delay : public Effect {
public:
    enum Mode { Tape, Dub, Digital, Diffuse } mode;
    float beatsL, beatsR, fb, mix, lpHz, hpHz, wowDepthMs, wowHz, drive, duck;
    bool pingpong;
    Delay(Rng& r, const FxLimits& lim) {
        mode = (Mode)r.weighted({1.2f, 1.f, 0.7f, 0.8f});
        beatsL = r.pick<float>({0.25f, 0.5f, 0.75f, 1.f / 3.f, 1.f, 0.125f});
        beatsR = r.chance(0.4f) ? r.pick<float>({0.25f, 0.5f, 0.75f, 1.f / 3.f, 0.375f}) : beatsL;
        fb = mode == Dub ? r.range(0.6f, 0.92f) : r.range(0.3f, 0.8f);
        mix = r.range(0.3f, 0.7f);
        lpHz = mode == Tape ? r.logRange(1800.f, 6000.f) : r.logRange(3000.f, 12000.f);
        hpHz = lim.lowRegister ? r.logRange(200.f, 500.f) : r.logRange(100.f, 700.f);
        wowDepthMs = mode == Tape ? r.range(0.6f, 3.5f) : mode == Diffuse ? r.range(0.2f, 1.f) : 0.f;
        wowHz = r.range(0.3f, 1.6f);
        drive = mode == Tape || mode == Dub ? r.range(1.f, 3.f) : 1.f;
        duck = r.range(0.f, 0.8f);
        pingpong = mode == Digital || r.chance(0.35f);
    }
    void process(Buffer& b, const Timeline& t, const Region&) override {
        const int n = b.size();
        const float sr = (float)b.sr;
        const int dL = std::max(1, t.beats(beatsL)), dR = std::max(1, t.beats(beatsR));
        const int cap = std::max(dL, dR) + b.sr / 10 + 8;
        std::vector<float> ll((size_t)cap, 0.f), lr((size_t)cap, 0.f);
        Biquad lpl, lpr, hpl, hpr, bpl, bpr;
        lpl.set(Biquad::LP, lpHz, 0.707f, 0, sr); lpr = lpl;
        hpl.set(Biquad::HP, hpHz, 0.707f, 0, sr); hpr = hpl;
        // Dub: a resonant band that sweeps slowly inside the loop
        float bpPh = 0.f;
        struct Ap { std::vector<float> z; int i = 0; float g = 0.6f; float run(float x) { float y = z[(size_t)i]; float v = x + g * y; z[(size_t)i] = v; if (++i >= (int)z.size()) i = 0; return y - g * v; } };
        Ap apl[3], apr[3];
        const int apT[3] = {142, 379, 773};
        for (int k = 0; k < 3; ++k) { apl[k].z.assign((size_t)(apT[k] * b.sr / 44100 + 1), 0.f); apr[k].z.assign((size_t)((apT[k] + 37) * b.sr / 44100 + 1), 0.f); }
        auto readLine = [&](const std::vector<float>& x, int w, float d) {
            float pos = (float)w - d;
            while (pos < 0) pos += (float)cap;
            const int i0 = (int)pos % cap, i1 = (i0 + 1) % cap;
            const float fr = pos - std::floor(pos);
            return x[(size_t)i0] * (1 - fr) + x[(size_t)i1] * fr;
        };
        float env = 0;
        const float att = std::exp(-1.f / (0.004f * sr)), rel = std::exp(-1.f / (0.12f * sr));
        int w = 0;
        for (int i = 0; i < n; ++i) {
            const float wow = wowDepthMs * 0.001f * sr * (0.7f * std::sin(2.f * kPi * wowHz * (float)i / sr) + 0.3f * std::sin(2.f * kPi * 7.3f * (float)i / sr));
            float yl = readLine(ll, w, (float)dL + wow), yr = readLine(lr, w, (float)dR + wow * 0.9f);
            yl = hpl.process(lpl.process(yl)); yr = hpr.process(lpr.process(yr));
            if (mode == Dub) {
                if ((i & 63) == 0) {
                    bpPh += 64.f / sr * 0.15f;
                    const float f = 400.f * std::pow(8.f, 0.5f - 0.5f * std::cos(2.f * kPi * bpPh));
                    const float z1 = bpl.z1, z2 = bpl.z2, z3 = bpr.z1, z4 = bpr.z2;
                    bpl.set(Biquad::Peak, f, 2.5f, 8.f, sr); bpr = bpl;
                    bpl.z1 = z1; bpl.z2 = z2; bpr.z1 = z3; bpr.z2 = z4;
                }
                yl = bpl.process(yl); yr = bpr.process(yr);
            }
            if (mode == Diffuse) for (int k = 0; k < 3; ++k) { yl = apl[k].run(yl); yr = apr[k].run(yr); }
            if (drive > 1.01f) { yl = std::tanh(yl * drive) / drive * 1.2f; yr = std::tanh(yr * drive) / drive * 1.2f; }
            const float inL = b.L[(size_t)i], inR = b.R[(size_t)i];
            if (pingpong) { ll[(size_t)w] = 0.5f * (inL + inR) + fb * yr; lr[(size_t)w] = fb * yl; }
            else { ll[(size_t)w] = inL + fb * yl; lr[(size_t)w] = inR + fb * yr; }
            const float x = std::max(std::fabs(inL), std::fabs(inR));
            env = x > env ? att * env + (1 - att) * x : rel * env + (1 - rel) * x;
            const float dg = 1.f / (1.f + duck * 8.f * env);
            b.L[(size_t)i] = inL + mix * yl * dg;
            b.R[(size_t)i] = inR + mix * yr * dg;
            if (++w >= cap) w = 0;
        }
    }
    std::string describe() const override {
        static const char* modeN[] = {"磁带延迟", "Dub 延迟", "数字乒乓延迟", "扩散延迟"};
        std::string time = beatsL == beatsR ? beatsName(beatsL) : "左 " + beatsName(beatsL) + " / 右 " + beatsName(beatsR);
        std::string s = fmt("%s（%s，反馈 %d%%，湿 %d%%，低通 %.0fHz", modeN[mode], time.c_str(), (int)(fb * 100), (int)(mix * 100), lpHz);
        if (wowDepthMs > 0) s += fmt("，抖动 %.1fms", wowDepthMs);
        if (drive > 1.01f) s += fmt("，回声饱和 x%.1f", drive);
        if (mode == Dub) s += "，回路里有共振扫频";
        if (duck > 0.2f) s += fmt("，湿声闪避 %d%%", (int)(duck * 100));
        if (pingpong && mode != Digital) s += "，乒乓";
        return s + "）";
    }
};

// ------------------------------------------------------------------ Grain delay
// Grains are read out of a feedback delay line: each one has its own pitch (from a chord / octave
// set), length, direction and pan. With feedback the pitched grains climb or fall in a spiral.
class GrainDelay : public Effect {
public:
    float beats, fb, mix, density, jitterBeats, reverseProb, grainMs;
    std::vector<float> pitches;
    bool synced;
    uint64_t seed;
    GrainDelay(Rng& r, const FxLimits& lim) {
        beats = r.pick<float>({0.25f, 0.5f, 0.75f, 1.f, 1.f / 3.f});
        fb = r.range(0.2f, 0.75f);
        mix = r.range(0.35f, 0.8f);
        density = r.range(12.f, 45.f);
        jitterBeats = r.range(0.f, 0.25f);
        reverseProb = r.range(0.f, 0.5f);
        grainMs = r.range(35.f, 180.f);
        synced = r.chance(0.4f) && false; // rhythmic grain mode off (user 11/4)
        if (lim.allowPitch) pitches = r.pick<std::vector<float>>({{12.f}, {12.f, 0.f}, {7.f, 12.f}, {-12.f, 0.f}, {12.f, 19.f, 24.f}, {0.f, 5.f, 12.f}, {-5.f, 7.f}, {0.f, 0.1f, -0.1f}});
        else pitches = {0.f, 0.f, 12.f};
        seed = r.next();
    }
    void process(Buffer& b, const Timeline& t, const Region&) override {
        const int n = b.size();
        const float sr = (float)b.sr;
        Rng r(seed);
        const int d = std::max(1, t.beats(beats));
        std::vector<float> lineL((size_t)n, 0.f), lineR((size_t)n, 0.f), outL((size_t)n, 0.f), outR((size_t)n, 0.f);
        struct Grain { double pos; float ratio; int len, age; bool rev; float gl, gr; };
        std::vector<Grain> active;
        const int synStep = std::max(64, t.beats(0.25f));
        double nextSpawn = d;
        Biquad lpl, lpr; lpl.set(Biquad::LP, 9000.f, 0.707f, 0, sr); lpr = lpl;
        for (int i = 0; i < n; ++i) {
            if ((double)i >= nextSpawn) {
                Grain g;
                g.ratio = semisToRatio(r.pick(pitches));
                g.len = std::max(64, (int)(grainMs * 0.001f * sr * r.range(0.7f, 1.3f)));
                g.rev = r.chance(reverseProb);
                // read window [start, start + len*ratio] must already be written (< i)
                const double span = (double)g.len * (double)g.ratio;
                double start = (double)i - (double)d - r.uni() * (double)t.beats(jitterBeats);
                start = std::min(start, (double)i - span - 2.0);
                g.pos = std::max(0.0, start);
                g.age = 0;
                const float pan = r.range(-0.9f, 0.9f);
                g.gl = std::sqrt(0.5f * (1 - pan)); g.gr = std::sqrt(0.5f * (1 + pan));
                active.push_back(g);
                nextSpawn += synced ? (double)synStep * (r.chance(0.3f) ? 0.5 : 1.0) : (double)sr / density * r.range(0.5f, 1.5f);
            }
            float sl = 0, sr2 = 0;
            for (auto& g : active) {
                const float w = hann(g.age, g.len);
                const double p = g.rev ? g.pos + (double)(g.len - 1 - g.age) * g.ratio : g.pos + (double)g.age * g.ratio;
                sl += readCubic(lineL, p) * w * g.gl;
                sr2 += readCubic(lineR, p) * w * g.gr;
                ++g.age;
            }
            active.erase(std::remove_if(active.begin(), active.end(), [](const Grain& g) { return g.age >= g.len; }), active.end());
            sl = lpl.process(sl); sr2 = lpr.process(sr2);
            outL[(size_t)i] = sl; outR[(size_t)i] = sr2;
            lineL[(size_t)i] = b.L[(size_t)i] + fb * std::tanh(sl);
            lineR[(size_t)i] = b.R[(size_t)i] + fb * std::tanh(sr2);
        }
        float dry = 0, wet = 0;
        for (int i = 0; i < n; ++i) { dry += b.L[(size_t)i] * b.L[(size_t)i]; wet += outL[(size_t)i] * outL[(size_t)i]; }
        const float k = wet > 1e-12f ? std::min(4.f, std::sqrt(dry / wet)) : 1.f;
        for (int i = 0; i < n; ++i) { b.L[(size_t)i] += mix * k * outL[(size_t)i]; b.R[(size_t)i] += mix * k * outR[(size_t)i]; }
    }
    std::string describe() const override {
        std::string p;
        for (float v : pitches) p += (p.empty() ? "" : "/") + fmt("%+g", std::round(v * 10) / 10);
        return fmt("颗粒延迟（%s，反馈 %d%%，湿 %d%%，%s，颗粒 %.0fms，音高 %s，倒放 %d%%，抖动 %.2f 拍）", beatsName(beats).c_str(), (int)(fb * 100),
                   (int)(mix * 100), synced ? "颗粒按 1/16 拍子出" : fmt("%.0f 粒/秒", density).c_str(), grainMs, p.c_str(), (int)(reverseProb * 100), jitterBeats);
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

// ------------------------------------------------------------------ Crystal (reverse pitched splice echo)
// Splices of the delayed signal are played backwards and re-pitched; with feedback each pass goes
// through the pitch again, so the echoes climb into a shimmering cascade. Two overlapping streams
// (50% Hann) keep it smooth; splices alternate left/right.
class Crystal : public Effect {
public:
    float spliceMs, delayMs, semis, fb, mix, width;
    bool reverse;
    Crystal(Rng& r, const FxLimits& lim) {
        spliceMs = r.logRange(40.f, 350.f);
        delayMs = r.range(0.f, 400.f);
        semis = lim.allowPitch ? r.pick<float>({12.f, 12.f, 7.f, -12.f, 5.f, 19.f, 0.f, -5.f}) : 0.f;
        fb = r.range(0.35f, 0.85f);
        mix = r.range(0.4f, 0.85f);
        width = r.range(0.4f, 1.f);
        reverse = r.chance(0.8f);
    }
    void process(Buffer& b, const Timeline&, const Region&) override {
        const int n = b.size();
        const float sr = (float)b.sr;
        const int S = std::max(256, (int)(spliceMs * 0.001f * sr)) & ~1;
        const float ratio = semisToRatio(semis);
        const int D = std::max((int)(delayMs * 0.001f * sr), (int)std::ceil((float)S * ratio) + 2);
        std::vector<float> lineL = b.L, lineR = b.R, outL((size_t)n, 0.f), outR((size_t)n, 0.f);
        Biquad hl, hr; hl.set(Biquad::HP, 150.f, 0.707f, 0, sr); hr = hl;
        int k = 0;
        for (int T = 0; T < n; T += S / 2, ++k) {
            const int src = T - D;
            if (src < 0) continue;
            const float pl = (k & 1) ? 1.f - 0.5f * width : 1.f, pr = (k & 1) ? 1.f : 1.f - 0.5f * width;
            for (int i = 0; i < S && T + i < n; ++i) {
                const int j = reverse ? S - 1 - i : i;
                const double p = (double)src + (double)j * ratio;
                const float w = hann(i, S);
                const float yl = readCubic(lineL, p) * w, yr = readCubic(lineR, p) * w;
                outL[(size_t)(T + i)] += yl * pl;
                outR[(size_t)(T + i)] += yr * pr;
                lineL[(size_t)(T + i)] += fb * yl;
                lineR[(size_t)(T + i)] += fb * yr;
            }
        }
        float dry = 0, wet = 0;
        for (int i = 0; i < n; ++i) { outL[(size_t)i] = hl.process(outL[(size_t)i]); outR[(size_t)i] = hr.process(outR[(size_t)i]); dry += b.L[(size_t)i] * b.L[(size_t)i]; wet += outL[(size_t)i] * outL[(size_t)i]; }
        const float kk = wet > 1e-12f ? std::min(3.f, std::sqrt(dry / wet)) : 1.f;
        for (int i = 0; i < n; ++i) { b.L[(size_t)i] = b.L[(size_t)i] * (1.f - 0.4f * mix) + mix * kk * outL[(size_t)i]; b.R[(size_t)i] = b.R[(size_t)i] * (1.f - 0.4f * mix) + mix * kk * outR[(size_t)i]; }
    }
    std::string describe() const override {
        return fmt("结晶（%s切片 %.0fms，延迟 %.0fms，每次回声 %+d 半音并反复叠加，反馈 %d%%，湿 %d%%，左右交替）", reverse ? "倒放" : "", spliceMs, delayMs,
                   (int)semis, (int)(fb * 100), (int)(mix * 100));
    }
};

// ------------------------------------------------------------------ Chroma (harmoniser / chord resonator)
// Makes any sound (noise, FX) play a chord: either pitch-shifted voices stacked in the chord, or a
// bank of tuned comb resonators excited by the sound. The chord can change on the junction.
class Chroma : public Effect {
public:
    enum Mode { Harmonize, Resonate } mode;
    float rootHz, fbk, mix;
    std::vector<float> chordA, chordB;
    bool change;
    Chroma(Rng& r, const FxLimits& lim) {
        mode = r.chance(0.55f) ? Resonate : Harmonize;
        rootHz = lim.rootHz > 20.f ? lim.rootHz : 110.f * semisToRatio((float)r.irange(0, 7));
        const std::vector<std::vector<float>> chords = {{0, 3, 7, 10}, {0, 3, 7, 14}, {0, 4, 7, 11}, {0, 5, 7, 12}, {0, 7, 12, 15}, {0, 3, 7, 12}};
        chordA = r.pick(chords);
        change = r.chance(0.5f);
        // common hip-hop moves on the bar line: i -> VI, i -> iv, i -> bVII
        const float shift = r.pick<float>({-4.f, 5.f, -2.f, 3.f});
        chordB = chordA;
        if (change) for (float& v : chordB) v += shift;
        if (mode == Harmonize) for (auto* c : {&chordA, &chordB}) { auto& v = *c; v.erase(v.begin()); if (v.size() > 2) v.resize(2); }
        fbk = r.range(0.93f, 0.985f);
        mix = mode == Resonate ? r.range(0.35f, 0.8f) : r.range(0.3f, 0.6f);
    }
    void process(Buffer& b, const Timeline& t, const Region&) override {
        const int n = b.size();
        const float sr = (float)b.sr;
        Buffer wet(n, b.sr);
        const int xf = b.sr / 50;
        auto blendB = [&](int i) { return std::clamp((float)(i - t.downbeat + xf) / (float)(2 * xf), 0.f, 1.f) * (change ? 1.f : 0.f); };
        if (mode == Resonate) {
            // one comb per chord note, moved into 180..900 Hz, with damping in the loop
            const size_t voices = chordA.size();
            auto fold = [](float f) { while (f < 180.f) f *= 2.f; while (f > 900.f) f *= 0.5f; return f; };
            std::vector<std::vector<float>> zl(voices), zr(voices);
            std::vector<float> lpL(voices, 0.f), lpR(voices, 0.f);
            const int maxD = (int)(sr / 170.f) + 4;
            for (size_t v = 0; v < voices; ++v) { zl[v].assign((size_t)maxD, 0.f); zr[v].assign((size_t)maxD, 0.f); }
            Biquad pre[2]; pre[0].set(Biquad::HP, 250.f, 0.707f, 0, sr); pre[1] = pre[0];
            int w = 0;
            for (int i = 0; i < n; ++i) {
                const float xl = pre[0].process(b.L[(size_t)i]) * 0.25f, xr = pre[1].process(b.R[(size_t)i]) * 0.25f;
                const float bb = blendB(i);
                float yl = 0, yr = 0;
                for (size_t v = 0; v < voices; ++v) {
                    const float fa = fold(rootHz * semisToRatio(chordA[v])), fb2 = fold(rootHz * semisToRatio(chordB[v]));
                    const float f = fa * std::pow(fb2 / fa, bb);
                    const float d = sr / f * (v & 1 ? 1.003f : 0.997f);
                    auto rd = [&](const std::vector<float>& z) {
                        float pos = (float)w - d; while (pos < 0) pos += (float)maxD;
                        const int i0 = (int)pos % maxD, i1 = (i0 + 1) % maxD; const float fr = pos - std::floor(pos);
                        return z[(size_t)i0] * (1 - fr) + z[(size_t)i1] * fr;
                    };
                    const float ol = rd(zl[v]), orr = rd(zr[v]);
                    lpL[v] = 0.6f * ol + 0.4f * lpL[v]; lpR[v] = 0.6f * orr + 0.4f * lpR[v];
                    zl[v][(size_t)w] = xl + fbk * lpL[v];
                    zr[v][(size_t)w] = xr + fbk * lpR[v];
                    yl += ol; yr += orr;
                }
                wet.L[(size_t)i] = yl; wet.R[(size_t)i] = yr;
                if (++w >= maxD) w = 0;
            }
        } else {
            const size_t voices = chordA.size();
            for (size_t v = 0; v < voices; ++v) {
                PitchShifter pa[2], pb[2];
                const int win = b.sr * 45 / 1000;
                const float det = v & 1 ? 0.06f : -0.06f;
                for (auto& p : pa) p.init(win, semisToRatio(chordA[v] + det));
                for (auto& p : pb) p.init(win, semisToRatio(chordB[v] + det));
                const float pan = voices > 1 ? -0.6f + 1.2f * (float)v / (float)(voices - 1) : 0.f;
                const float gl = std::sqrt(0.5f * (1 - pan)) * 1.41421f, gr = std::sqrt(0.5f * (1 + pan)) * 1.41421f;
                for (int i = 0; i < n; ++i) {
                    const float bb = blendB(i);
                    const float l = pa[0].process(b.L[(size_t)i]) * (1 - bb) + pb[0].process(b.L[(size_t)i]) * bb;
                    const float rr = pa[1].process(b.R[(size_t)i]) * (1 - bb) + pb[1].process(b.R[(size_t)i]) * bb;
                    wet.L[(size_t)i] += l * gl * 0.7f; wet.R[(size_t)i] += rr * gr * 0.7f;
                }
            }
        }
        float dry = 0, we = 0;
        for (int i = 0; i < n; ++i) { dry += b.L[(size_t)i] * b.L[(size_t)i]; we += wet.L[(size_t)i] * wet.L[(size_t)i]; }
        const float k = we > 1e-12f ? std::min(8.f, std::sqrt(dry / we)) : 1.f;
        for (int i = 0; i < n; ++i) { b.L[(size_t)i] = b.L[(size_t)i] * (1.f - 0.5f * mix) + mix * k * wet.L[(size_t)i]; b.R[(size_t)i] = b.R[(size_t)i] * (1.f - 0.5f * mix) + mix * k * wet.R[(size_t)i]; }
    }
    std::string describe() const override {
        static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        const int midi = (int)std::lround(69.f + 12.f * std::log2(rootHz / 440.f));
        auto list = [](const std::vector<float>& c) { std::string s; for (float v : c) s += (s.empty() ? "" : ",") + fmt("%+d", (int)v); return s; };
        std::string s = fmt("%s（根音 %s，和弦音程 %s", mode == Resonate ? "和弦共鸣" : "和声叠加", names[((midi % 12) + 12) % 12], list(chordA).c_str());
        if (change) s += "，强拍换到 " + list(chordB);
        if (mode == Resonate) s += fmt("，共鸣 %d%%", (int)(fbk * 100));
        return s + fmt("，湿 %d%%）", (int)(mix * 100));
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
    bool freeze, synced = false;
    float stepBeats = 0.25f;
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
        if (lim.allowPitch && r.chance(0.5f)) pitches = r.pick<std::vector<float>>({{0.f, 12.f}, {0.f, 3.f, 7.f, 12.f}, {0.f, 7.f, 12.f, 19.f}, {-12.f, 0.f, 5.f}});
        synced = r.chance(0.4f) && false; // rhythmic grain mode off (user 11/4)
        stepBeats = r.pick<float>({0.25f, 0.125f, 1.f / 6.f});
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
        const double gridStep = (double)std::max(64, t.beats(stepBeats));
        // synced: grains on a tempo grid counted back from the junction, like a rhythmic resynth
        double tpos = synced ? (double)t.downbeat - std::floor((double)(t.downbeat - a) / gridStep) * gridStep : (double)a;
        for (; tpos < e; tpos += synced ? gridStep : step * r.range(0.5f, 1.5f)) {
            if (tpos < 0) continue;
            if (synced && !r.chance(0.75f)) continue;
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
                const float w = synced ? std::min(1.f, (float)i / 96.f) * std::exp(-4.f * (float)i / (float)len) : hann(i, len) * 0.7f;
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
        if (synced) return fmt("节奏粒子重组（%s每 %s 一粒，颗粒 %dms，散布 %.2f 拍，倒放 %d%%，音高 %s，湿 %d%%）", freeze ? "冻结，" : "", beatsName(stepBeats).c_str(),
                               grainMs, sprayBeats, (int)(reverseProb * 100), p.c_str(), (int)(mix * 100));
        return fmt("粒子云（%s密度 %.0f 粒/秒，颗粒 %dms，散布 %.2f 拍，倒放 %d%%，音高 %s，湿 %d%%）", freeze ? "冻结，" : "", density,
                   grainMs, sprayBeats, (int)(reverseProb * 100), p.c_str(), (int)(mix * 100));
    }
};

} // namespace

// ------------------------------------------------------------------ Shaper (ShaperBox-style)
// 1-3 modules, each driven by a tempo-synced shape (LFO). A module can work on one band only.
class Shaper : public Effect {
public:
    enum Mod { Volume, Filter, Pan, Time, Crush, Width, Noise, ModCount };
    enum Wave { SawUp, SawDown, Tri, Square, Pump, Steps, Sine, WaveCount };
    enum Band { Full, Low, Mid, High };
    struct Module {
        Mod mod = Volume;
        Wave wave = Sine;
        Band band = Full;
        float rate = 0.5f;      // beats per cycle
        bool accel = false;     // rate speeds up 4x into the junction
        float afterMul = 1.f;   // rate multiplier after the junction (2 = half time)
        float depth = 0.8f;
        int filterType = 0;     // 0 LP 1 HP 2 BP
        float fLo = 200, fHi = 8000, q = 1;
        int timeMode = 0;       // 0 half-speed, 1 scratch, 2 rewind tail
        uint64_t seed = 1;
    };
    std::vector<Module> mods;

    Shaper(Rng& r, const FxLimits& lim) {
        const int n = 1 + r.weighted({0.45f, 0.4f, 0.15f});
        std::vector<float> mw{1.2f, 1.4f, 0.8f, 0.9f, 0.7f, 0.5f, 0.6f};
        if (lim.lowRegister) { mw[Pan] = 0; mw[Width] = 0; mw[Noise] = 0.2f; mw[Crush] *= 0.5f; }
        if (!lim.allowPitch) mw[Time] *= 0.3f;
        for (int k = 0; k < n; ++k) {
            Module m;
            m.mod = (Mod)r.weighted(mw);
            mw[m.mod] = 0; // no two modules of the same kind
            m.wave = (Wave)r.irange(0, WaveCount - 1);
            m.rate = r.pick<float>({0.25f, 0.5f, 0.5f, 1.f, 1.f / 3.f, 0.125f, 2.f});
            m.accel = r.chance(0.35f);
            m.afterMul = r.pick<float>({1.f, 1.f, 2.f, 0.5f});
            m.depth = r.range(0.5f, 1.f);
            m.band = (m.mod == Time || m.mod == Pan || m.mod == Width || m.mod == Noise || r.chance(0.55f)) ? Full
                     : (Band)r.irange(1, 3);
            if (lim.lowRegister && m.band == Full && (m.mod == Volume || m.mod == Crush) && r.chance(0.5f)) m.band = Mid;
            m.filterType = r.weighted({0.5f, 0.3f, 0.2f});
            m.fLo = r.logRange(80.f, 600.f);
            m.fHi = r.logRange(2500.f, 14000.f);
            m.q = r.logRange(0.7f, 7.f);
            m.timeMode = r.irange(0, 2);
            if (m.mod == Time) { m.rate = r.pick<float>({0.5f, 1.f, 0.25f}); m.accel = false; }
            m.seed = r.next();
            mods.push_back(m);
        }
    }

    static float shape(Wave w, float p, int cycle, uint64_t seed) {
        switch (w) {
        case SawUp: return p;
        case SawDown: return 1.f - p;
        case Tri: return 1.f - std::fabs(2.f * p - 1.f);
        case Square: return p < 0.5f ? 1.f : 0.f;
        case Pump: return 1.f - std::exp(-p * 7.f);
        case Steps: {
            uint64_t z = seed + (uint64_t)(cycle + 1000) * 0x9E3779B97F4A7C15ull;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; z ^= z >> 31;
            return (float)(z >> 40) / 16777216.f;
        }
        case Sine: default: return 0.5f - 0.5f * std::cos(2.f * kPi * p);
        }
    }

    // phase (in cycles) for every sample, grid-locked to the junction
    std::vector<double> phases(const Module& m, const Timeline& t, int n) const {
        std::vector<double> ph((size_t)n);
        const double beat = (double)t.beat();
        const double cyc = m.rate * beat;
        // before the junction: count cycles backwards from the junction so a cycle always ends on it
        double acc = 0;
        for (int i = std::min(n, t.downbeat) - 1; i >= 0; --i) {
            double rate = 1.0 / cyc;
            if (m.accel) rate *= 1.0 + 3.0 * std::max(0.0, 1.0 - (double)(t.downbeat - i) / (double)std::max(1, t.downbeat));
            acc -= rate;
            ph[(size_t)i] = acc;
        }
        acc = 0;
        for (int i = t.downbeat; i < n; ++i) { ph[(size_t)i] = acc; acc += 1.0 / (cyc * m.afterMul); }
        return ph;
    }

    void process(Buffer& b, const Timeline& t, const Region& r) override {
        const int n = b.size();
        const int a = std::clamp(r.start, 0, n);
        const float sr = (float)b.sr;
        const int protect = t.beats(0.125f); // the junction itself stays clean so the peak survives
        for (const Module& m : mods) {
            const auto ph = phases(m, t, n);
            auto val = [&](int i) {
                const double c = ph[(size_t)i], fl = std::floor(c);
                return shape(m.wave, (float)(c - fl), (int)fl, m.seed);
            };
            auto amt = [&](int i) { // depth, faded out right around the junction
                const int d = std::abs(i - t.downbeat);
                return m.depth * std::clamp((float)d / (float)std::max(1, protect), 0.f, 1.f);
            };
            // band split: work on `part`, keep `rest`
            Buffer part = b, rest(n, b.sr);
            if (m.band != Full) {
                const float f1 = m.band == Low ? 0.f : m.band == Mid ? 200.f : 3000.f;
                const float f2 = m.band == Low ? 200.f : m.band == Mid ? 3000.f : 0.f;
                for (int ch = 0; ch < 2; ++ch) {
                    auto& x = ch ? b.R : b.L;
                    auto& p = ch ? part.R : part.L;
                    auto& q = ch ? rest.R : rest.L;
                    Biquad h1, h2, l1, l2;
                    if (f1 > 0) { h1.set(Biquad::HP, f1, 0.7071f, 0, sr); h2 = h1; }
                    if (f2 > 0) { l1.set(Biquad::LP, f2, 0.7071f, 0, sr); l2 = l1; }
                    for (int i = 0; i < n; ++i) {
                        float y = x[(size_t)i];
                        if (f1 > 0) y = h2.process(h1.process(y));
                        if (f2 > 0) y = l2.process(l1.process(y));
                        p[(size_t)i] = y;
                        q[(size_t)i] = x[(size_t)i] - y; // complementary (phase-approximate) remainder
                    }
                }
            }
            switch (m.mod) {
            case Volume:
                for (int i = a; i < n; ++i) { const float g = 1.f - amt(i) * (1.f - val(i)); part.L[(size_t)i] *= g; part.R[(size_t)i] *= g; }
                break;
            case Filter: {
                Biquad fl[2];
                const Biquad::Type ty = m.filterType == 0 ? Biquad::LP : m.filterType == 1 ? Biquad::HP : Biquad::BP;
                for (int i = a; i < n; ++i) {
                    if (((i - a) & 31) == 0) {
                        const float v = m.filterType == 1 ? 1.f - val(i) : val(i);
                        const float d = amt(i);
                        // depth 0 -> filter fully open (LP high / HP low)
                        const float open = m.filterType == 1 ? 0.f : 1.f;
                        const float x = open + (v - open) * d;
                        const float f = m.fLo * std::pow(m.fHi / m.fLo, x);
                        const float q = ty == Biquad::BP ? std::max(0.7f, m.q * 0.5f) : 0.707f + (m.q - 0.707f) * d;
                        for (auto& f0 : fl) { const float z1 = f0.z1, z2 = f0.z2; f0.set(ty, f, q, 0, sr); f0.z1 = z1; f0.z2 = z2; }
                    }
                    float yl = fl[0].process(part.L[(size_t)i]), yr = fl[1].process(part.R[(size_t)i]);
                    if (ty == Biquad::BP) { const float d = amt(i); yl = part.L[(size_t)i] * (1 - d) + yl * d * 1.6f; yr = part.R[(size_t)i] * (1 - d) + yr * d * 1.6f; }
                    part.L[(size_t)i] = yl; part.R[(size_t)i] = yr;
                }
                break;
            }
            case Pan:
                for (int i = a; i < n; ++i) {
                    const float p = (2.f * val(i) - 1.f) * amt(i);
                    const float gl = std::sqrt(0.5f * (1 - p)) * 1.41421f, gr = std::sqrt(0.5f * (1 + p)) * 1.41421f;
                    part.L[(size_t)i] *= gl; part.R[(size_t)i] *= gr;
                }
                break;
            case Width:
                for (int i = a; i < n; ++i) {
                    const float mid = 0.5f * (part.L[(size_t)i] + part.R[(size_t)i]), side = 0.5f * (part.L[(size_t)i] - part.R[(size_t)i]);
                    const float w = 1.f + amt(i) * (2.f * val(i) - 1.f); // 0 (mono) .. 2 (wide)
                    part.L[(size_t)i] = mid + side * w; part.R[(size_t)i] = mid - side * w;
                }
                break;
            case Crush: {
                float holdL = 0, holdR = 0; int cnt = 0;
                for (int i = a; i < n; ++i) {
                    const float k = amt(i) * (1.f - val(i));
                    const int hold = 1 + (int)std::lround(k * 15.f);
                    if (cnt <= 0) { holdL = part.L[(size_t)i]; holdR = part.R[(size_t)i]; cnt = hold; }
                    --cnt;
                    const float steps = std::pow(2.f, 16.f - 12.f * k);
                    part.L[(size_t)i] = std::round(holdL * steps) / steps;
                    part.R[(size_t)i] = std::round(holdR * steps) / steps;
                }
                break;
            }
            case Noise: {
                // a noise layer that breathes with the shape, scaled to the sound's own level
                Rng nr(m.seed);
                Biquad hl, hr; hl.set(Biquad::HP, 3000.f, 0.7f, 0, sr); hr = hl;
                float env = 0;
                const float att = std::exp(-1.f / (0.005f * sr)), rel = std::exp(-1.f / (0.15f * sr));
                for (int i = a; i < n; ++i) {
                    const float x = std::max(std::fabs(part.L[(size_t)i]), std::fabs(part.R[(size_t)i]));
                    env = x > env ? att * env + (1 - att) * x : rel * env + (1 - rel) * x;
                    const float g = 0.15f * env * amt(i) * val(i);
                    part.L[(size_t)i] += hl.process(nr.range(-1.f, 1.f)) * g;
                    part.R[(size_t)i] += hr.process(nr.range(-1.f, 1.f)) * g;
                }
                break;
            }
            case Time: {
                // re-read each cycle: half speed, scratch (forward then back) or rewind (second half reversed)
                const Buffer src = part;
                for (int i = a; i < n; ++i) {
                    const double c = ph[(size_t)i], fl = std::floor(c);
                    const double p = c - fl;
                    double cycLen = (double)t.beats(m.rate) * (i >= t.downbeat ? m.afterMul : 1.0);
                    const double cycStart = (double)i - p * cycLen;
                    double rel;
                    if (m.timeMode == 0) rel = p * 0.5;                                // half speed
                    else if (m.timeMode == 1) rel = 0.5 * (1.0 - std::fabs(2.0 * p - 1.0)) + 0.25 * p; // scratch
                    else rel = p < 0.5 ? p : 1.0 - p;                                  // rewind
                    const float d = amt(i);
                    const double pos = (double)i * (1.0 - d) + (cycStart + rel * cycLen) * d;
                    // 3 ms crossfade at each cycle start against the straight read
                    const double fade = std::min(1.0, p * cycLen / (0.003 * sr));
                    part.L[(size_t)i] = (float)(readCubic(src.L, pos) * fade + src.L[(size_t)i] * (1.0 - fade));
                    part.R[(size_t)i] = (float)(readCubic(src.R, pos) * fade + src.R[(size_t)i] * (1.0 - fade));
                }
                break;
            }
            default: break;
            }
            if (m.band == Full) b = std::move(part);
            else for (int i = 0; i < n; ++i) { b.L[(size_t)i] = part.L[(size_t)i] + rest.L[(size_t)i]; b.R[(size_t)i] = part.R[(size_t)i] + rest.R[(size_t)i]; }
        }
    }

    std::string describe() const override {
        static const char* modN[] = {"音量", "滤波", "声像", "变速", "压碎", "宽度", "噪声"};
        static const char* waveN[] = {"锯齿上", "锯齿下", "三角", "方波", "泵", "随机阶梯", "正弦"};
        static const char* bandN[] = {"", "（只动低频）", "（只动中频）", "（只动高频）"};
        static const char* timeN[] = {"半速", "搓碟", "倒带"};
        static const char* fN[] = {"低通", "高通", "带通"};
        std::string s = "节奏塑形器（";
        for (size_t k = 0; k < mods.size(); ++k) {
            const Module& m = mods[k];
            if (k) s += "；";
            s += std::string(modN[m.mod]);
            if (m.mod == Time) s += std::string("·") + timeN[m.timeMode];
            if (m.mod == Filter) s += fmt("·%s %.0f–%.0fHz Q%.1f", fN[m.filterType], m.fLo, m.fHi, m.q);
            s += fmt(" %s 形状，每 %s 一次，深度 %.0f%%", waveN[m.wave], beatsName(m.rate).c_str(), m.depth * 100);
            if (m.accel) s += "，越近强拍越快";
            if (m.afterMul == 2.f) s += "，强拍后半速";
            if (m.afterMul == 0.5f) s += "，强拍后加倍";
            s += bandN[m.band];
        }
        return s + "）";
    }
};

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
    case FxType::Shaper: return "节奏塑形器";
    case FxType::GrainDelay: return "颗粒延迟";
    case FxType::Chroma: return "和声/共鸣";
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
    case FxType::Shaper: return std::make_unique<Shaper>(rng, lim);
    case FxType::GrainDelay: return std::make_unique<GrainDelay>(rng, lim);
    case FxType::Chroma: return std::make_unique<Chroma>(rng, lim);
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
    std::string s = multiband ? fmt("多段压缩（3 段，180Hz / 3.5kHz 分频，比率 %.1f:1，压缩量 %d%%%s）", mbRatio, (int)(compAmount * 100),
                                    compAmount > 0.01f ? "，含向上压缩和整体粘合" : "")
                              : std::string("整体 EQ（25Hz 低切）");
    s += fmt("；总 EQ（低架 %+.1fdB，高架 %+.1fdB）", tiltLowDb, tiltHighDb);
    if (spaceMix > 0.01f) s += fmt("；总混响（衰减 %.1fs，湿 %d%%）", spaceDecay, (int)(spaceMix * 100));
    else s += "；总混响关";
    return s;
}

namespace {
void compressBand(std::vector<float>& l, std::vector<float>& r, float ratio, int sr, float amount = 0.f, float attackMs = 5.f, float releaseMs = 80.f) {
    // threshold sits 6 dB (+ up to 12 dB with amount) under the band's loudest 50 ms;
    // with amount > 0 quiet parts are also lifted (upward compression, OTT-style)
    float pk = 0;
    const int w = sr / 20;
    for (int s = 0; s + w < (int)l.size(); s += w / 2) pk = std::max(pk, std::sqrt(0.5f * (rms(l, s, s + w) * rms(l, s, s + w) + rms(r, s, s + w) * rms(r, s, s + w))));
    if (pk < 1e-6f) return;
    const float depth = 6.f + 12.f * amount;
    const float thr = gainToDb(pk) - depth;
    const float upThr = thr - 18.f, upMax = 10.f * amount;
    const float at = std::exp(-1.f / (attackMs * 0.001f * sr)), rl = std::exp(-1.f / (releaseMs * 0.001f * sr));
    float env = 0, makeup = depth * (1.f - 1.f / ratio) * 0.5f;
    for (size_t i = 0; i < l.size(); ++i) {
        const float x = std::max(std::fabs(l[i]), std::fabs(r[i]));
        env = x > env ? at * env + (1 - at) * x : rl * env + (1 - rl) * x;
        const float db = gainToDb(env);
        const float over = db - thr;
        float gr = over > 0 ? -over * (1.f - 1.f / ratio) : 0.f;
        if (upMax > 0 && db < thr && db > upThr - 30.f) gr += std::min(upMax, (thr - std::max(db, upThr)) * 0.5f);
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
        compressBand(lowL, lowR, s.mbRatio, b.sr, s.compAmount, 10.f, 120.f);
        compressBand(midL, midR, s.mbRatio, b.sr, s.compAmount);
        compressBand(highL, highR, s.mbRatio, b.sr, s.compAmount, 2.f, 60.f);
        for (int i = 0; i < b.size(); ++i) {
            b.L[(size_t)i] = lowL[(size_t)i] + midL[(size_t)i] + highL[(size_t)i];
            b.R[(size_t)i] = lowR[(size_t)i] + midR[(size_t)i] + highR[(size_t)i];
        }
        // full-band glue after the split bands
        if (s.compAmount > 0.01f) compressBand(b.L, b.R, 2.f + 2.f * s.compAmount, b.sr, 0.f, 10.f, 150.f);
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

void finalizeLoudness(Buffer& b, const Timeline& t, bool fixedLength) {
    // level: loudest 300 ms window around the transition brought to -10 dBFS RMS
    const int w = b.sr * 3 / 10;
    float loud = 0;
    for (int s = 0; s + w <= b.size(); s += w / 3) loud = std::max(loud, stereoRms(b, s, s + w));
    if (loud > 1e-6f) {
        float g = dbToGain(-10.f) / loud;
        // long, mostly quiet files (2 bar builds) also get a body-loudness floor: gated mean RMS of
        // 50 ms frames to -17 dBFS, at most +6 dB above the peak-window gain; the limiter catches the rest
        const int fw = b.sr / 20;
        double acc = 0; int n = 0;
        for (int s = 0; s + fw <= b.size(); s += fw) {
            const float e = stereoRms(b, s, s + fw);
            if (e > loud * dbToGain(-30.f)) { acc += (double)e * e; ++n; }
        }
        if (n > 0) {
            const float body = (float)std::sqrt(acc / n);
            g = std::max(g, std::min(dbToGain(-17.f) / body, g * dbToGain(6.f)));
        }
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
    if (fixedLength) {
        const int f = std::min(b.size() / 4, b.sr / 100);
        for (int i = 0; i < f; ++i) { const float gg = (float)i / (float)f; b.L[(size_t)(b.size() - 1 - i)] *= gg; b.R[(size_t)(b.size() - 1 - i)] *= gg; }
        return;
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
