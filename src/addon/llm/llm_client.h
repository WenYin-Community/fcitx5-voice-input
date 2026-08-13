#pragma once

#include <atomic>
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

    /// 取消在途请求：中断当前阻塞的 HTTP 传输，后续调用立即返回。
    void Cancel() { cancelled_.store(true); }

private:
    Config config_;
    std::atomic<bool> cancelled_{false};
};

} // namespace fcitx
