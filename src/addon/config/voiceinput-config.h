#pragma once

#include <string>
#include <utility>

#include <fcitx-config/configuration.h>
#include <fcitx-config/option.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>

namespace fcitx {

struct AsrBackendAnnotation : public EnumAnnotation {
    void dumpDescription(RawConfig &config) const {
        EnumAnnotation::dumpDescription(config);
        config.setValueByPath("Enum/0", "openai");
        config.setValueByPath("EnumI18n/0", "OpenAI Compatible");
        config.setValueByPath("Enum/1", "mimo");
        config.setValueByPath("EnumI18n/1", "Xiaomi MiMo ASR");
    }
};

struct VoiceInputModeAnnotation : public EnumAnnotation {
    void dumpDescription(RawConfig &config) const {
        EnumAnnotation::dumpDescription(config);
        config.setValueByPath("Enum/0", "vad");
        config.setValueByPath("EnumI18n/0", _("VAD Auto-segment (hands-free)"));
        config.setValueByPath("Enum/1", "ptt");
        config.setValueByPath("EnumI18n/1", _("Hold hotkey to record (PTT)"));
    }
};

struct ApiFormatAnnotation : public EnumAnnotation {
    void dumpDescription(RawConfig &config) const {
        EnumAnnotation::dumpDescription(config);
        config.setValueByPath("Enum/0", "whisper");
        config.setValueByPath("EnumI18n/0",
            "Multipart Form (/audio/transcriptions)");
        config.setValueByPath("Enum/1", "chat");
        config.setValueByPath("EnumI18n/1", "OpenAI chat");
    }
};

struct OpenaiLanguageAnnotation : public EnumAnnotation {
    void dumpDescription(RawConfig &config) const {
        EnumAnnotation::dumpDescription(config);
        static const std::pair<const char*, const char*> kLanguages[] = {
            {"",   _("Default (Auto)")},
            {"en", "English"},
            {"zh", "中文"},
        };
        for (size_t i = 0; i < std::size(kLanguages); ++i) {
            config.setValueByPath("Enum/" + std::to_string(i), kLanguages[i].first);
            config.setValueByPath("EnumI18n/" + std::to_string(i), kLanguages[i].second);
        }
    }
};

FCITX_CONFIGURATION(VoiceInputConfig,
    // ASR backend selection
    Option<std::string, NoConstrain<std::string>,
           DefaultMarshaller<std::string>, AsrBackendAnnotation>
        asrBackend{this, "ASRBackend", _("ASR Backend"), "openai"};

    // API format (whisper or chat completions)
    Option<std::string, NoConstrain<std::string>,
           DefaultMarshaller<std::string>, ApiFormatAnnotation>
        apiFormat{this, "ApiFormat", _("API Format"), "whisper"};

    // Voice input mode
    Option<std::string, NoConstrain<std::string>,
           DefaultMarshaller<std::string>, VoiceInputModeAnnotation>
        voiceInputMode{this, "VoiceInputMode", _("Recording Mode"), "vad"};

    // Push-to-talk hotkey (default: Right Control)
    KeyListOption pttHotkey{this, "PTTHotkey", _("Push-to-Talk Hotkey"),
                            KeyList{Key(FcitxKey_Control_R)},
                            KeyListConstrain(
                                KeyConstrainFlags(KeyConstrainFlag::AllowModifierOnly)
                                | KeyConstrainFlag::AllowModifierLess)};

    // OpenAI-compatible API settings
    Option<std::string> openaiEndpoint{this, "OpenAIEndpoint",
                                        _("OpenAI API Endpoint"),
                                        "https://api.openai.com/v1"};
    Option<std::string> openaiApiKey{this, "OpenAIApiKey",
                                      _("OpenAI API Key"), ""};
    Option<std::string> openaiModel{this, "OpenAIModel",
                                     _("Voice Model"),
                                     "whisper-1"};
    Option<std::string, NoConstrain<std::string>,
           DefaultMarshaller<std::string>, OpenaiLanguageAnnotation>
        openaiLanguage{this, "OpenAILanguage", _("Output Language"), ""};

    // LLM post-processing settings
    Option<bool> llmEnabled{this, "LLMEnabled",
                             _("LLM Post-processing"), false};
    Option<std::string> llmModel{this, "LLMModel",
                                  _("Post-processing LLM Model"), ""};
    Option<std::string> llmSystemPrompt{this, "LLMSystemPrompt",
                                         _("Post-processing System Prompt"), ""};

    // Output behavior
    Option<bool> autoCommit{this, "AutoCommit",
                             _("Auto-Commit when no LLM"), true};

    // VAD (Voice Activity Detection)
    // Stored as 0-100 percentage internally; divide by 100 for 0.0-1.0
    Option<int, IntConstrain> vadThreshold{this, "VADThreshold",
                                             _("VAD Threshold (%)"), 50,
                                             IntConstrain(0, 100)};
    Option<int, IntConstrain> silenceThresholdMs{
        this, "SilenceThresholdMs", _("Silence Threshold (ms)"), 800,
        IntConstrain(100, 10000)};
);

} // namespace fcitx
