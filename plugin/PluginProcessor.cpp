#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Wav.h"

namespace {
const float kBeatChoices[] = {0.5f, 1.f, 2.f, 4.f, 8.f};
juce::StringArray beatNames() { return {"1/8", "1/4", "1/2", "1 bar", "2 bar"}; }
juce::String u8(const char* s) { return juce::String::fromUTF8(s); }

lb::Buffer toEngine(const juce::AudioBuffer<float>& in, double sr) {
    lb::Buffer b(in.getNumSamples(), (int)std::lround(sr));
    const int nch = in.getNumChannels();
    for (int i = 0; i < in.getNumSamples(); ++i) {
        b.L[(size_t)i] = nch > 0 ? in.getSample(0, i) : 0.f;
        b.R[(size_t)i] = nch > 1 ? in.getSample(1, i) : b.L[(size_t)i];
    }
    return b;
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout LBTraProcessor::layout() {
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    l.add(std::make_unique<AudioParameterChoice>(ParameterID{"rise", 1}, u8("上升长度"), beatNames(), 3));
    l.add(std::make_unique<AudioParameterChoice>(ParameterID{"fall", 1}, u8("下降长度"), beatNames(), 2));
    l.add(std::make_unique<AudioParameterChoice>(ParameterID{"style", 1}, u8("风格"), StringArray{u8("标准"), u8("脏"), u8("氛围"), u8("碎片")}, 0));
    // right column: each processor has an on/off switch and an amount
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"gateOn", 1}, "Gate", false));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"gate", 1}, "Gate Amount", NormalisableRange<float>(0.f, 1.f), 0.8f));
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"compOn", 1}, "Comp", true));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"comp", 1}, u8("压缩量"), NormalisableRange<float>(0.f, 1.f), 0.65f));
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"wideOn", 1}, "Wide", false));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"wide", 1}, "Wide Amount", NormalisableRange<float>(0.f, 1.f), 0.5f));
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"bassOn", 1}, "Bass", false));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"bass", 1}, "Bass Amount", NormalisableRange<float>(0.f, 1.f), 0.5f));
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"shapeOn", 1}, "Shape", false));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"shape", 1}, "Shape Amount", NormalisableRange<float>(0.f, 1.f), 0.5f));
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"finalReverb", 1}, u8("最终混响"), false));
    l.add(std::make_unique<AudioParameterChoice>(ParameterID{"finalEq", 1}, u8("最终 EQ"), StringArray{u8("关"), u8("电话"), u8("变暗")}, 0));
    l.add(std::make_unique<AudioParameterChoice>(ParameterID{"mode", 1}, u8("模式"), StringArray{"Inst", "Loop"}, 0));
    l.add(std::make_unique<AudioParameterChoice>(ParameterID{"loopBars", 1}, u8("每几小节"), StringArray{"1", "2", "4", "8"}, 2));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"gain", 1}, u8("输出音量"), NormalisableRange<float>(-24.f, 6.f), 0.f));
    return l;
}

LBTraProcessor::LBTraProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      params(*this, nullptr, "LBTra", layout()) {
    formats.registerBasicFormats();
    for (auto id : {"rise", "fall", "style", "comp", "finalReverb", "finalEq", "gateOn", "gate", "compOn", "wideOn", "wide", "bassOn", "bass", "shapeOn", "shape"}) params.addParameterListener(id, this);
    startTimer(150);
}

LBTraProcessor::~LBTraProcessor() {
    stopTimer();
    pool.removeAllJobs(true, 10000);
}

bool LBTraProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void LBTraProcessor::prepareToPlay(double sr, int) {
    if (std::fabs(sr - sampleRate) > 1.0) { sampleRate = sr; dirty = true; }
    sampleRate = sr;
    playPos = -1;
}

std::shared_ptr<const LBTraProcessor::Rendered> LBTraProcessor::current() const {
    const juce::SpinLock::ScopedLockType sl(swapLock);
    return live;
}

bool LBTraProcessor::loadSlot(int index, const juce::File& f, juce::String& error) {
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(f));
    if (!reader) { error = u8("读不了这个文件：") + f.getFileName(); return false; }
    const auto maxLen = (juce::int64)(reader->sampleRate * 30.0); // one-shots and short loops; cap at 30 s
    const int n = (int)std::min<juce::int64>(reader->lengthInSamples, maxLen);
    juce::AudioBuffer<float> buf((int)std::max(1u, std::min(2u, reader->numChannels)), n);
    reader->read(&buf, 0, n, 0, true, true);
    Slot& s = slots[(size_t)index];
    s.file = f;
    s.source.audio = lb::resampleTo(toEngine(buf, reader->sampleRate), 48000);
    s.source.analysis = lb::analyze(s.source.audio, f.getFileNameWithoutExtension().toStdString());
    s.loaded = true;
    dirty = true;
    return true;
}

void LBTraProcessor::clearSlot(int index) {
    slots[(size_t)index] = Slot{};
    dirty = true;
}

void LBTraProcessor::rollDice() {
    seed = (uint64_t)(juce::Random::getSystemRandom().nextInt64() & 0x7fffffffffffLL);
    if (seed == 0) seed = 1;
    dirty = true;
}

void LBTraProcessor::timerCallback() {
    // the host tempo changed: re-render the same dice so the timing stays exact
    const double bpm = lastBpm.load();
    if (renderedBpm > 0 && std::fabs(bpm - renderedBpm) > 0.05) dirty = true;
    if (dirty && !rendering) { dirty = false; startRender(); }
}

void LBTraProcessor::startRender() {
    std::vector<lb::Source> sources;
    for (auto& s : slots) if (s.loaded) sources.push_back(s.source);
    if (sources.empty()) {
        const juce::SpinLock::ScopedLockType sl(swapLock);
        live.reset();
        renderDone.sendChangeMessage();
        return;
    }
    lb::RenderRequest req;
    req.seed = seed;
    req.style = (lb::Style)(int)*params.getRawParameterValue("style");
    req.lengthBeats = kBeatChoices[(int)*params.getRawParameterValue("rise")];
    req.fallBeats = kBeatChoices[(int)*params.getRawParameterValue("fall")];
    req.bpm = (float)lastBpm.load();
    auto on = [this](const char* id) { return *params.getRawParameterValue(id) > 0.5f; };
    auto val = [this](const char* id) { return params.getRawParameterValue(id)->load(); };
    req.compAmount = on("compOn") ? val("comp") : 0.f;
    req.shaperAmount = on("shapeOn") ? val("shape") : 0.f;
    lb::OutputOptions out;
    out.gate = on("gateOn");
    out.gateDepth = val("gate");
    out.wide = on("wideOn") ? val("wide") : 0.f;
    out.bass = on("bassOn") ? val("bass") : 0.f;
    out.finalReverb = *params.getRawParameterValue("finalReverb") > 0.5f;
    out.finalEq = (lb::OutputOptions::EqMode)(int)*params.getRawParameterValue("finalEq");
    req.forcedOutput = out;
    const double hostSr = sampleRate;
    renderedBpm = req.bpm;
    rendering = true;
    pool.addJob([this, sources = std::move(sources), req, hostSr] {
        auto r = lb::renderTransition(sources, req);
        auto res = std::make_shared<Rendered>();
        lb::Buffer audio = r.audio;
        int downbeat = r.downbeatSample;
        if (std::lround(hostSr) != audio.sr) {
            const double k = hostSr / (double)audio.sr;
            audio = lb::resampleTo(audio, (int)std::lround(hostSr));
            downbeat = (int)std::lround(downbeat * k);
        }
        res->audio.setSize(2, audio.size());
        res->audio.copyFrom(0, 0, audio.L.data(), audio.size());
        res->audio.copyFrom(1, 0, audio.R.data(), audio.size());
        res->downbeat = downbeat;
        res->bpm = req.bpm;
        res->riseBeats = req.lengthBeats;
        res->fallBeats = req.fallBeats;
        res->report = juce::String::fromUTF8(r.report.c_str());
        res->seed = req.seed;
        {
            const juce::SpinLock::ScopedLockType sl(swapLock);
            // keep older renders alive here so the audio thread never frees a buffer
            retired[2] = retired[1]; retired[1] = retired[0]; retired[0] = live;
            live = res;
        }
        rendering = false;
        renderDone.sendChangeMessage();
    });
}

void LBTraProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    buffer.clear();

    std::shared_ptr<const Rendered> latest;
    {
        const juce::SpinLock::ScopedTryLockType sl(swapLock);
        if (sl.isLocked()) latest = live;
    }
    if (latest && latest != playing && playPos < 0) playing = latest; // swap only between hits
    if (!playing) playing = latest;

    double ppq = 0, bpm = lastBpm.load();
    bool hostPlaying = false;
    int barNum = 4, barDen = 4;
    if (auto* ph = getPlayHead()) {
        if (auto pos = ph->getPosition()) {
            if (auto b = pos->getBpm()) bpm = *b;
            if (auto p = pos->getPpqPosition()) ppq = *p;
            if (auto ts = pos->getTimeSignature()) { barNum = ts->numerator; barDen = ts->denominator; }
            hostPlaying = pos->getIsPlaying();
        }
    }
    if (bpm > 20 && bpm < 400) lastBpm = bpm;

    const bool loopMode = (int)*params.getRawParameterValue("mode") == 1;
    int startOffset = -1; // sample in this block where a new hit starts
    if (auditionRequest.exchange(false)) { if (latest) playing = latest; startOffset = 0; auditioning = true; }

    if (!loopMode) {
        for (const auto meta : midi) {
            const auto m = meta.getMessage();
            if (m.isNoteOn()) { if (latest) playing = latest; startOffset = meta.samplePosition; auditioning = false; }
        }
    } else if (hostPlaying && playing && playing->audio.getNumSamples() > 0) {
        auditioning = false;
        // chase the host: the junction lands on every Nth bar line
        const int bars = juce::StringArray{"1", "2", "4", "8"}[(int)*params.getRawParameterValue("loopBars")].getIntValue();
        const double qPerBar = (double)barNum * 4.0 / (double)barDen;
        const double L = bars * qPerBar;
        const double spq = sampleRate * 60.0 / bpm;
        const double riseQ = playing->downbeat / spq;
        const double fallQ = (playing->audio.getNumSamples() - playing->downbeat) / spq;
        double J = std::ceil((ppq - fallQ) / L) * L;
        if (J < L) J = L;
        if (J <= ppq - fallQ + 1e-9) J += L;
        const double S = J - riseQ;
        if (ppq >= S) playPos = (int64_t)std::llround((ppq - S) * spq);
        else if ((S - ppq) * spq < n) { playPos = -1; startOffset = (int)((S - ppq) * spq); }
        else playPos = -1;
    } else if (!auditioning) {
        playPos = -1; // loop mode follows the transport: stopped means silent
    }

    if (!playing) return;
    const auto& src = playing->audio;
    const int len = src.getNumSamples();
    const float g = juce::Decibels::decibelsToGain(params.getRawParameterValue("gain")->load());
    auto render = [&](int from, int to) {
        for (int i = from; i < to && playPos >= 0; ++i) {
            if (playPos >= len) { playPos = -1; break; }
            buffer.addSample(0, i, src.getSample(0, (int)playPos) * g);
            buffer.addSample(1, i, src.getSample(1, (int)playPos) * g);
            ++playPos;
        }
    };
    if (startOffset >= 0) {
        render(0, startOffset);
        playPos = 0;
        render(startOffset, n);
    } else {
        render(0, n);
    }
}

void LBTraProcessor::getStateInformation(juce::MemoryBlock& destData) {
    auto state = params.copyState();
    juce::XmlElement xml("LBTra");
    xml.addChildElement(state.createXml().release());
    auto* sl = xml.createNewChildElement("Slots");
    for (int i = 0; i < kSlots; ++i)
        if (slots[(size_t)i].loaded) {
            auto* e = sl->createNewChildElement("Slot");
            e->setAttribute("index", i);
            e->setAttribute("path", slots[(size_t)i].file.getFullPathName());
        }
    xml.setAttribute("seed", juce::String((juce::int64)seed));
    copyXmlToBinary(xml, destData);
}

void LBTraProcessor::setStateInformation(const void* data, int sizeInBytes) {
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (!xml || !xml->hasTagName("LBTra")) return;
    if (auto* p = xml->getChildByName(params.state.getType())) params.replaceState(juce::ValueTree::fromXml(*p));
    seed = (uint64_t)xml->getStringAttribute("seed", "1").getLargeIntValue();
    // loading files and analysing them belongs on the message thread
    juce::Array<std::pair<int, juce::String>> files;
    if (auto* sl = xml->getChildByName("Slots"))
        for (auto* e : sl->getChildIterator()) files.add({e->getIntAttribute("index"), e->getStringAttribute("path")});
    juce::MessageManager::callAsync([this, files, alive = std::weak_ptr<bool>(aliveFlag)] {
        if (alive.expired()) return;
        for (auto& f : files) {
            juce::String err;
            if (f.first >= 0 && f.first < kSlots) loadSlot(f.first, juce::File(f.second), err);
        }
        dirty = true;
    });
}

juce::AudioProcessorEditor* LBTraProcessor::createEditor() { return new LBTraEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new LBTraProcessor(); }
