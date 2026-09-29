<div align="center">

# fcitx5-voice-input

<p>
  <a href="https://github.com/WenYin-Community/fcitx5-voice-input/actions/workflows/ci.yml"><img src="https://img.shields.io/github/actions/workflow/status/WenYin-Community/fcitx5-voice-input/ci.yml?branch=main&logo=github&label=build" alt="Build"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-LGPL%20v3-blue.svg" alt="License"></a>
  <img src="https://img.shields.io/badge/platform-Linux-important" alt="Platform">
  <img src="https://img.shields.io/badge/fcitx5-%3E%3D5.1.19-blueviolet" alt="Fcitx5">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus" alt="C++20">
</p>

[中文](README.zh-CN.md)

---

</div>

**fcitx5-voice-input** is a Fcitx5 addon for voice input. Captures audio via PulseAudio (or PipeWire fallback), detects speech segments with Silero ONNX VAD, and transcribes via OpenAI-compatible API or Volcengine Doubao streaming ASR.

## Features

- Voice input (OpenAI Whisper API / compatible services, Volcengine Doubao streaming ASR, or Xiaomi MiMo ASR)
- Silero ONNX VAD for automatic speech segmentation (hands-free)
- Optional Push-to-Talk mode: hold a hotkey to record, release to commit
- Real-time partial transcript update during speech (Volcengine, or OpenAI Realtime `ApiMode=realtime`)
- Audio level meter in the status bar while recording
- Queue-based pipeline: Audio Capture → VAD → ASR → EventDispatcher → commit
- Graphical configuration via `fcitx5-configtool`
- Smart delayed stop on window switching

## Usage

### 1. Installation

#### AUR (Arch Linux)

The AUR package [`fcitx5-voice-input`](https://aur.archlinux.org/packages/fcitx5-voice-input)
belongs to the upstream project and builds from the upstream repository, not
from this one. Installing it therefore gives you upstream's build:

```bash
yay -S fcitx5-voice-input
```

To install *this* project on Arch, use the `PKGBUILD` and source tarball
attached to [Releases](https://github.com/WenYin-Community/fcitx5-voice-input/releases):

```bash
# after downloading PKGBUILD and the source tarball into one directory
makepkg -si
```

#### COPR (Fedora / openSUSE)

Not published: the COPR job was removed from the release workflow, so there
is no repository to enable. Install the RPM from
[Releases](#deb--rpm-packages) instead.

#### DEB / RPM packages

Download the package matching your distro from
[Releases](https://github.com/WenYin-Community/fcitx5-voice-input/releases)
(filenames carry the distro tag, e.g.
`fcitx5-voice-input_0.4.1_amd64_ubuntu-24.04.deb`):

```bash
sudo apt install ./fcitx5-voice-input_*_ubuntu-24.04.deb   # Ubuntu / Debian
sudo dnf install ./fcitx5-voice-input-*_fedora-44.rpm     # Fedora / openSUSE
```

> Ubuntu 24.04 and Debian 12 do not ship onnxruntime in their repositories;
> install its runtime yourself first (see
> [onnxruntime releases](https://github.com/microsoft/onnxruntime/releases)),
> otherwise the addon cannot be loaded.

#### Build from source

See [Build](#build) below.

### 2. Configuration

After installation, open `fcitx5-configtool`, find **Voice Input** in the Input Method list and add it.

Then open the Addon config for **VoiceInput**:

#### Main Config

The "UI option" column is the label shown in the config page, followed by the
config-file key in parentheses (useful when editing
`~/.config/fcitx5/conf/voiceinput*.conf` by hand).

> **Note:** option labels are Chinese-only in the current source (they are not
> translated), so an English UI shows the same Chinese labels listed below.
> Dropdown *values* are localized, so they read English or Chinese depending
> on your locale; the values below are the English ones.

| UI option | Description | Default |
|-----------|-------------|---------|
| `当前 ASR 后端` (`ActiveBackend`) | Which ASR backend to use (see the values below); the gear button ⚙ opens that backend's sub-config | `OpenAI-compatible API (OpenAI, Groq, etc.)` |
| `录音模式` (`VoiceInputMode`) | `VAD Auto-segment (hands-free)` segments speech automatically, or `Hold hotkey to record (PTT)` records while the hotkey is held | `VAD Auto-segment (hands-free)` |
| `按住说话热键` (`PTTHotkey`) | Hotkey to hold while recording (PTT mode only) | Right Ctrl |
| `语音检测阈值 (%)` (`VADThreshold`) | VAD sensitivity, 0–100; higher = less sensitive | `20` |
| `静音检测阈值 (毫秒)` (`SilenceThresholdMs`) | Silence duration that ends an utterance, 100–10000 | `800` |
| `启动帧数` (`StartFrames`) | Consecutive speech frames required to start, 1–10 | `2` |
| `预卷时长 (毫秒)` (`PreRollMs`) | Audio kept from before speech onset, 0–1000 | `300` |
| `最短语音时长 (毫秒)` (`MinSpeechMs`) | Utterances shorter than this are discarded, 100–10000 | `300` |
| `最长语音时长 (毫秒)` (`MaxSpeechMs`) | Longest utterance before a forced split, 1000–60000 | `30000` |

`当前 ASR 后端` values: `OpenAI-compatible API (OpenAI, Groq, etc.)`,
`WebSocket streaming (Volcengine Doubao)`, `Chat Completions format (Xiaomi MiMo)`.

Select the backend in `当前 ASR 后端`, then click the gear button ⚙ to open that backend's config page.

#### OpenAI Backend (sub-config)

| UI option | Description | Default |
|-----------|-------------|---------|
| `接口地址` (`BaseUrl`) | API base URL | `https://api.openai.com/v1` |
| `API 密钥` (`ApiKey`) | API key | **(required)** |
| `语音模型` (`Model`) | Model name | `whisper-1` |
| `输出语言` (`Language`) | Output language: `Default (Auto)` / `English` / `中文` | `Default (Auto)` |
| `LLM 后处理` (`LLMEnabled`) | Enable LLM post-processing | off |
| `后处理 LLM 模型` (`LLMModel`) | Post-processing model (empty = disabled) | (empty) |
| `后处理系统提示词` (`LLMSystemPrompt`) | Post-processing system prompt | (empty) |
| `无 LLM 时自动上屏` (`AutoCommit`) | Commit the result directly when no LLM is configured | on |
| `API 模式` (`ApiMode`) | `HTTP multipart /audio/transcriptions (Whisper API)`, `HTTP JSON /chat/completions (DashScope, MiMo)` or `WebSocket streaming (OpenAI GPT Realtime)` | `HTTP multipart /audio/transcriptions (Whisper API)` |
| `实时提交间隔 (毫秒)` (`CommitIntervalMs`) | Periodic commit interval in Realtime mode, 1000–30000; keeps long speech without pauses producing partials | `5000` |

Select `OpenAI-compatible API (OpenAI, Groq, etc.)` in `当前 ASR 后端`, click the gear button, and fill in your API key. Compatible with any OpenAI-format service:

- [OpenAI](https://platform.openai.com/) — `https://api.openai.com/v1`
- [Groq](https://console.groq.com/) — `https://api.groq.com/openai/v1`
- [SiliconFlow](https://cloud.siliconflow.com) — `https://api.siliconflow.com/v1`
- [Alibaba Cloud DashScope](https://help.aliyun.com/zh/model-studio/qwen-asr-api-reference) — `https://dashscope.aliyuncs.com/compatible-mode/v1`

  **Note:** DashScope's `qwen3-asr-flash` model uses the Chat Completions API instead of the standard Whisper API. Set `API 模式` to `HTTP JSON /chat/completions (DashScope, MiMo)` when using this provider. Equivalent config file entries:
  ```
  BaseUrl=https://dashscope.aliyuncs.com/compatible-mode/v1
  ApiKey=your_dashscope_api_key
  Model=qwen3-asr-flash
  ApiMode=chat
  Language=zh
  ```

  **GPT Realtime streaming (optional):** set `API 模式` to `WebSocket streaming (OpenAI GPT Realtime)` to transcribe incrementally (partials update the preedit live, the final result commits on speech end) via the OpenAI Realtime transcription session. With an OpenAI account, use `gpt-live-transcribe` (recommended, true continuous deltas) or `gpt-realtime-whisper` (compatible alternative).
  ```
  BaseUrl=https://api.openai.com/v1
  ApiKey=your_openai_api_key
  Model=gpt-live-transcribe
  ApiMode=realtime
  Language=zh
  ```
  **Note:** Realtime mode requires a WebSocket-capable endpoint, and `gpt-live-transcribe` / `gpt-realtime-whisper` need a paid-tier account (Free is not supported). Audio is sent at 24kHz (the addon automatically upsamples the captured 16kHz).

  **Xiaomi MiMo ASR (optional):** select `Chat Completions format (Xiaomi MiMo)` in `当前 ASR 后端` to use [Xiaomi MiMo](https://mimo.mi.com/) (`mimo-v2.5-asr`). MiMo runs on the OpenAI-compatible engine with `api-key` auth and Chat Completions format; the endpoint and model are normalized automatically, so you only need to fill in your MiMo API key in the OpenAI sub-config.

#### Volcengine Doubao Backend (sub-config)

Select `WebSocket streaming (Volcengine Doubao)` in `当前 ASR 后端`, then click the gear button to open this page.

| UI option | Description | Default |
|-----------|-------------|---------|
| `WebSocket 地址` (`Endpoint`) | WebSocket endpoint | `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async` |
| `认证模式` (`AuthMode`) | `API Key` or `App Key + Access Key` | `API Key` |
| `API 密钥` (`ApiKey`) | API key (new console) | **(required for `API Key` mode)** |
| `App 密钥` (`AppKey`) | App key (legacy console) | **(required for `App Key + Access Key` mode)** |
| `Access 密钥` (`AccessKey`) | Access token (legacy console) | **(required for `App Key + Access Key` mode)** |
| `资源 ID` (`ResourceId`) | Resource ID | `volc.seedasr.sauc.duration` |
| `音频分片 (毫秒)` (`ChunkMs`) | Audio chunk size per packet, 100–200 | `200` |
| `ITN 逆文本标准化` (`EnableITN`) | ITN text normalization | on |
| `标点符号` (`EnablePunc`) | Punctuation | on |
| `语义顺滑` (`EnableDDC`) | DDC smoothing | off |
| `二次识别` (`EnableNonstream`) | Second-pass recognition | on |
| `判停窗口 (毫秒)` (`EndWindowMs`) | Server-side end-of-speech window, 200–3000 | `800` |

Volcengine authentication requires a resource purchased from the [Volcengine console](https://console.volcengine.com/). The Resource ID depends on your model and purchase plan:

- Model 2.0 hourly: `volc.seedasr.sauc.duration`
- Model 2.0 concurrency: `volc.seedasr.sauc.concurrent`
- Model 1.0 hourly: `volc.bigasr.sauc.duration`
- Model 1.0 concurrency: `volc.bigasr.sauc.concurrent`

**Troubleshooting:** If recognition fails, check the addon log for `X-Tt-Logid` and provide it to Volcengine support.

### 3. How to Use

1. Switch to **Voice Input** IME
2. Start speaking — VAD automatically detects speech and records
3. With the **Volcengine** backend or OpenAI `ApiMode=realtime`, partial recognition text appears in the preedit area in real-time as you speak
4. Stop speaking (default 800ms silence timeout) — final recognition result is committed
5. Stay in Voice Input mode and continue speaking for consecutive recognition

When switching windows, the plugin delays stop by 200ms. Quick switch-back cancels the stop, avoiding unnecessary restarts.

## Build

### Dependencies

- `fcitx5` — Input method framework
- `libpulse-simple` — PulseAudio capture (preferred; at least one capture backend is required)
- `libpipewire-0.3` — PipeWire capture (fallback)
- `jsoncpp` — JSON parsing
- `libcurl` — HTTP/WebSocket client (>= 7.86.0, required for ASR)
- `zlib` — Gzip compression (required for Volcengine backend)
- `onnxruntime` — Silero VAD ONNX Runtime

> **Arch Linux:** `sudo pacman -S fcitx5 pulseaudio pipewire jsoncpp curl onnxruntime-cpu zlib`
>
> **Debian/Ubuntu:** `sudo apt install fcitx5 libpulse-dev libpipewire-0.3-dev libjsoncpp-dev libcurl4-openssl-dev libonnxruntime-dev zlib1g-dev`

### Build Steps

```bash
# Clone and init submodules (for Silero VAD model)
git clone https://github.com/WenYin-Community/fcitx5-voice-input.git
cd fcitx5-voice-input
git submodule update --init --recursive

# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr

# Build
cmake --build build -j"$(nproc)"

# Install
sudo cmake --install build --prefix /usr
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_TESTS` | `OFF` | Build tests |
| `ONNXRUNTIME_ROOT` | — | Custom ONNX Runtime install path |


## Notes

- **API Key Security**: API keys are stored in plain text in `~/.config/fcitx5/conf/voiceinput-openai.conf` and `~/.config/fcitx5/conf/voiceinput-volcengine.conf`. Ensure proper file permissions
- **Network Required**: All supported ASR backends are cloud services; an internet connection is required
- **Audio Device**: Selects an input source automatically at startup (prefers `alsa_input.*`, skips monitor and echo-cancel sources); there is no manual device picker in the config UI
- **VAD Model**: The Silero VAD model is distributed via git submodule (`third_party/silero-vad/`) and copied to the install directory at build time. Run `git submodule update --init --recursive` before building
- **PipeWire Users**: The PulseAudio backend works fine under pipewire-pulse. Native PipeWire is only used as fallback when PulseAudio is completely unavailable
- **Window Switching**: A 200ms delayed stop prevents unnecessary restarts on quick window switches. Long inactivity will stop the pipeline

## Architecture Overview

```
Audio Capture Thread → FrameQueue → VAD Worker Thread → SpeechEventQueue → ASR Worker Thread → ResultQueue → EventDispatcher → commitString

SpeechEvent types: Begin (speech onset) → Audio (32ms frames, batched to 200ms by Pipeline) → End (silence) / Cancel (too short)
```

Three worker threads + main thread, connected by `ThreadSafeQueue`. See [ARCHITECTURE.md](docs/architecture/ARCHITECTURE.md) for details.

## License

GNU Lesser General Public License v3.0
