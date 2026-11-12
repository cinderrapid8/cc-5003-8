# 参考回声处理服务（远程采访录音 AEC）

输入为发送给扬声器的**远端参考信号**与话筒**混合信号**，输出供录制使用的**近端声音**。
回声消除由成熟开源库 **SpeexDSP 1.2.1（MDF 自适应滤波 AEC）** 的真实 C API 完成，
不训练、不依赖任何语音模型。

## 1. 目录结构

```
src/
  main.cpp          CLI 入口（demo / server / accept 三种模式）
  signal_gen.*      可重复信号生成：类语音 + 已知延迟/衰减/房间响应的回声通路
  aec_processor.*   核心处理：真实 Speex AEC、块适配、参考对齐、中断检测与状态标记
  metrics.*         分段评估：ERLE、回声泄漏、近端保留、与真值的相关性
  wav_io.*          16-bit PCM WAV 读写（试听/导出）
  json.*            轻量 JSON 解析/生成（网页 API 用，无外部依赖）
  pipeline.*        生成→处理→评估→记录→导出 的编排与运行清单
  http_server.*     POSIX socket 内建 HTTP 服务（静态页 + JSON API + 文件下载）
web/index.html      配置网页（块长度、参考延迟、场景、采样率、旁路、试听）
tests/
  test_contracts.cpp  处理契约单元测试（不丢采样、旁路、中断标记、重采样、可复现）
  verify_run.py       独立核对脚本：从导出的 WAV 重新计算指标，不信任 C++ 结果
  acceptance.sh       端到端验收（四类压力场景 + 20ms 块）
third_party/         本地编译的 SpeexDSP（无需 root / 系统安装）
```

## 2. 构建

SpeexDSP 已在 `third_party/speexdsp` 本地编译（静态/动态库均有）。若需重建：

```bash
make            # 产出 ./aec_demo（无警告，-Wall -Wextra -std=c++17）
make test       # 处理契约单元测试
make accept     # 端到端验收 + 独立 WAV 核对
```

## 3. 运行方式

```bash
# 单次演示（命令行打印分段指标，WAV 与清单写入 run_data/<run_id>/）
./aec_demo demo --scenario full --duration 12 \
                --frame-ms 10 --ref-delay-ms 120 --filter-ms 256

# 网页服务：浏览器打开 http://localhost:8080
./aec_demo server --port 8080 --web-root web

# 全部验收场景
./aec_demo accept
```

网页可配置：测试场景、**块长度（10/20ms）**、**参考延迟**、滤波器长度、
两路采样率、时长、随机种子、旁路开关；提供每路 WAV 的在线试听与下载。

## 4. 回声通路（演示用，可重复）

`signal_gen.cpp` 用固定种子（默认 `20261112`）确定性生成：

- 两路**相互独立**的类语音信号（共振峰谐振器 + 基音脉冲/噪声激励 + 通话停顿包络），
  非真实录音、无需模型；同种子逐字节一致。
- 回声通路：`回声 = 远端信号 ⊛ 房间冲激响应`，冲激响应含
  **已知直达延迟（默认 120ms）、衰减增益（0.55）、指数衰减+稀疏反射的房间响应**。
- 话筒混合：`mic = 回声 + 独立近端语音 + 微弱底噪`，并保留 `near`/`echo` 真值用于评估。

## 5. 关键设计与诚实性约束

### 5.1 统一时间基准与块适配（不丢采样）
- 两路先经 **Speex 重采样器**统一到内部采样率；较短一路零填充补齐，绝不截断。
- 调用方按任意大小（代码里刻意用 137 采样的奇数块）推入 `BlockAdapter`，
  由适配缓冲凑成 AEC 固定帧；末尾不足一帧时**零填充到整帧并标记 `TAIL_FLUSH`**，
  真实样本数与位置全部保留。单元测试覆盖各种非整帧长度。
- 参考按配置的参考延迟做样本级移位对齐，使 AEC 输入与声学回声时间对应；
  实际施加的延迟采样数写入运行清单。

### 5.2 参考缺失 / 时间关系未知（不假装消除）
- 显式参考中断（有效性时间线为假）：该帧标记 `REFERENCE_MISSING`，
  **旁路直通话筒**，不调用减法，不伪造输出，并记录中断起止与原因。
- 持续 300ms 以上的参考静音：作为“时间关系未知”**信息性记录**，
  但消除仍继续（零参考是 AEC 的合法输入），自然说话停顿不会被误判为中断。
- 核对脚本对中断段有专门断言：该段 ERLE 必须接近 0、输出与回声保持高相关，
  若出现“假消除”直接判失败。

### 5.3 真实库调用
每个对齐帧调用 `speex_echo_state_init` / `speex_echo_ctl(SET_SAMPLING_RATE)` /
`speex_echo_cancellation`；采样率转换调用 `speex_resampler_*`。
滤波器长度、帧长、处理帧数与库版本（speexdsp 1.2.1）均写入清单。

## 6. 评估方法（不以输出能量变小判定成功）

`metrics.cpp` 借助生成阶段的 `near`/`echo` 真值分解，按场景段分别计算：

- **ERLE（回声返回损耗增强）**：仅在有回声的段有意义，量化回声残留；
- **回声泄漏 dB / 输出与回声相关系数**：残留回声还有多少；
- **近端保留 dB / 输出与近端相关系数**：双讲时近端语音是否保住。

典型实测（16kHz、10ms 帧、256ms 滤波器、120ms 对齐）：

| 场景段 | ERLE | 输出~回声相关 | 输出~近端相关 |
|---|---|---|---|
| 收敛后仅远端 | 约 17–20 dB | ≈ 0 | 0 |
| 双讲 | 约 10–11 dB | 0.17 | **0.83** |
| 参考中断 | ≈ 0 dB | **0.93** | 0 |
| 延迟突变恢复窗 | 负值（恶化） | 升高 | 0 |

延迟突变后 ERLE 暂时变负、再随自适应滤波重收敛逐步恢复，是 Speex MDF 的真实行为，
系统如实呈现而非掩盖。参考延迟配错（扫描见 `tests/delay_sweep.cpp`）时 ERLE 明显恶化，
可用于反证时间基准对应的重要性。

## 7. 运行记录（版本 / 参数 / 中断）

每次运行在 `run_data/<run_id>/` 产出：

```
mic_mix.wav  far_reference.wav  near_clean.wav  echo_only.wav
aec_output.wav  bypass_mic.wav  manifest.json
```

`manifest.json` 记录：服务与 SpeexDSP 版本、输入生成器/RIR 模型版本与回声参数、
全部处理参数（含实际 AEC 帧长、滤波器长度、参考延迟采样数）、中断段列表、
分段指标与汇总。旁路 WAV 用于 A/B 试听对比。

## 8. 验收场景与结果核对

```bash
tests/acceptance.sh                 # 一键跑全部
python3 -I tests/verify_run.py run_data/<run_id>   # 单独核对某次运行
```

覆盖：**延迟突变、输入采样率不匹配（44.1k↔16k）、参考中断、两路同时发声**，
外加综合压力与 20ms 块尺寸。核对脚本独立重读 WAV 重算 ERLE/相关性，
并断言：收敛段 ERLE ≥ 8dB、双讲近端相关 ≥ 0.5、中断段无假消除、
延迟突变确实造成可观测恶化。脚本与服务指标交叉一致（误差 < 1dB）。
