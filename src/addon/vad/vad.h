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
    void SetUtteranceQueue(ThreadSafeQueue<Utterance>* queue);
    void SetVadStatusCallback(VadStatusCallback cb);
    void SetLevelCallback(LevelCallback cb);

    void Start();
    void Stop();

    // When true, skip VAD model and push all audio directly to utterance queue
    void SetDirectPush(bool direct) { directPush_ = direct; }

    // Test seam: overrides the model created in Start().
    void SetVadModel(std::unique_ptr<VadModel> model);

    bool IsRunning() const { return running_.load(); }

private:
    void WorkerLoop();
    void ProcessFrame(const AudioFrame& frame, float probability,
                      const Config& cfg);
    void FlushUtterance(int64_t endMs, const Config& cfg);
    void AppendPreRoll(const std::array<int16_t, kWindowSize>& pcm,
                       const Config& cfg);
    void ResetSession();

    Config config_;

    // Protects config_ (written by SetConfig on the main thread, read by
    // WorkerLoop on the VAD thread).
    std::mutex configMutex_;

    std::unique_ptr<VadModel> silero_;

    ThreadSafeQueue<AudioFrame>* frameQueue_ = nullptr;
    ThreadSafeQueue<Utterance>* utteranceQueue_ = nullptr;

    std::unique_ptr<std::thread> thread_;
    std::atomic<bool> running_{false};

    // Callback
    VadStatusCallback vadStatusCb_;
    LevelCallback levelCb_;

    // Direct push mode (skip VAD model)
    bool directPush_ = false;

    // Session state
    enum class State { Idle, Speaking };
    State state_ = State::Idle;

    std::deque<int16_t> preRoll_;
    std::vector<int16_t> currentAudio_;

    int speechFrames_ = 0;
    int silenceFrames_ = 0;
    int64_t startMs_ = 0;
    int64_t lastSpeechMs_ = 0;
};

} // namespace fcitx
