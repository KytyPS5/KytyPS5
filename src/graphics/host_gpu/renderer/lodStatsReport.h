#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_LODSTATSREPORT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_LODSTATSREPORT_H_

// GET_LOD_STATS report layout and the per-image counting field, free of Vulkan so they can be
// tested on the CPU. Used by LodStatsCounter (lodStats.h) and the per-draw shader data.
//
// Report, as ASTRO BOT's texture streamer reads it:
//   64-byte header; dword 0 non-zero marks the report complete.
//   256 64-bit entries, one per mip-statistics counter:
//     bits 0..23   samples whose LOD the T# MIN_LOD clamp raised. The streamer promotes a texture
//                  to full resolution only while the field is non-zero, and does not evict it
//                  while it is non-zero.
//     bits 24..31  the counter id.
//     bits 56..59  finest mip level sampled; 0xF when nothing was sampled.

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace Libs::Graphics::LodStatsReport {

constexpr uint32_t Counters   = 256;
constexpr uint32_t Entries    = Counters + 1; // the shader's last entry absorbs images without id
constexpr uint32_t ReportSize = 64 + Counters * 8;
constexpr uint32_t Unsampled  = 0xffffffffu; // reset value of a finest-level word
// ImageField bit set when the image's T# has no mip-statistics counter: the instrumented shader
// records nothing for it.
constexpr uint32_t NoCounterFlag = 0x8000u;

// Whether the T# enables a mip-statistics counter (MipStatsCntEn, dword 5 bit 25).
[[nodiscard]] inline bool HasCounter(const uint32_t* tsharp) {
	return ((tsharp[5] >> 25u) & 1u) != 0u;
}

// The 32-bit per-image field of an instrumented shader's data, from the image's T# dwords:
//   bits 0..7   MipStatsCntId (dword 6 bits 0..7)
//   bits 8..11  BASE_LEVEL (dword 3 bits 12..15): recorded levels are absolute
//   bit 15      set when MipStatsCntEn (dword 5 bit 25) is clear
//   bits 16..27 U4.8 LOD threshold, the T# MIN_LOD (dword 1 bits 8..19): a sample finer than
//               it counts, i.e. the resident-level clamp raised it.
[[nodiscard]] inline uint32_t ImageField(const uint32_t* tsharp) {
	if (!HasCounter(tsharp)) {
		return NoCounterFlag;
	}
	const uint32_t id         = tsharp[6] & 0xffu;
	const uint32_t base_level = (tsharp[3] >> 12u) & 0xfu;
	const uint32_t min_lod    = (tsharp[1] >> 8u) & 0xfffu;
	return id | (base_level << 8u) | (min_lod << 16u);
}

// CPU model of one sample recorded by the instrumented shader (EmitMipStatsSample and
// EmitMipStatsRecord): `lod` is the unclamped LOD relative to the view's base level. `words` is
// the counter buffer (Entries finest-level words, then Entries counts).
inline void RecordSample(uint32_t* words, uint32_t field, float lod) {
	if ((field & NoCounterFlag) != 0u) {
		return;
	}
	const uint32_t counter   = field & 0xffu;
	const float    absolute  = lod + static_cast<float>((field >> 8u) & 0xfu);
	const float    clamped   = std::clamp(absolute, 0.0f, 14.0f);
	const auto     level     = static_cast<uint32_t>(clamped); // floor: clamped is not negative
	const float    threshold = static_cast<float>((field >> 16u) & 0xfffu) / 256.0f;
	words[counter]           = std::min(words[counter], level);
	if (clamped < threshold) {
		words[Entries + counter]++;
	}
}

// One report entry from a counter's finest-level and count words.
[[nodiscard]] inline uint64_t PackEntry(uint32_t finest, uint32_t count, uint32_t counter) {
	const uint64_t level = finest == Unsampled ? 0xfu : std::min<uint32_t>(finest, 14u);
	return (level << 56u) | (uint64_t {counter & 0xffu} << 24u) |
	       std::min<uint32_t>(count, 0xffffffu);
}

// Fills a complete report (header dword 0 = 1) from the copied counter words.
inline void PackReport(const uint32_t* words, uint8_t* report) {
	std::memset(report, 0, ReportSize);
	const uint32_t valid = 1;
	std::memcpy(report, &valid, sizeof(valid));
	for (uint32_t counter = 0; counter < Counters; counter++) {
		const auto entry = PackEntry(words[counter], words[Entries + counter], counter);
		std::memcpy(report + 64 + counter * sizeof(uint64_t), &entry, sizeof(entry));
	}
}

} // namespace Libs::Graphics::LodStatsReport

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_LODSTATSREPORT_H_
