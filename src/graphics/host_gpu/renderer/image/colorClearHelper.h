#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORCLEARHELPER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORCLEARHELPER_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <compare>
#include <span>
#include <vector>

namespace Libs::Graphics {

class CommandScheduler;
struct GraphicContext;

// Colour metadata clears decided on the GPU: classify each slice, then draw its colour or nothing.
class ColorClearHelper final {
public:
	// The colour travels as raw bits, so an integer clear reaches its attachment unchanged.
	struct Candidate {
		uint8_t                 code = 0;
		std::array<uint32_t, 4> color {};
	};

	static constexpr uint32_t MaxCandidates = 5;
	static constexpr uint32_t MaxSlices     = 2048;

	ColorClearHelper(GraphicContext& graphics, CommandScheduler& scheduler);
	~ColorClearHelper();
	KYTY_CLASS_NO_COPY(ColorClearHelper);

	[[nodiscard]] bool SupportsFormat(vk::Format format) const;
	// With `consume`, a matched slice is rewritten to 0xff, as the CPU materialization does.
	void Classify(vk::CommandBuffer command, vk::Buffer metadata, uint64_t offset,
	              uint64_t slice_size, uint32_t slices, std::span<const Candidate> candidates,
	              bool consume);
	// Must follow the Classify that produced `slice`.
	void Draw(vk::CommandBuffer command, vk::ImageView view, vk::Format format, uint32_t samples,
	          vk::Extent2D extent, uint32_t slice);

private:
	struct PipelineKey {
		uint32_t   samples = 1;
		vk::Format format  = vk::Format::eUndefined;

		auto operator<=>(const PipelineKey&) const = default;
	};

	struct Pipeline {
		PipelineKey  key;
		vk::Pipeline handle = nullptr;
	};

	[[nodiscard]] vk::Pipeline GetDrawPipeline(PipelineKey key);
	[[nodiscard]] vk::Pipeline GetClassifyPipeline();

	GraphicContext&         m_graphics;
	CommandScheduler&       m_scheduler;
	Buffer                  m_records;
	vk::DescriptorSetLayout m_classify_layout          = nullptr;
	vk::PipelineLayout      m_classify_pipeline_layout = nullptr;
	vk::Pipeline            m_classify_pipeline        = nullptr;
	vk::DescriptorSetLayout m_draw_layout              = nullptr;
	vk::PipelineLayout      m_draw_pipeline_layout     = nullptr;
	vk::ShaderModule        m_vertex_shader            = nullptr;
	vk::ShaderModule        m_fragment_shader          = nullptr;
	vk::ShaderModule        m_fragment_shader_uint     = nullptr;
	vk::ShaderModule        m_fragment_shader_sint     = nullptr;
	std::vector<Pipeline>   m_draw_pipelines;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORCLEARHELPER_H_
