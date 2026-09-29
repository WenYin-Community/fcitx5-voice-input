# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

fcitx5-voice-input 是一个 Fcitx5 addon（共享库），实现语音输入功能。音频捕获 → Silero ONNX VAD 分段 → OpenAI 兼容 API 语音识别 → 自动上屏。

## 构建命令

```bash
# 克隆后必须初始化子模块（Silero VAD 模型）
git submodule update --init --recursive

# 配置 + 构建
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j"$(nproc)"

# 安装（写入系统路径，需要 sudo）
sudo cmake --install build --prefix /usr
```

CMake 选项：`BUILD_TESTS`（默认 OFF，开启后构建 `tests/` 单元测试）、`ONNXRUNTIME_ROOT`（自定义 ONNX Runtime 路径）。

## 依赖

fcitx5（pkg-config 名 `fcitx5` 或 `Fcitx5Core`）、`jsoncpp`、`libcurl`（>= 7.86.0）、`zlib`、`onnxruntime`，以及录音后端二者至少其一：`libpulse-simple`（优先）、`libpipewire-0.3`（fallback）。两个录音库均为运行期 dlopen，无链接期依赖。

**Arch**: `sudo pacman -S fcitx5 pulseaudio pipewire jsoncpp curl onnxruntime-cpu`
**Debian**: `sudo apt install fcitx5 libpulse-dev libpipewire-0.3-dev libjsoncpp-dev libcurl4-openssl-dev libonnxruntime-dev`

## 架构

### 一句话版本

> 一个 Fcitx5 Addon 共享库，内部三个工作线程 + 主线程，通过 ThreadSafeQueue 连接。

### 数据流

```
音频捕获线程 → FrameQueue → VAD Worker 线程 → SpeechEventQueue → ASR Worker 线程 → ResultQueue → EventDispatcher → 主线程 commitString
```

- **主线程**（Fcitx5 事件循环）：activate/deactivate 管理、PollResults() 轮询 ResultQueue 并 commitString、状态文字更新
- **音频捕获线程**：PulseAudio 优先（libpulse-simple 同步读取），失败后 PipeWire fallback（pw_stream + lock-free ring buffer）
- **VAD Worker 线程**：消费 FrameQueue，Silero ONNX predict() 返回概率，Idle/Speaking 状态机，按 SpeechEvent（Begin/Audio/End/Cancel）推入 SpeechEventQueue
- **ASR Worker 线程**：消费 SpeechEventQueue，按后端类型建立会话——OpenAI 兼容走 HTTP（multipart WAV 或 Chat Completions JSON），Realtime/Volcengine 走 WebSocket 流式——结果推入 ResultQueue

### 关键源文件

```
src/addon/
├── engine.cpp/.h              # VoiceInputEngine — Fcitx5 InputMethodEngineV2 入口
├── types.h                    # AudioFrame / SpeechEvent / AsrResult 数据类型
├── config/voiceinput-config.h # FCITX_CONFIGURATION 宏定义（所有配置键）
├── capture/
│   ├── audio_capture.h        # AudioCapture 抽象接口
│   ├── pulse_audio_capture.*  # PulseAudio 后端（优先）
│   └── pipewire_capture.*     # PipeWire 后端（fallback）
├── vad/
│   ├── silero_vad.*           # Silero ONNX 封装
│   └── vad.*                  # VADWorker 状态机（pre-roll 缓冲、静音超时分段）
├── pipeline/pipeline.*        # Pipeline 编排器（3 队列 + 3 线程生命周期管理）
├── asr/
│   ├── asr_engine.h            # AsrEngine 抽象接口（StartSession 工厂）
│   ├── asr_session.h           # AsrSession 接口（FeedAudio/End/Cancel）
│   ├── openai_asr.*            # OpenAI 兼容 ASR（HTTP multipart / Chat Completions）
│   ├── realtime_asr.*          # OpenAI Realtime 流式（WebSocket, 16k→24k）
│   ├── volcengine_asr.*        # 火山引擎豆包流式（WebSocket）
│   ├── session_reaper.*        # 游离会话回收线程
│   ├── wav_encoder.*           # WAV 编码（multipart 模式）
│   └── utils/base64.*          # base64 编码（chat 模式 data URI）
├── llm/llm_client.*            # LLM 后处理客户端（OpenAI 兼容 chat）
└── utils/
    ├── audio_buffer.h         # AudioRingBuffer — Lock-free SPSC（仅 PipeWire 内部使用）
    └── thread_safe_queue.h    # ThreadSafeQueue<T> — mutex+cv 通用队列
```

## 关键约定

- **构建产物**：`voice-input-addon.so`（无 `lib` 前缀，fcitx5 addon 命名要求）
- **音频格式**：16kHz mono int16，512 samples/window（32ms）
- **Addon 注册**：`FCITX_ADDON_FACTORY(VoiceInputAddonFactory)` 必须在 `namespace fcitx` 外部
- **PipeWire 回调约束**：`on_process` 内 ≤100μs，只写 ring buffer，禁止阻塞/VAD/内存分配
- **Generation 过滤**：每次 activate/deactivate 递增 `activeGeneration_`（atomic），PollResults 只提交匹配当前 generation 的结果
- **延迟停止**：deactivate 延迟 200ms 才真正停止 pipeline，快速切回窗口时 activate 会取消 pending 的停止任务
- **Config 热加载**：`setConfig()` 保存到 `conf/voiceinput.conf`，pipeline 运行时更新配置
- **PulseAudio vs PipeWire**：PulseAudio 优先（兼容 PulseAudio 和 pipewire-pulse），PipeWire 仅作 fallback（当 PulseAudio 完全不可用时）
