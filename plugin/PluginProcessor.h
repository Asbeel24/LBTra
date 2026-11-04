// LBTra plugin: 5 sound slots -> dice -> one rendered transition, played in sync with the host.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include "Engine.h"
#include <array>
#include <atomic>
#include <memory>

class LBTraProcessor : public juce::AudioProcessor, private juce::Timer, private juce::AudioProcessorValueTreeState::Listener {
public:
    static constexpr int kSlots = lb::kMaxLayers;

    struct Slot {
        juce::File file;
        bool loaded = false;
        lb::Source source;
    };

    // a finished render, ready for the audio thread
    struct Rendered {
        juce::AudioBuffer<float> audio; // at the host sample rate
        int downbeat = 0;               // samples from the start to the junction
        double bpm = 90.0;
        float riseBeats = 4.f, fallBeats = 2.f;
        juce::String report;
        uint64_t seed = 0;
    };

    LBTraProcessor();
    ~LBTraProcessor() override;

    // AudioProcessor
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    using AudioProcessor::processBlock;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "LBTra"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // message thread API (editor)
    bool loadSlot(int index, const juce::File& f, juce::String& error);
    void clearSlot(int index);
    const Slot& slot(int index) const { return slots[(size_t)index]; }
    void rollDice();                 // new seed, render
    void requestRender() { dirty = true; }
    bool isRendering() const { return rendering.load(); }
    std::shared_ptr<const Rendered> current() const;
    double hostBpm() const { return lastBpm.load(); }
    void auditionNow() { auditionRequest = true; }

    juce::AudioProcessorValueTreeState params;
    juce::ChangeBroadcaster renderDone; // editor listens (message thread)

private:
    void timerCallback() override;
    void parameterChanged(const juce::String&, float) override { dirty = true; }
    void startRender();
    static juce::AudioProcessorValueTreeState::ParameterLayout layout();

    juce::AudioFormatManager formats;
    std::array<Slot, kSlots> slots;
    uint64_t seed = 1;

    // render thread
    juce::ThreadPool pool{1};
    std::atomic<bool> rendering{false}, dirty{false};
    std::atomic<double> lastBpm{90.0};
    double renderedBpm = 0;

    // current render (swapped under a spin lock, never held across processing)
    mutable juce::SpinLock swapLock;
    std::shared_ptr<const Rendered> live;
    std::shared_ptr<const Rendered> retired[3];
    std::shared_ptr<bool> aliveFlag = std::make_shared<bool>(true);

    // playback state (audio thread)
    double sampleRate = 48000.0;
    int64_t playPos = -1;              // sample index into the rendered audio, -1 = idle
    std::atomic<bool> auditionRequest{false};
    bool auditioning = false;
    std::shared_ptr<const Rendered> playing; // keeps the buffer alive while it plays
};
