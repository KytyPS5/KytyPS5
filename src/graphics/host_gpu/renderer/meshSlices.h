#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHSLICES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHSLICES_H_

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics {

struct MeshSlice {
	uint32_t base_group    = 0;
	uint32_t base_instance = 0;
	uint32_t groups        = 0;
	uint32_t instances     = 0;
};

struct MeshGridLimits {
	uint32_t groups    = 0;
	uint32_t instances = 0;
	uint32_t total     = 0;
};

template <typename Emit>
void ForEachMeshSlice(uint32_t groups, uint32_t instances, const MeshGridLimits& limits,
                      Emit&& emit) {
	const uint32_t row_limit = std::min(limits.groups, limits.total);
	if (groups == 0 || instances == 0 || row_limit == 0 || limits.instances == 0) {
		return;
	}
	if (groups <= row_limit) {
		const uint32_t rows = std::min(limits.instances, limits.total / groups);
		for (uint32_t base = 0; base < instances;) {
			const uint32_t count = std::min(rows, instances - base);
			emit(MeshSlice {0, base, groups, count});
			base += count;
		}
		return;
	}
	for (uint32_t instance = 0; instance < instances; instance++) {
		for (uint32_t base = 0; base < groups;) {
			const uint32_t count = std::min(row_limit, groups - base);
			emit(MeshSlice {base, instance, count, 1});
			base += count;
		}
	}
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHSLICES_H_
