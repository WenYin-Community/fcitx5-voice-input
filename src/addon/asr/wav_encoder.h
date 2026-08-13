#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fcitx {

// Encode 16kHz mono float PCM (in [-1, 1]) to WAV file bytes.
std::vector<uint8_t> FloatPcmToWav(const float* pcm, size_t frames);

// Base64 encode for the /chat/completions input_audio data URL.
std::string Base64Encode(const uint8_t* data, size_t len);

} // namespace fcitx
