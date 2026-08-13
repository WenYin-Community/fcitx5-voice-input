// Minimal dependency-free unit tests for fcitx5-voice-input.
// Build with: cmake -B build -DBUILD_TESTS=ON && cmake --build build && ctest --test-dir build

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "asr/wav_encoder.h"
#include "types.h"
#include "utils/thread_safe_queue.h"
#include "vad/silero_vad.h"
#include "vad/vad.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        auto va = (a);                                                           \
        auto vb = (b);                                                           \
        if (!(va == vb)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s == %s (got %lld vs %lld)\n",    \
                         __FILE__, __LINE__, #a, #b, (long long)va,              \
                         (long long)vb);                                         \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

// ── ThreadSafeQueue ──────────────────────────────────────────────────

void TestQueueFifo() {
    ThreadSafeQueue<int> q;
    CHECK(q.Empty());
    CHECK_EQ(q.Size(), 0u);
    q.Push(1);
    q.Push(2);
    q.Push(3);
    CHECK_EQ(q.Size(), 3u);
    int v = 0;
    CHECK(q.TryPop(v));
    CHECK_EQ(v, 1);
    CHECK(q.TryPop(v));
    CHECK_EQ(v, 2);
    CHECK(q.TryPop(v));
    CHECK_EQ(v, 3);
    CHECK(!q.TryPop(v));
    CHECK(q.Empty());
}

void TestQueueConcurrent() {
    ThreadSafeQueue<int> q;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 2000;
    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t) {
        producers.emplace_back([&q, t] {
            for (int i = 0; i < kPerThread; ++i) q.Push(t * kPerThread + i);
        });
    }
    for (auto& th : producers) th.join();
    CHECK_EQ(q.Size(), static_cast<size_t>(kThreads * kPerThread));
    int v = 0;
    int count = 0;
    while (q.TryPop(v)) ++count;
    CHECK_EQ(count, kThreads * kPerThread);
}

// ── WAV / Base64 ─────────────────────────────────────────────────────

void TestWavEncode() {
    float pcm[] = {0.5f, -0.5f, 1.0f, -1.0f, 0.0f};
    auto wav = fcitx::FloatPcmToWav(pcm, 5);
    CHECK_EQ(wav.size(), 44u + 10u);
    CHECK(std::memcmp(wav.data(), "RIFF", 4) == 0);
    CHECK(std::memcmp(wav.data() + 8, "WAVE", 4) == 0);
    CHECK(std::memcmp(wav.data() + 36, "data", 4) == 0);

    uint32_t dataSize = 0;
    std::memcpy(&dataSize, wav.data() + 40, 4);
    CHECK_EQ(dataSize, 10u);

    auto sampleAt = [&wav](size_t i) {
        int16_t s = 0;
        std::memcpy(&s, wav.data() + 44 + i * 2, 2);
        return s;
    };
    CHECK_EQ(sampleAt(0), static_cast<int16_t>(16383));   // 0.5 * 32767
    CHECK_EQ(sampleAt(1), static_cast<int16_t>(-16383));  // -0.5 * 32767
    CHECK_EQ(sampleAt(2), static_cast<int16_t>(32767));   // clamped
    CHECK_EQ(sampleAt(3), static_cast<int16_t>(-32767));  // clamped
    CHECK_EQ(sampleAt(4), static_cast<int16_t>(0));
}

void TestBase64() {
    CHECK(fcitx::Base64Encode(nullptr, 0) == "");
    const char* man = "Man";
    CHECK(fcitx::Base64Encode(reinterpret_cast<const uint8_t*>(man), 3) == "TWFu");
    const char* abc = "abc";
    CHECK(fcitx::Base64Encode(reinterpret_cast<const uint8_t*>(abc), 3) == "YWJj");
    const char* ab = "ab";
    CHECK(fcitx::Base64Encode(reinterpret_cast<const uint8_t*>(ab), 2) == "YWI=");
}

// ── VADWorker state machine ─────────────────────────────────────────

class MockVad : public fcitx::VadModel {
public:
    // Per-frame speech probabilities; last value repeats.
    std::shared_ptr<std::vector<float>> probs =
        std::make_shared<std::vector<float>>();
    bool ready = true;

    float Predict(const int16_t*, size_t) override {
        int idx = calls < static_cast<int>(probs->size()) ? calls
                                                          : static_cast<int>(probs->size()) - 1;
        ++calls;
        return (*probs)[idx];
    }
    void Reset() override {}
    bool IsReady() const override { return ready; }

private:
    int calls = 0;
};

struct VadTestHarness {
    fcitx::VADWorker worker;
    ThreadSafeQueue<fcitx::AudioFrame> frames;
    ThreadSafeQueue<fcitx::Utterance> utterances;
    std::shared_ptr<std::vector<float>> probs;

    VadTestHarness() : probs(std::make_shared<std::vector<float>>()) {
        fcitx::VADWorker::Config cfg;
        cfg.speechThreshold = 0.5f;
        cfg.silenceThreshold = 0.35f;
        cfg.startFrames = 2;
        cfg.preRollMs = 64;     // 2 frames
        cfg.endSilenceMs = 96;  // 3 frames
        cfg.minSpeechMs = 96;   // 3 frames
        cfg.maxSpeechMs = 20000;
        worker.SetConfig(cfg);
        worker.SetFrameQueue(&frames);
        worker.SetUtteranceQueue(&utterances);
        auto vad = std::make_unique<MockVad>();
        vad->probs = probs;
        worker.SetVadModel(std::move(vad));
    }

    ~VadTestHarness() { worker.Stop(); }

    // Push `count` frames of `level`-amplitude audio, timestamps 32ms apart.
    void Push(int count, float level) {
        for (int i = 0; i < count; ++i) {
            fcitx::AudioFrame f;
            f.timestamp_ms = static_cast<int64_t>(i * fcitx::kFrameMs);
            std::fill(f.pcm.begin(), f.pcm.end(),
                      static_cast<int16_t>(level * 32000.0f));
            frames.Push(f);
        }
    }
};

bool WaitForQueueNotEmpty(ThreadSafeQueue<fcitx::Utterance>& q, int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (q.Size() > 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void TestVadSpeechOnset() {
    VadTestHarness h;
    // 2 onset frames, 3 speech frames, 3 trailing silence frames
    h.probs->assign({0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.0f, 0.0f, 0.0f});
    h.worker.Start();
    h.Push(8, 0.5f);

    fcitx::Utterance u;
    CHECK(WaitForQueueNotEmpty(h.utterances, 2000));
    CHECK(h.utterances.TryPop(u));
    // pre-roll (2) + speech (3) + trailing silence (3) = 8 frames
    CHECK_EQ(u.pcm.size(), 8u * fcitx::kWindowSize);
    // first sample is the pre-roll audio of frame 0
    CHECK_EQ(u.pcm[0], static_cast<int16_t>(0.5f * 32000.0f));
    CHECK(!WaitForQueueNotEmpty(h.utterances, 300));  // nothing left
}

void TestVadShortUtteranceDiscarded() {
    VadTestHarness h;
    // Shortest possible utterance is pre-roll (2) + 1 trailing frame = 3
    // frames (96ms); raise minSpeechMs so this one is below the limit.
    fcitx::VADWorker::Config cfg;
    cfg.speechThreshold = 0.5f;
    cfg.silenceThreshold = 0.35f;
    cfg.startFrames = 2;
    cfg.preRollMs = 64;
    cfg.endSilenceMs = 96;
    cfg.minSpeechMs = 192;  // 6 frames — this utterance is 5 frames
    cfg.maxSpeechMs = 20000;
    h.worker.SetConfig(cfg);

    // onset at frame 1, then silence: 3 frames total < minSpeechMs
    h.probs->assign({0.9f, 0.9f, 0.0f, 0.0f, 0.0f});
    h.worker.Start();
    h.Push(5, 0.5f);

    CHECK(!WaitForQueueNotEmpty(h.utterances, 500));
}

void TestVadMaxSpeechForceFlush() {
    VadTestHarness h;
    // Override maxSpeechMs: 96ms = 3 frames, so the 3rd speech frame flushes.
    fcitx::VADWorker::Config cfg;
    cfg.speechThreshold = 0.5f;
    cfg.silenceThreshold = 0.35f;
    cfg.startFrames = 2;
    cfg.preRollMs = 64;     // 2 frames
    cfg.endSilenceMs = 96;  // 3 frames
    cfg.minSpeechMs = 96;   // 3 frames
    cfg.maxSpeechMs = 96;
    h.worker.SetConfig(cfg);

    // frames 0-1: onset; frame 2: 3rd frame reaches maxSpeech -> flush.
    // frames 3-4: next utterance onset; frames 5-7: silence -> flush.
    h.probs->assign({0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.0f, 0.0f, 0.0f});
    h.worker.Start();
    h.Push(8, 0.5f);

    fcitx::Utterance u;
    CHECK(WaitForQueueNotEmpty(h.utterances, 2000));
    CHECK(h.utterances.TryPop(u));
    CHECK_EQ(u.pcm.size(), 3u * fcitx::kWindowSize);  // pre-roll 2 + frame 2
    CHECK(WaitForQueueNotEmpty(h.utterances, 2000));
    CHECK(h.utterances.TryPop(u));
    // frame 3-4 onset, frame 5 appended reaches maxSpeech again: 3 frames
    CHECK_EQ(u.pcm.size(), 3u * fcitx::kWindowSize);
    CHECK(!WaitForQueueNotEmpty(h.utterances, 300));
}

void TestVadDirectPushShortUtterance() {
    VadTestHarness h;
    h.worker.SetDirectPush(true);
    h.worker.Start();
    h.Push(1, 0.5f);  // 32ms — would fail minSpeechMs in VAD mode

    // In direct push mode the worker flushes as soon as the queue is idle,
    // so wait for the utterance instead of racing Stop() against the
    // worker thread.
    fcitx::Utterance u;
    CHECK(WaitForQueueNotEmpty(h.utterances, 2000));
    CHECK(h.utterances.TryPop(u));
    CHECK_EQ(u.pcm.size(), 1u * fcitx::kWindowSize);
    h.worker.Stop();
    CHECK(!h.utterances.TryPop(u));
}

} // anonymous namespace

int main() {
    TestQueueFifo();
    TestQueueConcurrent();
    TestWavEncode();
    TestBase64();
    TestVadSpeechOnset();
    TestVadShortUtteranceDiscarded();
    TestVadMaxSpeechForceFlush();
    TestVadDirectPushShortUtterance();

    if (g_failures == 0) {
        std::printf("All tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d test(s) failed\n", g_failures);
    return 1;
}
