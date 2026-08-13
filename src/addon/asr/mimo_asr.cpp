#include "mimo_asr.h"

#include <chrono>
#include <cstring>
#include <sstream>

#include <curl/curl.h>
#include <fcitx-utils/log.h>
#include <json/json.h>

namespace fcitx {
namespace {

// ── WAV writing helpers ─────────────────────────────────────────────
#pragma pack(push, 1)
struct WavHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t fileSize = 0;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmtSize = 16;
    uint16_t audioFormat = 1;       // PCM
    uint16_t numChannels = 1;       // mono
    uint32_t sampleRate = 16000;
    uint32_t byteRate = 32000;      // sampleRate * 1 * 2
    uint16_t blockAlign = 2;        // 1 * 2
    uint16_t bitsPerSample = 16;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t dataSize = 0;
};
#pragma pack(pop)

std::vector<uint8_t> FloatPcmToWav(const float* pcm, size_t frames) {
    WavHeader header;
    size_t dataBytes = frames * sizeof(int16_t);
    header.fileSize = static_cast<uint32_t>(sizeof(WavHeader) - 8 + dataBytes);
    header.dataSize = static_cast<uint32_t>(dataBytes);

    std::vector<uint8_t> wav(sizeof(WavHeader) + dataBytes);
    std::memcpy(wav.data(), &header, sizeof(WavHeader));

    auto* samplePtr = reinterpret_cast<int16_t*>(wav.data() + sizeof(WavHeader));
    for (size_t i = 0; i < frames; ++i) {
        float s = pcm[i];
        if (s < -1.0f) s = -1.0f;
        if (s > 1.0f) s = 1.0f;
        samplePtr[i] = static_cast<int16_t>(s * 32767.0f);
    }

    return wav;
}

// ── Base64 encoding ─────────────────────────────────────────────────
const char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const uint8_t* data, size_t len) {
    std::string result;
    result.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);

        result += kBase64Table[(n >> 18) & 0x3F];
        result += kBase64Table[(n >> 12) & 0x3F];
        result += (i + 1 < len) ? kBase64Table[(n >> 6) & 0x3F] : '=';
        result += (i + 2 < len) ? kBase64Table[n & 0x3F] : '=';
    }
    return result;
}

// ── Curl write callback ─────────────────────────────────────────────
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), total);
    return total;
}

} // anonymous namespace

// ── MiMoAsrEngine ──────────────────────────────────────────────────

MiMoAsrEngine::MiMoAsrEngine() = default;

MiMoAsrEngine::~MiMoAsrEngine() {
    cancelled_ = true;
    while (activeWorkers_.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool MiMoAsrEngine::Init(const Config& config) {
    apiEndpoint_ = config.apiEndpoint;
    apiKey_ = config.apiKey;
    modelName_ = config.modelName;
    language_ = config.language;

    if (apiEndpoint_.empty()) {
        apiEndpoint_ = "https://api.xiaomimimo.com/v1";
    }
    if (!apiEndpoint_.empty() && apiEndpoint_.back() == '/') {
        apiEndpoint_.pop_back();
    }
    if (modelName_.empty()) {
        modelName_ = "mimo-v2.5-asr";
    }
    if (language_.empty()) {
        language_ = "auto";
    }

    std::string maskedKey = apiKey_.empty() ? "(none)" :
        apiKey_.substr(0, 8) + "..." + apiKey_.substr(apiKey_.size() - 4);
    FCITX_INFO() << "[voice-input:mimo] Init: endpoint=" << apiEndpoint_
                 << " model=" << modelName_
                 << " language=" << language_
                 << " apiKey=" << maskedKey;

    if (apiKey_.empty()) {
        FCITX_ERROR() << "[voice-input:mimo] API key not configured";
        return false;
    }

    return true;
}

void MiMoAsrEngine::Start() {
    pcmBuffer_.clear();
    FCITX_DEBUG() << "[voice-input:mimo] Start (buffer cleared)";
}

void MiMoAsrEngine::FeedAudio(const float* pcm, size_t frames) {
    pcmBuffer_.insert(pcmBuffer_.end(), pcm, pcm + frames);
    float durSec = pcmBuffer_.size() / 16000.0f;
    if (static_cast<int>(pcmBuffer_.size()) % 16000 < static_cast<int>(frames)) {
        FCITX_DEBUG() << "[voice-input:mimo] FeedAudio: buffer=" << pcmBuffer_.size()
                      << " frames (" << durSec << "s)";
    }
}

void MiMoAsrEngine::Stop() {
    if (pcmBuffer_.empty()) {
        FCITX_WARN() << "[voice-input:mimo] Stop with empty buffer — no audio to transcribe";
        if (resultCb_) {
            resultCb_("", true);
        }
        return;
    }

    float durSec = pcmBuffer_.size() / 16000.0f;
    FCITX_INFO() << "[voice-input:mimo] Stop: " << pcmBuffer_.size()
                 << " frames (" << durSec << "s), starting transcription";

    // Detached worker: Stop() never blocks the caller (previously it joined
    // the HTTP thread here, freezing Fcitx5 for up to the curl timeout).
    // Audio moves in with the lambda, so concurrent workers never touch
    // pcmBuffer_; a ticket lock keeps execution strictly FIFO.
    std::thread([this, audio = std::move(pcmBuffer_)]() mutable {
        activeWorkers_.fetch_add(1);
        int ticket;
        {
            std::lock_guard<std::mutex> lock(ticketMutex_);
            ticket = nextTicket_++;
        }
        {
            std::unique_lock<std::mutex> lock(ticketMutex_);
            ticketCv_.wait(lock, [this, ticket] { return ticket == servedTicket_; });
        }
        TranscribeWorker(std::move(audio));
        {
            std::lock_guard<std::mutex> lock(ticketMutex_);
            servedTicket_++;
            ticketCv_.notify_all();
        }
        activeWorkers_.fetch_sub(1);
    }).detach();
}

void MiMoAsrEngine::TranscribeWorker(std::vector<float> audio) {
    auto finishEmpty = [this]() {
        if (resultCb_) {
            resultCb_("", true);
        }
    };

    size_t audioFrames = audio.size();
    float audioDurSec = audioFrames / 16000.0f;
    FCITX_INFO() << "[voice-input:mimo] TranscribeWorker: processing "
                 << audioFrames << " frames (" << audioDurSec << "s)";

    if (cancelled_) {
        FCITX_INFO() << "[voice-input:mimo] Cancelled before WAV encoding";
        return;
    }

    auto encodeStart = std::chrono::steady_clock::now();
    std::vector<uint8_t> wavData = FloatPcmToWav(audio.data(), audio.size());
    auto encodeEnd = std::chrono::steady_clock::now();
    auto encodeUs = std::chrono::duration_cast<std::chrono::microseconds>(encodeEnd - encodeStart).count();
    FCITX_DEBUG() << "[voice-input:mimo] WAV encoding: " << wavData.size()
                  << " bytes in " << encodeUs << "us (" << (encodeUs / 1000) << "ms)";

    if (cancelled_) {
        FCITX_INFO() << "[voice-input:mimo] Cancelled before HTTP request";
        return;
    }

    auto httpStart = std::chrono::steady_clock::now();
    std::string response = DoHttpRequest(wavData);
    auto httpEnd = std::chrono::steady_clock::now();
    auto httpMs = std::chrono::duration_cast<std::chrono::milliseconds>(httpEnd - httpStart).count();

    if (cancelled_) {
        FCITX_INFO() << "[voice-input:mimo] Cancelled after HTTP request";
        return;
    }

    FCITX_INFO() << "[voice-input:mimo] HTTP response: " << response.size()
                 << " bytes in " << httpMs << "ms";

    if (response.empty()) {
        FCITX_ERROR() << "[voice-input:mimo] Empty response from API";
        if (errorCb_) {
            errorCb_("Empty response from API");
        }
        finishEmpty();
        return;
    }

    Json::Value json;
    Json::Reader reader;
    if (!reader.parse(response, json)) {
        FCITX_ERROR() << "[voice-input:mimo] JSON parse error: "
                       << reader.getFormattedErrorMessages()
                       << " response=" << response.substr(0, 200);
        if (errorCb_) {
            errorCb_("JSON parse error");
        }
        finishEmpty();
        return;
    }

    if (json.isMember("error")) {
        std::string errMsg = json["error"].get("message", Json::Value("unknown error")).asString();
        FCITX_ERROR() << "[voice-input:mimo] API error: " << errMsg;
        if (errorCb_) {
            errorCb_("API error: " + errMsg);
        }
        finishEmpty();
        return;
    }

    // Parse Chat Completion response: choices[0].message.content
    std::string text;
    if (json.isMember("choices") && json["choices"].isArray() && !json["choices"].empty()) {
        const auto& msg = json["choices"][0].get("message", Json::Value(Json::objectValue));
        text = msg.get("content", Json::Value("")).asString();
    }

    FCITX_INFO() << "[voice-input:mimo] transcript: \""
                 << text << "\" (" << text.size() << " chars)";
    if (resultCb_) {
        resultCb_(text, true);
    }
}

std::string MiMoAsrEngine::DoHttpRequest(const std::vector<uint8_t>& wavData) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        FCITX_ERROR() << "[voice-input:mimo] Failed to initialize libcurl";
        if (errorCb_) errorCb_("Failed to initialize libcurl");
        return "";
    }

    std::string response;
    struct curl_slist* headers = nullptr;

    // URL: {endpoint}/chat/completions
    std::string url = apiEndpoint_ + "/chat/completions";
    FCITX_INFO() << "[voice-input:mimo] POST " << url
                 << " (wav=" << wavData.size() << " bytes)";

    // Base64-encode the WAV data
    std::string audioBase64 = Base64Encode(wavData.data(), wavData.size());
    std::string dataUrl = "data:audio/wav;base64," + audioBase64;

    // Build JSON request body
    Json::Value body;
    body["model"] = modelName_;

    Json::Value message;
    message["role"] = "user";

    Json::Value content;
    Json::Value inputAudio;
    inputAudio["data"] = dataUrl;
    content["type"] = "input_audio";
    content["input_audio"] = inputAudio;

    message["content"].append(content);
    body["messages"].append(message);

    Json::Value asrOptions;
    asrOptions["language"] = language_;
    body["asr_options"] = asrOptions;

    Json::StreamWriterBuilder writerBuilder;
    writerBuilder["indentation"] = "";
    std::string jsonBody = Json::writeString(writerBuilder, body);

    FCITX_DEBUG() << "[voice-input:mimo] Request body size: " << jsonBody.size()
                  << " bytes (base64 audio: " << audioBase64.size() << " chars)";

    // Headers
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string authHeader = "api-key: " + apiKey_;
    headers = curl_slist_append(headers, authHeader.c_str());

    // Curl options
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(jsonBody.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "fcitx5-voice-input/0.1.0");
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                     +[](void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                         auto* self = static_cast<MiMoAsrEngine*>(clientp);
                         return self->cancelled_ ? 1 : 0;
                     });
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    std::string contentType;
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
                     +[](void* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
                         auto* ct = static_cast<std::string*>(userdata);
                         std::string line(static_cast<char*>(ptr), size * nmemb);
                         if (line.rfind("Content-Type:", 0) == 0) {
                             *ct = line.substr(line.find(':') + 1);
                             if (!ct->empty() && (*ct)[0] == ' ') ct->erase(0, 1);
                             if (!ct->empty() && (*ct).back() == '\r') ct->pop_back();
                             if (!ct->empty() && (*ct).back() == '\n') ct->pop_back();
                         }
                         return size * nmemb;
                     });
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &contentType);

    CURLcode res = curl_easy_perform(curl);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res == CURLE_ABORTED_BY_CALLBACK) {
        return "";
    }
    if (res != CURLE_OK) {
        std::string err = curl_easy_strerror(res);
        FCITX_ERROR() << "[voice-input:mimo] HTTP request failed: " << err
                      << " (url=" << url << ")";
        if (errorCb_) errorCb_("HTTP request failed: " + err);
        return "";
    }

    FCITX_DEBUG() << "[voice-input:mimo] HTTP " << httpCode
                  << " response=" << response.size() << " bytes"
                  << " contentType=" << contentType;

    if (httpCode != 200) {
        std::string errMsg = "HTTP " + std::to_string(httpCode);
        if (!response.empty()) {
            Json::Value ejson;
            Json::Reader ereader;
            if (ereader.parse(response, ejson) && ejson.isMember("error")) {
                errMsg += ": " + ejson["error"].get("message", Json::Value(response)).asString();
            } else {
                errMsg += ": " + response;
            }
        }
        FCITX_ERROR() << "[voice-input:mimo] API error: " << errMsg;
        if (errorCb_) errorCb_("API error: " + errMsg);
        return "";
    }

    return response;
}

} // namespace fcitx
