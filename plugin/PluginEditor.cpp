#include "PluginEditor.h"

namespace {
juce::String u8(const char* s) { return juce::String::fromUTF8(s); }
const juce::Colour kBg(0xff0e0e10), kPanel(0xff151518), kLine(0xff26262c), kText(0xffececef), kDim(0xff7c7c85), kFaint(0xff3a3a42),
    kAccent(0xffff6a3d);
juce::Font font(float h, bool bold = false) { return juce::Font(juce::FontOptions(h, bold ? juce::Font::bold : juce::Font::plain)); }
} // namespace

// ------------------------------------------------------------------ look
LBTraEditor::Look::Look() {
    setColour(juce::ComboBox::textColourId, kText);
    setColour(juce::PopupMenu::backgroundColourId, kPanel);
    setColour(juce::PopupMenu::textColourId, kText);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, kLine);
    setColour(juce::PopupMenu::highlightedTextColourId, kText);
    setColour(juce::TextEditor::backgroundColourId, kBg);
    setColour(juce::TextEditor::textColourId, kDim);
    setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::ScrollBar::thumbColourId, kFaint);
}
void LBTraEditor::Look::drawComboBox(juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox&) {
    g.setColour(kLine);
    g.drawLine(0.f, (float)h - 0.5f, (float)w, (float)h - 0.5f, 1.f);
    juce::Path p;
    const float cx = (float)w - 8.f, cy = (float)h * 0.5f;
    p.addTriangle(cx - 3.5f, cy - 2.f, cx + 3.5f, cy - 2.f, cx, cy + 2.5f);
    g.setColour(kDim);
    g.fillPath(p);
}
juce::Font LBTraEditor::Look::getComboBoxFont(juce::ComboBox&) { return font(13.f); }
void LBTraEditor::Look::drawPopupMenuBackground(juce::Graphics& g, int w, int h) {
    g.fillAll(kPanel);
    g.setColour(kLine);
    g.drawRect(0, 0, w, h);
}
void LBTraEditor::Look::drawToggleButton(juce::Graphics& g, juce::ToggleButton& b, bool, bool) {
    const bool on = b.getToggleState();
    auto r = b.getLocalBounds().toFloat();
    auto dot = r.removeFromRight(r.getHeight()).reduced(r.getHeight() * 0.3f);
    g.setColour(on ? kAccent : kFaint);
    g.fillEllipse(dot);
    g.setColour(on ? kText : kDim);
    g.setFont(font(13.f));
    g.drawText(b.getButtonText(), r, juce::Justification::centredLeft);
}

// ------------------------------------------------------------------ sample row
class LBTraEditor::SampleRow : public juce::Component, public juce::FileDragAndDropTarget {
public:
    SampleRow(LBTraEditor& e, int i) : ed(e), index(i) {}
    void refresh() {
        const auto& s = ed.proc.slot(index);
        thumb.clear();
        if (s.loaded) {
            const auto& a = s.source.audio;
            const int bins = 160, n = a.size();
            float mx = 1e-6f;
            for (int k = 0; k < bins; ++k) {
                float pk = 0;
                for (int i = k * n / bins; i < (k + 1) * n / bins; ++i) pk = std::max(pk, std::max(std::fabs(a.L[(size_t)i]), std::fabs(a.R[(size_t)i])));
                thumb.push_back(pk);
                mx = std::max(mx, pk);
            }
            for (auto& v : thumb) v /= mx;
        }
        repaint();
    }
    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds();
        g.setColour(hover ? kLine.withAlpha(0.6f) : juce::Colours::transparentBlack);
        g.fillRect(r);
        g.setColour(kLine);
        g.drawLine(0.f, (float)getHeight() - 0.5f, (float)getWidth(), (float)getHeight() - 0.5f, 1.f);
        const auto& s = ed.proc.slot(index);
        auto num = r.removeFromLeft(34);
        g.setColour(s.loaded ? kText : kFaint);
        g.setFont(font(12.f));
        g.drawText(juce::String(index + 1), num, juce::Justification::centred);
        r.removeFromRight(10);
        if (!s.loaded) {
            g.setColour(kFaint);
            g.setFont(font(13.f));
            g.drawText("Drop sample", r, juce::Justification::centredLeft);
            return;
        }
        const auto& a = s.source.analysis;
        auto text = r.removeFromLeft(std::min(220, r.getWidth() / 2));
        g.setColour(kText);
        g.setFont(font(14.f));
        g.drawText(s.file.getFileNameWithoutExtension(), text.removeFromTop(text.getHeight() / 2 + 3).withTrimmedBottom(1), juce::Justification::bottomLeft, true);
        g.setColour(kDim);
        g.setFont(font(11.f));
        juce::String tag = juce::String::fromUTF8(a.shapeName().c_str()) + u8(" · ") + juce::String::fromUTF8(a.regName().c_str());
        if (a.f0 > 0) tag += u8(" · ") + juce::String::fromUTF8(a.noteName().c_str());
        g.drawText(tag, text.withTrimmedTop(2), juce::Justification::topLeft, true);
        // waveform thumbnail
        auto w = r.reduced(8, 12).toFloat();
        g.setColour(kDim.withAlpha(0.55f));
        const float mid = w.getCentreY(), hh = w.getHeight() * 0.5f;
        for (size_t k = 0; k < thumb.size(); ++k) {
            const float x = w.getX() + w.getWidth() * (float)k / (float)thumb.size();
            g.drawVerticalLine((int)x, mid - thumb[k] * hh, mid + thumb[k] * hh + 1.f);
        }
    }
    void mouseDown(const juce::MouseEvent& e) override {
        if (e.mods.isPopupMenu()) { ed.proc.clearSlot(index); refresh(); return; }
        chooser = std::make_unique<juce::FileChooser>("Load sample", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
            if (fc.getResult().existsAsFile()) ed.loadInto(index, fc.getResult());
        });
    }
    bool isInterestedInFileDrag(const juce::StringArray&) override { return true; }
    void fileDragEnter(const juce::StringArray&, int, int) override { hover = true; repaint(); }
    void fileDragExit(const juce::StringArray&) override { hover = false; repaint(); }
    void filesDropped(const juce::StringArray& files, int, int) override {
        hover = false;
        int i = index; // several files fill this row and the ones below it
        for (auto& f : files) { if (i >= LBTraProcessor::kSlots) break; ed.loadInto(i++, juce::File(f)); }
        repaint();
    }
private:
    LBTraEditor& ed;
    int index;
    bool hover = false;
    std::vector<float> thumb;
    std::unique_ptr<juce::FileChooser> chooser;
};

// ------------------------------------------------------------------ dice
class LBTraEditor::Dice : public juce::Component {
public:
    explicit Dice(LBTraEditor& e) : ed(e) { setMouseCursor(juce::MouseCursor::PointingHandCursor); }
    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat().reduced(4);
        const float s = std::min(r.getWidth(), r.getHeight());
        auto sq = juce::Rectangle<float>(s, s).withCentre(r.getCentre());
        const bool busy = ed.proc.isRendering();
        g.setColour(down ? kText.withAlpha(0.15f) : juce::Colours::transparentBlack);
        g.fillRoundedRectangle(sq, s * 0.18f);
        g.setColour(busy ? kAccent : (over ? kText : kText.withAlpha(0.85f)));
        g.drawRoundedRectangle(sq, s * 0.18f, 1.6f);
        const int pips[5][2] = {{0, 0}, {2, 0}, {1, 1}, {0, 2}, {2, 2}};
        const float step = s * 0.28f, rad = s * 0.075f;
        for (auto& p : pips) {
            const float cx = sq.getX() + s * 0.22f + step * (float)p[0], cy = sq.getY() + s * 0.22f + step * (float)p[1];
            g.fillEllipse(cx - rad, cy - rad, rad * 2, rad * 2);
        }
    }
    void mouseEnter(const juce::MouseEvent&) override { over = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { over = false; repaint(); }
    void mouseDown(const juce::MouseEvent&) override { down = true; repaint(); }
    void mouseUp(const juce::MouseEvent& e) override {
        down = false;
        if (getLocalBounds().contains(e.getPosition())) ed.proc.rollDice();
        repaint();
    }
private:
    LBTraEditor& ed;
    bool over = false, down = false;
};

// ------------------------------------------------------------------ Inst / Loop
class LBTraEditor::ModeSwitch : public juce::Component {
public:
    explicit ModeSwitch(LBTraProcessor& p)
        : param(*p.params.getParameter("mode")), att(param, [this](float) { repaint(); }) {}
    void paint(juce::Graphics& g) override {
        const bool loop = param.getValue() > 0.5f; // normalised: Inst = 0, Loop = 1
        auto r = getLocalBounds().toFloat().reduced(1);
        g.setColour(kLine);
        g.drawRoundedRectangle(r, r.getHeight() * 0.5f, 1.f);
        auto half = r.withWidth(r.getWidth() * 0.5f);
        auto sel = loop ? half.withX(half.getRight()) : half;
        g.setColour(kText);
        g.fillRoundedRectangle(sel.reduced(2), sel.getHeight() * 0.5f - 2);
        g.setFont(font(12.f, true));
        g.setColour(loop ? kDim : kBg);
        g.drawText("Inst", half, juce::Justification::centred);
        g.setColour(loop ? kBg : kDim);
        g.drawText("Loop", half.withX(half.getRight()), juce::Justification::centred);
    }
    void mouseDown(const juce::MouseEvent& e) override {
        const bool loop = e.x > getWidth() / 2;
        att.setValueAsCompleteGesture(loop ? 1.f : 0.f);
        repaint();
    }
private:
    juce::RangedAudioParameter& param;
    juce::ParameterAttachment att;
};

// ------------------------------------------------------------------ switch + amount row (Gate, Comp, Wide, Bass, Shape)
class LBTraEditor::SwitchRow : public juce::Component {
public:
    SwitchRow(LBTraProcessor& p, const juce::String& name, const char* onId, const char* amtId)
        : label(name), onParam(*p.params.getParameter(onId)), amtParam(*p.params.getParameter(amtId)),
          onAtt(onParam, [this](float) { repaint(); }), amtAtt(amtParam, [this](float) { repaint(); }) {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }
    void paint(juce::Graphics& g) override {
        const bool on = onParam.getValue() > 0.5f;
        const float amt = amtParam.getValue();
        auto r = getLocalBounds().toFloat();
        auto lab = r.removeFromLeft(58);
        g.setColour(on ? kText : kDim);
        g.setFont(font(13.f));
        g.drawText(label, lab, juce::Justification::centredLeft);
        auto dot = r.removeFromRight(16).withSizeKeepingCentre(9, 9);
        g.setColour(on ? kAccent : kFaint);
        g.fillEllipse(dot);
        bar = r.reduced(6, 0).withSizeKeepingCentre(r.getWidth() - 12, 3);
        g.setColour(kLine);
        g.fillRoundedRectangle(bar, 1.5f);
        g.setColour(on ? kText.withAlpha(0.9f) : kFaint);
        g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * amt), 1.5f);
        g.fillEllipse(bar.getX() + bar.getWidth() * amt - 4, bar.getCentreY() - 4, 8, 8);
        g.setColour(kDim);
        g.setFont(font(10.f));
        if (on) g.drawText(juce::String((int)std::lround(amt * 100)), bar.translated(0, 6).withHeight(12), juce::Justification::centredRight);
    }
    void mouseDown(const juce::MouseEvent& e) override {
        dragging = bar.expanded(0, 10).contains(e.position);
        if (dragging) { amtAtt.beginGesture(); setAmount(e.position.x); }
        else onAtt.setValueAsCompleteGesture(onParam.getValue() > 0.5f ? 0.f : 1.f);
    }
    void mouseDrag(const juce::MouseEvent& e) override { if (dragging) setAmount(e.position.x); }
    void mouseUp(const juce::MouseEvent&) override { if (dragging) amtAtt.endGesture(); dragging = false; }
    void mouseDoubleClick(const juce::MouseEvent&) override { onAtt.setValueAsCompleteGesture(onParam.getValue() > 0.5f ? 0.f : 1.f); }
private:
    void setAmount(float x) {
        const float v = std::clamp((x - bar.getX()) / std::max(1.f, bar.getWidth()), 0.f, 1.f);
        amtAtt.setValueAsPartOfGesture(amtParam.convertFrom0to1(v));
        if (onParam.getValue() < 0.5f) onAtt.setValueAsCompleteGesture(1.f); // moving the amount switches it on
    }
    juce::String label;
    juce::RangedAudioParameter& onParam;
    juce::RangedAudioParameter& amtParam;
    juce::ParameterAttachment onAtt, amtAtt;
    juce::Rectangle<float> bar;
    bool dragging = false;
};

// ------------------------------------------------------------------ result strip
class LBTraEditor::ResultStrip : public juce::Component {
public:
    explicit ResultStrip(LBTraProcessor& p) : proc(p) {}
    void paint(juce::Graphics& g) override {
        auto cur = proc.current();
        auto r = getLocalBounds().toFloat().reduced(10, 6);
        if (!cur || cur->audio.getNumSamples() == 0) return;
        const auto& a = cur->audio;
        const int n = a.getNumSamples(), w = std::max(1, (int)r.getWidth());
        const float mid = r.getCentreY(), hh = r.getHeight() * 0.5f;
        g.setColour(kText.withAlpha(0.75f));
        for (int x = 0; x < w; ++x) {
            const int s0 = (int)((juce::int64)x * n / w), s1 = std::max(s0 + 1, (int)((juce::int64)(x + 1) * n / w));
            float pk = 0;
            for (int i = s0; i < s1 && i < n; ++i) pk = std::max(pk, std::max(std::fabs(a.getSample(0, i)), std::fabs(a.getSample(1, i))));
            g.drawVerticalLine((int)r.getX() + x, mid - pk * hh, mid + pk * hh + 1.f);
        }
        const float jx = r.getX() + (float)cur->downbeat / (float)n * r.getWidth();
        g.setColour(kAccent);
        g.fillRect(jx - 0.75f, r.getY() - 2, 1.5f, r.getHeight() + 4);
    }
    void mouseDown(const juce::MouseEvent&) override { proc.auditionNow(); } // click the result to hear it
private:
    LBTraProcessor& proc;
};

// ------------------------------------------------------------------ "more edit"
class LBTraEditor::MorePanel : public juce::Component {
public:
    explicit MorePanel(LBTraProcessor& p) : proc(p) {
        add(rise, riseL, "rise", "Rise");
        add(fall, fallL, "fall", "Fall");
        add(style, styleL, "style", "Style");
        add(finalEq, eqL, "finalEq", "Final EQ");
        add(loopBars, barsL, "loopBars", "Loop every (bars)");
        reverb.setButtonText("Final reverb");
        reverbAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(proc.params, "finalReverb", reverb);
        addAndMakeVisible(reverb);
        gain.setSliderStyle(juce::Slider::LinearHorizontal);
        gain.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        gain.setColour(juce::Slider::trackColourId, kText.withAlpha(0.8f));
        gain.setColour(juce::Slider::backgroundColourId, kLine);
        gain.setColour(juce::Slider::thumbColourId, kText);
        gain.onValueChange = [this] { showGain(); };
        gainAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc.params, "gain", gain);
        showGain();
        addAndMakeVisible(gain);
        style1(gainL);
        addAndMakeVisible(gainL);
        report.setMultiLine(true);
        report.setReadOnly(true);
        report.setScrollbarsShown(true);
        report.setFont(font(12.f));
        addAndMakeVisible(report);
    }
    void setReport(const juce::String& s) { report.setText(s, false); }
    void paint(juce::Graphics& g) override { g.fillAll(kBg); }
    void resized() override {
        auto r = getLocalBounds().reduced(16, 12);
        auto grid = r.removeFromTop(104);
        auto row1 = grid.removeFromTop(48), row2 = grid.withTrimmedTop(8);
        auto cell = [](juce::Rectangle<int>& row, int w) { return row.removeFromLeft(w).reduced(6, 0); };
        place(cell(row1, row1.getWidth() / 4), riseL, rise);
        place(cell(row1, row1.getWidth() / 3), fallL, fall);
        place(cell(row1, row1.getWidth() / 2), styleL, style);
        place(cell(row1, row1.getWidth()), eqL, finalEq);
        place(cell(row2, row2.getWidth() / 4), barsL, loopBars);
        auto g2 = cell(row2, row2.getWidth() / 3);
        gainL.setBounds(g2.removeFromTop(16)); gain.setBounds(g2.removeFromTop(24));
        reverb.setBounds(cell(row2, row2.getWidth() / 2).withTrimmedTop(16).withHeight(24));
        r.removeFromTop(10);
        report.setBounds(r);
    }
private:
    void showGain() { gainL.setText("Output  " + juce::String(gain.getValue(), 1) + " dB", juce::dontSendNotification); }
    static void style1(juce::Label& l) { l.setColour(juce::Label::textColourId, kDim); l.setFont(font(11.f)); l.setBorderSize({0, 0, 0, 0}); }
    void add(juce::ComboBox& b, juce::Label& l, const char* id, const char* text) {
        if (auto* c = dynamic_cast<juce::AudioParameterChoice*>(proc.params.getParameter(id))) b.addItemList(c->choices, 1);
        combos.push_back(std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(proc.params, id, b));
        l.setText(text, juce::dontSendNotification);
        style1(l);
        addAndMakeVisible(b);
        addAndMakeVisible(l);
    }
    static void place(juce::Rectangle<int> c, juce::Label& l, juce::Component& b) { l.setBounds(c.removeFromTop(16)); b.setBounds(c.removeFromTop(26)); }
    LBTraProcessor& proc;
    juce::ComboBox rise, fall, style, finalEq, loopBars;
    juce::Label riseL, fallL, styleL, eqL, barsL, gainL;
    juce::ToggleButton reverb;
    juce::Slider gain;
    juce::TextEditor report;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> combos;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> reverbAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gainAtt;
};

class LBTraEditor::MoreButton : public juce::Component {
public:
    std::function<void()> onClick;
    bool open = false;
    MoreButton() { setMouseCursor(juce::MouseCursor::PointingHandCursor); setTooltip("More edit"); }
    void setTooltip(const juce::String&) {}
    void paint(juce::Graphics& g) override {
        // three short sliders: an "edit" glyph; filled when the panel is open
        auto r = getLocalBounds().toFloat().withSizeKeepingCentre(18, 14);
        g.setColour(open ? kAccent : (over ? kText : kDim));
        const float xs[3] = {0.3f, 0.7f, 0.45f};
        for (int k = 0; k < 3; ++k) {
            const float y = r.getY() + r.getHeight() * (float)k / 2.f;
            g.drawLine(r.getX(), y, r.getRight(), y, 1.2f);
            g.fillEllipse(r.getX() + r.getWidth() * xs[k] - 2.5f, y - 2.5f, 5, 5);
        }
    }
    void mouseEnter(const juce::MouseEvent&) override { over = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { over = false; repaint(); }
    void mouseUp(const juce::MouseEvent&) override { if (onClick) onClick(); }
private:
    bool over = false;
};

// ------------------------------------------------------------------ editor
LBTraEditor::LBTraEditor(LBTraProcessor& p) : AudioProcessorEditor(p), proc(p) {
    setLookAndFeel(&look);
    for (int i = 0; i < LBTraProcessor::kSlots; ++i) {
        rows.push_back(std::make_unique<SampleRow>(*this, i));
        addAndMakeVisible(*rows.back());
        rows.back()->refresh();
    }
    dice = std::make_unique<Dice>(*this);
    addAndMakeVisible(*dice);
    modeSwitch = std::make_unique<ModeSwitch>(proc);
    addAndMakeVisible(*modeSwitch);
    const char* names[5][3] = {{"Gate", "gateOn", "gate"}, {"Comp", "compOn", "comp"}, {"Wide", "wideOn", "wide"}, {"Bass", "bassOn", "bass"}, {"Shape", "shapeOn", "shape"}};
    for (auto& n : names) {
        switches.push_back(std::make_unique<SwitchRow>(proc, n[0], n[1], n[2]));
        addAndMakeVisible(*switches.back());
    }
    strip = std::make_unique<ResultStrip>(proc);
    addAndMakeVisible(*strip);
    more = std::make_unique<MorePanel>(proc);
    addChildComponent(*more);
    moreButton = std::make_unique<MoreButton>();
    moreButton->onClick = [this] {
        more->setVisible(!more->isVisible());
        moreButton->open = more->isVisible();
        moreButton->repaint();
    };
    addAndMakeVisible(*moreButton);

    proc.renderDone.addChangeListener(this);
    changeListenerCallback(nullptr);
    setResizable(true, true);
    setResizeLimits(640, 400, 1400, 880);
    if (auto* c = getConstrainer()) c->setFixedAspectRatio(760.0 / 460.0);
    setSize(760, 460);
    startTimerHz(10);
}

LBTraEditor::~LBTraEditor() {
    proc.renderDone.removeChangeListener(this);
    setLookAndFeel(nullptr);
}

void LBTraEditor::loadInto(int index, const juce::File& f) {
    juce::String err;
    if (!proc.loadSlot(index, f, err)) { message = err; messageTicks = 40; }
    rows[(size_t)index]->refresh();
    repaint();
}

juce::String LBTraEditor::resultName() const {
    auto cur = proc.current();
    if (!cur) return "LBTra";
    return "LB-" + juce::String::toHexString((juce::int64)(cur->seed & 0xFFFFFF)).toUpperCase().paddedLeft('0', 6);
}

void LBTraEditor::changeListenerCallback(juce::ChangeBroadcaster*) {
    auto cur = proc.current();
    more->setReport(cur ? cur->report : juce::String());
    strip->repaint();
    for (auto& r : rows) r->refresh();
    repaint();
}

void LBTraEditor::timerCallback() {
    if (messageTicks > 0 && --messageTicks == 0) { message.clear(); repaint(); }
    dice->repaint();
    static bool wasRendering = false;
    if (wasRendering != proc.isRendering()) { wasRendering = proc.isRendering(); repaint(); }
}

void LBTraEditor::paint(juce::Graphics& g) {
    g.fillAll(kBg);
    const float k = (float)getWidth() / 760.f;
    const int top = (int)(46 * k), rightW = (int)(190 * k);
    g.setColour(kLine);
    g.drawLine(0.f, (float)top - 0.5f, (float)getWidth(), (float)top - 0.5f, 1.f);
    g.drawLine((float)(getWidth() - rightW) + 0.5f, (float)top, (float)(getWidth() - rightW) + 0.5f, (float)getHeight(), 1.f);
    // name bar
    auto bar = juce::Rectangle<int>(0, 0, getWidth() - rightW, top).reduced((int)(18 * k), 0);
    g.setColour(kText);
    g.setFont(font(17.f * k, true));
    g.drawText(resultName(), bar, juce::Justification::centredLeft);
    g.setFont(font(11.f * k));
    g.setColour(message.isNotEmpty() ? kAccent : kDim);
    juce::String info = message;
    if (info.isEmpty()) {
        if (proc.isRendering()) info = "rendering...";
        else if (auto cur = proc.current())
            info = juce::String(cur->bpm, 1) + " BPM  ·  rise " + juce::StringArray{"1/8", "1/4", "1/2", "1 bar", "2 bar"}[juce::jlimit(0, 4, (int)std::round(std::log2(cur->riseBeats * 2.f)))] +
                   "  ·  fall " + juce::StringArray{"1/8", "1/4", "1/2", "1 bar", "2 bar"}[juce::jlimit(0, 4, (int)std::round(std::log2(cur->fallBeats * 2.f)))];
        else info = "drop up to 5 samples, then roll";
    }
    g.drawText(info, bar.withTrimmedRight((int)(40 * k)), juce::Justification::centredRight);
    auto brand = juce::Rectangle<int>(getWidth() - rightW, 0, rightW, top).reduced((int)(16 * k), 0);
    g.setColour(kFaint);
    g.setFont(font(11.f * k, true));
    g.drawText("LBTRA", brand, juce::Justification::centredRight);
}

void LBTraEditor::resized() {
    const float k = (float)getWidth() / 760.f;
    const int top = (int)(46 * k), rightW = (int)(190 * k);
    auto area = getLocalBounds();
    area.removeFromTop(top);
    auto right = area.removeFromRight(rightW).reduced((int)(18 * k), (int)(16 * k));
    auto left = area;
    moreButton->setBounds(getWidth() - rightW - (int)(40 * k), 0, (int)(36 * k), top);

    auto stripArea = left.removeFromBottom((int)(54 * k));
    strip->setBounds(stripArea);
    const int rowH = left.getHeight() / LBTraProcessor::kSlots;
    for (auto& r : rows) r->setBounds(left.removeFromTop(rowH));
    more->setBounds(getLocalBounds().withTrimmedTop(top).withTrimmedRight(rightW + 1));

    dice->setBounds(right.removeFromTop((int)(78 * k)).withSizeKeepingCentre((int)(72 * k), (int)(72 * k)));
    right.removeFromTop((int)(12 * k));
    modeSwitch->setBounds(right.removeFromTop((int)(26 * k)));
    right.removeFromTop((int)(14 * k));
    const int sh = right.getHeight() / (int)switches.size();
    for (auto& s : switches) s->setBounds(right.removeFromTop(sh));
}
