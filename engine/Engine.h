// The dice: turns up to 5 analysed sounds + a seed into one transition sample.
#pragma once
#include "Analysis.h"
#include "Effects.h"
#include <optional>

namespace lb {

enum class Style { Neutral, Dirty, Ambient, Fragment };
const char* styleName(Style s);

struct Source {
    Buffer audio;      // at the engine sample rate
    Analysis analysis;
};

struct RenderRequest {
    uint64_t seed = 1;
    Style style = Style::Neutral;
    float lengthBeats = 4.f;   // 0.5 = 1/8, 1 = 1/4, 2 = 1/2, 4 = 1 bar, 8 = 2 bar
    float bpm = 90.f;
    std::optional<OutputOptions> forcedOutput; // user toggles; empty = let the dice choose
    bool arrangeOnly = false;  // stage 1: arrangement only, no effects / envelopes / bus colour
    // stage 2 timing rule: rise = lengthBeats (start -> junction), fall = after the junction.
    // fallBeats > 0 fixes the file to exactly rise + fall; 0 keeps the free tail.
    float fallBeats = 0.f;
    std::optional<float> compAmount; // 0..1 bus compression; empty = dice
};

struct RenderResult {
    Buffer audio;
    int downbeatSample = 0;
    std::string report; // Chinese, markdown
};

constexpr int kMaxLayers = 5;
constexpr int kMaxFxPerTrack = 3;

RenderResult renderTransition(const std::vector<Source>& sources, const RenderRequest& req);

} // namespace lb
