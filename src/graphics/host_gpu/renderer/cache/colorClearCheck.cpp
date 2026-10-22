#include "graphics/host_gpu/renderer/cache/colorClearCheck.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "gpu_tiler_shaders/color_clear_check_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"

#include <algorithm>
#include <array>

namespace Libs::Graphics {

namespace {

constexpr uint64_t PredicateRingSize = 1024 * 1024;

struct CheckParams {
	uint32_t meta_base;
	uint32_t slice_words;
	uint32_t pred_base;
	uint32_t code_count;
	uint32_t codes_low;
	uint32_t codes_high;
	uint32_t reset;
};

} // namespace

ColorClearCheck::ColorClearCheck(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler) {
	if (!m_graphics.conditional_rendering_enabled) {
		return;
	}
	const vk::DescriptorSetLayoutBinding bindings[] {
	    {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	};
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = static_cast<uint32_t>(std::size(bindings));
	layout_info.pBindings    = bindings;
	RequireVulkanSuccess(
	    m_graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_descriptor_layout),
	    "create color-clear check descriptor layout");

	vk::PushConstantRange push {};
	push.stageFlags = vk::ShaderStageFlagBits::eCompute;
	push.size       = sizeof(CheckParams);
	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &m_descriptor_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr, &m_pipeline_layout),
	    "create color-clear check pipeline layout");

	const auto module = CompileSPV(COLOR_CLEAR_CHECK_SPV, m_graphics.device);
	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage  = vk::ShaderStageFlagBits::eCompute;
	stage.module = module;
	stage.pName  = "main";
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage  = stage;
	pipeline_info.layout = m_pipeline_layout;
	const auto result    = m_graphics.device.createComputePipelines(nullptr, 1, &pipeline_info,
	                                                                nullptr, &m_pipeline);
	m_graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create color-clear check pipeline");
	SetVulkanObjectNameF(m_graphics.device, m_pipeline, "Color Clear Check");

	m_predicates = std::make_unique<Buffer>(
	    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0,
	    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eConditionalRenderingEXT |
	        vk::BufferUsageFlagBits::eTransferSrc,
	    PredicateRingSize);
	SetVulkanObjectNameF(m_graphics.device, m_predicates->Handle(), "Color Clear Predicates");
}

ColorClearCheck::~ColorClearCheck() {
	m_predicates.reset();
	if (m_pipeline != nullptr) {
		m_graphics.device.destroyPipeline(m_pipeline, nullptr);
	}
	if (m_pipeline_layout != nullptr) {
		m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	}
	if (m_descriptor_layout != nullptr) {
		m_graphics.device.destroyDescriptorSetLayout(m_descriptor_layout, nullptr);
	}
}

ColorClearCheck::Predicates ColorClearCheck::Record(vk::Buffer meta, uint64_t meta_offset,
                                                    uint64_t meta_size, uint64_t slice_size,
                                                    uint32_t slices,
                                                    std::span<const uint8_t> codes, bool reset) {
	EXIT_IF(!Supported() || slices == 0 || codes.empty() || codes.size() > MaxCodes ||
	        slice_size == 0 || slice_size % 4 != 0 || meta_size < slice_size * slices);
	const auto& limits = m_graphics.GetPhysicalDeviceProperties().limits;
	const auto  align  = std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);

	// Predicates live in a ring that wraps after many frames; before reusing it, wait for the
	// work recorded since the previous wrap (normally long finished).
	const auto bytes = Common::AlignUp(uint64_t {slices} * codes.size() * sizeof(uint32_t), align);
	EXIT_IF(bytes > PredicateRingSize);
	if (m_cursor + bytes > PredicateRingSize) {
		if (m_first_tick != 0) {
			m_scheduler.Wait(m_first_tick);
		}
		m_cursor     = 0;
		m_first_tick = 0;
	}
	if (m_first_tick == 0) {
		m_first_tick = m_scheduler.CurrentTick();
	}
	const auto pred_offset = m_cursor;
	m_cursor += bytes;

	// Storage bindings must start at an aligned offset; address the rest through push constants.
	const auto meta_binding = Common::AlignDown(meta_offset, align);
	const auto meta_skew    = meta_offset - meta_binding;
	EXIT_IF(meta_skew % 4 != 0);

	const vk::DescriptorBufferInfo infos[] {
	    {meta, meta_binding, meta_skew + slice_size * slices},
	    {m_predicates->Handle(), pred_offset, bytes},
	};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t index = 0; index < writes.size(); index++) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}
	CheckParams params {};
	params.meta_base   = static_cast<uint32_t>(meta_skew / 4);
	params.slice_words = static_cast<uint32_t>(slice_size / 4);
	params.pred_base   = 0;
	params.code_count  = static_cast<uint32_t>(codes.size());
	for (size_t k = 0; k < codes.size(); k++) {
		if (k < 4) {
			params.codes_low |= uint32_t {codes[k]} << (8u * k);
		} else {
			params.codes_high = codes[k];
		}
	}
	params.reset = reset ? 1u : 0u;

	m_scheduler.EndRendering();
	const auto command = m_scheduler.Current().Handle();
	vk::MemoryBarrier before {};
	before.srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eMemoryRead;
	before.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                        vk::PipelineStageFlagBits::eComputeShader, {}, 1, &before, 0, nullptr,
	                        0, nullptr);
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0,
	                             static_cast<uint32_t>(writes.size()), writes.data());
	command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params),
	                      &params);
	command.dispatch(slices, 1, 1);
	vk::MemoryBarrier after {};
	after.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	after.dstAccessMask = vk::AccessFlagBits::eConditionalRenderingReadEXT |
	                      vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                        vk::PipelineStageFlagBits::eConditionalRenderingEXT |
	                            vk::PipelineStageFlagBits::eAllCommands,
	                        {}, 1, &after, 0, nullptr, 0, nullptr);
	return {m_predicates->Handle(), pred_offset};
}

} // namespace Libs::Graphics
