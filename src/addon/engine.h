#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/event.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/instance.h>

#include "config/voiceinput-config.h"
#include "asr/asr_engine.h"
#include "pipeline/pipeline.h"
#include "types.h"

namespace fcitx {

class VoiceInputEngine : public InputMethodEngineV2 {
public:
    VoiceInputEngine(Instance* instance);
    ~VoiceInputEngine() override;

    void activate(const InputMethodEntry& entry,
                  InputContextEvent& event) override;

    void deactivate(const InputMethodEntry& entry,
                    InputContextEvent& event) override;

    void keyEvent(const InputMethodEntry& entry, KeyEvent& keyEvent) override;

    std::vector<InputMethodEntry> listInputMethods() override;

    const Configuration* getConfig() const override { return &config_; }
    void setConfig(const RawConfig& rawConfig) override;
    void reloadConfig() override;

    const Configuration* getSubConfig(const std::string& path) const override;
    void setSubConfig(const std::string& path,
                      const RawConfig& rawConfig) override;

    std::string subModeLabelImpl(const InputMethodEntry& entry,
                                 InputContext& ic) override;

private:
    void InitializeIfNeeded();
    void OnAsrResult(const std::string& text);
    void PollResults();
    void ClearUI();
    void SetStatus(const std::string& text);
    std::unique_ptr<AsrEngine> CreateAsrEngine();
    void ReloadActiveAsrClient();
    void ReloadLLMClient();

    Instance* instance_;
    std::unique_ptr<Pipeline> pipeline_;
    EventDispatcher eventDispatcher_;
    std::unique_ptr<EventSourceTime> delayedStopEvent_;
    VoiceInputConfig config_;
    OpenAIAsrConfig openaiConfig_;
    VolcengineAsrConfig volcengineConfig_;

    InputContext* activeIc_ = nullptr;
    std::atomic<uint64_t> activeGeneration_{0};
    std::atomic<uint64_t> sessionGeneration_{0};
    uint64_t pendingStopGeneration_ = 0;
    bool initialized_ = false;

    std::string statusText_;
    std::string pendingPreeditText_;
    uint64_t pendingPreeditUtteranceId_ = 0;

    // Push-to-talk state
    bool pttActive_ = false;
    int pttHeldKeyCode_ = 0;  // hardware scancode of held hotkey
    std::unique_ptr<EventSourceTime> pttDelayedStopEvent_;

    // Audio level visualization
    std::atomic<int> audioLevel_{0};
    std::atomic<bool> recording_{false};
    std::unique_ptr<EventSourceTime> levelTimer_;
};

} // namespace fcitx
