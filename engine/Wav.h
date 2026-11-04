#pragma once
#include "Core.h"
#include <string>

namespace lb {

// Reads 16/24/32-bit PCM and 32/64-bit float WAV (incl. WAVE_FORMAT_EXTENSIBLE).
// Mono files are duplicated to both channels; channels beyond 2 are ignored.
bool readWav(const std::string& path, Buffer& out, std::string& error);

// Writes 24-bit PCM stereo.
bool writeWav24(const std::string& path, const Buffer& in, std::string& error);

} // namespace lb
