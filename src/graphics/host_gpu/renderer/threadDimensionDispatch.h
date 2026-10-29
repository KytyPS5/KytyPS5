#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_THREADDIMENSIONDISPATCH_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_THREADDIMENSIONDISPATCH_H_

#include "common/abi.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstdint>

// An indirect dispatch in thread dimensions reads thread counts, not group counts, and an
// earlier command of the same submission can write them: the CPU cannot read them when the
// dispatch is recorded. A helper dispatch turns them, on the GPU, into the group counts of the
// host's indirect dispatch and into the extent that the dispatched shader reads from its shader
// data buffer.

namespace Libs::Graphics {

struct GraphicContext;

class ThreadDimensionDispatch {
public:
	explicit ThreadDimensionDispatch(GraphicContext& graphics);
	~ThreadDimensionDispatch();
	KYTY_CLASS_NO_COPY(ThreadDimensionDispatch);

	struct Target {
		vk::Buffer buffer = nullptr;
		uint64_t   offset = 0; // aligned for a storage buffer
		uint64_t   size   = 0;
	};

	// Records the conversion outside a render pass. `args` at any dword offset holds the thread
	// counts; three group counts go to the start of `groups`; the three thread counts go to dword
	// `extent_dword` of `extent`. Afterwards the groups can be read as indirect arguments and the
	// extent by any shader.
	void Record(vk::CommandBuffer command, vk::Buffer args_buffer, uint64_t args_offset,
	            std::array<uint32_t, 3> group_size, const Target& groups, const Target& extent,
	            uint32_t extent_dword, float max_groups = 0.0f);
	// A copy of group counts that dispatches nothing when they ask for more than `max_groups` in
	// all. Written to `groups`; the arguments are left as they are.
	void RecordLimited(vk::CommandBuffer command, vk::Buffer args_buffer, uint64_t args_offset,
	                   const Target& groups, const Target& unused_extent, float max_groups) {
		Record(command, args_buffer, args_offset, {1u, 1u, 1u}, groups, unused_extent, 0u, max_groups);
	}

private:
	GraphicContext&         m_graphics;
	vk::DescriptorSetLayout m_descriptor_layout = nullptr;
	vk::PipelineLayout      m_pipeline_layout   = nullptr;
	vk::Pipeline            m_pipeline          = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_THREADDIMENSIONDISPATCH_H_
