#include "vad.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <chrono>
#include <thread>

#include <fcitx-utils/log.h>

#include "silero_vad.h"

namespace fcitx {

namespace {

std::string DefaultSileroModelPath() {
    return std::string(VOICE_INPUT_MODEL_DIR) + "/silero_vad.onnx";
}

size_t PreRollSamples(int preRollMs) {
    return static_cast<size_t>(kSampleRate) * preRollMs / 1000;
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
                 << " speechThresh=" << config.speechThreshold
                 << " silenceThresh=" << config.silenceThreshold
                 << " startFrames=" << config.startFrames
                 << " preRollMs=" << config.preRollMs
                 << " endSilenceMs=" << config.endSilenceMs
                 << " minSpeechMs=" << config.minSpeechMs
                 << " maxSpeechMs=" << config.maxSpeechMs;
}

void VADWorker::SetFrameQueue(ThreadSafeQueue<AudioFrame>* queue) {
    frameQueue_ = queue;
}

void VADWorker::SetSpeechEventQueue(ThreadSafeQueue<SpeechEvent>* queue) {
    speechEventQueue_ = queue;
}

void VADWorker::SetVadStatusCallback(VadStatusCallback cb) {
    vadStatusCb_ = std::move(cb);
}

void VADWorker::SetLevelCallback(LevelCallback cb) {
    levelCb_ = std::move(cb);
}

void VADWorker::SetVadModel(std::unique_ptr<VadModel> model) {
    silero_ = std::move(model);
    // 与 Start() 的路径缓存对齐：注入的模型不再触发"路径变化重建"
    std::string modelPath = config_.sileroModelPath.empty()
                                ? DefaultSileroModelPath()
                                : config_.sileroModelPath;
    loadedModelPath_ = modelPath;
}

void VADWorker::Start() {
    if (running_) return;

    // 队列是必需的协作对象，由 Pipeline::Init 在 Start 之前接好；
    // 缺失属于装配错误，Debug 构建下立即暴露而不是静默丢音频
    assert(frameQueue_);
    assert(speechEventQueue_);

    if (!directPush_) {
        // Init Silero（跨会话缓存模型实例，避免每次切换输入法都在主线程
        // 重建 ONNX Session 造成卡顿；模型路径变化时重建）
        std::string modelPath = config_.sileroModelPath.empty()
                                    ? DefaultSileroModelPath()
                                    : config_.sileroModelPath;
        if (!silero_ || loadedModelPath_ != modelPath) {
            silero_ = std::make_unique<SileroVad>(modelPath);
            loadedModelPath_ = modelPath;
        }
        if (!silero_->IsReady()) {
            FCITX_ERROR() << "[voice-input:vadworker] SileroVad init failed";
            silero_.reset();
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
    // 保留 silero_：模型实例跨会话复用（见 Start）
    FCITX_INFO() << "[voice-input:vadworker] Stopped";
}

void VADWorker::WorkerLoop() {
    Config config;
    while (running_) {
        AudioFrame frame;

        if (!frameQueue_->TryPop(frame)) {
            // Direct push mode: end the session when the queue is idle
            if (directPush_ && sessionActive_) {
                FlushUtterance(frame.timestamp_ms);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        // 快照配置：SetConfig 可能在主线程并发改写 config_
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            config = config_;
        }

        // Compute audio level (RMS → 0-10 scale) for UI visualization
        if (levelCb_) {
            float sumSq = 0.0f;
            for (auto s : frame.pcm) sumSq += static_cast<float>(s) * s;
            float rms = std::sqrt(sumSq / frame.pcm.size());
            int level = static_cast<int>(std::min(rms / 3000.0f * 10.0f, 10.0f));
            levelCb_(level);
        }

        if (directPush_) {
            // PTT mode: bypass VAD, stream everything as a session
            if (!sessionActive_) {
                sessionActive_ = true;
                startMs_ = frame.timestamp_ms;

                SpeechEvent begin;
                begin.type = SpeechEventType::Begin;
                begin.timestamp_ms = startMs_;
                speechEventQueue_->Push(std::move(begin));
                if (vadStatusCb_) vadStatusCb_(true);
            }

            SpeechEvent audio;
            audio.type = SpeechEventType::Audio;
            audio.timestamp_ms = frame.timestamp_ms;
            audio.pcm.assign(frame.pcm.begin(), frame.pcm.end());
            speechEventQueue_->Push(std::move(audio));
            lastSpeechMs_ = frame.timestamp_ms;
            continue;
        }

        float prob = silero_->Predict(frame.pcm.data(), frame.pcm.size());
        if (prob < 0.0f) {
            // Inference failed
            continue;
        }

        ProcessFrame(frame, prob, config);
    }

    // Flush remaining audio on stop (direct push mode)
    if (directPush_ && sessionActive_) {
        FlushUtterance(lastSpeechMs_);
    }
}

void VADWorker::FlushUtterance(int64_t endMs) {
    if (!sessionActive_) return;
    sessionActive_ = false;

    // PTT mode: the user explicitly held the key, so short utterances
    // ("好", "嗯") are not filtered by minSpeechMs.
    SpeechEvent end;
    end.type = SpeechEventType::End;
    end.timestamp_ms = endMs;
    speechEventQueue_->Push(std::move(end));
    if (vadStatusCb_) vadStatusCb_(false);
    ResetSession();
}

void VADWorker::ProcessFrame(const AudioFrame& frame, float probability,
                             const Config& config) {
    bool speechStart = probability >= config.speechThreshold;
    bool speechKeep = probability >= config.silenceThreshold;

    if (state_ == State::Idle) {
        if (speechStart) {
            speechFrames_++;
            if (speechFrames_ >= config.startFrames) {
                // 本帧触发 onset：pre-roll 只包含它之前的音频，本帧随后
                // 作为普通音频单独推送，避免同一帧被发送两次
                state_ = State::Speaking;
                startMs_ = frame.timestamp_ms - config.preRollMs;

                SpeechEvent begin;
                begin.type = SpeechEventType::Begin;
                begin.timestamp_ms = startMs_;
                speechEventQueue_->Push(std::move(begin));

                if (!preRoll_.empty()) {
                    SpeechEvent preAudio;
                    preAudio.type = SpeechEventType::Audio;
                    preAudio.timestamp_ms = startMs_;
                    preAudio.pcm.assign(preRoll_.begin(), preRoll_.end());
                    speechEventQueue_->Push(std::move(preAudio));
                }

                SpeechEvent audio;
                audio.type = SpeechEventType::Audio;
                audio.timestamp_ms = frame.timestamp_ms;
                audio.pcm.assign(frame.pcm.begin(), frame.pcm.end());
                speechEventQueue_->Push(std::move(audio));

                silenceFrames_ = 0;
                lastSpeechMs_ = frame.timestamp_ms;
                speechFrames_ = 0;

                FCITX_INFO() << "[voice-input:vadworker] Speech onset"
                             << " startMs=" << startMs_
                             << " preRollSamples=" << preRoll_.size();
                if (vadStatusCb_) {
                    vadStatusCb_(true);
                }
            } else {
                AppendPreRoll(frame.pcm, PreRollSamples(config.preRollMs));
            }
        } else {
            speechFrames_ = 0;
            AppendPreRoll(frame.pcm, PreRollSamples(config.preRollMs));
        }
        return;
    }

    // State::Speaking
    SpeechEvent audio;
    audio.type = SpeechEventType::Audio;
    audio.timestamp_ms = frame.timestamp_ms;
    audio.pcm.assign(frame.pcm.begin(), frame.pcm.end());
    speechEventQueue_->Push(std::move(audio));

    if (speechKeep) {
        silenceFrames_ = 0;
        lastSpeechMs_ = frame.timestamp_ms;
    } else {
        silenceFrames_++;
    }

    int endSilenceFrames =
        config.endSilenceMs / kFrameMs;
    bool silenceEnd = silenceFrames_ >= endSilenceFrames;

    int maxDurationMs = config.maxSpeechMs;
    bool tooLong =
        (lastSpeechMs_ - startMs_) >= maxDurationMs;

    if (silenceEnd || tooLong) {
        int durationMs = static_cast<int>((lastSpeechMs_ - startMs_));
        if (durationMs >= config.minSpeechMs) {
            SpeechEvent end;
            end.type = SpeechEventType::End;
            end.timestamp_ms = frame.timestamp_ms;
            speechEventQueue_->Push(std::move(end));
            FCITX_INFO() << "[voice-input:vadworker] Utterance end, "
                         << (durationMs / 1000) << "." << (durationMs % 1000) << "s";
        } else {
            SpeechEvent cancel;
            cancel.type = SpeechEventType::Cancel;
            cancel.timestamp_ms = frame.timestamp_ms;
            speechEventQueue_->Push(std::move(cancel));
            FCITX_DEBUG() << "[voice-input:vadworker] Utterance too short ("
                          << durationMs << "ms < " << config.minSpeechMs
                          << "ms), cancelled";
        }

        silero_->Reset();
        if (vadStatusCb_) {
            vadStatusCb_(false);
        }
        ResetSession();
    }
}

void VADWorker::AppendPreRoll(
    const std::array<int16_t, kWindowSize>& pcm, size_t maxPreRollSamples) {
    for (auto sample : pcm) {
        preRoll_.push_back(sample);
    }

    while (preRoll_.size() > maxPreRollSamples) {
        preRoll_.pop_front();
    }
}

void VADWorker::ResetSession() {
    state_ = State::Idle;
    preRoll_.clear();
    speechFrames_ = 0;
    silenceFrames_ = 0;
    startMs_ = 0;
    lastSpeechMs_ = 0;
    sessionActive_ = false;
}

} // namespace fcitx
