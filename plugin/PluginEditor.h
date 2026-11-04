#pragma once
#include "PluginProcessor.h"
#include <juce_gui_extra/juce_gui_extra.h>

class LBTraEditor : public juce::AudioProcessorEditor, private juce::ChangeListener, private juce::Timer {
public:
    explicit LBTraEditor(LBTraProcessor&);
    ~LBTraEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    class SlotView : public juce::Component, public juce::FileDragAndDropTarget {
    public:
        SlotView(LBTraEditor& e, int i) : ed(e), index(i) {}
        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
        bool isInterestedInFileDrag(const juce::StringArray&) override { return true; }
        void fileDragEnter(const juce::StringArray&, int, int) override { hover = true; repaint(); }
        void fileDragExit(const juce::StringArray&) override { hover = false; repaint(); }
        void filesDropped(const juce::StringArray& files, int, int) override;
    private:
        LBTraEditor& ed;
        int index;
        bool hover = false;
        std::unique_ptr<juce::FileChooser> chooser;
    };
    class Waveform : public juce::Component {
    public:
        explicit Waveform(LBTraProcessor& p) : proc(p) {}
        void paint(juce::Graphics&) override;
    private:
        LBTraProcessor& proc;
    };

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void loadInto(int index, const juce::File& f);
    void addCombo(juce::ComboBox& box, juce::Label& label, const char* paramId, const char* text);

    LBTraProcessor& proc;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    std::vector<std::unique_ptr<SlotView>> slots;
    Waveform wave{proc};
    juce::TextButton dice, audition;
    juce::ComboBox rise, fall, style, finalEq, mode, loopBars;
    juce::Label riseL, fallL, styleL, finalEqL, modeL, loopBarsL, compL, gainL, status, title;
    juce::Slider comp, gain;
    juce::ToggleButton finalReverb;
    juce::TextEditor report;
    std::vector<std::unique_ptr<ComboAttachment>> comboAtt;
    std::unique_ptr<SliderAttachment> compAtt, gainAtt;
    std::unique_ptr<ButtonAttachment> reverbAtt;
};
