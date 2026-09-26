#pragma once

#include <mutex>

extern "C" {
#include "libatrac9.h"
}

namespace Libs::Audio::Ajm {

// Atrac9InitDecoder regenerates LibAtrac9's process-wide tables (MDCT/IMDCT windows, sin/cos and
// shuffle tables, Huffman lookups, gradient curves) on every call, and decoders are set up from
// several game threads at once: AJM config parsing and instance reset, NGS2 sampler setup and
// waveform parsing. Two of those calls racing write the same globals concurrently, so every call
// goes through this one lock. Atrac9GetHandle, Atrac9GetCodecInfo and the Atrac9Decode* entries
// only touch their handle (and read the tables), so they stay unguarded.
inline int AjmAtrac9InitDecoder(void* handle, unsigned char* config_data) {
	static std::mutex mutex;

	std::lock_guard lock(mutex);
	return Atrac9InitDecoder(handle, config_data);
}

} // namespace Libs::Audio::Ajm
