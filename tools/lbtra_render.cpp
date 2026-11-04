// Offline listening tool: analyse up to 5 input sounds, roll the dice N times,
// write one WAV per roll plus a Chinese report (report.md).
//
//   lbtra_render [--count 10] [--bpm 90] [--seed 1] [--out renders] a.wav b.wav ...
#include "../engine/Engine.h"
#include "../engine/Wav.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static std::string baseName(const std::string& p) {
    std::string n = fs::path(p).stem().string();
    return n;
}

int main(int argc, char** argv) {
    int count = 10;
    float bpm = 90.f;
    uint64_t seed = 1;
    std::string outDir = "renders";
    bool arrangeOnly = false;
    std::string prefix;
    float riseFixed = 0, fallFixed = -1, comp = -1;
    std::vector<std::string> inputs;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--count" && i + 1 < argc) count = std::atoi(argv[++i]);
        else if (a == "--bpm" && i + 1 < argc) bpm = (float)std::atof(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--out" && i + 1 < argc) outDir = argv[++i];
        else if (a == "--arrange-only") arrangeOnly = true;
        else if (a == "--prefix" && i + 1 < argc) prefix = argv[++i];
        else if (a == "--rise" && i + 1 < argc) riseFixed = (float)std::atof(argv[++i]);
        else if (a == "--fall" && i + 1 < argc) fallFixed = (float)std::atof(argv[++i]);
        else if (a == "--comp" && i + 1 < argc) comp = (float)std::atof(argv[++i]);
        else inputs.push_back(a);
    }
    if (inputs.empty()) {
        std::fprintf(stderr, "usage: lbtra_render [--count N] [--bpm B] [--seed S] [--out DIR] [--arrange-only] [--rise BEATS] [--fall BEATS|auto] [--comp 0..1] [--prefix P] a.wav b.wav ... (max %d)\n", lb::kMaxLayers);
        return 1;
    }
    if ((int)inputs.size() > lb::kMaxLayers) inputs.resize((size_t)lb::kMaxLayers);

    std::vector<lb::Source> sources;
    std::string report = "# 离线试听报告\n\n## 输入声音识别\n\n| 声音 | 类型 | 响度形状 | 音域 | 音高 | 时长 | 起音 | 说明 |\n| --- | --- | --- | --- | --- | --- | --- | --- |\n";
    for (auto& p : inputs) {
        lb::Source s;
        std::string err;
        if (!lb::readWav(p, s.audio, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        s.audio = lb::resampleTo(s.audio, 48000);
        s.analysis = lb::analyze(s.audio, baseName(p));
        const auto& a = s.analysis;
        char row[512];
        std::snprintf(row, sizeof row, "| %s | %s | %s | %s | %s | %.2f 秒 | %.0f ms | 音高置信 %.0f%%，平坦度 %.3f，频谱重心 %.0f Hz%s |\n", a.name.c_str(),
                      a.kindName().c_str(), a.shapeName().c_str(), a.regName().c_str(), a.noteName().c_str(), a.duration, a.attackMs, a.pitchConfidence * 100, a.flatness,
                      a.centroid, a.rising ? "，能量往后走高" : "");
        report += row;
        sources.push_back(std::move(s));
    }

    fs::create_directories(outDir);
    const std::vector<float> falls = {2.f, 1.f, 4.f, 1.f, 2.f, 2.f, 4.f, 1.f, 2.f, 4.f};
    const std::vector<float> lengths = {4.f, 2.f, 8.f, 1.f, 4.f, 2.f, 8.f, 0.5f, 4.f, 8.f};
    const std::vector<lb::Style> styles = {lb::Style::Neutral, lb::Style::Dirty, lb::Style::Ambient, lb::Style::Fragment};
    const char* styleSlug[] = {"standard", "dirty", "ambient", "fragment"};
    const char* lenSlug[] = {"1-8", "1-4", "1-2", "1bar", "2bar"};
    auto lenIdx = [](float b) { return b == 0.5f ? 0 : b == 1.f ? 1 : b == 2.f ? 2 : b == 4.f ? 3 : 4; };

    report += "\n## 渲染结果\n\n每个文件的强拍位置 = 过渡的末尾，后面是 impact 和尾音。\n";
    for (int i = 0; i < count; ++i) {
        lb::RenderRequest req;
        req.seed = seed + (uint64_t)i * 7919u;
        req.style = styles[(size_t)i % styles.size()];
        req.lengthBeats = lengths[(size_t)i % lengths.size()];
        req.bpm = bpm;
        req.arrangeOnly = arrangeOnly;
        if (riseFixed > 0) req.lengthBeats = riseFixed;
        if (!arrangeOnly) req.fallBeats = fallFixed >= 0 ? fallFixed : falls[(size_t)i % falls.size()];
        if (comp >= 0) req.compAmount = comp;
        if (arrangeOnly) req.style = lb::Style::Neutral;
        auto r = lb::renderTransition(sources, req);
        char name[128];
        if (arrangeOnly) std::snprintf(name, sizeof name, "%s%02d_arrange_%s.wav", prefix.c_str(), i + 1, lenSlug[lenIdx(req.lengthBeats)]);
        else std::snprintf(name, sizeof name, "%s%02d_%s_up%s_down%s.wav", prefix.c_str(), i + 1, styleSlug[(int)req.style], lenSlug[lenIdx(req.lengthBeats)], lenSlug[lenIdx(req.fallBeats)]);
        std::string err;
        if (!lb::writeWav24((fs::path(outDir) / name).string(), r.audio, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        report += "\n### " + std::string(name) + "\n\n" + r.report;
        std::printf("wrote %s (%.2fs)\n", name, r.audio.seconds());
    }
    std::ofstream(fs::path(outDir) / (prefix + "report.md")) << report;
    return 0;
}
