#include "graphics/host_gpu/renderer/cache/metaClearCheck.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "gpu_tiler_shaders/gpu_meta_clear_check_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <array>

namespace Libs::Graphics {

namespace {

constexpr uint64_t PredicateBufferSize = 256 * 1024;

struct CheckParameters {
	uint32_t slice_dwords = 0;
	uint32_t code_count   = 0;
	uint32_t codes_low    = 0;
	uint32_t codes_high   = 0;
	uint32_t expand       = 0;
};

} // namespace

MetaClearCheck::MetaClearCheck(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_predicates(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
                                         vk::BufferUsageFlagBits::eStorageBuffer |
                                             vk::BufferUsageFlagBits::eConditionalRenderingEXT,
                                         PredicateBufferSize) {
	SetVulkanObjectNameF(m_graphics.device, m_predicates.Handle(), "Kyty.MetaClearPredicates");
	const vk::DescriptorSetLayoutBinding bindings[] {
	    {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	};
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = std::size(bindings);
	layout_info.pBindings    = bindings;
	RequireVulkanSuccess(
	    m_graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_set_layout),
	    "create metadata clear descriptor layout");

	const vk::PushConstantRange  push_range {vk::ShaderStageFlagBits::eCompute, 0,
	                                         sizeof(CheckParameters)};
	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &m_set_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_range;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr, &m_pipeline_layout),
	    "create metadata clear pipeline layout");

	const auto module = CompileSPV(GPU_META_CLEAR_CHECK_SPV, m_graphics.device);
	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage  = vk::ShaderStageFlagBits::eCompute;
	stage.module = module;
	stage.pName  = "main";
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage  = stage;
	pipeline_info.layout = m_pipeline_layout;
	const auto result =
	    m_graphics.device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr, &m_pipeline);
	m_graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create metadata clear pipeline");
}

MetaClearCheck::~MetaClearCheck() {
	m_graphics.device.destroyPipeline(m_pipeline, nullptr);
	m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_set_layout, nullptr);
}

uint64_t MetaClearCheck::Record(vk::CommandBuffer command, const Buffer& metadata, uint64_t offset,
                                uint64_t slice_size, uint32_t slices,
                                std::span<const uint8_t> codes, bool expand) {
	const uint64_t bytes = uint64_t {slices} * codes.size() * sizeof(uint32_t);
	EXIT_IF(codes.empty() || codes.size() > MaxCodes || slices == 0 || slice_size == 0 ||
	        slice_size % sizeof(uint32_t) != 0 || slice_size / sizeof(uint32_t) > UINT32_MAX ||
	        bytes > m_predicates.Size());
	const auto alignment = std::max<uint64_t>(
	    m_graphics.GetPhysicalDeviceProperties().limits.minStorageBufferOffsetAlignment, 4);
	auto start = Common::AlignUp(m_cursor, alignment);
	if (start + bytes > m_predicates.Size()) {
		start = 0;
	}
	m_cursor = start + bytes;

	CheckParameters parameters {.slice_dwords = static_cast<uint32_t>(slice_size / 4),
	                            .code_count   = static_cast<uint32_t>(codes.size()),
	                            .expand       = expand ? 1u : 0u};
	for (uint32_t index = 0; index < codes.size(); index++) {
		auto& packed = index < 4 ? parameters.codes_low : parameters.codes_high;
		packed |= uint32_t {codes[index]} << ((index & 3u) * 8u);
	}

	// Earlier metadata writes become visible, and earlier predicate reads finish before reuse.
	vk::MemoryBarrier2 before {};
	before.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	before.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
	before.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	before.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
	vk::DependencyInfo dependency {};
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers    = &before;
	command.pipelineBarrier2(dependency);

	const vk::DescriptorBufferInfo infos[] {
	    {metadata.Handle(), offset, slice_size * slices},
	    {m_predicates.Handle(), start, bytes},
	};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t index = 0; index < writes.size(); ++index) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0, writes);
	command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
	                      sizeof(parameters), &parameters);
	command.dispatch(slices, 1, 1);

	vk::MemoryBarrier2 after {};
	after.srcStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	after.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	after.dstStageMask  = vk::PipelineStageFlagBits2::eConditionalRenderingEXT |
	                      vk::PipelineStageFlagBits2::eAllCommands;
	after.dstAccessMask = vk::AccessFlagBits2::eConditionalRenderingReadEXT |
	                      vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	dependency.pMemoryBarriers = &after;
	command.pipelineBarrier2(dependency);
	return start;
}

} // namespace Libs::Graphics
