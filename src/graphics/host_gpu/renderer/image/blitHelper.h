#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BLITHELPER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BLITHELPER_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <compare>
#include <cstdint>
#include <vector>

namespace Libs::Graphics {

class CommandScheduler;
class Image;
struct BlitHelperTestAccess;
struct EqaaShadow;
struct GraphicContext;

class BlitHelper final {
public:
	inline static constexpr auto ColorToMsDepthLayout =
	    vk::ImageLayout::eDepthStencilAttachmentOptimal;

	BlitHelper(GraphicContext& graphics, CommandScheduler& scheduler);
	~BlitHelper();
	KYTY_CLASS_NO_COPY(BlitHelper);

	void ReinterpretColorAsMsDepth(Image& source, Image& destination);

	// Copies each pixel's anchor sample of `aspects` from the shadow into its owner's view, or
	// every sample into an owner that stores them all.
	void EqaaSelect(Image& owner, EqaaShadow& shadow, vk::ImageAspectFlags aspects);
	// Writes the owner's view of `aspects` into every sample of the shadow, sample for sample
	// from an owner that stores them all.
	void EqaaExpand(Image& owner, EqaaShadow& shadow, vk::ImageAspectFlags aspects);
	[[nodiscard]] static vk::Format EqaaColorFormat(vk::Format format);
	// Packed unsigned floats have no UINT view, but every texel they encode is exact in fp32.
	[[nodiscard]] static constexpr bool EqaaColorIsFloat(vk::Format format) {
		return format == vk::Format::eB10G11R11UfloatPack32;
	}

private:
	friend struct BlitHelperTestAccess;

	enum class EqaaKind : uint8_t {
		ColorSelect,
		ColorExpand,
		DepthSelect,
		DepthExpand,
		StencilSelect,
		StencilExpand,
		FloatSelect,
		FloatExpand,
		Count
	};

	struct EqaaPipelineKey {
		EqaaKind   kind    = EqaaKind::ColorSelect;
		vk::Format format  = vk::Format::eUndefined;
		uint32_t   samples = 1;

		auto operator<=>(const EqaaPipelineKey&) const = default;
	};

	[[nodiscard]] vk::Pipeline GetEqaaPipeline(EqaaPipelineKey key);
	void RecordEqaaLayer(Image& source, uint32_t source_level, uint32_t source_layer, Image& target,
	                     uint32_t target_level, uint32_t target_layer, vk::ImageAspectFlags aspects,
	                     bool select, uint16_t anchor_select);
	// Copies every sample of `layers` layers between two images of one sample count.
	void CopyEqaaSamples(Image& source, uint32_t source_level, uint32_t source_layer, Image& target,
	                     uint32_t target_level, uint32_t target_layer, uint32_t layers,
	                     vk::ImageAspectFlags aspects);

	struct PipelineKey {
		uint32_t   samples = 1;
		vk::Format format  = vk::Format::eUndefined;

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
	std::vector<Pipeline>   m_pipelines;
	vk::PipelineLayout                                                 m_eqaa_layout = nullptr;
	std::array<vk::ShaderModule, static_cast<size_t>(EqaaKind::Count)> m_eqaa_shaders {};
	std::vector<std::pair<EqaaPipelineKey, vk::Pipeline>>              m_eqaa_pipelines;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BLITHELPER_H_
