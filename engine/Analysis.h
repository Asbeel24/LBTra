#pragma once
#include "Core.h"
#include <string>

namespace lb {

enum class Kind { Tonal, Percussive, FX, Loop };
enum class Register { Low, Mid, High };
// Loudness shape over time: decides where a sound can sit in a transition.
enum class Shape { Rise, Fall, Hit, Sustain, Loop };

struct Analysis {
    std::string name;
    Kind kind = Kind::FX;
    Register reg = Register::Mid;
    Shape shape = Shape::Sustain;
    float halfEnergyRatio = 1; // energy of 2nd half / 1st half (audible part)
    float duration = 0;        // seconds
    float attackMs = 0;        // first audible frame -> loudest frame
    float peakPos = 0;         // 0..1, where the loudest point sits
    float f0 = 0;              // Hz, 0 if no stable pitch
    float pitchConfidence = 0; // fraction of loud frames with a clear pitch
    float pitchSpread = 0;     // semitone std-dev of the detected pitch
    float flatness = 0;        // 0 = tonal, 1 = white noise
    float centroid = 0;        // Hz
    int onsets = 0;
    float onsetRegularity = 0; // 0..1, 1 = perfectly even spacing
    bool rising = false;       // energy builds towards the end (riser)
    bool sharpAttack = false;
    int loudestSample = 0;     // index of the loudest 10 ms frame
    int firstAudible = 0;      // first sample above -45 dB of peak
    int lastAudible = 0;

    std::string kindName() const;    // Chinese label
    std::string regName() const;
    std::string shapeName() const;
    std::string noteName() const;
};

Analysis analyze(const Buffer& b, const std::string& name);

} // namespace lb
