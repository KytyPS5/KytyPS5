#include "graphics/guest_gpu/mipStats.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics::MipStats {

namespace {

constexpr uint64_t kNotSampled  = uint64_t {0xf} << 56u;
constexpr uint64_t kSampledMip0 = 1; // finest mip 0, used

// Counters bound since the last reset, one bit each. Graphics and compute queues bind textures.
std::array<std::atomic<uint64_t>, kCounterCount / 64> g_sampled {};

} // namespace

bool Enabled() noexcept {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_LOD_STATS");
		return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
	}();
	return enabled;
}

void NoteSampled(uint32_t counter) noexcept {
	counter %= kCounterCount;
	g_sampled[counter / 64].fetch_or(uint64_t {1} << (counter % 64), std::memory_order_relaxed);
}

void Report(void* dst, uint32_t size) noexcept {
	auto* bytes = static_cast<uint8_t*>(dst);
	std::memset(bytes, 0, size);
	if (size >= sizeof(uint32_t)) {
		// Games check this dword before reading the counters.
		const uint32_t valid = 1;
		std::memcpy(bytes, &valid, sizeof(valid));
	}
	if (size <= kHeaderSize) {
		return;
	}
	const auto count = std::min<uint32_t>((size - kHeaderSize) / sizeof(uint64_t), kCounterCount);
	for (uint32_t counter = 0; counter < count; ++counter) {
		const bool hit =
		    ((g_sampled[counter / 64].load(std::memory_order_relaxed) >> (counter % 64)) & 1u) != 0;
		const uint64_t word = hit ? kSampledMip0 : kNotSampled;
		std::memcpy(bytes + kHeaderSize + counter * sizeof(uint64_t), &word, sizeof(word));
	}
}

void Reset() noexcept {
	for (auto& bits: g_sampled) {
		bits.store(0, std::memory_order_relaxed);
	}
}

} // namespace Libs::Graphics::MipStats
