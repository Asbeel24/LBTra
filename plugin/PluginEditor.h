// Minimal UI (from the producer's sketch): name bar on top, 5 sample rows on the left,
// dice + Inst/Loop + five switch-and-amount rows on the right, everything else behind "more".
#pragma once
#include "PluginProcessor.h"
#include <juce_gui_extra/juce_gui_extra.h>

class LBTraEditor : public juce::AudioProcessorEditor, private juce::ChangeListener, private juce::Timer {
public:
    explicit LBTraEditor(LBTraProcessor&);
    ~LBTraEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

    class Look : public juce::LookAndFeel_V4 {
    public:
        Look();
        void drawComboBox(juce::Graphics&, int w, int h, bool down, int, int, int, int, juce::ComboBox&) override;
        juce::Font getComboBoxFont(juce::ComboBox&) override;
        void drawPopupMenuBackground(juce::Graphics&, int w, int h) override;
        void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool, bool) override;
    };

private:
    class SampleRow;
    class Dice;
    class ModeSwitch;
    class SwitchRow;
    class ResultStrip;
    class MorePanel;
    class MoreButton;

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void loadInto(int index, const juce::File& f);
    juce::String resultName() const;

    LBTraProcessor& proc;
    Look look;
    std::vector<std::unique_ptr<SampleRow>> rows;
    std::unique_ptr<Dice> dice;
    std::unique_ptr<ModeSwitch> modeSwitch;
    std::vector<std::unique_ptr<SwitchRow>> switches;
    std::unique_ptr<ResultStrip> strip;
    std::unique_ptr<MorePanel> more;
    std::unique_ptr<MoreButton> moreButton;
    juce::String message; // last error / hint, shown in the name bar
    int messageTicks = 0;
};
