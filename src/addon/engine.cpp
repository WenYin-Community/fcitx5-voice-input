#include <cassert>
#include <string>
#include <sys/stat.h>

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/log.h>
// Ubuntu 24.04 等旧发行版的 fcitx5 仅有弃用的 standardpath.h，
// 新版（fcitx5 >= 5.1.x）提供 standardpaths.h，条件编译兼容两者
#if __has_include(<fcitx-utils/standardpaths.h>)
#include <fcitx-utils/standardpaths.h>
#define VOICE_INPUT_HAS_STANDARDPATHS
#else
#include <fcitx-utils/standardpath.h>
#endif
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
#include "asr/realtime_asr.h"
#include "asr/volcengine_asr.h"
#include "llm/llm_client.h"

namespace fcitx {

namespace {

// 配置文件可能含 API Key 等敏感凭据，保存后收紧为仅所有者可读写
void RestrictConfigFilePermissions(const std::string& relativePath) {
#ifdef VOICE_INPUT_HAS_STANDARDPATHS
    auto configDir = StandardPaths::global().userDirectory(StandardPathsType::Config);
    std::string fullPath = (configDir / relativePath).string();
#else
    auto configDir = StandardPath::global().userDirectory(StandardPath::Type::Config);
    std::string fullPath = configDir + "/" + relativePath;
#endif
    if (::chmod(fullPath.c_str(), S_IRUSR | S_IWUSR) != 0) {
        FCITX_WARN() << "[voice-input] Failed to set 0600 permissions on "
                     << fullPath;
    }
}

// 端点走明文协议且非本机回环时，API Key 将明文传输，给出醒目警告
bool EndpointUsesPlaintext(const std::string& endpoint) {
    if (endpoint.empty()) return false;
    if (endpoint.rfind("https://", 0) == 0 || endpoint.rfind("wss://", 0) == 0)
        return false;
    if (endpoint.rfind("http://", 0) == 0 || endpoint.rfind("ws://", 0) == 0) {
        if (endpoint.find("localhost") != std::string::npos ||
            endpoint.find("127.0.0.1") != std::string::npos ||
            endpoint.find("[::1]") != std::string::npos)
            return false;
        return true;
    }
    return false;
}

} // namespace

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
    readAsIni(openaiConfig_, "conf/voiceinput-openai.conf");
    readAsIni(volcengineConfig_, "conf/voiceinput-volcengine.conf");
    FCITX_INFO() << "[voice-input] reloadConfig: backend="
                 << *config_.activeBackend;
}

void VoiceInputEngine::setConfig(const RawConfig& rawConfig) {
    config_.load(rawConfig, true);
    FCITX_INFO() << "[voice-input] setConfig: backend="
                 << *config_.activeBackend;

    bool saved = safeSaveAsIni(config_, "conf/voiceinput.conf");
    RestrictConfigFilePermissions("conf/voiceinput.conf");
    FCITX_INFO() << "[voice-input] setConfig saved=" << saved;

    if (initialized_) {
        pipeline_->SetConfig(config_);
        ReloadActiveAsrClient();
    }
}

const Configuration* VoiceInputEngine::getSubConfig(
    const std::string& path) const {
    FCITX_INFO() << "[voice-input] getSubConfig: path=" << path;
    if (path == "asr/openai") {
        return &openaiConfig_;
    }
    if (path == "asr/volcengine") {
        return &volcengineConfig_;
    }
    FCITX_WARN() << "[voice-input] getSubConfig: unknown path=" << path;
    return nullptr;
}

void VoiceInputEngine::setSubConfig(const std::string& path,
                                    const RawConfig& rawConfig) {
    FCITX_INFO() << "[voice-input] setSubConfig: path=" << path;
    if (path == "asr/openai") {
        openaiConfig_.load(rawConfig, true);
        safeSaveAsIni(openaiConfig_, "conf/voiceinput-openai.conf");
        RestrictConfigFilePermissions("conf/voiceinput-openai.conf");
        FCITX_INFO() << "[voice-input] Saved openai sub-config";
    } else if (path == "asr/volcengine") {
        volcengineConfig_.load(rawConfig, true);
        safeSaveAsIni(volcengineConfig_, "conf/voiceinput-volcengine.conf");
        RestrictConfigFilePermissions("conf/voiceinput-volcengine.conf");
        FCITX_INFO() << "[voice-input] Saved volcengine sub-config";
    }

    if (initialized_) {
        ReloadActiveAsrClient();
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
        SetStatus(_("按住热键说话"));
    } else {
        SetStatus(_("语音输入就绪"));
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
    entries.emplace_back("voiceinput", _("Voice Input"), "zh_CN",
                         "voiceinput");
    entries.back().setConfigurable(true);
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
        SetStatus(_("语音输入就绪"));
        pendingPreeditText_.clear();
        pendingPreeditUtteranceId_ = 0;
        FCITX_DEBUG() << "[voice-input] Preedit committed on keyEvent";
    }

    // Push-to-talk hotkey handling
    if (config_.voiceInputMode.value() != "ptt") return;
    assert(pipeline_);

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
            // Stop the level timer so the status stays "Recognizing..."
            recording_.store(false);
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
            SetStatus(_("识别中..."));
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
            SetStatus(_("录音中..."));
            FCITX_INFO() << "[voice-input] PTT pressed";
        }
    }
}

void VoiceInputEngine::OnAsrResult(const std::string& text) {
    uint64_t generation = sessionGeneration_.load();
    // 语音转写内容属敏感个人信息，仅记录长度而非内容
    FCITX_DEBUG() << "[voice-input] OnAsrResult: len=" << text.size()
                  << " sessionGen=" << generation
                  << " activeGen=" << activeGeneration_.load();
    eventDispatcher_.schedule([this, generation]() {
        if (generation == 0 || activeGeneration_.load() != generation) {
            FCITX_INFO() << "[voice-input] PollResults skipped: gen="
                         << generation << " active=" << activeGeneration_.load();
            return;
        }
        PollResults();
    });
}

void VoiceInputEngine::PollResults() {
    auto& queue = pipeline_->ResultQueue();
    AsrResult result;
    while (queue.TryPop(result)) {
        bool valid = (!result.text.empty() || result.isError)
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
                     << " error=" << result.isError
                     << " valid=" << valid;

        if (valid) {
            if (result.isError) {
                FCITX_WARN() << "[voice-input] PollResult: ASR error uid="
                             << result.utteranceId;
                activeIc_->inputPanel().reset();
                activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
                std::string msg = _("语音识别失败");
                if (!result.errorText.empty()) {
                    std::string detail = result.errorText;
                    if (detail.size() > 60) detail = detail.substr(0, 60) + "...";
                    msg += ": " + detail;
                }
                SetStatus(msg);
                continue;
            }
            if (result.isLLMRefined) {
                if (result.isPartial) {
                    // Streaming partial: update preedit in-place
                    if (result.utteranceId == pendingPreeditUtteranceId_) {
                        activeIc_->inputPanel().setPreedit(Text(result.text));
                        activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
                        statusText_ = result.text;
                        activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
                    }
                } else if (result.utteranceId == pendingPreeditUtteranceId_) {
                    FCITX_DEBUG() << "[voice-input] LLM commit: uid="
                                  << result.utteranceId
                                  << " len=" << result.text.size();
                    activeIc_->commitString(result.text);
                    activeIc_->inputPanel().reset();
                    activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
                    SetStatus(_("语音输入就绪"));
                    pendingPreeditText_.clear();
                    pendingPreeditUtteranceId_ = 0;
                } else {
                    FCITX_DEBUG() << "[voice-input] LLM stale skip: uid="
                                  << result.utteranceId
                                  << " pendingUid=" << pendingPreeditUtteranceId_;
                }
            } else {
                bool llmActive = openaiConfig_.llmEnabled.value()
                              && !openaiConfig_.llmModel.value().empty();
                FCITX_DEBUG() << "[voice-input] Preedit: uid=" << result.utteranceId
                             << " len=" << result.text.size()
                             << " llmActive=" << llmActive;

                if (result.isPartial) {
                    activeIc_->inputPanel().setPreedit(Text(result.text));
                    activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
                    statusText_ = result.text;
                    activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
                    continue;
                }

                if (llmActive) {
                    activeIc_->inputPanel().setPreedit(Text(result.text));
                    activeIc_->inputPanel().setAuxDown(Text(_("修正中...")));
                    statusText_ = result.text;
                    activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
                    activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea);
                    pendingPreeditText_ = result.text;
                    pendingPreeditUtteranceId_ = result.utteranceId;
                } else if (openaiConfig_.autoCommit.value()) {
                    activeIc_->commitString(result.text);
                    activeIc_->inputPanel().reset();
                    activeIc_->updateUserInterface(UserInterfaceComponent::InputPanel);
                    SetStatus(_("语音输入就绪"));
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

std::unique_ptr<AsrEngine> VoiceInputEngine::CreateAsrEngine() {
    auto backend = *config_.activeBackend;
    FCITX_INFO() << "[voice-input] CreateAsrEngine: backend=" << backend;
    auto asrConfig = AsrEngine::Config{};
    std::unique_ptr<AsrEngine> asr;

    if (backend == "volcengine") {
        asrConfig.apiEndpoint = *volcengineConfig_.endpoint;
        if (EndpointUsesPlaintext(asrConfig.apiEndpoint)) {
            FCITX_WARN() << "[voice-input] Volcengine endpoint is not TLS, "
                         << "API credentials will be sent in plaintext: "
                         << asrConfig.apiEndpoint;
        }
        asrConfig.apiKey = *volcengineConfig_.apiKey;
        asrConfig.authMode = *volcengineConfig_.authMode;
        asrConfig.appKey = *volcengineConfig_.appKey;
        asrConfig.accessKey = *volcengineConfig_.accessKey;
        asrConfig.resourceId = *volcengineConfig_.resourceId;
        asrConfig.modelName = "bigmodel";
        asrConfig.chunkMs = *volcengineConfig_.chunkMs;
        asrConfig.enableItN = *volcengineConfig_.enableITN;
        asrConfig.enablePunc = *volcengineConfig_.enablePunc;
        asrConfig.enableDdc = *volcengineConfig_.enableDDC;
        asrConfig.enableNonstream = *volcengineConfig_.enableNonstream;
        asrConfig.endWindowMs = *volcengineConfig_.endWindowMs;
        FCITX_INFO() << "[voice-input] Volcengine config: endpoint="
                     << asrConfig.apiEndpoint
                     << " apiKey=" << (asrConfig.apiKey.empty() ? "(empty)" : "***");
        asr = std::make_unique<VolcengineAsrEngine>();
    } else {
        asrConfig.apiEndpoint = *openaiConfig_.baseUrl;
        if (EndpointUsesPlaintext(asrConfig.apiEndpoint)) {
            FCITX_WARN() << "[voice-input] OpenAI endpoint is not TLS, "
                         << "API credentials will be sent in plaintext: "
                         << asrConfig.apiEndpoint;
        }
        asrConfig.apiKey = *openaiConfig_.apiKey;
        asrConfig.modelName = *openaiConfig_.model;
        asrConfig.apiMode = *openaiConfig_.apiMode;
        asrConfig.commitIntervalMs = *openaiConfig_.commitIntervalMs;
        auto language = *openaiConfig_.language;
        if (language == "auto") {
            language.clear();
        }
        asrConfig.language = language;

        if (backend == "mimo") {
            // MiMo runs on the OpenAI-compatible engine with api-key auth
            // and chat format; normalize its defaults here.
            if (asrConfig.apiEndpoint.empty()
                || asrConfig.apiEndpoint == "https://api.openai.com/v1") {
                asrConfig.apiEndpoint = "https://api.xiaomimimo.com/v1";
            }
            if (asrConfig.modelName.empty() || asrConfig.modelName == "whisper-1") {
                asrConfig.modelName = "mimo-v2.5-asr";
                openaiConfig_.model.setValue(asrConfig.modelName);
            }
            if (asrConfig.language.empty()) {
                asrConfig.language = "auto";
            }
            asrConfig.apiMode = "chat";
            asrConfig.authScheme = "api-key";
        }

        FCITX_INFO() << "[voice-input] OpenAI config: endpoint="
                     << asrConfig.apiEndpoint
                     << " model=" << asrConfig.modelName
                     << " apiMode=" << asrConfig.apiMode;
        if (asrConfig.apiMode == "realtime") {
            asr = std::make_unique<RealtimeAsrEngine>();
        } else {
            asr = std::make_unique<OpenaiAsrEngine>();
        }
    }

    if (asr->Init(asrConfig)) {
        FCITX_INFO() << "[voice-input] ASR init OK: " << asr->Name();
        return asr;
    }

    FCITX_WARN() << "[voice-input] ASR init failed: " << backend;
    return nullptr;
}

void VoiceInputEngine::ReloadActiveAsrClient() {
    auto asr = CreateAsrEngine();
    if (asr) {
        pipeline_->SetAsrEngine(std::move(asr));
        FCITX_INFO() << "[voice-input] ASR client replaced";
    } else {
        FCITX_WARN() << "[voice-input] ASR client NOT replaced (init failed)";
        SetStatus(_("语音识别未配置"));
    }
    ReloadLLMClient();
}

void VoiceInputEngine::ReloadLLMClient() {
    bool llmEnabled = openaiConfig_.llmEnabled.value();
    std::string llmModel = openaiConfig_.llmModel.value();
    if (!llmEnabled || llmModel.empty()) {
        pipeline_->SetLLMClient(nullptr);
        return;
    }
    auto llmConfig = LLMClient::Config{};
    llmConfig.endpoint = *openaiConfig_.baseUrl;
    llmConfig.apiKey = *openaiConfig_.apiKey;
    llmConfig.model = llmModel;
    llmConfig.systemPrompt = *openaiConfig_.llmSystemPrompt;
    if (*config_.activeBackend == "mimo") {
        if (llmConfig.endpoint.empty()
            || llmConfig.endpoint == "https://api.openai.com/v1") {
            llmConfig.endpoint = "https://api.xiaomimimo.com/v1";
        }
    }

    auto llm = std::make_unique<LLMClient>(std::move(llmConfig));
    pipeline_->SetLLMClient(std::move(llm));
    FCITX_INFO() << "[voice-input] LLM post-processing enabled: model=" << llmModel;
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
                SetStatus(_("正在录音中..."));
                eventDispatcher_.schedule([this]() {
                    if (!activeIc_) return;
                    activeIc_->inputPanel().setPreedit(Text(" "));
                    activeIc_->inputPanel().setAuxDown(Text(_("正在录音中...")));
                    activeIc_->updateUserInterface(
                        UserInterfaceComponent::InputPanel);
                });
            } else {
                recording_.store(false);
                SetStatus(_("语音输入就绪"));
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
            SetStatus(std::string(_("录音中...")) + " [" + bar + "]");
            return true;
        });

    pipeline_->Init(config_);

    ReloadActiveAsrClient();
}

} // namespace fcitx

class VoiceInputAddonFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance* create(fcitx::AddonManager* manager) override {
        return new fcitx::VoiceInputEngine(manager->instance());
    }
};
FCITX_ADDON_FACTORY(VoiceInputAddonFactory);
