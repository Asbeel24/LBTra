// Prints what the engine recognises in each input sound.
#include "../engine/Analysis.h"
#include "../engine/Wav.h"
#include <cstdio>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        lb::Buffer b; std::string err;
        if (!lb::readWav(argv[i], b, err)) { std::fprintf(stderr, "%s\n", err.c_str()); continue; }
        b = lb::resampleTo(b, 48000);
        auto a = lb::analyze(b, argv[i]);
        std::printf("%s\n  类型=%s 音域=%s 时长=%.2fs 起音=%.0fms 峰值位置=%.2f\n  音高=%.1fHz(%s) 置信=%.2f 抖动=%.2f半音 平坦度=%.3f 重心=%.0fHz 起音点=%d 规律=%.2f 上升=%d\n",
            argv[i], a.kindName().c_str(), a.regName().c_str(), a.duration, a.attackMs, a.peakPos, a.f0, a.noteName().c_str(),
            a.pitchConfidence, a.pitchSpread, a.flatness, a.centroid, a.onsets, a.onsetRegularity, (int)a.rising);
    }
}
