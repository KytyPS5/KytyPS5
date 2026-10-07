#include "graphics/host_gpu/renderer/cache/pageDiff.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "gpu_tiler_shaders/buffer_page_diff_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <cstring>

namespace Libs::Graphics {

namespace {

constexpr uint64_t MinCopyBytes = 1ull << 20u;

[[nodiscard]] uint64_t ResultWords(uint64_t pages) {
	return (pages + 31u) / 32u;
}

[[nodiscard]] vk::BufferMemoryBarrier2
RangeBarrier(vk::Buffer buffer, uint64_t offset, uint64_t size,
             vk::PipelineStageFlags2 source_stage, vk::AccessFlags2 source_access,
             vk::PipelineStageFlags2 destination_stage, vk::AccessFlags2 destination_access) {
	vk::BufferMemoryBarrier2 barrier {};
	barrier.srcStageMask        = source_stage;
	barrier.srcAccessMask       = source_access;
	barrier.dstStageMask        = destination_stage;
	barrier.dstAccessMask       = destination_access;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = buffer;
	barrier.offset              = offset;
	barrier.size                = size;
	return barrier;
}

void Barriers(vk::CommandBuffer command, std::span<const vk::BufferMemoryBarrier2> barriers) {
	vk::DependencyInfo dependency {};
	dependency.bufferMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
	dependency.pBufferMemoryBarriers    = barriers.data();
	command.pipelineBarrier2(dependency);
}

} // namespace

PageDiff::PageDiff(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler),
      m_results(graphics, scheduler, MemoryUsage::Download, 0,
                vk::BufferUsageFlagBits::eStorageBuffer, MaxPending * SlotBytes) {
	// Snapshots, copies and result slots are bound at these offsets.
	const auto alignment =
	    m_graphics.physical_device_properties.limits.minStorageBufferOffsetAlignment;
	EXIT_IF(alignment == 0 || SlotBytes % alignment != 0 || PageSize % alignment != 0);
	SetVulkanObjectNameF(m_graphics.device, m_results.Handle(), "Kyty.PageDiffResults");

	const vk::DescriptorSetLayoutBinding bindings[] {
	    {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	};
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = static_cast<uint32_t>(std::size(bindings));
	layout_info.pBindings    = bindings;
	RequireVulkanSuccess(
	    m_graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_descriptor_layout),
	    "create page-diff descriptor layout");

	const vk::PushConstantRange push_range {vk::ShaderStageFlagBits::eCompute, 0, sizeof(uint32_t)};
	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &m_descriptor_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_range;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr, &m_pipeline_layout),
	    "create page-diff pipeline layout");

	const auto                        module = CompileSPV(BUFFER_PAGE_DIFF_SPV, m_graphics.device);
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
	RequireVulkanSuccess(result, "create page-diff pipeline");
	SetVulkanObjectNameF(m_graphics.device, m_pipeline, "Kyty.PageDiff");
}

PageDiff::~PageDiff() {
	m_graphics.device.destroyPipeline(m_pipeline, nullptr);
	m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_descriptor_layout, nullptr);
}

bool PageDiff::HasFreeSlot() const noexcept {
	return std::ranges::find(m_slot_used, false) != m_slot_used.end();
}

std::optional<PageDiff::Ticket> PageDiff::Snapshot(const Buffer& source, uint64_t offset,
                                                   uint64_t pages) {
	if (pages == 0 || pages > MaxPages || offset > source.Size() ||
	    pages * PageSize > source.Size() - offset ||
	    pages * PageSize > m_graphics.physical_device_properties.limits.maxStorageBufferRange) {
		return std::nullopt;
	}
	const auto free_slot = std::ranges::find(m_slot_used, false);
	if (free_slot == m_slot_used.end()) {
		return std::nullopt;
	}
	const auto bytes = pages * PageSize;
	if (m_copies == nullptr || bytes > m_copies->Size() - m_copy_used) {
		// Earlier snapshots of this batch keep their copy memory until their comparisons ran.
		const auto previous = m_copies == nullptr ? 0 : m_copies->Size();
		const auto size     = std::max(Common::AlignUp(bytes, MinCopyBytes), previous);
		if (m_copies != nullptr) {
			m_retired.push_back(std::move(m_copies));
		}
		m_copies = std::make_unique<Buffer>(
		    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0,
		    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst, size);
		m_copy_used = 0;
		SetVulkanObjectNameF(m_graphics.device, m_copies->Handle(), "Kyty.PageDiffCopies");
	}

	const auto slot   = static_cast<uint32_t>(free_slot - m_slot_used.begin());
	m_slot_used[slot] = true;
	// A comparison that never ran reports every page as changed.
	const auto words = ResultWords(pages);
	std::memset(m_results.Mapped().data() + slot * SlotBytes, 0xff, words * sizeof(uint32_t));
	m_results.Flush(slot * SlotBytes, words * sizeof(uint32_t));

	const Ticket ticket {.source        = source.Handle(),
	                     .source_offset = offset,
	                     .copy          = m_copies->Handle(),
	                     .copy_offset   = m_copy_used,
	                     .pages         = pages,
	                     .slot          = slot};
	m_copy_used += bytes;

	m_scheduler.EndRendering();
	const auto command = m_scheduler.Current().Handle();
	using Stage        = vk::PipelineStageFlagBits2;
	using Access       = vk::AccessFlagBits2;
	const std::array before {
	    RangeBarrier(ticket.source, offset, bytes, Stage::eAllCommands, Access::eMemoryWrite,
	                 Stage::eTransfer, Access::eTransferRead),
	    RangeBarrier(ticket.copy, ticket.copy_offset, bytes, Stage::eAllCommands, {},
	                 Stage::eTransfer, Access::eTransferWrite),
	};
	Barriers(command, before);
	const vk::BufferCopy region {offset, ticket.copy_offset, bytes};
	command.copyBuffer(ticket.source, ticket.copy, 1, &region);
	const std::array after {
	    RangeBarrier(ticket.source, offset, bytes, Stage::eTransfer, {}, Stage::eAllCommands, {}),
	    RangeBarrier(ticket.copy, ticket.copy_offset, bytes, Stage::eTransfer,
	                 Access::eTransferWrite, Stage::eComputeShader, Access::eShaderStorageRead),
	};
	Barriers(command, after);
	return ticket;
}

void PageDiff::Compare(const Ticket& ticket) {
	EXIT_IF(ticket.slot >= MaxPending || !m_slot_used[ticket.slot]);
	const auto bytes  = ticket.pages * PageSize;
	const auto words  = ResultWords(ticket.pages);
	const auto offset = ticket.slot * SlotBytes;

	m_scheduler.EndRendering();
	const auto command = m_scheduler.Current().Handle();
	using Stage        = vk::PipelineStageFlagBits2;
	using Access       = vk::AccessFlagBits2;
	const std::array before {
	    RangeBarrier(ticket.source, ticket.source_offset, bytes, Stage::eAllCommands,
	                 Access::eMemoryWrite, Stage::eComputeShader, Access::eShaderStorageRead),
	};
	Barriers(command, before);

	const vk::DescriptorBufferInfo infos[] {
	    {ticket.source, ticket.source_offset, bytes},
	    {ticket.copy, ticket.copy_offset, bytes},
	    {m_results.Handle(), offset, words * sizeof(uint32_t)},
	};
	std::array<vk::WriteDescriptorSet, 3> writes {};
	for (uint32_t index = 0; index < writes.size(); ++index) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0, writes);
	const auto page_count = static_cast<uint32_t>(ticket.pages);
	command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
	                      sizeof(page_count), &page_count);
	command.dispatch(static_cast<uint32_t>(words), 1, 1);

	const std::array after {
	    RangeBarrier(m_results.Handle(), offset, words * sizeof(uint32_t), Stage::eComputeShader,
	                 Access::eShaderStorageWrite, Stage::eHost, Access::eHostRead),
	    RangeBarrier(ticket.source, ticket.source_offset, bytes, Stage::eComputeShader, {},
	                 Stage::eAllCommands, {}),
	    RangeBarrier(ticket.copy, ticket.copy_offset, bytes, Stage::eComputeShader, {},
	                 Stage::eAllCommands, {}),
	};
	Barriers(command, after);
}

void PageDiff::EndBatch() {
	m_copy_used = 0;
	for (auto& retired: m_retired) {
		m_scheduler.DeferOperation([owner = std::move(retired)]() mutable { owner.reset(); });
	}
	m_retired.clear();
}

std::span<const uint32_t> PageDiff::Result(const Ticket& ticket) {
	EXIT_IF(ticket.slot >= MaxPending || !m_slot_used[ticket.slot]);
	const auto words  = ResultWords(ticket.pages);
	const auto offset = ticket.slot * SlotBytes;
	m_results.Invalidate(offset, words * sizeof(uint32_t));
	return {reinterpret_cast<const uint32_t*>(m_results.Mapped().data() + offset),
	        static_cast<size_t>(words)};
}

void PageDiff::Free(const Ticket& ticket) {
	EXIT_IF(ticket.slot >= MaxPending || !m_slot_used[ticket.slot]);
	m_slot_used[ticket.slot] = false;
}

} // namespace Libs::Graphics
