#include <string>

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/log.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <fcitx/instance.h>
#include <fcitx/userinterface.h>

#include "engine.h"

#include "asr/openai_asr.h"
#include "llm/llm_client.h"

namespace fcitx {

VoiceInputEngine::VoiceInputEngine(Instance* instance)
    : instance_(instance), pipeline_(std::make_unique<Pipeline>()) {
    fcitx::registerDomain(FCITX_GETTEXT_DOMAIN, VOICE_INPUT_LOCALE_DIR);
    eventDispatcher_.attach(&instance_->eventLoop());
    reloadConfig();
}

VoiceInputEngine::~VoiceInputEngine() {
    pipeline_->Abort();
    eventDispatcher_.detach();
}

void VoiceInputEngine::reloadConfig() {
    readAsIni(config_, "conf/voiceinput.conf");
}

void VoiceInputEngine::setConfig(const RawConfig& rawConfig) {
    // Snapshot engine-affecting keys before loading (config_ can't be copied)
    auto backend = config_.asrBackend.value();
    auto format = config_.apiFormat.value();
    auto endpoint = config_.openaiEndpoint.value();
    auto apiKey = config_.openaiApiKey.value();
    auto model = config_.openaiModel.value();
    auto language = config_.openaiLanguage.value();
    auto llmEnabled = config_.llmEnabled.value();
    auto llmModel = config_.llmModel.value();
    auto llmPrompt = config_.llmSystemPrompt.value();

    config_.load(rawConfig, true);

    bool saved = safeSaveAsIni(config_, "conf/voiceinput.conf");
    FCITX_INFO() << "[voice-input] setConfig saved=" << saved;

    if (initialized_) {
        pipeline_->SetConfig(config_);

        bool enginesChanged = config_.asrBackend.value() != backend
            || config_.apiFormat.value() != format
            || config_.openaiEndpoint.value() != endpoint
            || config_.openaiApiKey.value() != apiKey
            || config_.openaiModel.value() != model
            || config_.openaiLanguage.value() != language
            || config_.llmEnabled.value() != llmEnabled
            || config_.llmModel.value() != llmModel
            || config_.llmSystemPrompt.value() != llmPrompt;
        if (enginesChanged) {
            FCITX_INFO() << "[voice-input] Engine config changed, will recreate before next session";
            enginesDirty_ = true;
        }
    }
}

void VoiceInputEngine::activate(const InputMethodEntry& entry,
                                InputContextEvent& event) {
    FCITX_UNUSED(entry);
    FCITX_UNUSED(event);
    InitializeIfNeeded();
    activeIc_ = event.inputContext();
    uint64_t generation = activeGeneration_.fetch_add(1) + 1;
    pendingStopGeneration_ = generation;
    sessionGeneration_.store(generation);

    pipeline_->SetGeneration(generation);

    bool wasRunning = pipeline_->IsRunning();
    bool isPTT = (config_.voiceInputMode.value() == "ptt");

    if (!isPTT) {
        // VAD mode: auto-start pipeline
        pipeline_->Start();
    }

    FCITX_INFO() << "[voice-input] Activate: gen=" << generation
                 << " wasRunning=" << wasRunning
                 << " ptt=" << isPTT
                 << " ic=" << (activeIc_ != nullptr);

    statusText_.clear();
    if (isPTT) {
        SetStatus(_("Hold hotkey to record"));
    } else {
        SetStatus(_("Voice input ready"));
    }
}

void VoiceInputEngine::deactivate(const InputMethodEntry& entry,
                                  InputContextEvent& event) {
    FCITX_UNUSED(entry);
    FCITX_UNUSED(event);
    uint64_t generation = activeGeneration_.fetch_add(1) + 1;
    pendingStopGeneration_ = generation;

    FCITX_INFO() << "[voice-input] Deactivate: gen=" << generation;

    pttActive_ = false;
    pttHeldKeyCode_ = 0;
    pttDelayedStopEvent_.reset();
    recording_.store(false);
    ClearUI();

    delayedStopEvent_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC,
        now(CLOCK_MONOTONIC) + 200000,
        0,
        [this, generation](EventSourceTime*, uint64_t) {
            if (pendingStopGeneration_ != generation) {
                FCITX_INFO() << "[voice-input] DelayedStop: cancelled gen="
                             << generation;
                return true;
            }
            FCITX_INFO() << "[voice-input] DelayedStop: executing gen="
                         << generation;
            sessionGeneration_.store(0);
            pipeline_->Stop();
            activeIc_ = nullptr;
            return true;
        });
    delayedStopEvent_->setOneShot();
}

std::vector<InputMethodEntry> VoiceInputEngine::listInputMethods() {
    std::vector<InputMethodEntry> entries;
    entries.emplace_back("voiceinput", _("Fcitx5 Voice Input"), "zh_CN",
                         "voiceinput");
    entries.back().setConfigurable(true);
    entries.back().setIcon("fcitx5-voice-input");
    return entries;
}

void VoiceInputEngine::keyEvent(const InputMethodEntry& entry,
                                KeyEvent& keyEvent) {
    FCITX_UNUSED(entry);

    // Commit pending preedit on any key press
    if (!pendingPreeditText_.empty() && activeIc_ && !keyEvent.isRelease()) {
        activeIc_->commitString(pendingPreeditText_);
        activeIc_->inputPanel().reset();
        activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
        SetStatus(_("Voice input ready"));
        pendingPreeditText_.clear();
        pendingPreeditUtteranceId_ = 0;
    }

    // Push-to-talk hotkey handling
    if (config_.voiceInputMode.value() != "ptt") return;
    if (!pipeline_) return;

    const auto& hotkeys = config_.pttHotkey.value();
    bool isPTTKey = false;
    for (const auto& k : hotkeys) {
        if (keyEvent.rawKey().sym() == k.sym()) {
            isPTTKey = true;
            break;
        }
    }
    if (!isPTTKey) return;

    if (keyEvent.isRelease() && keyEvent.rawKey().code() == pttHeldKeyCode_) {
        pttHeldKeyCode_ = 0;
        if (pttActive_) {
            pttActive_ = false;
            // Delayed stop: capture trailing audio for 200ms
            uint64_t gen = sessionGeneration_.load();
            pttDelayedStopEvent_ = instance_->eventLoop().addTimeEvent(
                CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 200000, 0,
                [this, gen](EventSourceTime*, uint64_t) {
                    if (pttActive_) return true;  // cancelled by new press
                    pipeline_->StopCapture();
                    FCITX_INFO() << "[voice-input] PTT delayed stop";
                    return true;
                });
            pttDelayedStopEvent_->setOneShot();
            SetStatus(_("Recognizing..."));
            FCITX_INFO() << "[voice-input] PTT released";
        }
    } else if (!keyEvent.isRelease()) {
        // Cancel pending delayed stop on new press
        pttDelayedStopEvent_.reset();
        pttHeldKeyCode_ = keyEvent.rawKey().code();
        if (!pttActive_) {
            pttActive_ = true;
            recording_.store(true);
            pipeline_->Start();
            SetStatus(_("Recording..."));
            FCITX_INFO() << "[voice-input] PTT pressed";
        }
    }
}

void VoiceInputEngine::OnAsrResult(const std::string& text) {
    uint64_t generation = sessionGeneration_.load();
    FCITX_INFO() << "[voice-input] OnAsrResult: text='"
                 << text.substr(0, 30) << "'"
                 << " sessionGen=" << generation
                 << " activeGen=" << activeGeneration_.load();

    eventDispatcher_.schedule([this, generation]() {
        if (generation == 0 || activeGeneration_.load() != generation) {
            return;
        }
        PollResults();
    });
}

void VoiceInputEngine::PollResults() {
    auto& queue = pipeline_->ResultQueue();
    AsrResult result;
    while (queue.TryPop(result)) {
        bool valid = !result.text.empty()
                  && result.generation != 0
                  && activeGeneration_.load() == result.generation
                  && activeIc_ != nullptr;

        FCITX_DEBUG() << "[voice-input] PollResult:"
                     << " text=\"" << result.text << "\""
                     << " gen=" << result.generation
                     << " activeGen=" << activeGeneration_.load()
                     << " uid=" << result.utteranceId
                     << " pendingUid=" << pendingPreeditUtteranceId_
                     << " refined=" << result.isLLMRefined
                     << " valid=" << valid;

        if (valid) {
            if (result.isLLMRefined) {
                if (result.utteranceId == pendingPreeditUtteranceId_) {
                    FCITX_INFO() << "[voice-input] LLM commit: uid=" << result.utteranceId
                                 << " text=\"" << result.text << "\"";
                    activeIc_->commitString(result.text);
                    activeIc_->inputPanel().reset();
                    activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
                    SetStatus(_("Voice input ready"));
                    pendingPreeditText_.clear();
                    pendingPreeditUtteranceId_ = 0;
                } else {
                    FCITX_DEBUG() << "[voice-input] LLM stale skip: uid="
                                  << result.utteranceId
                                  << " pendingUid=" << pendingPreeditUtteranceId_;
                }
            } else {
                bool llmActive = config_.llmEnabled.value()
                              && !config_.llmModel.value().empty();
                FCITX_INFO() << "[voice-input] Preedit: uid=" << result.utteranceId
                             << " text=\"" << result.text << "\""
                             << " llmActive=" << llmActive;
                if (llmActive) {
                    activeIc_->inputPanel().setPreedit(Text(result.text));
                    activeIc_->inputPanel().setAuxDown(Text(_("Refining...")));
                    statusText_ = result.text;
                    activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
                    activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
                    pendingPreeditText_ = result.text;
                    pendingPreeditUtteranceId_ = result.utteranceId;
                } else if (config_.autoCommit.value()) {
                    activeIc_->commitString(result.text);
                    SetStatus(_("Voice input ready"));
                    pendingPreeditText_.clear();
                    pendingPreeditUtteranceId_ = 0;
                } else {
                    activeIc_->inputPanel().setPreedit(Text(result.text));
                    statusText_ = result.text;
                    activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
                    activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
                    pendingPreeditText_ = result.text;
                    pendingPreeditUtteranceId_ = result.utteranceId;
                }
            }
        }
    }
}

void VoiceInputEngine::SetStatus(const std::string& text) {
    eventDispatcher_.schedule([this, text]() {
        statusText_ = text;
        if (activeIc_) {
            activeIc_->inputPanel().setAuxDown(Text(text));
            activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
            activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
        }
    });
}

void VoiceInputEngine::ClearUI() {
    uint64_t gen = activeGeneration_.load();
    eventDispatcher_.schedule([this, gen]() {
        if (activeGeneration_.load() != gen) return;
        statusText_.clear();
        if (activeIc_) {
            // Commit pending preedit before clearing
            if (!pendingPreeditText_.empty()) {
                activeIc_->commitString(pendingPreeditText_);
                pendingPreeditText_.clear();
                pendingPreeditUtteranceId_ = 0;
            }
            activeIc_->inputPanel().reset();
            activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
            activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
        }
    });
}

std::string VoiceInputEngine::subModeLabelImpl(const InputMethodEntry& entry,
                                                InputContext& ic) {
    FCITX_UNUSED(entry);
    if (&ic == activeIc_ && !statusText_.empty()) {
        return statusText_;
    }
    return {};
}

void VoiceInputEngine::InitializeIfNeeded() {
    if (initialized_) return;
    initialized_ = true;

    pipeline_->SetResultCallback(
        [this](const std::string& text) {
            OnAsrResult(text);
        });

    pipeline_->SetVadStatusCallback(
        [this](bool speaking) {
            if (speaking) {
                recording_.store(true);
                eventDispatcher_.schedule([this]() {
                    if (!activeIc_) return;
                    activeIc_->inputPanel().setPreedit(Text(" "));
                    activeIc_->updateUserInterface(
                        UserInterfaceComponent::InputPanel);
                });
            } else {
                recording_.store(false);
                SetStatus(_("Voice input ready"));
            }
        });

    pipeline_->SetLevelCallback(
        [this](int level) {
            audioLevel_.store(level);
        });

    // Persistent level timer, created once on the main thread. All state it
    // reads (recording_, audioLevel_) is atomic, so it never races with the
    // VAD worker; the old per-session create/reset crossed threads.
    levelTimer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, 0, 200000,
        [this](EventSourceTime*, uint64_t) {
            if (!recording_.load()) return true;
            int lvl = audioLevel_.load();
            std::string bar;
            for (int i = 0; i < 10; i++)
                bar += (i < lvl) ? "█" : "░";
            SetStatus(std::string(_("Recording...")) + " [" + bar + "]");
            return true;
        });

    pipeline_->Init(config_);

    pipeline_->SetRecreateCallback([this]() { TryRecreateEngines(); });

    RecreateEngines();
}

void VoiceInputEngine::RecreateEngines() {
    auto onAsrError = [this](const std::string& err) {
        std::string msg = err;
        if (msg.size() > 60) msg = msg.substr(0, 60) + "...";
        SetStatus(std::string(_("Recognition failed: ")) + msg);
    };

    auto asrConfig = AsrEngine::Config{};
    asrConfig.apiEndpoint = config_.openaiEndpoint.value();
    asrConfig.apiKey = config_.openaiApiKey.value();
    asrConfig.modelName = config_.openaiModel.value();
    asrConfig.language = config_.openaiLanguage.value();
    asrConfig.apiFormat = config_.apiFormat.value();

    std::string backend = config_.asrBackend.value();
    std::unique_ptr<AsrEngine> asr;

    if (backend == "mimo") {
        // MiMo runs on the OpenAI-compatible engine with api-key auth and
        // chat format; normalize its defaults here.
        if (asrConfig.apiEndpoint.empty()
            || asrConfig.apiEndpoint == "https://api.openai.com/v1") {
            asrConfig.apiEndpoint = "https://api.xiaomimimo.com/v1";
        }
        if (asrConfig.modelName.empty() || asrConfig.modelName == "whisper-1") {
            asrConfig.modelName = "mimo-v2.5-asr";
            config_.openaiModel.setValue(asrConfig.modelName);
        }
        if (asrConfig.language.empty()) {
            asrConfig.language = "auto";
        }
        asrConfig.apiFormat = "chat";
        asrConfig.authScheme = "api-key";
        auto mimo = std::make_unique<OpenaiCompatAsrEngine>();
        if (mimo->Init(asrConfig)) {
            mimo->SetErrorCallback(onAsrError);
            asr = std::move(mimo);
            FCITX_INFO() << "[voice-input] Using MiMo ASR: "
                         << asrConfig.apiEndpoint
                         << " model=" << asrConfig.modelName;
        } else {
            FCITX_WARN() << "[voice-input] MiMo ASR init failed";
            SetStatus(_("ASR not configured"));
        }
    } else {
        auto openai = std::make_unique<OpenaiCompatAsrEngine>();
        if (openai->Init(asrConfig)) {
            openai->SetErrorCallback(onAsrError);
            asr = std::move(openai);
            FCITX_INFO() << "[voice-input] Using OpenAI-compatible ASR: "
                         << config_.openaiEndpoint.value()
                         << " model=" << asrConfig.modelName;
        } else {
            FCITX_WARN() << "[voice-input] OpenAI ASR init failed";
            SetStatus(_("ASR not configured"));
        }
    }

    if (asr) {
        pipeline_->SetAsrEngine(std::move(asr));
    }

    // LLM post-processing
    bool llmEnabled = config_.llmEnabled.value();
    std::string llmModel = config_.llmModel.value();
    if (llmEnabled && !llmModel.empty()) {
        auto llmConfig = LLMClient::Config{};
        llmConfig.endpoint = config_.openaiEndpoint.value();
        llmConfig.apiKey = config_.openaiApiKey.value();
        llmConfig.model = llmModel;
        llmConfig.systemPrompt = config_.llmSystemPrompt.value();

        auto llm = std::make_unique<LLMClient>(std::move(llmConfig));
        pipeline_->SetLLMClient(std::move(llm));
        FCITX_INFO() << "[voice-input] LLM post-processing enabled: "
                     << " model=" << llmModel;
    }
}

void VoiceInputEngine::TryRecreateEngines() {
    if (!enginesDirty_) return;
    enginesDirty_ = false;
    RecreateEngines();
}

} // namespace fcitx

class VoiceInputAddonFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance* create(fcitx::AddonManager* manager) override {
        return new fcitx::VoiceInputEngine(manager->instance());
    }
};
FCITX_ADDON_FACTORY(VoiceInputAddonFactory);
