#pragma once

#include <string>

namespace fcitx {

class LLMClient {
public:
    struct Config {
        std::string endpoint;
        std::string apiKey;
        std::string model;
        std::string systemPrompt;
    };

    LLMClient(Config config);
    ~LLMClient();

    LLMClient(const LLMClient&) = delete;
    LLMClient& operator=(const LLMClient&) = delete;

    // Returns processed text on success, empty on failure.
    std::string Process(const std::string& text);

private:
    Config config_;
};

} // namespace fcitx
