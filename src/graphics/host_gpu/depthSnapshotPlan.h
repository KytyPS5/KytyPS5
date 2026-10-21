#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_DEPTHSNAPSHOTPLAN_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_DEPTHSNAPSHOTPLAN_H_

#include <cstdint>

namespace Libs::Graphics {

// Whether a depth-bounds draw can read the depth snapshot of the previous one.
//
// On a host without the depth-bounds test the pixel shader reads a copy of the depth target
// (the snapshot). The copy costs a full-target transfer per draw (8 MiB at 1080p, 75 draws a frame
// in the test title), while between two such draws the depth target usually does not change: the
// draws write no depth. A snapshot is reused while the target is the same image, mip and layer,
// nothing has written the image since (its content generation), and the pass clear state is the
// same: a pass that clears depth first gives every pixel the clear value, and the snapshot is a
// fill with that value instead of a copy.
//
// A fill does not depend on what the image held, so in a clearing pass the content generation
// is not compared: the renderer counts each draw of the pass as a write, and each depth-bounds
// draw ends the pass, so the next one starts again with the clear.
struct DepthSnapshotState {
	// The target of this draw: the image (slot and generation) and its layer. The snapshot is
	// taken of mip 0 only.
	uint64_t target_image       = 0;
	uint32_t target_layer       = 0;
	uint64_t content_generation = 0;
	bool     load_clear         = false;
	float    clear_value        = 0.0f;
	// The snapshot held from an earlier draw, if any.
	bool     cached             = false;
	uint64_t cached_target_image = 0;
	uint32_t cached_target_layer = 0;
	uint64_t cached_generation  = 0;
	bool     cached_load_clear  = false;
	float    cached_clear_value = 0.0f;
};

enum class DepthSnapshotStep : uint8_t {
	// The held snapshot shows the target as it is: read it again.
	Reuse,
	// Copy the depth target into the snapshot.
	Copy,
	// The pass clears depth first: fill the snapshot with the clear value.
	Fill,
};

[[nodiscard]] constexpr DepthSnapshotStep PlanDepthSnapshot(const DepthSnapshotState& s) noexcept {
	const bool both_clear  = s.load_clear && s.cached_load_clear;
	const bool same_target = s.cached && s.cached_target_image == s.target_image &&
	                         s.cached_target_layer == s.target_layer &&
	                         (both_clear || s.cached_generation == s.content_generation);
	const bool same_clear =
	    s.cached_load_clear == s.load_clear && (!s.load_clear || s.cached_clear_value == s.clear_value);
	if (same_target && same_clear) {
		return DepthSnapshotStep::Reuse;
	}
	return s.load_clear ? DepthSnapshotStep::Fill : DepthSnapshotStep::Copy;
}

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_DEPTHSNAPSHOTPLAN_H_ */
