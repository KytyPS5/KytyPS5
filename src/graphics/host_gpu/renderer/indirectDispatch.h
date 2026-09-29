#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTDISPATCH_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTDISPATCH_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <vulkan/vulkan.hpp>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;

// DISPATCH_INDIRECT with the thread-dimension initiator carries thread counts, not workgroup
// counts. The command processor used to read them on the CPU, but the previous dispatch writes
// them on the GPU, so every read drained the GPU; the title issues about 200 of these a frame.
// This records a one-invocation pass that converts the counts on the GPU instead.
class IndirectDispatchGroups {
public:
	IndirectDispatchGroups(GraphicContext& graphics, CommandScheduler& scheduler);
	~IndirectDispatchGroups();
	KYTY_CLASS_NO_COPY(IndirectDispatchGroups);

	struct Result {
		vk::Buffer        groups_buffer;
		vk::DeviceSize    groups_offset = 0;
		// A copy of the thread counts that stays valid until the dispatch has run, unlike the
		// cache buffer the guest's arguments live in, which a later binding may merge away.
		vk::DeviceAddress threads = 0;
	};
	// Records the conversion of the three thread counts at `threads` and returns the workgroup
	// counts' location, ready for dispatchIndirect. Binds a compute pipeline and push state, so
	// call it before committing the guest dispatch's bindings.
	[[nodiscard]] Result Convert(vk::CommandBuffer command, vk::DeviceAddress threads,
	                             const std::array<uint32_t, 3>& local_size);

private:
	// Ring of converted counts. An entry is rewritten only after 4096 later conversions, each
	// ordered behind the indirect reads before it.
	static constexpr uint32_t Entries      = 4096;
	static constexpr uint32_t EntryDwords  = 8; // groups at 0, thread counts at 4

	GraphicContext&         m_graphics;
	Buffer                  m_groups;
	vk::DescriptorSetLayout m_set_layout      = nullptr;
	vk::PipelineLayout      m_pipeline_layout = nullptr;
	vk::Pipeline            m_pipeline        = nullptr;
	uint32_t                m_next            = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTDISPATCH_H_
