// Per-track effects (max 3 per track), envelopes, bus and output processors.
// Every processor works on a whole timeline buffer at once (offline render).
#pragma once
#include "Core.h"
#include <memory>
#include <string>

namespace lb {

struct Timeline {
    int sr = 48000;
    float bpm = 90.f;
    int downbeat = 0;    // sample index of the bar line the transition lands on
    int length = 0;      // total samples
    int beat() const { return (int)std::lround(60.0 * sr / bpm); }
    int beats(float b) const { return (int)std::lround(b * 60.0 * sr / bpm); }
};

// The audible span of one track on the timeline.
struct Region {
    int start = 0, end = 0;
};

class Effect {
public:
    virtual ~Effect() = default;
    virtual void process(Buffer& b, const Timeline& t, const Region& r) = 0;
    virtual std::string describe() const = 0; // Chinese, with parameters
};

enum class FxType { Reverb, ReverseReverb, Delay, Eq, Distortion, Crystal, Stutter, Granular, Count };
const char* fxName(FxType t);

// Parameter ranges come from the dice; `rng` is consumed only inside make().
struct FxLimits {
    float maxReverbSec = 6.f;
    float maxDriveDb = 24.f;
    bool lowRegister = false;
    bool allowPitch = true;
};
std::unique_ptr<Effect> makeEffect(FxType t, Rng& rng, const FxLimits& lim, const Timeline& tl, const Region& r);

// ---- shared building blocks
void freeverb(const Buffer& in, Buffer& wet, float decaySec, float damp, float width, int predelay);

// ---- envelopes from the pre-generated curve library
struct EnvelopeChoice {
    enum Kind { None, Swell, Pump, LowpassOpen, HighpassRise, TapeStop, Dive, FadeOut } kind = None;
    std::string describe() const;
};
void applyEnvelope(const EnvelopeChoice& e, Buffer& b, const Timeline& t, const Region& r);

// ---- bus
struct BusSettings {
    bool multiband = true;        // else gentle EQ
    float mbRatio = 3.f;
    float compAmount = 0.f;       // 0..1: deeper threshold, higher ratio, upward compression (OTT-style), glue comp
    float tiltLowDb = 0, tiltHighDb = 0;
    float spaceMix = 0.f, spaceDecay = 1.5f;
    std::string describe() const;
};
void processBus(Buffer& b, const BusSettings& s, const Timeline& t);

// ---- output options the user can toggle
struct OutputOptions {
    bool gate = false;
    float gateStepBeats = 0.25f;
    uint32_t gatePattern = 0xFFFF; // 16 steps, bit 0 = first step
    bool finalReverb = false;
    float finalReverbMix = 0.25f;
    enum EqMode { EqOff, Telephone, Darken } finalEq = EqOff;
    std::string describe() const;
};
void processOutput(Buffer& b, const OutputOptions& o, const Timeline& t);

// Loudness to a common level + peak limiter at -1 dBFS + tail fade.
// fixedLength: keep the buffer length (stage 2 timing rule) instead of trimming the tail.
void finalizeLoudness(Buffer& b, const Timeline& t, bool fixedLength = false);

} // namespace lb
