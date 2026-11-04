# LBTra

过渡 / Fill 生成插件。用户放入最多 5 个声音（one-shot、音效，偶尔 loop），插件先识别每个声音，再由骰子在识别结果允许的范围内随机处理，最后混成一个按节拍触发的 sample。

方案文档：https://claude.ai/artifact/REUdbm3d594DPz5B8FXGXW

## 现在的阶段：离线试听

还没有界面和插件。`lbtra_render` 是命令行工具：读入音频，跑识别 + 骰子，一次输出 N 个过渡 WAV 和一份中文报告（每个文件用了哪些效果和参数）。

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/lbtra_render --count 10 --bpm 90 --seed 1 --out renders a.wav b.wav c.wav d.wav
./build/lbtra_analyze a.wav b.wav   # 只看识别结果
```

同一个种子永远渲染出同一个结果，方便对比和复现。

## 结构

| 目录 | 内容 |
| --- | --- |
| `engine/Core.h` | 立体声缓冲、确定性随机数、Biquad、FFT、变速 |
| `engine/Wav.*` | WAV 读写（16/24/32 bit PCM、32/64 bit float） |
| `engine/Analysis.*` | 声音识别：有调 one-shot / 打击 / 音效 / loop，音域、音高、起音、是否上升 |
| `engine/Effects.*` | 每轨效果（混响、倒放混响、延迟/粒子延迟、EQ/扫频、失真、结晶回声、卡顿连击、粒子云），包络库，总线（多段压缩、总 EQ、总混响），输出选项（门限切片、最终混响、最终 EQ），响度与限幅 |
| `engine/Engine.*` | 骰子：给每轨选角色、变调、最多 3 个效果、包络，再走总线和输出 |
| `tools/` | 命令行工具 |

引擎只用 C++17 标准库，后面直接放进 JUCE 插件。

## 插件（JUCE）

```
cmake -S . -B build -DLBTRA_BUILD_PLUGIN=ON          # 自动下载 JUCE 8.0.4
cmake --build build --config Release --target LBTra_All
```

产物在 `build/LBTra_artefacts/Release/`（VST3、Mac 上还有 AU、独立运行版）。GitHub Actions 的 plugin 任务会编译 Mac（通用二进制）和 Windows 版并上传为下载包。

- 5 个声音槽：拖入音频（可一次拖多个），自动识别，右键清空。
- 上升 / 下降长度、风格、压缩量旋钮、最终混响、最终 EQ、输出音量。trance gate 暂时不做。
- 摇骰子 = 新种子重新渲染；改参数或宿主速度变化会用同一个种子自动重渲染。
- Play 模式：MIDI 音符触发，从上升开头播放（音符放在小节线前"上升长度"处）。
- Loop 模式：宿主走带时每 N 小节自动触发，衔接点落在小节线上。
- 工程保存时记住声音文件路径、种子和参数。
