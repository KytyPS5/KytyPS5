#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHDRAWSPLIT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHDRAWSPLIT_H_

// A mesh draw dispatches its instances as workgroup rows; a draw with more instances than one host
// draw takes goes in several, each passing its first instance to the shader (the mesh draw
// parameters' last dword), which adds it to the workgroup row. Free of Vulkan so it can be tested.

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics::MeshDrawSplit {

// Instances one host draw of `groups` workgroups per row takes.
[[nodiscard]] inline uint32_t RowsPerDraw(uint64_t max_rows, uint64_t max_total, uint32_t groups) {
	return static_cast<uint32_t>(
	    std::max<uint64_t>(1, std::min<uint64_t>(max_rows, max_total / std::max(groups, 1u))));
}

// Calls func(first_instance, instance_count) for each host draw of a draw of `instances` instances.
template <typename Func>
void ForEachDraw(uint32_t instances, uint32_t rows_per_draw, Func&& func) {
	for (uint32_t first = 0; first < instances;) {
		const auto count = std::min(rows_per_draw, instances - first);
		func(first, count);
		first += count;
	}
}

} // namespace Libs::Graphics::MeshDrawSplit

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHDRAWSPLIT_H_ */
