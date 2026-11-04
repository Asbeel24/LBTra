// Headless check of the plugin's host sync: Loop mode must put the junction on the bar line.
//   lbtra_plugin_test a.wav b.wav ...
#include "../PluginProcessor.h"
#include <cstdio>
#include <thread>

struct FakePlayHead : juce::AudioPlayHead {
    double bpm = 120, ppq = 0;
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo p;
        p.setBpm(bpm);
        p.setPpqPosition(ppq);
        p.setIsPlaying(playing);
        p.setTimeSignature(TimeSignature{4, 4});
        return p;
    }
};

static int runTest(int argc, char** argv) {
    std::unique_ptr<juce::AudioProcessor> base(createPluginFilter());
    auto& p = dynamic_cast<LBTraProcessor&>(*base);
    const double sr = 44100;
    const int block = 512;
    FakePlayHead ph;
    p.setPlayHead(&ph);
    p.prepareToPlay(sr, block);
    for (int i = 1; i < argc && i <= LBTraProcessor::kSlots; ++i) {
        juce::String err;
        if (!p.loadSlot(i - 1, juce::File(argv[i]), err)) { std::printf("load failed: %s\n", err.toRawUTF8()); return 1; }
    }
    p.params.getParameter("mode")->setValueNotifyingHost(1.f);      // Loop
    p.params.getParameter("loopBars")->setValueNotifyingHost(1.f / 3.f); // "2"
    // one silent block so the processor learns the tempo, then wait for the render
    juce::AudioBuffer<float> buf(2, block);
    juce::MidiBuffer midi;
    p.processBlock(buf, midi);
    p.rollDice();
    juce::Thread::sleep(300);
    for (int k = 0; k < 600 && (!p.current() || p.isRendering()); ++k) juce::Thread::sleep(50);
    auto cur = p.current();
    if (!cur) { std::printf("no render\n"); return 1; }
    std::printf("render: %d samples, junction at %d (%.3f s), bpm %.1f\n", cur->audio.getNumSamples(), cur->downbeat, cur->downbeat / sr, cur->bpm);

    // 10 bars at 120 BPM, 4/4: junctions expected at ppq 8, 16, 24, 32
    const double spq = sr * 60.0 / ph.bpm;
    const int total = (int)(40 * spq);
    std::vector<float> out((size_t)total, 0.f);
    for (int pos = 0; pos < total; pos += block) {
        ph.ppq = pos / spq;
        buf.clear();
        p.processBlock(buf, midi);
        for (int i = 0; i < block && pos + i < total; ++i) out[(size_t)(pos + i)] = std::max(std::fabs(buf.getSample(0, i)), std::fabs(buf.getSample(1, i)));
    }
    // loudest 10 ms frame inside each 2-bar window, compared with the bar line
    int bad = 0;
    for (int bar = 1; bar <= 4; ++bar) {
        const int J = (int)(bar * 8 * spq);
        const int a = std::max(0, J - (int)(4 * spq)), b = std::min(total, J + (int)(2 * spq));
        const int h = (int)(sr / 100);
        double best = 0; int bestAt = a;
        for (int s = a; s + h <= b; s += h / 2) {
            double e = 0;
            for (int i = s; i < s + h; ++i) e += out[(size_t)i] * out[(size_t)i];
            if (e > best) { best = e; bestAt = s + h / 2; }
        }
        const double offMs = (bestAt - J) / sr * 1000;
        std::printf("bar line at ppq %d: loudest point %+.1f ms from it\n", bar * 8, offMs);
        if (std::fabs(offMs) > 25) ++bad;
    }
    // stop transport: must go silent
    ph.playing = false;
    buf.clear();
    p.processBlock(buf, midi);
    std::printf("stopped block peak %.4f\n", buf.getMagnitude(0, block));
    p.setPlayHead(nullptr);
    return bad ? 1 : 0;
}

int main(int argc, char** argv) {
    // the processor's render timer runs on the message thread, so the test itself runs on a worker
    juce::ScopedJuceInitialiser_GUI init;
    int result = 1;
    std::thread t([&] {
        result = runTest(argc, argv);
        juce::MessageManager::getInstance()->stopDispatchLoop();
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
    t.join();
    return result;
}
