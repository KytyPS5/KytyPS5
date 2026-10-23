#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_PAGEDIFF_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_PAGEDIFF_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>

namespace Libs::Graphics {

class CommandScheduler;
struct GraphicContext;

// Marks, one bit per page, where a buffer range differs from an earlier copy of it.
class PageDiff final {
public:
	static constexpr uint64_t PageSize = 4096;
	static constexpr uint32_t MaxPages = 65535;

	PageDiff(GraphicContext& graphics, CommandScheduler& scheduler);
	~PageDiff();
	KYTY_CLASS_NO_COPY(PageDiff);

	// `changed` must hold zeroed bits for every page; `size` is a whole number of pages.
	void Compare(vk::CommandBuffer command, vk::Buffer current, uint64_t current_offset,
	             vk::Buffer snapshot, vk::Buffer changed, uint64_t size);

private:
	[[nodiscard]] vk::Pipeline GetPipeline();

	GraphicContext&         m_graphics;
	CommandScheduler&       m_scheduler;
	vk::DescriptorSetLayout m_layout          = nullptr;
	vk::PipelineLayout      m_pipeline_layout = nullptr;
	vk::Pipeline            m_pipeline        = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_PAGEDIFF_H_
