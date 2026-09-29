<div align="center">

# fcitx5-voice-input

<p>
  <a href="https://github.com/WenYin-Community/fcitx5-voice-input/actions/workflows/ci.yml"><img src="https://img.shields.io/github/actions/workflow/status/WenYin-Community/fcitx5-voice-input/ci.yml?branch=main&logo=github&label=build" alt="Build"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-LGPL%20v3-blue.svg" alt="License"></a>
  <img src="https://img.shields.io/badge/platform-Linux-important" alt="Platform">
  <img src="https://img.shields.io/badge/fcitx5-%3E%3D5.1.19-blueviolet" alt="Fcitx5">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus" alt="C++20">
</p>

[English](README.md) | **中文**

---

</div>

**fcitx5-voice-input** 是一个 Fcitx5 语音输入插件。通过 PulseAudio（或 PipeWire fallback）捕获音频，使用 Silero ONNX VAD 检测人声分段，通过 OpenAI 兼容 API 或火山引擎豆包流式语音进行识别。

## 功能

- 中文语音输入（OpenAI Whisper API / 兼容服务、火山引擎豆包流式语音，或小米 MiMo ASR）
- Silero ONNX VAD 自动分段录音（免按键）
- 可选按住说话（PTT）模式：按住热键录音，松开上屏
- 说话过程中实时显示识别中间结果（火山引擎后端，或 OpenAI Realtime `ApiMode=realtime`）
- 录音时状态栏显示音量电平条
- 队列管道架构：音频采集 → VAD 分段 → ASR 识别 → EventDispatcher 上屏
- 通过 `fcitx5-configtool` 图形化配置
- 窗口快速切换自动延迟停止，防止误停

## 使用

### 1. 安装

#### AUR (Arch Linux)

```bash
yay -S fcitx5-voice-input
# 或
paru -S fcitx5-voice-input
# 或手动构建
git clone https://aur.archlinux.org/fcitx5-voice-input.git
cd fcitx5-voice-input
makepkg -si
```

#### COPR (Fedora / openSUSE)

COPR 发布已接入 release 流程，但需先在
[copr.fedorainfracloud.org](https://copr.fedorainfracloud.org/) 创建项目，
打 tag 发布时才会推送。项目建好后：

```bash
sudo dnf copr enable <COPR 用户名>/fcitx5-voice-input
sudo dnf install fcitx5-voice-input
```

在此之前，请从 [Releases](#deb--rpm-安装包) 安装 RPM 包。

#### DEB / RPM 安装包

从 [Releases](https://github.com/WenYin-Community/fcitx5-voice-input/releases)
下载对应发行版的包（文件名带发行版标识，如
`fcitx5-voice-input_0.4.1_amd64_ubuntu-24.04.deb`）：

```bash
sudo apt install ./fcitx5-voice-input_*_ubuntu-24.04.deb   # Ubuntu / Debian
sudo dnf install ./fcitx5-voice-input-*_fedora-44.rpm     # Fedora / openSUSE
```

> Ubuntu 24.04 与 Debian 12 官方仓库没有 onnxruntime，需先自行安装其运行时
> （可从 [onnxruntime releases](https://github.com/microsoft/onnxruntime/releases)
> 获取），否则 addon 无法加载。

#### 手动编译安装

见下方 [编译构建](#编译构建)。

### 2. 配置

安装后，打开 `fcitx5-configtool`，在 Input Method 列表中找到 **Voice Input** 并添加到输入法列表。

然后在 Addon 配置中找到 **VoiceInput**：

#### 主配置

下表「界面选项」列即配置页中显示的名称，括号内为配置文件中的键名
（手工编辑 `~/.config/fcitx5/conf/voiceinput*.conf` 时用得上）。
下拉框取值会随界面语言本地化，下表按中文界面列出。

| 界面选项 | 说明 | 默认值 |
|----------|------|--------|
| `当前 ASR 后端` (`ActiveBackend`) | 选择使用哪个 ASR 后端；点齿轮按钮 ⚙ 打开该后端的子配置页 | `OpenAI 兼容 API（OpenAI、Groq 等）` |
| `录音模式` (`VoiceInputMode`) | `VAD 自动分段（免按键）`自动检测人声分段，或 `按住热键说话（PTT）`按住热键期间录音 | `VAD 自动分段（免按键）` |
| `按住说话热键` (`PTTHotkey`) | 按住说话的热键（仅 PTT 模式生效） | 右 Ctrl |
| `语音检测阈值 (%)` (`VADThreshold`) | VAD 灵敏度，0–100，越高越不易触发 | `20` |
| `静音检测阈值 (毫秒)` (`SilenceThresholdMs`) | 静音多久判定一段语音结束，100–10000 | `800` |
| `启动帧数` (`StartFrames`) | 连续多少帧判定说话开始，1–10 | `2` |
| `预卷时长 (毫秒)` (`PreRollMs`) | 说话开始前预取的音频时长，0–1000 | `300` |
| `最短语音时长 (毫秒)` (`MinSpeechMs`) | 短于此长度的语音段直接丢弃，100–10000 | `300` |
| `最长语音时长 (毫秒)` (`MaxSpeechMs`) | 超过此长度强制分段，1000–60000 | `30000` |

在 `当前 ASR 后端` 中选择后端后，点击齿轮按钮 ⚙ 打开对应后端的配置页。

#### OpenAI 后端（子配置）

| 界面选项 | 说明 | 默认值 |
|----------|------|--------|
| `接口地址` (`BaseUrl`) | API 地址 | `https://api.openai.com/v1` |
| `API 密钥` (`ApiKey`) | API 密钥 | **（必填）** |
| `语音模型` (`Model`) | 模型名 | `whisper-1` |
| `输出语言` (`Language`) | 输出语言：`默认（自动）` / `English` / `中文` | `默认（自动）` |
| `LLM 后处理` (`LLMEnabled`) | 是否启用 LLM 后处理 | 关 |
| `后处理 LLM 模型` (`LLMModel`) | 后处理模型（留空即禁用） | （空） |
| `后处理系统提示词` (`LLMSystemPrompt`) | 后处理系统提示词 | （空） |
| `无 LLM 时自动上屏` (`AutoCommit`) | 未配置 LLM 时直接上屏识别结果 | 开 |
| `API 模式` (`ApiMode`) | `HTTP multipart /audio/transcriptions（Whisper API）`、`HTTP JSON /chat/completions（百炼、MiMo）` 或 `WebSocket 流式（OpenAI GPT Realtime）` | `HTTP multipart /audio/transcriptions（Whisper API）` |
| `实时提交间隔 (毫秒)` (`CommitIntervalMs`) | Realtime 模式下的周期性提交间隔，1000–30000，使长句无停顿也能持续输出增量 | `5000` |

在 `当前 ASR 后端` 中选择 `OpenAI 兼容 API（OpenAI、Groq 等）`，点击齿轮按钮，然后填入 API 密钥。支持所有 OpenAI 兼容服务，如：

- [OpenAI](https://platform.openai.com/) — `https://api.openai.com/v1`
- [Groq](https://console.groq.com/) — `https://api.groq.com/openai/v1`
- [硅基流动 (SiliconFlow)](https://siliconflow.cn/) — `https://api.siliconflow.cn/v1`
- [阿里云百炼 (DashScope)](https://help.aliyun.com/zh/model-studio/qwen-asr-api-reference) — `https://dashscope.aliyuncs.com/compatible-mode/v1`

  **注意：** 阿里云百炼使用的 `qwen3-asr-flash` 模型不走标准的 Whisper API，需要通过 Chat Completions 接口调用。使用时需将 `API 模式` 设为 `HTTP JSON /chat/completions（百炼、MiMo）`，并补充对应的 DashScope API 密钥。对应的配置文件写法：
  ```
  BaseUrl=https://dashscope.aliyuncs.com/compatible-mode/v1
  ApiKey=your_dashscope_api_key
  Model=qwen3-asr-flash
  ApiMode=chat
  Language=zh
  ```
  百炼 ASR 的具体接口文档请参考[阿里云官方文档](https://help.aliyun.com/zh/model-studio/qwen-asr-api-reference)。

  **Realtime 流式实时转录（可选）：** 将 `API 模式` 设为 `WebSocket 流式（OpenAI GPT Realtime）`，即可通过 OpenAI Realtime 转录会话实现**边说边出**增量识别结果（实时刷入候选框 preedit，说话结束提交上屏）。使用 OpenAI 官方账号时，模型推荐 `gpt-live-transcribe`（官方推荐，真正连续增量）或 `gpt-realtime-whisper`（兼容备选）。配置示例：
  ```
  BaseUrl=https://api.openai.com/v1
  ApiKey=your_openai_api_key
  Model=gpt-live-transcribe
  ApiMode=realtime
  Language=zh
  ```
  **注意：** Realtime 模式需要支持 WebSocket 的连接端点，且 `gpt-live-transcribe` / `gpt-realtime-whisper` 需付费 Tier 账号（Free 不支持）。音频以 24kHz 发送（插件会自动将采集的 16kHz 重采样到 24kHz）。

  **小米 MiMo ASR（可选）：** 在 `当前 ASR 后端` 中选择 `Chat Completions 格式（小米 MiMo）` 即可使用[小米 MiMo](https://mimo.mi.com/)（`mimo-v2.5-asr`）。MiMo 运行在 OpenAI 兼容引擎上，使用 `api-key` 认证与 Chat Completions 格式；端点和模型会自动归一化，只需在 OpenAI 子配置里填写小米的 API 密钥。

#### 火山引擎豆包后端（子配置）

在 `当前 ASR 后端` 中选择 `WebSocket 流式（火山引擎豆包）`，然后点击齿轮按钮打开本页。

| 界面选项 | 说明 | 默认值 |
|----------|------|--------|
| `WebSocket 地址` (`Endpoint`) | WebSocket 地址 | `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async` |
| `认证模式` (`AuthMode`) | `API 密钥` 或 `App 密钥 + Access 密钥` | `API 密钥` |
| `API 密钥` (`ApiKey`) | API 密钥（新版控制台） | **`API 密钥` 模式必填** |
| `App 密钥` (`AppKey`) | App 密钥（旧版控制台） | **`App 密钥 + Access 密钥` 模式必填** |
| `Access 密钥` (`AccessKey`) | Access 密钥（旧版控制台） | **`App 密钥 + Access 密钥` 模式必填** |
| `资源 ID` (`ResourceId`) | 资源 ID | `volc.seedasr.sauc.duration` |
| `音频分片 (毫秒)` (`ChunkMs`) | 单包音频长度，100–200 | `200` |
| `ITN 逆文本标准化` (`EnableITN`) | ITN 文本规范化 | 开 |
| `标点符号` (`EnablePunc`) | 标点符号 | 开 |
| `语义顺滑` (`EnableDDC`) | 语义顺滑 | 关 |
| `二次识别` (`EnableNonstream`) | 二次识别 | 开 |
| `判停窗口 (毫秒)` (`EndWindowMs`) | 服务端判停窗口，200–3000 | `800` |

火山引擎需要先在[火山引擎控制台](https://console.volcengine.com/)购买语音识别资源。资源 ID 取决于模型和购买方式：

- 模型 2.0 小时版：`volc.seedasr.sauc.duration`
- 模型 2.0 并发版：`volc.seedasr.sauc.concurrent`
- 模型 1.0 小时版：`volc.bigasr.sauc.duration`
- 模型 1.0 并发版：`volc.bigasr.sauc.concurrent`

**故障排查：** 如果识别失败，在插件日志中查找 `X-Tt-Logid`，并提交给火山引擎技术支持。

### 3. 使用

1. 切换到 **Voice Input** 输入法
2. 开始说话，VAD 自动检测人声并录音
3. 使用**火山引擎**后端或 OpenAI `ApiMode=realtime` 时，说话过程中会实时显示识别中间结果
4. 停止说话（默认 800ms 静音超时），最终识别结果自动上屏
5. 保持语音输入模式，继续说话可连续识别

切换窗口时插件会自动延迟 200ms 停止，快速切回会取消停止，避免不必要的重启。

## 编译构建

### 依赖

- `fcitx5` — 输入法框架
- `libpulse-simple` — PulseAudio 音频捕获（优先；两个录音后端至少需要一个）
- `libpipewire-0.3` — PipeWire 音频捕获（fallback）
- `jsoncpp` — JSON 解析
- `libcurl` — HTTP/WebSocket 客户端（>= 7.86.0，ASR 必需）
- `zlib` — Gzip 压缩（火山引擎后端必需）
- `onnxruntime` — Silero VAD ONNX Runtime

> **Arch Linux:** `sudo pacman -S fcitx5 pulseaudio pipewire jsoncpp curl onnxruntime-cpu zlib`
>
> **Debian/Ubuntu:** `sudo apt install fcitx5 libpulse-dev libpipewire-0.3-dev libjsoncpp-dev libcurl4-openssl-dev libonnxruntime-dev zlib1g-dev`

### 编译

```bash
# 克隆并初始化子模块（获取 Silero VAD 模型）
git clone https://github.com/WenYin-Community/fcitx5-voice-input.git
cd fcitx5-voice-input
git submodule update --init --recursive

# 配置
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr

# 编译
cmake --build build -j"$(nproc)"

# 安装
sudo cmake --install build --prefix /usr
```

### CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `BUILD_TESTS` | `OFF` | 构建测试 |
| `ONNXRUNTIME_ROOT` | — | ONNX Runtime 自定义安装路径 |


## 注意事项

- **API Key 安全**：API Key 明文存储在 `~/.config/fcitx5/conf/voiceinput-openai.conf` 和 `~/.config/fcitx5/conf/voiceinput-volcengine.conf` 中，请注意文件权限
- **网络要求**：所有受支持的 ASR 后端均为云服务，需要网络连接
- **音频设备**：启动时自动选择输入源（优先 `alsa_input.*`，跳过 monitor 与回声消除源）；配置界面暂不提供手动选择设备的选项
- **VAD 模型**：Silero VAD 模型通过 git submodule 分发（`third_party/silero-vad/`），编译时自动复制到安装目录。构建前务必执行 `git submodule update --init --recursive`
- **PipeWire 用户**：PulseAudio 后端也能在 pipewire-pulse 下正常工作，仅在 PulseAudio 完全不可用时 fallback 到 PipeWire 直连
- **窗口切换**：快速切换窗口时插件使用延迟停止机制（200ms），不会频繁重启流水线。长时间切出后会自动停止

## 架构简介

```
音频捕获线程 → FrameQueue → VAD Worker 线程 → SpeechEventQueue → ASR Worker 线程 → ResultQueue → EventDispatcher → commitString

SpeechEvent 类型: Begin（说话开始）→ Audio（32ms 帧，Pipeline 聚合到 200ms）→ End（静音）/ Cancel（语音太短丢弃）
```

三个工作线程 + 主线程，通过 `ThreadSafeQueue` 连接各阶段。详见 [ARCHITECTURE.md](docs/architecture/ARCHITECTURE.md)。

## 许可证

GNU Lesser General Public License v3.0
