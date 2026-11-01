#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BLITHELPER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BLITHELPER_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <compare>
#include <vector>

namespace Libs::Graphics {

class CommandScheduler;
class Image;
struct GraphicContext;

class BlitHelper final {
public:
	inline static constexpr auto ColorToMsDepthLayout =
	    vk::ImageLayout::eDepthStencilAttachmentOptimal;

	BlitHelper(GraphicContext& graphics, CommandScheduler& scheduler);
	~BlitHelper();
	KYTY_CLASS_NO_COPY(BlitHelper);

	void ReinterpretColorAsMsDepth(Image& source, Image& destination);
	// Whether CopyDepthToColor() and CopyColorToDepth() take `depth` (D32, with or without a
	// stencil aspect) and `color` (R32): 2D images of one level, one layer and one sample, of one
	// extent.
	[[nodiscard]] static bool CanCopyDepthColor(const Image& depth, const Image& color);
	// The depth aspect of `depth` copied into `color` with a compute shader, and back with a
	// fragment shader that writes the depth; the stencil aspect keeps its contents.
	void CopyDepthToColor(Image& depth, Image& color);
	void CopyColorToDepth(Image& color, Image& depth);

private:
	struct PipelineKey {
		uint32_t   samples = 1;
		vk::Format format  = vk::Format::eUndefined;
		// The fragment shader: false for ReinterpretColorAsMsDepth(), true for CopyColorToDepth().
		bool       copy    = false;

		auto operator<=>(const PipelineKey&) const = default;
	};

	struct Pipeline {
		PipelineKey  key;
		vk::Pipeline handle = nullptr;
	};

	[[nodiscard]] vk::Pipeline GetPipeline(PipelineKey key);

	GraphicContext&         m_graphics;
	CommandScheduler&       m_scheduler;
	vk::DescriptorSetLayout m_descriptor_layout = nullptr;
	vk::PipelineLayout      m_pipeline_layout   = nullptr;
	vk::ShaderModule        m_vertex_shader     = nullptr;
	vk::ShaderModule        m_fragment_shader   = nullptr;
	vk::ShaderModule        m_copy_shader       = nullptr;
	std::vector<Pipeline>   m_pipelines;
	// CopyDepthToColor(), created on first use.
	vk::DescriptorSetLayout m_to_color_descriptor_layout = nullptr;
	vk::PipelineLayout      m_to_color_layout            = nullptr;
	vk::Pipeline            m_to_color_pipeline          = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BLITHELPER_H_
