#include "vad.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include <fcitx-utils/log.h>

#include "silero_vad.h"

namespace fcitx {

namespace {

std::string DefaultSileroModelPath() {
    return std::string(VOICE_INPUT_MODEL_DIR) + "/silero_vad.onnx";
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

VADWorker::VADWorker() = default;

VADWorker::~VADWorker() {
    Stop();
}

void VADWorker::SetConfig(const Config& config) {
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        config_ = config;
    }

    FCITX_INFO() << "[voice-input:vadworker] Config:"
                 << " speechThresh=" << config_.speechThreshold
                 << " silenceThresh=" << config_.silenceThreshold
                 << " startFrames=" << config_.startFrames
                 << " preRollMs=" << config_.preRollMs
                 << " endSilenceMs=" << config_.endSilenceMs
                 << " minSpeechMs=" << config_.minSpeechMs
                 << " maxSpeechMs=" << config_.maxSpeechMs;
}

void VADWorker::SetFrameQueue(ThreadSafeQueue<AudioFrame>* queue) {
    frameQueue_ = queue;
}

void VADWorker::SetUtteranceQueue(ThreadSafeQueue<Utterance>* queue) {
    utteranceQueue_ = queue;
}

void VADWorker::SetVadStatusCallback(VadStatusCallback cb) {
    vadStatusCb_ = std::move(cb);
}

void VADWorker::SetLevelCallback(LevelCallback cb) {
    levelCb_ = std::move(cb);
}

void VADWorker::Start() {
    if (running_) return;

    if (!directPush_) {
        // Init Silero VAD model
        std::string modelPath = config_.sileroModelPath.empty()
                                    ? DefaultSileroModelPath()
                                    : config_.sileroModelPath;
        silero_ = std::make_unique<SileroVad>(modelPath);
        if (!silero_->IsReady()) {
            FCITX_ERROR() << "[voice-input:vadworker] SileroVad init failed";
            return;
        }
    } else {
        FCITX_INFO() << "[voice-input:vadworker] Direct push mode (VAD model skipped)";
    }

    ResetSession();
    running_ = true;
    thread_ = std::make_unique<std::thread>(&VADWorker::WorkerLoop, this);
    FCITX_INFO() << "[voice-input:vadworker] Started";
}

void VADWorker::Stop() {
    if (!running_) return;
    running_ = false;
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
    thread_.reset();
    silero_.reset();
    FCITX_INFO() << "[voice-input:vadworker] Stopped";
}

void VADWorker::WorkerLoop() {
    Config cfg;
    while (running_) {
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            cfg = config_;
        }

        AudioFrame frame;

        if (!frameQueue_ || !frameQueue_->TryPop(frame)) {
            // Direct push mode: flush accumulated audio when idle
            if (directPush_ && !currentAudio_.empty()
                && utteranceQueue_ && utteranceQueue_->Size() < 2) {
                FlushUtterance(frame.timestamp_ms, cfg);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        // Compute audio level (RMS → 0-10 scale)
        if (levelCb_) {
            float sumSq = 0.0f;
            for (auto s : frame.pcm) sumSq += static_cast<float>(s) * s;
            float rms = std::sqrt(sumSq / frame.pcm.size());
            int level = static_cast<int>(std::min(rms / 3000.0f * 10.0f, 10.0f));
            levelCb_(level);
        }

        if (directPush_) {
            // Bypass VAD model, accumulate all audio
            if (currentAudio_.empty()) {
                startMs_ = frame.timestamp_ms;
                if (vadStatusCb_) vadStatusCb_(true);
            }
            currentAudio_.insert(currentAudio_.end(),
                                 frame.pcm.begin(), frame.pcm.end());
            lastSpeechMs_ = frame.timestamp_ms;
            continue;
        }

        float prob = silero_->Predict(frame.pcm.data(), frame.pcm.size());
        if (prob < 0.0f) {
            // Inference failed
            continue;
        }

        ProcessFrame(frame, prob, cfg);
    }

    // Flush remaining audio on stop
    if (directPush_ && !currentAudio_.empty() && utteranceQueue_) {
        FlushUtterance(lastSpeechMs_, cfg);
    }
}

void VADWorker::ProcessFrame(const AudioFrame& frame, float probability,
                             const Config& cfg) {
    bool speechStart = probability >= cfg.speechThreshold;
    bool speechKeep = probability >= cfg.silenceThreshold;

    AppendPreRoll(frame.pcm, cfg);

    if (state_ == State::Idle) {
        if (speechStart) {
            speechFrames_++;
            if (speechFrames_ >= cfg.startFrames) {
                // Speech onset
                state_ = State::Speaking;
                startMs_ = frame.timestamp_ms - cfg.preRollMs;

                currentAudio_.clear();
                currentAudio_.insert(currentAudio_.end(),
                                    preRoll_.begin(), preRoll_.end());

                silenceFrames_ = 0;
                lastSpeechMs_ = frame.timestamp_ms;
                speechFrames_ = 0;

                FCITX_INFO() << "[voice-input:vadworker] Speech onset"
                             << " startMs=" << startMs_
                             << " preRollSamples=" << preRoll_.size();
                if (vadStatusCb_) {
                    vadStatusCb_(true);
                }
            }
        } else {
            speechFrames_ = 0;
        }
        return;
    }

    // State::Speaking
    currentAudio_.insert(currentAudio_.end(),
                         frame.pcm.begin(), frame.pcm.end());

    if (speechKeep) {
        silenceFrames_ = 0;
        lastSpeechMs_ = frame.timestamp_ms;
    } else {
        silenceFrames_++;
    }

    int endSilenceFrames =
        cfg.endSilenceMs / kFrameMs;
    bool silenceEnd = silenceFrames_ >= endSilenceFrames;

    int maxSamples = kSampleRate * cfg.maxSpeechMs / 1000;
    bool tooLong = currentAudio_.size() >= static_cast<size_t>(maxSamples);

    if (silenceEnd || tooLong) {
        FlushUtterance(frame.timestamp_ms, cfg);
    }
}

void VADWorker::FlushUtterance(int64_t endMs, const Config& cfg) {
    int durationMs =
        static_cast<int>((lastSpeechMs_ - startMs_));
    int minSpeechSamples =
        kSampleRate * cfg.minSpeechMs / 1000;

    // PTT mode: the user explicitly held the key, so short utterances
    // ("好", "嗯") must not be filtered by minSpeechMs.
    if (directPush_ || static_cast<int>(currentAudio_.size()) >= minSpeechSamples) {
        Utterance u;
        u.start_ms = startMs_;
        u.end_ms = lastSpeechMs_;
        u.pcm = std::move(currentAudio_);

        float durSec = static_cast<float>(durationMs) / 1000.0f;
        FCITX_INFO() << "[voice-input:vadworker] Utterance: "
                     << durSec << "s, "
                     << u.pcm.size() << " samples";

        if (utteranceQueue_) {
            utteranceQueue_->Push(std::move(u));
        }
    } else {
        FCITX_DEBUG() << "[voice-input:vadworker] Utterance too short ("
                      << durationMs << "ms < " << config_.minSpeechMs
                      << "ms), discarded";
    }

    if (silero_) silero_->Reset();
    if (vadStatusCb_) {
        vadStatusCb_(false);
    }
    ResetSession();
}

void VADWorker::AppendPreRoll(
    const std::array<int16_t, kWindowSize>& pcm, const Config& cfg) {
    for (auto sample : pcm) {
        preRoll_.push_back(sample);
    }

    size_t maxPreRollSamples =
        static_cast<size_t>(kSampleRate) * cfg.preRollMs / 1000;
    while (preRoll_.size() > maxPreRollSamples) {
        preRoll_.pop_front();
    }
}

void VADWorker::ResetSession() {
    state_ = State::Idle;
    preRoll_.clear();
    currentAudio_.clear();
    speechFrames_ = 0;
    silenceFrames_ = 0;
    startMs_ = 0;
    lastSpeechMs_ = 0;
}

} // namespace fcitx
