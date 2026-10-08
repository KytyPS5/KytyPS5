#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_METACLEARCHECK_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_METACLEARCHECK_H_

#include "common/abi.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <cstdint>
#include <span>

namespace Libs::Graphics {

// Tests color-metadata slices for a uniform fast-clear code on the GPU. Each slice gets one
// 32-bit predicate per candidate code, for conditional rendering of the matching clear.
class MetaClearCheck {
public:
	static constexpr uint32_t MaxCodes = 8;

	MetaClearCheck(GraphicContext& graphics, CommandScheduler& scheduler);
	~MetaClearCheck();
	KYTY_CLASS_NO_COPY(MetaClearCheck);

	// Records the test of `slices` slices of `slice_size` bytes at `offset` in `metadata`;
	// matching slices are expanded to 0xff when `expand` is set. Returns the offset of the
	// slice-major predicates in PredicateBuffer().
	[[nodiscard]] uint64_t   Record(vk::CommandBuffer command, const Buffer& metadata,
	                                uint64_t offset, uint64_t slice_size, uint32_t slices,
	                                std::span<const uint8_t> codes, bool expand);
	[[nodiscard]] vk::Buffer PredicateBuffer() const noexcept { return m_predicates.Handle(); }

private:
	GraphicContext&         m_graphics;
	Buffer                  m_predicates;
	uint64_t                m_cursor          = 0;
	vk::DescriptorSetLayout m_set_layout      = nullptr;
	vk::PipelineLayout      m_pipeline_layout = nullptr;
	vk::Pipeline            m_pipeline        = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_METACLEARCHECK_H_
