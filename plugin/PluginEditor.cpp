#include "PluginEditor.h"

namespace {
juce::String u8(const char* s) { return juce::String::fromUTF8(s); }
const juce::Colour kBg(0xff121317), kPanel(0xff1c1e24), kLine(0xff2c2f38), kText(0xffe8e8ea), kDim(0xff8a8d96), kAccent(0xffff7a3d), kWave(0xff6ec6ff);
} // namespace

// ------------------------------------------------------------------ slot
void LBTraEditor::SlotView::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat().reduced(2);
    g.setColour(hover ? kLine.brighter(0.3f) : kPanel);
    g.fillRoundedRectangle(r, 6);
    g.setColour(kLine);
    g.drawRoundedRectangle(r, 6, 1);
    const auto& s = ed.proc.slot(index);
    auto inner = getLocalBounds().reduced(10, 8);
    g.setColour(kDim);
    g.setFont(juce::FontOptions(12.f));
    g.drawText(u8("声音 ") + juce::String(index + 1), inner.removeFromTop(16), juce::Justification::left);
    if (!s.loaded) {
        g.setColour(kDim.withAlpha(0.7f));
        g.drawFittedText(u8("把音频拖到这里\n或点击选择"), inner, juce::Justification::centred, 2);
        return;
    }
    const auto& a = s.source.analysis;
    g.setColour(kText);
    g.setFont(juce::FontOptions(14.f, juce::Font::bold));
    g.drawText(s.file.getFileNameWithoutExtension(), inner.removeFromTop(20), juce::Justification::left, true);
    g.setFont(juce::FontOptions(12.f));
    g.setColour(kAccent);
    g.drawText(juce::String::fromUTF8(a.kindName().c_str()), inner.removeFromTop(17), juce::Justification::left, true);
    g.setColour(kDim);
    juce::String line = u8("形状 ") + juce::String::fromUTF8(a.shapeName().c_str()) + u8(" · ") + juce::String::fromUTF8(a.regName().c_str());
    g.drawText(line, inner.removeFromTop(17), juce::Justification::left, true);
    juce::String line2 = juce::String(a.duration, 2) + u8(" 秒");
    if (a.f0 > 0) line2 += u8(" · ") + juce::String::fromUTF8(a.noteName().c_str());
    g.drawText(line2, inner.removeFromTop(17), juce::Justification::left, true);
    g.setColour(kDim.withAlpha(0.6f));
    g.drawText(u8("右键清空"), inner.removeFromBottom(14), juce::Justification::right);
}

void LBTraEditor::SlotView::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) { ed.proc.clearSlot(index); repaint(); return; }
    chooser = std::make_unique<juce::FileChooser>(u8("选择音频"), juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
        if (fc.getResult().existsAsFile()) ed.loadInto(index, fc.getResult());
    });
}

void LBTraEditor::SlotView::filesDropped(const juce::StringArray& files, int, int) {
    hover = false;
    // several files at once fill this slot and the ones after it
    int i = index;
    for (auto& f : files) {
        if (i >= LBTraProcessor::kSlots) break;
        ed.loadInto(i++, juce::File(f));
    }
    repaint();
}

// ------------------------------------------------------------------ waveform
void LBTraEditor::Waveform::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat();
    g.setColour(kPanel);
    g.fillRoundedRectangle(r, 6);
    auto cur = proc.current();
    if (!cur || cur->audio.getNumSamples() == 0) {
        g.setColour(kDim);
        g.drawText(u8("放入声音后按“摇骰子”"), getLocalBounds(), juce::Justification::centred);
        return;
    }
    const auto& a = cur->audio;
    const int n = a.getNumSamples(), w = std::max(1, getWidth() - 16);
    const float mid = r.getCentreY(), h = r.getHeight() * 0.42f;
    g.setColour(kWave);
    for (int x = 0; x < w; ++x) {
        const int s0 = (int)((juce::int64)x * n / w), s1 = std::max(s0 + 1, (int)((juce::int64)(x + 1) * n / w));
        float peak = 0;
        for (int i = s0; i < s1 && i < n; ++i) peak = std::max(peak, std::max(std::fabs(a.getSample(0, i)), std::fabs(a.getSample(1, i))));
        g.drawVerticalLine(8 + x, mid - peak * h, mid + peak * h);
    }
    const float jx = 8.f + (float)cur->downbeat / (float)n * (float)w;
    g.setColour(kAccent);
    g.drawLine(jx, r.getY() + 4, jx, r.getBottom() - 4, 2.f);
    g.setFont(juce::FontOptions(12.f));
    g.drawText(u8("衔接点"), (int)jx + 4, 4, 60, 16, juce::Justification::left);
    g.setColour(kDim);
    g.drawText(juce::String(cur->bpm, 1) + " BPM", getLocalBounds().reduced(8, 4), juce::Justification::topRight);
}

// ------------------------------------------------------------------ editor
LBTraEditor::LBTraEditor(LBTraProcessor& p) : AudioProcessorEditor(p), proc(p) {
    for (int i = 0; i < LBTraProcessor::kSlots; ++i) {
        slots.push_back(std::make_unique<SlotView>(*this, i));
        addAndMakeVisible(*slots.back());
    }
    addAndMakeVisible(wave);
    title.setText("LBTra", juce::dontSendNotification);
    title.setFont(juce::FontOptions(22.f, juce::Font::bold));
    title.setColour(juce::Label::textColourId, kText);
    addAndMakeVisible(title);

    dice.setButtonText(u8("摇骰子"));
    dice.setColour(juce::TextButton::buttonColourId, kAccent);
    dice.setColour(juce::TextButton::textColourOffId, juce::Colours::black);
    dice.onClick = [this] { proc.rollDice(); };
    addAndMakeVisible(dice);
    audition.setButtonText(u8("试听"));
    audition.onClick = [this] { proc.auditionNow(); };
    addAndMakeVisible(audition);

    addCombo(rise, riseL, "rise", "上升");
    addCombo(fall, fallL, "fall", "下降");
    addCombo(style, styleL, "style", "风格");
    addCombo(finalEq, finalEqL, "finalEq", "最终 EQ");
    addCombo(mode, modeL, "mode", "模式");
    addCombo(loopBars, loopBarsL, "loopBars", "Loop 每几小节");

    for (auto* s : {&comp, &gain}) {
        s->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 18);
        s->setColour(juce::Slider::rotarySliderFillColourId, kAccent);
        addAndMakeVisible(*s);
    }
    comp.textFromValueFunction = [](double v) { return juce::String((int)std::lround(v * 100)) + "%"; };
    gain.textFromValueFunction = [](double v) { return juce::String(v, 1) + " dB"; };
    compAtt = std::make_unique<SliderAttachment>(proc.params, "comp", comp);
    gainAtt = std::make_unique<SliderAttachment>(proc.params, "gain", gain);
    for (auto [l, t] : {std::pair{&compL, "压缩量"}, std::pair{&gainL, "输出音量"}}) {
        l->setText(u8(t), juce::dontSendNotification);
        l->setJustificationType(juce::Justification::centred);
        l->setColour(juce::Label::textColourId, kDim);
        addAndMakeVisible(*l);
    }
    finalReverb.setButtonText(u8("最终混响"));
    finalReverb.setColour(juce::ToggleButton::textColourId, kText);
    reverbAtt = std::make_unique<ButtonAttachment>(proc.params, "finalReverb", finalReverb);
    addAndMakeVisible(finalReverb);

    status.setColour(juce::Label::textColourId, kDim);
    addAndMakeVisible(status);
    report.setMultiLine(true);
    report.setReadOnly(true);
    report.setScrollbarsShown(true);
    report.setColour(juce::TextEditor::backgroundColourId, kPanel);
    report.setColour(juce::TextEditor::textColourId, kDim);
    report.setColour(juce::TextEditor::outlineColourId, kLine);
    report.setFont(juce::FontOptions(12.f));
    addAndMakeVisible(report);

    proc.renderDone.addChangeListener(this);
    changeListenerCallback(nullptr);
    setResizable(true, true);
    setResizeLimits(820, 560, 1600, 1100);
    setSize(980, 640);
    startTimerHz(5);
}

LBTraEditor::~LBTraEditor() { proc.renderDone.removeChangeListener(this); }

void LBTraEditor::addCombo(juce::ComboBox& box, juce::Label& label, const char* id, const char* text) {
    auto* param = dynamic_cast<juce::AudioParameterChoice*>(proc.params.getParameter(id));
    if (param) box.addItemList(param->choices, 1);
    comboAtt.push_back(std::make_unique<ComboAttachment>(proc.params, id, box));
    label.setText(u8(text), juce::dontSendNotification);
    label.setColour(juce::Label::textColourId, kDim);
    addAndMakeVisible(box);
    addAndMakeVisible(label);
}

void LBTraEditor::loadInto(int index, const juce::File& f) {
    juce::String err;
    if (!proc.loadSlot(index, f, err)) status.setText(err, juce::dontSendNotification);
    slots[(size_t)index]->repaint();
}

void LBTraEditor::changeListenerCallback(juce::ChangeBroadcaster*) {
    auto cur = proc.current();
    report.setText(cur ? cur->report : juce::String(), false);
    wave.repaint();
    for (auto& s : slots) s->repaint();
}

void LBTraEditor::timerCallback() {
    juce::String s = proc.isRendering() ? u8("正在渲染……") : u8("就绪");
    s += u8("   宿主速度 ") + juce::String(proc.hostBpm(), 1) + " BPM";
    if ((int)*proc.params.getRawParameterValue("mode") == 0) s += u8("   Play：MIDI 音符触发，从上升开头播放");
    else s += u8("   Loop：走带时自动触发，衔接点落在小节线上");
    status.setText(s, juce::dontSendNotification);
}

void LBTraEditor::paint(juce::Graphics& g) { g.fillAll(kBg); }

void LBTraEditor::resized() {
    auto r = getLocalBounds().reduced(14);
    auto top = r.removeFromTop(34);
    title.setBounds(top.removeFromLeft(120));
    dice.setBounds(top.removeFromRight(130).reduced(2));
    top.removeFromRight(8);
    audition.setBounds(top.removeFromRight(90).reduced(2));
    r.removeFromTop(8);
    auto slotRow = r.removeFromTop(130);
    const int sw = slotRow.getWidth() / LBTraProcessor::kSlots;
    for (auto& s : slots) s->setBounds(slotRow.removeFromLeft(sw).reduced(3, 0));
    r.removeFromTop(10);
    wave.setBounds(r.removeFromTop(120));
    r.removeFromTop(10);
    auto controls = r.removeFromTop(92);
    auto knobs = controls.removeFromRight(200);
    auto k1 = knobs.removeFromLeft(100), k2 = knobs;
    compL.setBounds(k1.removeFromTop(18)); comp.setBounds(k1);
    gainL.setBounds(k2.removeFromTop(18)); gain.setBounds(k2);
    auto row1 = controls.removeFromTop(46), row2 = controls;
    auto place = [](juce::Rectangle<int>& row, juce::Label& l, juce::Component& c, int w) {
        auto cell = row.removeFromLeft(w).reduced(4, 2);
        l.setBounds(cell.removeFromTop(16));
        c.setBounds(cell);
    };
    place(row1, riseL, rise, 110); place(row1, fallL, fall, 110); place(row1, styleL, style, 110); place(row1, finalEqL, finalEq, 110);
    place(row2, modeL, mode, 110); place(row2, loopBarsL, loopBars, 130);
    finalReverb.setBounds(row2.removeFromLeft(130).reduced(4, 10).withTrimmedTop(10));
    r.removeFromTop(6);
    status.setBounds(r.removeFromBottom(22));
    report.setBounds(r);
}
