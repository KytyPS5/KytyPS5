#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_MIPSTATS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_MIPSTATS_H_

#include <cstdint>

// The texture units' mip statistics counters. A texture whose T# sets MIP_STATS_CNT_EN counts its
// sampling in counter MIP_STATS_CNT_ID; GET_LOD_STATS writes the counters to memory (a 64-byte
// header, then one 64-bit word per counter) and can reset them. In a word, bits 56-59 hold the
// finest mip sampled (15: not sampled) and bits 0-23 are nonzero once the texture was used. Games
// stream texture mips by these reports (Astro Bot drops mips nobody sampled for a while).
//
// Kyty does not see individual samples, so with KYTY_LOD_STATS=1 a texture counts as sampled at
// mip 0 when a draw or dispatch binds it, and counters nothing bound since the last reset report
// "not sampled". Without it, every report says that all textures were sampled at mip 0, so
// streaming games keep every texture they ever loaded at full resolution.
namespace Libs::Graphics::MipStats {

constexpr uint32_t kCounterCount = 256;
constexpr uint32_t kHeaderSize   = 64;

[[nodiscard]] bool Enabled() noexcept;
void               NoteSampled(uint32_t counter) noexcept;
// Writes a GET_LOD_STATS report of `size` bytes to `dst`.
void Report(void* dst, uint32_t size) noexcept;
void Reset() noexcept;

} // namespace Libs::Graphics::MipStats

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_MIPSTATS_H_ */
