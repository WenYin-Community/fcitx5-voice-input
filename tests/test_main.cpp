// Minimal dependency-free unit tests for fcitx5-voice-input.
// Build with: cmake -B build -DBUILD_TESTS=ON && cmake --build build && ctest --test-dir build

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "asr/utils/base64.h"
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

void TestQueueBounded() {
    ThreadSafeQueue<int> q(3);
    q.Push(1);
    q.Push(2);
    q.Push(3);
    q.Push(4);  // drops 1, keeps newest
    CHECK_EQ(q.Size(), 3u);
    int v = 0;
    CHECK(q.TryPop(v));
    CHECK_EQ(v, 2);
}

// ── Base64 ───────────────────────────────────────────────────────────

void TestBase64() {
    CHECK(fcitx::Base64Encode(nullptr, 0) == "");
    const char* man = "Man";
    CHECK(fcitx::Base64Encode(reinterpret_cast<const uint8_t*>(man), 3) == "TWFu");
    const char* abc = "abc";
    CHECK(fcitx::Base64Encode(reinterpret_cast<const uint8_t*>(abc), 3) == "YWJj");
    const char* ab = "ab";
    CHECK(fcitx::Base64Encode(reinterpret_cast<const uint8_t*>(ab), 2) == "YWI=");
}

// ── VADWorker state machine (SpeechEvent stream) ────────────────────

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
    ThreadSafeQueue<fcitx::SpeechEvent> events;
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
        worker.SetSpeechEventQueue(&events);
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

    // Push `count` frames whose samples carry the 1-based frame index, so a
    // frame that is delivered twice is distinguishable from its neighbours.
    void PushTagged(int count) {
        for (int i = 0; i < count; ++i) {
            fcitx::AudioFrame f;
            f.timestamp_ms = static_cast<int64_t>(i * fcitx::kFrameMs);
            std::fill(f.pcm.begin(), f.pcm.end(), static_cast<int16_t>(i + 1));
            frames.Push(f);
        }
    }

    // Wait for the worker to go idle, then drain all events.
    std::vector<fcitx::SpeechEvent> DrainEvents(int timeoutMs) {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
        size_t lastSize = 0;
        int quiet = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            size_t s = events.Size();
            if (s > 0 && s == lastSize) {
                if (++quiet >= 5) break;  // stable for 50ms
            } else {
                quiet = 0;
            }
            lastSize = s;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::vector<fcitx::SpeechEvent> out;
        fcitx::SpeechEvent e;
        while (events.TryPop(e)) out.push_back(std::move(e));
        return out;
    }
};

void TestVadSpeechOnset() {
    VadTestHarness h;
    // 1 leading silence frame, 4 speech frames, 3 trailing silence frames.
    // Onset lands on frame 2, so the pre-roll buffer holds the two frames
    // before it (silence + first speech frame) = the configured 64ms.
    h.probs->assign({0.0f, 0.9f, 0.9f, 0.9f, 0.9f, 0.0f, 0.0f, 0.0f});
    h.worker.Start();
    h.Push(8, 0.5f);

    auto events = h.DrainEvents(2000);
    CHECK_EQ(events.size(), 9u);  // Begin + preRoll + 6 Audio + End
    CHECK(events[0].type == fcitx::SpeechEventType::Begin);
    // pre-roll audio event carries 2 frames (64ms)
    CHECK(events[1].type == fcitx::SpeechEventType::Audio);
    CHECK_EQ(events[1].pcm.size(), 2u * fcitx::kWindowSize);
    CHECK_EQ(events[1].pcm[0], static_cast<int16_t>(0.5f * 32000.0f));
    CHECK(events.back().type == fcitx::SpeechEventType::End);
}

void TestVadShortUtteranceCancelled() {
    VadTestHarness h;
    // Shortest possible utterance is pre-roll (2) + 1 trailing frame = 3
    // frames (96ms); raise minSpeechMs so this one gets a Cancel.
    fcitx::VADWorker::Config cfg;
    cfg.speechThreshold = 0.5f;
    cfg.silenceThreshold = 0.35f;
    cfg.startFrames = 2;
    cfg.preRollMs = 64;
    cfg.endSilenceMs = 96;
    cfg.minSpeechMs = 192;  // 6 frames — this utterance is 5 frames
    cfg.maxSpeechMs = 20000;
    h.worker.SetConfig(cfg);

    h.probs->assign({0.9f, 0.9f, 0.0f, 0.0f, 0.0f});
    h.worker.Start();
    h.Push(5, 0.5f);

    auto events = h.DrainEvents(2000);
    CHECK(!events.empty());
    CHECK(events.back().type == fcitx::SpeechEventType::Cancel);
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

    // frames 0-1: onset; frame 2 reaches maxSpeech -> flush.
    // frames 3-4: next onset; frame 5 reaches maxSpeech again -> flush.
    // frames 6-7: silence, no session.
    h.probs->assign({0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.0f, 0.0f});
    h.worker.Start();
    h.Push(8, 0.5f);

    auto events = h.DrainEvents(2000);
    int ends = 0;
    for (const auto& e : events) {
        if (e.type == fcitx::SpeechEventType::End) ++ends;
    }
    CHECK_EQ(ends, 2);  // frame 2 flushes, frame 5 flushes (too long)
}

void TestVadDirectPushShortUtterance() {
    VadTestHarness h;
    h.worker.SetDirectPush(true);
    h.worker.Start();
    h.Push(1, 0.5f);  // 32ms — would be Cancel in VAD mode

    // The worker flushes when the queue is idle: Begin + Audio + End.
    auto events = h.DrainEvents(2000);
    CHECK_EQ(events.size(), 3u);
    CHECK(events[0].type == fcitx::SpeechEventType::Begin);
    CHECK(events[1].type == fcitx::SpeechEventType::Audio);
    CHECK_EQ(events[1].pcm.size(), 1u * fcitx::kWindowSize);
    CHECK(events[2].type == fcitx::SpeechEventType::End);
}

// The pre-roll buffer and the onset frame must not overlap: the frame that
// trips onset is sent once, preceded only by audio captured before it.
void TestVadOnsetFrameNotDuplicated() {
    VadTestHarness h;
    h.probs->assign({0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.0f, 0.0f, 0.0f});
    h.worker.Start();
    h.PushTagged(8);

    auto events = h.DrainEvents(2000);

    // Flatten the audio payloads into the sequence of frame tags sent to ASR.
    std::vector<int16_t> tags;
    for (const auto& e : events) {
        if (e.type != fcitx::SpeechEventType::Audio) continue;
        for (size_t off = 0; off < e.pcm.size(); off += fcitx::kWindowSize) {
            tags.push_back(e.pcm[off]);
        }
    }

    CHECK(!tags.empty());
    // Strictly increasing: no tag repeats, so no frame is sent twice.
    bool strictlyIncreasing = true;
    for (size_t i = 1; i < tags.size(); ++i) {
        if (tags[i] <= tags[i - 1]) strictlyIncreasing = false;
    }
    CHECK(strictlyIncreasing);
    // Onset frame (tag 2) closes the pre-roll, and the stream runs to tag 8.
    CHECK_EQ(static_cast<int>(tags.back()), 8);
    CHECK_EQ(tags.size(), 8u);
}

} // anonymous namespace

int main() {
    TestQueueFifo();
    TestQueueConcurrent();
    TestQueueBounded();
    TestBase64();
    TestVadSpeechOnset();
    TestVadShortUtteranceCancelled();
    TestVadMaxSpeechForceFlush();
    TestVadDirectPushShortUtterance();
    TestVadOnsetFrameNotDuplicated();

    if (g_failures == 0) {
        std::printf("All tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d test(s) failed\n", g_failures);
    return 1;
}
