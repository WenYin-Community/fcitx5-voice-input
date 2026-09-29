#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "types.h"
#include "utils/thread_safe_queue.h"

namespace fcitx {

class VadModel;

class VADWorker {
public:
    // VAD tuning parameters
    struct Config {
        float speechThreshold = 0.5f;
        float silenceThreshold = 0.35f;
        int startFrames = 2;           // consecutive speech frames to trigger onset
        int preRollMs = 300;           // audio before onset to include
        int endSilenceMs = 700;        // silence before ending utterance
        int minSpeechMs = 300;         // minimum utterance duration
        int maxSpeechMs = 30000;       // maximum utterance duration
        std::string sileroModelPath;   // empty = installed default
    };

    VADWorker();
    ~VADWorker();

    VADWorker(const VADWorker&) = delete;
    VADWorker& operator=(const VADWorker&) = delete;

    using VadStatusCallback = std::function<void(bool speaking)>;
    using LevelCallback = std::function<void(int level)>;  // level: 0-10

    void SetConfig(const Config& config);
    void SetFrameQueue(ThreadSafeQueue<AudioFrame>* queue);
    void SetSpeechEventQueue(ThreadSafeQueue<SpeechEvent>* queue);
    void SetVadStatusCallback(VadStatusCallback cb);
    void SetLevelCallback(LevelCallback cb);

    // When true (PTT mode), skip the VAD model and emit Begin/Audio/End
    // events for all captured audio; short utterances are not filtered.
    // 原子量：主线程可在 worker 运行期间切换模式（setConfig 热加载）
    void SetDirectPush(bool direct) {
        directPush_.store(direct);
        // 切换模式时把上一模式未收尾的语音段结束掉，避免会话悬挂
        if (!direct) flushRequested_.store(true);
    }

    // PTT 松开、采集停止后由主线程调用：把当前语音段收尾（推 End）。
    // 显式请求而非用「队列空闲」推断，否则正常录音的帧间隙会被误判为结束。
    void RequestFlush() { flushRequested_.store(true); }

    // 恢复采集时清除未兑现的收尾请求。必须在开始新一段录音前调用，
    // 否则上一次（如激活时的预停）留下的请求会在新段首个帧间隙里误解发。
    void ClearFlushRequest() { flushRequested_.store(false); }

    // Test seam: overrides the model created in Start().
    void SetVadModel(std::unique_ptr<VadModel> model);

    void Start();
    void Stop();

    bool IsRunning() const { return running_.load(); }

private:
    void WorkerLoop();
    void ProcessFrame(const AudioFrame& frame, float probability,
                      const Config& config);
    void FlushUtterance(int64_t endMs);
    void AppendPreRoll(const std::array<int16_t, kWindowSize>& pcm,
                       size_t maxPreRollSamples);
    void ResetSession();

    Config config_;
    std::mutex configMutex_;  // SetConfig（主线程）与 worker 线程快照隔离

    std::unique_ptr<VadModel> silero_;
    std::string loadedModelPath_;  // 已加载模型路径（模型缓存复用判断）

    ThreadSafeQueue<AudioFrame>* frameQueue_ = nullptr;
    ThreadSafeQueue<SpeechEvent>* speechEventQueue_ = nullptr;

    std::unique_ptr<std::thread> thread_;
    std::atomic<bool> running_{false};

    // Callback
    VadStatusCallback vadStatusCb_;
    LevelCallback levelCb_;

    // Direct push mode (PTT): skip VAD model
    std::atomic<bool> directPush_{false};
    bool sessionActive_ = false;  // directPush 会话进行中
    std::atomic<bool> flushRequested_{false};

    // Session state
    enum class State { Idle, Speaking };
    State state_ = State::Idle;

    std::deque<int16_t> preRoll_;

    int speechFrames_ = 0;
    int silenceFrames_ = 0;
    int64_t startMs_ = 0;
    int64_t lastSpeechMs_ = 0;
};

} // namespace fcitx
