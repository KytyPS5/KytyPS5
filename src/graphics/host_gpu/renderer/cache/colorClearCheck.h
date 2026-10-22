#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_COLORCLEARCHECK_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_COLORCLEARCHECK_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <memory>
#include <span>

namespace Libs::Graphics {

class Buffer;
class CommandScheduler;
struct GraphicContext;

// Decides on the GPU whether color metadata slices hold a uniform fast-clear code, so binding a
// surface whose metadata the GPU just wrote does not need a synchronous readback. The results
// are 32-bit predicates for VK_EXT_conditional_rendering.
class ColorClearCheck {
public:
	static constexpr uint32_t MaxCodes = 5;

	ColorClearCheck(GraphicContext& graphics, CommandScheduler& scheduler);
	~ColorClearCheck();
	KYTY_CLASS_NO_COPY(ColorClearCheck);

	[[nodiscard]] bool Supported() const noexcept { return m_pipeline != nullptr; }

	struct Predicates {
		vk::Buffer buffer;
		uint64_t   offset; // predicate (slice, code) is at offset + (slice * codes + code) * 4
	};

	// Records the check of `slices` consecutive slices of `slice_size` bytes that start at
	// `meta_offset` in `meta`. A slice whose bytes all equal codes[k] sets predicate k; with
	// `reset`, such a slice is then overwritten with 0xff.
	[[nodiscard]] Predicates Record(vk::Buffer meta, uint64_t meta_offset, uint64_t meta_size,
	                                uint64_t slice_size, uint32_t slices,
	                                std::span<const uint8_t> codes, bool reset);

private:
	GraphicContext&         m_graphics;
	CommandScheduler&       m_scheduler;
	vk::DescriptorSetLayout m_descriptor_layout = nullptr;
	vk::PipelineLayout      m_pipeline_layout   = nullptr;
	vk::Pipeline            m_pipeline          = nullptr;
	std::unique_ptr<Buffer> m_predicates;
	uint64_t                m_cursor     = 0;
	uint64_t                m_first_tick = 0; // first submission tick since the ring last wrapped
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_COLORCLEARCHECK_H_
