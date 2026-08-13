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
 * ASR engine for Xiaomi MiMo ASR (mimo-v2.5-asr).
 *
 * Uses /v1/chat/completions endpoint with input_audio content.
 * Endpoint defaults to https://api.xiaomimimo.com/v1.
 */
class MiMoAsrEngine : public AsrEngine {
public:
    MiMoAsrEngine();
    ~MiMoAsrEngine() override;

    MiMoAsrEngine(const MiMoAsrEngine&) = delete;
    MiMoAsrEngine& operator=(const MiMoAsrEngine&) = delete;

    bool Init(const Config& config) override;
    void Start() override;
    void FeedAudio(const float* pcm, size_t frames) override;
    void Stop() override;
    const char* Name() const override { return "mimo"; }

private:
    void TranscribeWorker(std::vector<float> audio);
    std::string DoHttpRequest(const std::vector<uint8_t>& wavData);

    // Config
    std::string apiEndpoint_;
    std::string apiKey_;
    std::string modelName_;
    std::string language_;

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
