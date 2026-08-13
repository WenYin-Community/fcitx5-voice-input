#include "wav_encoder.h"

#include <cstring>

namespace fcitx {
namespace {

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

const char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // anonymous namespace

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
        // Clamp to [-1, 1]
        if (s < -1.0f) s = -1.0f;
        if (s > 1.0f) s = 1.0f;
        samplePtr[i] = static_cast<int16_t>(s * 32767.0f);
    }

    return wav;
}

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

} // namespace fcitx
