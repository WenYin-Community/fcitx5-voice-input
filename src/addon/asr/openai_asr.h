#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "asr_engine.h"

namespace fcitx {

/**
 * ASR engine for OpenAI Whisper API and compatible providers
 * (Groq, Together AI, DeepSeek, etc.).
 *
 * User configures the endpoint, API key, and model name at runtime.
 * Audio is sent as a WAV file via multipart/form-data POST request.
 */
class OpenaiCompatAsrEngine : public AsrEngine {
public:
    OpenaiCompatAsrEngine();
    ~OpenaiCompatAsrEngine() override;

    OpenaiCompatAsrEngine(const OpenaiCompatAsrEngine&) = delete;
    OpenaiCompatAsrEngine& operator=(const OpenaiCompatAsrEngine&) = delete;

    bool Init(const Config& config) override;
    void Start() override;
    void FeedAudio(const float* pcm, size_t frames) override;
    void Stop() override;
    const char* Name() const override { return "openai-compat"; }

private:
    void TranscribeWorker(std::vector<float> audio);

    // HTTP POST multipart/form-data to the API endpoint
    std::string DoHttpRequest(const std::vector<uint8_t>& wavData);

    // Config
    std::string apiEndpoint_;
    std::string apiKey_;
    std::string modelName_;
    std::string language_;
    std::string apiFormat_; // "whisper" or "chat"

    // Audio buffer (accumulated during recording)
    std::vector<float> pcmBuffer_;

    // Thread management
    std::atomic<bool> cancelled_{false};
    // Transcription workers run detached; a ticket lock keeps them strictly
    // FIFO so results are pushed in utterance order. Destruction waits for
    // all in-flight workers via activeWorkers_.
    std::mutex ticketMutex_;
    std::condition_variable ticketCv_;
    int nextTicket_ = 0;
    int servedTicket_ = 0;
    std::atomic<int> activeWorkers_{0};
};

} // namespace fcitx
