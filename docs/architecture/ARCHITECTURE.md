# fcitx5-voice-input 架构设计

> **状态:** 当前实现文档（与代码同步）。

## 设计目标

1. **无独立进程（daemon）** — 全部逻辑跑在 Fcitx5 addon 内部，线程隔离
2. **无 CLI 二进制** — 配置靠 fcitx5-configtool
3. **无 Qt GUI 依赖** — 不引入 Qt，避免 50MB+ 的依赖膨胀
4. **云端 ASR** — OpenAI 兼容 API（whisper-1）/ 火山引擎豆包流式
5. **最小依赖** — Fcitx5 + PipeWire/PulseAudio + jsoncpp + libcurl + onnxruntime

---

## 一句话架构

> **一个 Fcitx5 Addon（共享库），内部三个工作线程。**

```
Fcitx5 进程 (voice-input-addon.so)
├── [主线] 输入法激活/停用、状态显示、上屏（EventDispatcher 轮询结果）
├── [音频线] PulseAudio 优先 → PipeWire fallback → AudioFrame → FrameQueue
├── [VAD线] 消费 FrameQueue → Silero ONNX VAD → SpeechEvent (Begin/Audio/End/Cancel) → SpeechEventQueue
└── [ASR线] 消费 SpeechEventQueue → OpenAI / Volcengine ASR → ResultQueue → 回调主线程
```

没有 daemon、没有 D-Bus、没有 CLI 二进制、没有 Qt GUI。

---

## 为什么可以砍掉 daemon？

### 传统方案（fcitx5-vinput）的选择

```
Fcitx5 Addon ← D-Bus IPC → vinput-daemon (独立进程)
                              ├── PipeWire 音频
                              └── ASR 推理
```

理由是："ASR 推理会卡 UI，放另一个进程里安全。"

### 实际问题

ASR 推理是 CPU/网络密集型操作，在**同一进程的另一个线程**里跑和在**另一个进程**里跑，对 UI 响应的影响是完全一样的——都不阻塞主线程。区别只有：

| 维度 | 独立进程 | 独立线程 |
|------|---------|---------|
| 隔离性 | 更强（崩溃不影响 Fcitx5） | 弱一些（线程崩溃拖整个进程） |
| 复杂度 | ❌ systemd 管理、D-Bus 定义、进程通信 | ✅ 零额外开销 |
| 模型加载 | 重复加载（addon 一份、daemon 一份） | ✅ 共享内存 |
| 延迟 | ❌ 加一次 D-Bus 序列化+反序列化 | ✅ 直接内存访问 |
| 用户操作 | ❌ `systemctl --user start vinput-daemon` | ✅ 装好即用 |
| 调试 | ❌ 跨进程追踪困难 | ✅ 单进程 GDB 一把梭 |

---

## 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                     Fcitx5 进程                              │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐   │
│  │                voice-input-addon.so                   │   │
│  │                                                      │   │
│  │  ┌─────────────────────────────────────────────┐     │   │
│  │  │         主线程 (Fcitx5 事件循环)              │     │   │
│  │  │  ├── activate() → pipeline.Start()            │     │   │
│  │  │  ├── deactivate() → 延迟 Stop()               │     │   │
│  │  │  ├── PollResults() → commitString()           │     │   │
│  │  │  └── 状态同步 → subModeLabel 显示状态文字      │     │   │
│  │  └─────────────────────────────────────────────┘     │   │
│  │                         ↕ FrameQueue                  │   │
│  │  ┌─────────────────────────────────────────────┐     │   │
│  │  │        音频捕获线程 (Capture Thread)          │     │   │
│  │  │  ├── PulseAudio 优先，失败后 PipeWire fallback │     │   │
│  │  │  ├── 读取音频 → 封装 AudioFrame               │     │   │
│  │  │  └── 推入 FrameQueue                         │     │   │
│  │  └─────────────────────────────────────────────┘     │   │
│  │                         ↕ FrameQueue                  │   │
│  │  ┌─────────────────────────────────────────────┐     │   │
│  │  │         VAD Worker 线程                      │     │   │
│  │  │  ├── 消费 FrameQueue                         │     │   │
│  │  │  ├── Silero ONNX predict() 返回概率           │     │   │
│  │  │  ├── Idle/Speaking 状态机                    │     │   │
│  │  │  ├── pre-roll 缓冲 + 静音超时分段             │     │   │
│  │  │  └── 完整说话段 → SpeechEventQueue            │     │   │
│  │  └─────────────────────────────────────────────┘     │   │
│  │                         ↕ SpeechEventQueue            │   │
│  │  ┌─────────────────────────────────────────────┐     │   │
│  │  │         ASR Worker 线程                      │     │   │
│  │  │  ├── 消费 SpeechEventQueue                   │     │   │
│  │  │  ├── int16 → float32 转换                   │     │   │
│  │  │  ├── OpenAI 兼容 API（HTTP multipart）   │     │   │
│  │  │  └── 结果 → ResultQueue + 回调主线程          │     │   │
│  │  └─────────────────────────────────────────────┘     │   │
│  │                         ↕ ResultQueue                │   │
│  │  ┌─────────────────────────────────────────────┐     │   │
│  │  │       EventDispatcher（主线程轮询）           │     │   │
│  │  │  ├── OnAsrResult() → schedule PollResults()  │     │   │
│  │  │  └── PollResults() → commitString()          │     │   │
│  │  └─────────────────────────────────────────────┘     │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐   │
│  │           fcitx5-configtool 配置界面                  │   │
│  │  ├── ASR Backend: [openai ▼]                         │   │
│  │  ├── OpenAI: Endpoint / API Key / Model               │   │
│  │  ├── Volcengine: Endpoint / Auth / Key / Resource     │   │
│  │  ├── Audio Source: [Default (Auto) ▼]                 │   │
│  │  ├── VAD Threshold / Silence Threshold                │   │
│  │  └── LLM Model / System Prompt (可选)                 │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

---

## 组件详述

### 1. 输入法引擎 (`src/addon/engine.cpp/.h`)

```cpp
class VoiceInputEngine : public fcitx::InputMethodEngineV2 {
    // Fcitx5 生命周期
    void reloadConfig() override;     // 重载配置
    void setConfig() override;        // 配置变更时保存
    void activate() override;         // 切换到语音输入 → pipeline.Start()
    void deactivate() override;       // 切出 → 延迟 200ms Stop()
    void keyEvent() override;         // PTT 热键检测；其余按键仅用于提交 pending preedit
    string subModeLabelImpl() override; // 显示状态文字（如"录音中..."）

    // 结果处理（主线程）
    void OnAsrResult(const string& text);  // ASR 结果回调
    void PollResults();                    // 轮询 ResultQueue → commitString()
    void ClearUI();                        // 清理 preedit 与状态文字
    void SetStatus(const string& text);    // 更新状态栏文字

    // 引擎管理
    void InitializeIfNeeded();             // 延迟初始化
    unique_ptr<AsrEngine> CreateAsrEngine(); // 按 ActiveBackend 构造引擎
    void ReloadActiveAsrClient();          // 配置变更后替换引擎
    void ReloadLLMClient();                // 配置变更后替换 LLM 客户端

private:
    Instance* instance_;
    unique_ptr<Pipeline> pipeline_;
    EventDispatcher eventDispatcher_;
    VoiceInputConfig config_;              // 主配置
    OpenAIAsrConfig openaiConfig_;         // OpenAI 子配置
    VolcengineAsrConfig volcengineConfig_; // 火山引擎子配置
    InputContext* activeIc_;
    atomic<uint64_t> activeGeneration_;    // 当前有效 generation
    atomic<uint64_t> sessionGeneration_;   // 当前 session generation
    bool pttActive_;                       // PTT 按下状态
    atomic<int> audioLevel_;               // 电平条数值
    atomic<bool> recording_;               // 是否录音中
};
```

**关键设计：**
- `activeGeneration_` 随每次 activate/deactivate 递增，用于过滤过期结果
- `deactivate()` 延迟 200ms 才真正停止，防止快速切换窗口时误停
- `setConfig()` 保存到 `conf/voiceinput.conf`，热更新 pipeline

### 2. 配置 (`src/addon/config/`)

配置分三层：主配置 + 两个后端子配置（`getSubConfig()`/`setSubConfig()` 暴露给
配置界面，路径分别对应 `fcitx://config/addon/voiceinput/asr/{openai,volcengine}`），
全部通过 fcitx5 `FCITX_CONFIGURATION` 宏定义，由 fcitx5-configtool 可视化编辑。
API Key 写入的配置文件权限会被收紧（`RestrictConfigFilePermissions`）。

```
src/addon/config/
└── voiceinput-config.h       # FCITX_CONFIGURATION 宏定义全部配置键
```

**配置键：**（以下为实际键名，分组对应主配置与两个子配置）

| 键 | 类型 | 默认值 | 说明 |
|----|------|--------|------|
| `ActiveBackend` | String | `openai` | ASR 后端（openai / volcengine / mimo） |
| `VoiceInputMode` | String | `vad` | 录音模式（vad 自动分段 / ptt 按住说话） |
| `PTTHotkey` | KeyList | 右 Ctrl | 按住说话热键（ptt 模式生效） |
| `VADThreshold` | Int 0-100 | `20` | VAD 阈值百分比 |
| `SilenceThresholdMs` | Int 100-10000 | `800` | 静音超时毫秒数 |
| `StartFrames` | Int 1-10 | `2` | 连续多少帧触发 onset |
| `PreRollMs` | Int 0-1000 | `300` | 说话开始前预取音频（ms） |
| `MinSpeechMs` | Int 100-10000 | `300` | 最短有效语音段（ms） |
| `MaxSpeechMs` | Int 1000-60000 | `30000` | 最长语音段（ms，超时强制分段） |
| `BaseUrl` | String | `https://api.openai.com/v1` | OpenAI 兼容 API Endpoint |
| `ApiKey` | String | `""` | API Key |
| `Model` | String | `whisper-1` | 模型名 |
| `Language` | String | `auto` | 输出语言（auto / en / zh） |
| `ApiMode` | String | `whisper` | API 模式（whisper / chat / realtime） |
| `CommitIntervalMs` | Int 1000-30000 | `5000` | realtime 模式周期提交间隔（ms） |
| `LLMEnabled` | Bool | `false` | LLM 后处理开关 |
| `LLMModel` | String | `""` | LLM 模型（空=禁用） |
| `LLMSystemPrompt` | String | `""` | LLM 系统提示词 |
| `AutoCommit` | Bool | `true` | 无 LLM 时自动上屏 |
| `Endpoint` | String | `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async` | 火山引擎 WebSocket 地址 |
| `AuthMode` | String | `api_key` | 认证模式（api_key / app_access_key） |
| `AppKey` | String | `""` | App Key（旧版控制台） |
| `AccessKey` | String | `""` | Access Key（旧版控制台） |
| `ResourceId` | String | `volc.seedasr.sauc.duration` | 资源 ID |
| `ChunkMs` | Int 100-200 | `200` | 音频分包（ms） |
| `EnableITN` | Bool | `true` | ITN 文本规范化 |
| `EnablePunc` | Bool | `true` | 标点符号 |
| `EnableDDC` | Bool | `false` | 语义顺滑 |
| `EnableNonstream` | Bool | `true` | 二遍识别 |
| `EndWindowMs` | Int 200-3000 | `800` | 判停窗口（ms） |

录音设备在启动时自动选择（`pactl list sources short`，优先 `alsa_input.*`，
排除 monitor 与 echoCancel 源），配置界面暂不提供手动选择设备的选项。

无高级 JSON 配置层。

### 3. 音频捕获 (`src/addon/capture/`)

```cpp
// 捕获后端抽象接口
class AudioCapture {
public:
    virtual ~AudioCapture() = default;
    virtual bool Start() = 0;      // 开始捕获，返回成功失败
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;
    virtual const char* Name() const = 0;
    virtual void SetFrameQueue(ThreadSafeQueue<AudioFrame>* queue);
};
```

**PulseAudio 后端（优先）：**
- 使用 `libpulse-simple` 同步读取 API
- 创建独立线程 `CaptureLoop()` 循环读取
- 兼容传统 PulseAudio 和 pipewire-pulse 模拟层
- 启动时 `pactl list sources short` 自动选择输入源（优先 `alsa_input.*`，
  排除 monitor 与 echoCancel 源）

**PipeWire 后端（fallback）：**
- 使用 `pw_thread_loop` + `pw_stream` 实时音频回调
- `on_process` 回调（≤100μs）只写 lock-free ring buffer
- 独立 `DrainLoop()` 线程从 ring buffer 读取 → 封装 AudioFrame → FrameQueue
- 不依赖 WirePlumber 的特定版本

**选择策略：** Pipeline 中先尝试 PulseAudio，失败后 fallback 到 PipeWire 直连。
两个库均运行期 dlopen，编译期各自可选。

### 4. VAD (`src/addon/vad/`)

```
src/addon/vad/
├── silero_vad.cpp/.h    # Silero ONNX 封装
└── vad.cpp/.h           # VADWorker 状态机
```

**SileroVAD：** 轻量 ONNX Runtime 封装，输入 512 samples int16，返回 0~1 概率。维护内部状态（context + h/c），调用 `Reset()` 重置 session。

**VADWorker（独立线程）：**

```
Input:  FrameQueue → AudioFrame (512 int16, 32ms)
Output: SpeechEventQueue → SpeechEvent (Begin / Audio / End / Cancel)

状态机: Idle ──(连续 speechFrames ≥ startFrames)──→ Speaking
        Speaking ──(silenceFrames ≥ endSilenceFrames || 超长)──→ End / Cancel → Idle
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `speechThreshold` | 0.2 | 说话判定阈值（由 `VADThreshold=20` ÷ 100 得到，Pipeline 覆盖） |
| `silenceThreshold` | 0.14 | 静音判定阈值（= speechThreshold × 0.7） |
| `startFrames` | 2 | 连续几帧说话触发 onset |
| `preRollMs` | 300 | onset 前保留音频（毫秒） |
| `endSilenceMs` | 800 | 连续静音多久结束说话段（来自 `SilenceThresholdMs`） |
| `minSpeechMs` | 300 | 最短说话段（太短丢弃） |
| `maxSpeechMs` | 30000 | 最长说话段（超长强制结束） |

VAD 不产出整段音频，而是每帧 Audio 事件 + Begin/End 边界信号。Pipeline 负责将 32ms 粒度帧批量聚合到 200ms 再送入 ASR。

VAD 模型从 `third_party/silero-vad/` 子模块编译时复制到安装目录。

### 5. 音频流水线 (`src/addon/pipeline/`)

Pipeline 是核心编排器，管理所有队列和工作线程。

```cpp
class Pipeline {
    // Queues（均带容量上限，超限丢最旧，防内存无界增长）
    ThreadSafeQueue<AudioFrame> frameQueue_{1024};        // 捕获 → VAD
    ThreadSafeQueue<SpeechEvent> speechEventQueue_{256};  // VAD → ASR
    ThreadSafeQueue<AsrResult> resultQueue_{128};         // ASR → 主线程

    unique_ptr<AudioCapture> capture_;     // 捕获后端
    unique_ptr<VADWorker> vadWorker_;      // VAD 工作线程
    unique_ptr<thread> asrThread_;         // ASR 调度线程
    shared_ptr<AsrEngine> asrEngine_;      // ASR 引擎（Session 工厂）
    shared_ptr<AsrSession> activeSession_; // 当前活跃会话
    unique_ptr<SessionReaper> reaper_;     // 游离会话回收
    unique_ptr<LLMClient> llmClient_;      // 可选 LLM 后处理
    vector<float> pendingAsrAudio_;        // 200ms 批处理缓冲

    atomic<bool> running_;
    atomic<uint64_t> generation_;          // 与 engine 的 activeGeneration 同步
    shared_ptr<atomic<bool>> resultGuard_; // Abort 后置 false，丢弃在途回调
};
```

**生命周期：**
1. `Init(config)` — 配置 VADWorker，连接队列
2. `SetAsrEngine(engine)` — 绑定回调并原子替换引擎（锁内发布指针）
3. `Start()` — 启动捕获 → VADWorker → ASR 调度线程
4. `Stop()` — 反向停止：捕获 → VAD → ASR → 清空队列
5. `Abort()` — 析构路径的强制清理：取消所有会话、清空队列、失效 resultGuard_

**结果流转：**
```
ASR 引擎回调 → Pipeline 内部闭包 → Push(AsrResult) → ResultQueue
                                                       ↓
engine::OnAsrResult()  → eventDispatcher_.schedule() → PollResults()
                                                       ↓
                                               commitString(text)
```

### 6. ASR 引擎 (`src/addon/asr/`)

```cpp
// AsrEngine — 全局单例（Pipeline 持有），工厂模式：StartSession() 产出独立会话
class AsrEngine {
public:
    struct Config {
        std::string modelName;
        // OpenAI 兼容（云端）
        std::string apiEndpoint;
        std::string apiKey;
        std::string language = "zh";
        std::string apiMode = "whisper";     // whisper / chat / realtime
        std::string authScheme = "bearer";   // bearer / api-key（MiMo）
        int commitIntervalMs = 5000;         // realtime 周期提交
        // 火山引擎豆包（云端）
        std::string authMode;
        std::string appKey;
        std::string accessKey;
        std::string resourceId;
        int chunkMs = 200;
        bool enableItN = true;
        bool enablePunc = true;
        bool enableDdc = false;
        bool enableNonstream = true;
        int endWindowMs = 800;
    };

    virtual bool Init(const Config& config) = 0;
    virtual std::shared_ptr<AsrSession> StartSession() = 0;
    virtual void CancelAllSessions();        // weak_ptr 追踪活跃会话，批量取消
    virtual const char* Name() const = 0;

    void SetResultCallback(AsrSession::ResultCallback cb);
    void SetErrorCallback(AsrSession::ErrorCallback cb);
};

// AsrSession — 单次识别的会话对象
class AsrSession {
public:
    virtual void FeedAudio(const float* pcm, size_t frames) = 0;
    virtual void End() = 0;                                  // 结束并取最终结果
    virtual void Cancel() = 0;
    virtual void JoinWithTimeout(std::chrono::milliseconds) = 0;
    virtual void StartWorker() = 0;   // 须在 shared_ptr 建立后调用
};
```

**各引擎实现（`ActiveBackend` 选择）：**

- `OpenaiAsrEngine`（`openai`，默认）— 按 `ApiMode` 分流：`whisper` 构建 WAV
  （16000Hz mono S16LE）POST multipart 到 `audio/transcriptions`；`chat` 把 WAV 转
  base64 data URI 放进 `chat/completions`（百炼 `qwen3-asr-flash` 走此法）。
  兼容 Groq / SiliconFlow 等服务。
- `RealtimeAsrEngine`（`openai` + `ApiMode=realtime`）— WebSocket 持久会话，
  16k→24k 重采样后持续推送，解析 delta 增量回调 partial。
- `VolcengineAsrEngine`（`volcengine`）— 豆包流式 WebSocket，支持 api_key 与
  app_access_key 两种认证。
- 小米 MiMo（`mimo`）— 复用 OpenAI 兼容引擎，强制 `chat` 格式 + `api-key` 认证。

**会话线程模型：**

| 引擎 | FeedAudio | End |
|------|-----------|-----|
| OpenAI（whisper/chat） | 追加到内部 buffer | 启动 HTTP worker → 发送整段 |
| OpenAI（realtime） | 入队 audioChunks_ → WS worker 实时发送 | flush + commit → 等 final → 关闭 WS |
| Volcengine | 入队 audioChunks_ → WS worker 实时发送 | 发 final 包 → 等响应 → 关闭 WS |

### 7. 线程安全队列 (`src/addon/utils/`)

```cpp
// ThreadSafeQueue<T> — 通用 mutex+cv 队列
// 用于 FrameQueue / SpeechEventQueue / ResultQueue
// 支持: Push / TryPop / Pop(阻塞) / Empty / Size / Stop

// AudioRingBuffer — Lock-free SPSC ring buffer
// 仅 PipeWire 内部使用: on_process 回调写(生产者) → DrainLoop 读(消费者)
// float32 数据，固定容量 65536 samples
// 无 Clear() 方法（与 PipeWire 回调存在 data race）
```

**为什么 ThreadSafeQueue 而不是 ring buffer 贯穿全局？**
- ThreadSafeQueue 更通用，支持多生产者多消费者
- VADWorker 和 ASRWorker 需要条件等待（有数据才醒来），mutex+cv 更自然
- Ring buffer 零拷贝优势在 PipeWire 回调场景才真正需要（≤100μs 约束）

---

## Fcitx5 集成细节

### 录音状态指示

通过 `subModeLabelImpl()` 返回当前状态文字，fcitx5 会显示在候选词区上方：

```
语音输入就绪              ← 空闲
按住热键说话              ← PTT 模式待按
录音中... [██████░░░░]   ← 录音中（电平条为 10 格，由 200ms 定时器刷新）
识别中...                ← ASR 进行中
修正中...                ← LLM 后处理中
```

状态通过 `activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea)` 更新。

### 输入法激活与延迟停止

```cpp
void VoiceInputEngine::activate(...) {
    activeIc_ = event.inputContext();
    activeGeneration_++;
    pipeline_->SetGeneration(generation);
    pipeline_->Start();
}

void VoiceInputEngine::deactivate(...) {
    pendingStopGeneration_ = generation;
    // 200ms 延迟停止，期间若重新 activate 则取消
    delayedStopEvent_ = instance_->eventLoop().addTimeEvent(...);
}
```

窗口快速切换时，activate 会取消 pending 的延迟停止任务，避免不必要的重启。

### 结果过滤机制

每轮 activate/deactivate 递增 `activeGeneration_`，pipeline 中每个 AsrResult 携带当前的 `generation_` 值。主线程 `PollResults()` 只提交匹配当前 generation 的结果，避免异步结果错乱。

---

## 线程安全模型

```
主线程 (Fcitx5 事件循环)
    │  队列写入: activeGeneration_, pendingStopGeneration_
    │  队列读取: ResultQueue (TryPop)
    │  事件驱动: EventDispatcher::schedule()
    ▼
┌──────────────────────────────────────────┐
│           ThreadSafeQueue<T>              │
│  - std::mutex + condition_variable        │
│  - 多生产者多消费者安全                    │
└──────────────────────────────────────────┘
    ▲                ▲               ▲
    │                │               │
  FrameQueue     SpeechEventQueue  ResultQueue
     │                │               │
 捕获线程          VAD Worker       ASR Worker
```

### 关键线程安全策略

1. **Generation 同步** — `std::atomic<uint64_t>` 隔离过期结果
2. **音频传递** — `ThreadSafeQueue<AudioFrame>`（mutex+cv 通用队列）
3. **语音事件** — `ThreadSafeQueue<SpeechEvent>`（VAD → ASR 事件流）
4. **PipeWire 回调** — Lock-free `AudioRingBuffer`（SPSC，仅 <100μs 写入）
5. **结果回传** — `EventDispatcher::schedule()` 调度到主线程轮询
6. **配置热更新** — `setConfig()` 只更新配置对象，不重启线程

---

## 错误处理策略

| 场景 | 行为 |
|------|------|
| PulseAudio 启动失败 | 日志警告，自动 fallback 到 PipeWire |
| PipeWire 启动失败 | 日志错误，capture_ 置空，pipeline 不启动 |
| VAD 模型加载失败 | VADWorker 不启动，日志错误 |
| ASR HTTP 请求失败 | 丢弃当前段，继续监听下一段 |
| ASR API Key 未配置 | 对应引擎 Init 返回 false，状态栏提示“语音识别未配置” |
| Volcengine 认证失败 | `VolcengineAsrEngine` Init 返回 false |
| Volcengine WebSocket 断开 | 丢弃当前段，继续下一个语音段 |
| Realtime WebSocket 断开 | 保持 sessionId 重连（最多 3 次），超限则丢弃当前段 |
| Volcengine 服务端错误 | 日志打印响应中的 `message` 和 `X-Tt-Logid` |
| OpenAI API 返回空结果 | 丢弃不上屏 |

---

## 构建与打包

### 依赖

```
fcitx5-voice-input
├── fcitx5                      # 输入法框架
├── libpulse-simple             # PulseAudio 音频捕获 (优先，与 pipewire 至少其一)
├── pipewire-0.3                # PipeWire 音频捕获 (fallback)
├── jsoncpp                     # JSON 解析
├── libcurl                     # HTTP/WebSocket 客户端（ASR 必需，>= 7.86.0）
├── zlib                        # Gzip 压缩（火山引擎后端必需）
└── onnxruntime                 # Silero VAD ONNX Runtime
```

两个录音库均在运行期 dlopen（无链接期依赖），缺任一个仅失去对应 capture 后端；
两个都缺则 CMake 报错。

### 构建选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `BUILD_TESTS` | OFF | 构建 `tests/` 单元测试 |
| `ONNXRUNTIME_ROOT` | "" | ONNX Runtime 自定义路径 |

### 构建产物

```
build/
└── voice-input-addon.so        # 唯一的输出产物
```

### 打包

- **Arch Linux**: `aur/PKGBUILD`（依赖 fcitx5 / jsoncpp / curl / onnxruntime-cpu /
  zlib；录音后端列为 optdepends）。Silero 模型作为固定 commit 的独立 source 下载
- **RPM**: CPack 生成（Fedora/openSUSE）；另有 `rpm/fcitx5-voice-input.spec`
  可手工 `rpmbuild` 出 SRPM/RPM
- **DEB**: CPack 自动生成（Ubuntu/Debian）
- **发布**: tag `v*` 触发 `release.yml`，产出各发行版包并生成 draft release。
  不向发行版仓库推送：AUR 同名包归上游维护，COPR 任务已移除

---

## 项目目录结构

```
fcitx5-voice-input/
├── README.md / README.zh-CN.md
├── LICENSE
├── CMakeLists.txt               # 顶配 CMake（包含所有源文件）
├── AGENTS.md / CLAUDE.md
├── .gitmodules
│
├── src/
│   └── addon/                   # 唯一的代码目录
│       ├── engine.cpp/.h       # VoiceInputEngine Fcitx5 入口
│       ├── types.h             # AudioFrame / SpeechEvent / AsrResult 类型定义
│       ├── voiceinput.conf.in  # Fcitx5 addon 配置模板（@PROJECT_VERSION@ 替换）
│       ├── config/
│       │   └── voiceinput-config.h   # FCITX_CONFIGURATION 宏定义
│       ├── capture/
│       │   ├── audio_capture.h              # 捕获后端抽象接口
│       │   ├── pulse_audio_capture.cpp/.h  # PulseAudio 优先
│       │   └── pipewire_capture.cpp/.h     # PipeWire fallback
│       ├── vad/
│       │   ├── silero_vad.cpp/.h  # Silero ONNX 封装
│       │   └── vad.cpp/.h         # VADWorker 状态机
│       ├── pipeline/
│       │   └── pipeline.cpp/.h    # 管道编排（3 队列 + 3 线程）
│       ├── asr/
│       │   ├── asr_engine.cpp/.h  # AsrEngine 抽象接口（StartSession 工厂）
│       │   ├── asr_session.h      # AsrSession 接口（FeedAudio/End/Cancel）
│       │   ├── openai_asr.cpp/.h  # OpenAI 兼容 ASR（whisper / chat 模式）
│       │   ├── realtime_asr.cpp/.h # OpenAI Realtime 流式（WS, 16k→24k）
│       │   ├── volcengine_asr.cpp/.h # 火山引擎豆包流式 ASR（WS）
│       │   ├── session_reaper.cpp/.h # 游离会话回收线程
│       │   └── utils/base64.cpp/.h   # base64（chat data URI、realtime 音频）
│       ├── llm/
│       │   └── llm_client.cpp/.h   # LLM 后处理客户端（OpenAI 兼容 chat）
│       └── utils/
│           ├── audio_buffer.h         # Lock-free SPSC ring buffer
│           └── thread_safe_queue.h    # mutex+cv 通用队列
│
├── third_party/
│   └── silero-vad/              # git submodule，VAD ONNX 模型 + 原始 Python 实现
│
├── po/
│   └── zh_CN.po                # 中文翻译
│
├── aur/
│   └── PKGBUILD                # Arch Linux 构建配方（发布为 release 附件）
├── rpm/
│   └── fcitx5-voice-input.spec # RPM 规格（可手工 rpmbuild 出 SRPM/RPM）
│
├── cmake/                       # 自定义 FindXXX.cmake
├── tests/                       # 单元测试
├── .github/
│   ├── workflows/
│   │   ├── ci.yml               # PR/push：多发行版构建矩阵 + 测试 + 链接校验
│   │   └── release.yml          # tag v*：构建产物 + draft release
│   └── actions/                 # 复用的 composite action（build / aur）
│
└── docs/                        # 架构、ADR、调研与开发文档
```

---

## Route Map

### Phase 1: 核心 ✅
- [x] CMake 构建框架 + Fcitx5 Addon 骨架
- [x] PulseAudio + PipeWire 音频捕获
- [x] Silero ONNX VAD 分段
- [x] OpenAI 兼容 API ASR 引擎
- [x] 录音→VAD→ASR→上屏完整流水线
- [x] fcitx5-configtool 配置界面

### Phase 2: 扩展 ✅
- [x] 火山引擎豆包流式 ASR（WebSocket）
- [x] OpenAI Realtime 流式转录（边说边出）
- [x] 小米 MiMo ASR（OpenAI 兼容 chat 格式）
- [x] 按住说话（PTT）模式 + 音量电平指示
- [x] LLM 后处理（OpenAI 兼容 chat）
- [x] 多发行版打包（DEB / RPM / Arch PKGBUILD）+ draft release 自动产出
- [x] 单元测试（`tests/`，CI 内执行）

### Phase 3: 打磨 ⏳
- [ ] Command 引擎（外部命令云 ASR）
- [ ] 场景系统
- [ ] 热词优化

---

## FAQ

### Q: Fcitx5 线程崩溃不就输入法没了？
A: ASR 线程独立运行，崩溃后 pipeline 会自动停止。但当前实现**没有** `std::async` + 超时兜底机制，崩溃会拖垮整个进程。这是已知风险。

### Q: 主线程不是还卡？
A: 主线程只负责事件分发和上屏。音频捕获（实时音频）在独立线程，HTTP ASR 请求（网络 IO 密集型）也在独立线程。唯一的"卡"是主线程的 `PollResults()` 轮询，但只做队列读取和 `commitString()`，微秒级操作。

### Q: 没有高级 JSON 配置层了？
A: 是的。`FCITX_CONFIGURATION` 宏已能满足当前所有配置需求（主配置 + OpenAI/火山引擎子配置 + VAD 参数）。将来场景系统需要复杂结构时可能重新引入 JSON 配置。

---

*—— 当前实现文档，与代码同步。*
