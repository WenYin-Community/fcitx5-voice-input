#include "llm_client.h"

#include <chrono>
#include <cstring>
#include <string>

#include <curl/curl.h>
#include <json/json.h>

#include <fcitx-utils/log.h>

namespace fcitx {

namespace {

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* response = static_cast<std::string*>(userp);
    size_t total = size * nmemb;
    response->append(static_cast<char*>(contents), total);
    return total;
}

// 进度回调：Cancel() 后中断在途 HTTP 传输（返回非 0 中止请求）
int CancelProgressCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t,
                           curl_off_t) {
    auto* cancelled = static_cast<std::atomic<bool>*>(clientp);
    return cancelled->load() ? 1 : 0;
}

// Extract "text" field from a JSON response.
// Returns empty string on parse failure or missing field.
std::string ExtractJsonText(const std::string& content) {
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(content, root)) {
        FCITX_WARN() << "[voice-input:llm] Response is not valid JSON";
        return {};
    }
    if (!root.isMember("text") || !root["text"].isString()) {
        FCITX_WARN() << "[voice-input:llm] JSON response missing 'text' field";
        return {};
    }
    return root["text"].asString();
}

// Build system prompt with JSON format instruction appended.
std::string BuildSystemPrompt(const std::string& userPrompt) {
    std::string prompt = userPrompt;
    if (!prompt.empty()) {
        prompt += "\n\n";
    }
    prompt += "你必须以JSON格式回复，格式为：{\"text\": \"修正后的文本\"}。";
    return prompt;
}

} // namespace

LLMClient::LLMClient(Config config)
    : config_(std::move(config)) {}

LLMClient::~LLMClient() = default;

std::string LLMClient::Process(const std::string& text) {
    if (config_.model.empty()) {
        return text;
    }

    // Build URL
    std::string url = config_.endpoint;
    if (!url.empty() && url.back() != '/') {
        url += '/';
    }
    url += "chat/completions";

    // Build JSON body
    Json::Value body;
    body["model"] = config_.model;
    body["temperature"] = 0.1;

    Json::Value messages(Json::arrayValue);

    {
        Json::Value sysMsg;
        sysMsg["role"] = "system";
        sysMsg["content"] = BuildSystemPrompt(config_.systemPrompt);
        messages.append(sysMsg);
    }

    Json::Value userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = text;
    messages.append(userMsg);

    body["messages"] = messages;

    Json::StreamWriterBuilder writer;
    std::string bodyStr = Json::writeString(writer, body);

    FCITX_DEBUG() << "[voice-input:llm] POST " << url
                 << " model=" << config_.model
                 << " input=" << text.size() << " chars"
                 << " body=" << bodyStr.size() << " bytes";

    // HTTP request
    auto tStart = std::chrono::steady_clock::now();

    CURL* curl = curl_easy_init();
    if (!curl) {
        FCITX_ERROR() << "[voice-input:llm] Failed to init curl";
        return {};
    }

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string authHeader = "Authorization: Bearer " + config_.apiKey;
    headers = curl_slist_append(headers, authHeader.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, bodyStr.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "fcitx5-voice-input/" VOICE_INPUT_VERSION);
    // Cancel() 后中断在途传输
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CancelProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancelled_);

    CURLcode res = curl_easy_perform(curl);

    long httpCode = 0;
    long osErrno = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_getinfo(curl, CURLINFO_OS_ERRNO, &osErrno);

    auto tEnd = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    FCITX_DEBUG() << "[voice-input:llm] HTTP " << httpCode
                  << " response=" << response.size() << " bytes"
                  << " elapsed=" << elapsedMs << "ms";

    if (res != CURLE_OK || httpCode != 200) {
        std::string curlErr = curl_easy_strerror(res);
        const char* diagnostic = "";
        switch (res) {
        case CURLE_COULDNT_RESOLVE_HOST: diagnostic = " (DNS resolution failed)"; break;
        case CURLE_COULDNT_CONNECT:      diagnostic = " (TCP connect failed, check endpoint/firewall)"; break;
        case CURLE_SSL_CONNECT_ERROR:    diagnostic = " (SSL handshake failed)"; break;
        case CURLE_OPERATION_TIMEDOUT:   diagnostic = " (connection timed out, check network)"; break;
        case CURLE_URL_MALFORMAT:        diagnostic = " (malformed URL)"; break;
        case CURLE_HTTP_RETURNED_ERROR:  diagnostic = " (HTTP error response)"; break;
        default: break;
        }
        FCITX_WARN() << "[voice-input:llm] Request failed: "
                     << curlErr << diagnostic
                     << " http=" << httpCode
                     << " elapsed=" << elapsedMs << "ms"
                     << " osErrno=" << osErrno;
        return {};
    }

    // Parse response
    Json::Value json;
    Json::Reader reader;
    if (!reader.parse(response, json)) {
        FCITX_WARN() << "[voice-input:llm] JSON parse failed"
                     << " elapsed=" << elapsedMs << "ms"
                     << " responseHead=" << response.substr(0, 200);
        return {};
    }

    std::string content = json["choices"][0]["message"]["content"].asString();
    if (content.empty()) {
        FCITX_WARN() << "[voice-input:llm] Empty response content"
                     << " elapsed=" << elapsedMs << "ms";
        return {};
    }

    // Try JSON extraction, fallback to raw LLM output
    std::string extracted = ExtractJsonText(content);
    std::string result = extracted.empty() ? text : extracted;

    FCITX_DEBUG() << "[voice-input:llm] Response: http=" << httpCode
                 << " elapsed=" << elapsedMs << "ms"
                 << " output=" << result.size() << " chars";

    FCITX_DEBUG() << "[voice-input:llm] Done: raw=" << text.size()
                 << " chars → out=" << result.size() << " chars";
    return result;
}

} // namespace fcitx
