#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "config/voiceinput-config.h"
#include "capture/audio_capture.h"
#include "vad/vad.h"
#include "asr/asr_engine.h"
#include "asr/asr_session.h"
#include "asr/session_reaper.h"
#include "llm/llm_client.h"
#include "types.h"
#include "utils/thread_safe_queue.h"

namespace fcitx {

class Pipeline {
public:
    using ResultCallback = std::function<void(const std::string& text)>;

    Pipeline();
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void Init(const VoiceInputConfig& config);
    void SetAsrEngine(std::unique_ptr<AsrEngine> engine);
    void SetLLMClient(std::unique_ptr<LLMClient> client);
    void SetResultCallback(ResultCallback cb);
    void SetVadStatusCallback(VADWorker::VadStatusCallback cb);
    void SetLevelCallback(VADWorker::LevelCallback cb);
    void SetGeneration(uint64_t gen) { generation_.store(gen); }

    bool Start();
    // PTT 预启动：先把 VAD/ASR 线程拉起来但不采集，待按下热键时只需开采集
    bool Prepare();
    void Stop();
    // 只停音频采集（PTT 松开）：VAD/ASR 线程保留，让在途会话仍能跑完出结果。
    // 采集停掉后再次 Start 会走恢复路径，不会重建线程。
    void StopCapture();
    void Abort();
    bool IsRunning() const { return running_.load(); }

    ThreadSafeQueue<AsrResult>& ResultQueue() { return resultQueue_; }

    void SetConfig(const VoiceInputConfig& config);

private:
    bool StartCapture();
    bool StartWorkers(bool withCapture);
    void ApplyVadMode();
    void AsrDispatcherLoop();

    // Queues（带容量上限：超限丢最旧，防止异常路径下内存无界增长）
    // 1024 帧 ≈ 32s 音频缓冲；256 事件 ≈ 256 段语音；128 结果
    ThreadSafeQueue<AudioFrame> frameQueue_{1024};
    ThreadSafeQueue<SpeechEvent> speechEventQueue_{256};
    ThreadSafeQueue<AsrResult> resultQueue_{128};

    // Workers
    std::unique_ptr<VADWorker> vadWorker_;
    std::unique_ptr<std::thread> asrThread_;

    // Capture
    std::unique_ptr<AudioCapture> capture_;

    // ASR session management
    std::shared_ptr<AsrEngine> asrEngine_;
    std::shared_ptr<AsrSession> activeSession_;
    uint64_t activeSessionId_{0};
    std::unordered_map<uint64_t, uint64_t> sessionGenerationMap_;
    std::mutex sessionMapMutex_;   // 保护 sessionGenerationMap_（worker/ASR/主线程三方）
    std::mutex engineMutex_;       // 保护 asrEngine_ 指针替换与使用
    std::unique_ptr<SessionReaper> reaper_;

    // ASR streaming batching
    std::vector<float> pendingAsrAudio_;

    // LLM
    std::unique_ptr<LLMClient> llmClient_;

    // State
    std::atomic<bool> running_{false};
    // PTT：采集被 StopCapture 暂停、但流水线仍在运行（见 Start 的恢复分支）
    bool capturePaused_ = false;
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> utteranceCounter_{0};
    // 回调守卫：Abort 后置 false，引擎 worker 线程的异步回调据此丢弃
    std::shared_ptr<std::atomic<bool>> resultGuard_ =
        std::make_shared<std::atomic<bool>>(true);

    // Config
    VoiceInputConfig config_;

    // Callback
    ResultCallback resultCb_;
};

} // namespace fcitx
