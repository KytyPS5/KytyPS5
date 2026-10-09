#ifndef KYTY_GRAPHICS_SHADER_MINLODSHIFT_H_
#define KYTY_GRAPHICS_SHADER_MINLODSHIFT_H_

// T# MinLod on hosts without VK_EXT_image_view_min_lod: the view starts `MinLodShift` levels higher. The host
// (view creation) and the recompiler (resinfo correction) both derive that shift here, so they cannot disagree.

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics {

// `min_lod` is the T# clamp relative to the view base in 1/256 level units, `level_count` the view's levels.
// The view starts at ceil(min_lod / 256) levels higher, never past its last level.
constexpr uint32_t MinLodShift(uint32_t level_count, uint32_t min_lod) {
	if (min_lod == 0 || level_count == 0) {
		return 0;
	}
#ifdef KYTY_MINLOD_MUTANT_FLOOR
	return std::min(min_lod / 256u, level_count - 1u);
#elif defined(KYTY_MINLOD_MUTANT_NO_CAP)
	return (min_lod + 255u) / 256u;
#else
	return std::min((min_lod + 255u) / 256u, level_count - 1u);
#endif
}

// True when MinLod has a fractional part (MinLod % 256 != 0); the shift rounds it up to a whole level.
constexpr bool MinLodIsFractional(uint32_t min_lod) {
	return (min_lod & 255u) != 0;
}

// What RESINFO must report for a T# whose MinLod was folded into the view base level. The hardware answers
// from the T# alone (extent at BASE_LEVEL, LAST_LEVEL - BASE_LEVEL + 1 levels), whatever MinLod says; the
// shifted view would answer 2^shift times smaller and `shift` levels fewer.
struct MinLodResinfo {
	uint32_t shift     = 0; // 0 = no correction (view size is already right)
	uint32_t extent[3] = {1, 1, 1}; // width/height/depth at BASE_LEVEL
	uint32_t levels    = 0;

	[[nodiscard]] constexpr bool Active() const { return shift != 0; }

	constexpr bool operator==(const MinLodResinfo&) const = default;
};

// `min_lod_abs` is the T# MinLod (U4.8, absolute); width/height/depth are the level-0 extents of the T#.
constexpr MinLodResinfo MakeMinLodResinfo(bool remap, uint32_t base_level, uint32_t last_level, uint32_t min_lod_abs,
                                          uint32_t width, uint32_t height, uint32_t depth) {
	MinLodResinfo result;
	if (!remap || last_level < base_level) {
		return result;
	}
	const uint32_t level_count = last_level - base_level + 1u;
	const uint32_t min_lod_rel = min_lod_abs > base_level * 256u ? min_lod_abs - base_level * 256u : 0u;
	const uint32_t shift       = MinLodShift(level_count, min_lod_rel);
	if (shift == 0) {
		return result;
	}
	result.shift = shift;
#ifdef KYTY_MINLOD_MUTANT_RESINFO
	// Mutant: report what the shifted view reports.
	const uint32_t at = base_level + shift;
	result.levels     = level_count - shift;
#else
	const uint32_t at = base_level;
	result.levels     = level_count;
#endif
	result.extent[0] = std::max(width >> std::min(at, 31u), 1u);
	result.extent[1] = std::max(height >> std::min(at, 31u), 1u);
	result.extent[2] = std::max(depth >> std::min(at, 31u), 1u);
	return result;
}

// RESINFO extent of mip `lod` (relative to BASE_LEVEL) for one dimension.
constexpr uint32_t MinLodResinfoExtent(uint32_t extent_at_base, uint32_t lod) {
	return std::max(extent_at_base >> std::min(lod, 31u), 1u);
}

} // namespace Libs::Graphics

#endif /* KYTY_GRAPHICS_SHADER_MINLODSHIFT_H_ */
