#include "graphics/host_gpu/renderer/cache/faultManager.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "gpu_tiler_shaders/fault_buffer_process_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <bit>
#include <cinttypes>
#include <cstring>
#include <limits>
#include <vector>

namespace Libs::Graphics {

namespace {

constexpr size_t MaxPageFaults = 1024;
// Pages V#-table stores may write between two reads; more stay in the bitmap for the next read.
constexpr size_t MaxPageWrites = 16384;

} // namespace

FaultManager::Reader::Reader(GraphicContext& graphics, CommandScheduler& scheduler, size_t capacity)
    : download(graphics, scheduler, MemoryUsage::Download, 0, AllFlags,
               MaxPendingFaults * capacity * sizeof(uint64_t)),
      area_size(capacity * sizeof(uint64_t)) {}

FaultManager::FaultManager(GraphicContext& graphics, CommandScheduler& scheduler,
                           BufferCache& buffer_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_buffer_cache(buffer_cache),
      m_fault_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                     BufferCache::FAULT_BUFFER_SIZE),
      m_faults(graphics, scheduler, MaxPageFaults), m_writes(graphics, scheduler, MaxPageWrites) {
	SetVulkanObjectNameF(m_graphics.device, m_fault_buffer.Handle(), "Fault Buffer");

	const vk::DescriptorSetLayoutBinding bindings[] {
	    {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	};
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = std::size(bindings);
	layout_info.pBindings    = bindings;
	RequireVulkanSuccess(
	    m_graphics.device.createDescriptorSetLayout(&layout_info, nullptr,
	                                                &m_fault_process_desc_layout),
	    "create fault-buffer descriptor layout");

	const auto module = CompileSPV(FAULT_BUFFER_PROCESS_SPV, m_graphics.device);

	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount = 1;
	pipeline_layout_info.pSetLayouts    = &m_fault_process_desc_layout;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                           &m_fault_process_pipeline_layout),
	    "create fault-buffer pipeline layout");

	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage  = vk::ShaderStageFlagBits::eCompute;
	stage.module = module;
	stage.pName  = "main";
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage  = stage;
	pipeline_info.layout = m_fault_process_pipeline_layout;
	const auto result = m_graphics.device.createComputePipelines(
	    nullptr, 1, &pipeline_info, nullptr, &m_fault_process_pipeline);
	m_graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create fault-buffer pipeline");
	SetVulkanObjectNameF(m_graphics.device, m_fault_process_pipeline, "Fault Buffer Parser");
}

FaultManager::~FaultManager() {
	m_graphics.device.destroyPipeline(m_fault_process_pipeline, nullptr);
	m_graphics.device.destroyPipelineLayout(m_fault_process_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_fault_process_desc_layout, nullptr);
}

void FaultManager::PrepareFaultBuffer() {
	if (!m_cleared) {
		// Shaders only set bits: both bitmaps must start clear.
		m_fault_buffer.Fill(0, m_fault_buffer.Size(), 0);
		m_cleared = true;
	}
}

void FaultManager::ProcessFaultBuffer() {
	Process(m_faults, 0, [this](std::span<const uint64_t> pages) {
		RangeSet fault_ranges;
		for (const auto address: pages) {
			fault_ranges.Add(address, BufferCache::CACHING_PAGESIZE);
			LOGF("Accessed non-GPU cached memory at 0x%016" PRIx64 "\n", address);
		}
		fault_ranges.ForEach([this](uint64_t start, uint64_t end) {
			EXIT_IF(end - start > std::numeric_limits<uint32_t>::max());
			(void)m_buffer_cache.FindBuffer(start, end - start);
		});
	});
}

void FaultManager::ProcessWriteBuffer(PagesHandler&& handler) {
	Process(m_writes, BufferCache::WRITE_BITMAP_OFFSET, std::move(handler));
}

void FaultManager::Process(Reader& reader, uint64_t bitmap_offset, PagesHandler&& handler) {
	if (const auto wait_tick = reader.ticks[reader.current]; wait_tick != 0) {
		m_scheduler.Wait(wait_tick);
		m_scheduler.PopPendingOperations();
	}

	const auto area_size = reader.area_size;
	const auto offset    = reader.current * area_size;
	auto*      mapped    = reader.download.Mapped().data() + offset;
	// The parser appends after the count; the host reads only the entries it counted.
	std::memset(mapped, 0, sizeof(uint64_t));
	reader.download.Flush(offset, sizeof(uint64_t));

	constexpr auto           bitmap_size = BufferCache::CACHING_NUMPAGES / 8;
	vk::BufferMemoryBarrier2 pre_barrier {};
	pre_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	pre_barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	pre_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	pre_barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
	pre_barrier.buffer        = m_fault_buffer.Handle();
	pre_barrier.offset         = bitmap_offset;
	pre_barrier.size           = bitmap_size;
	auto post_barrier          = pre_barrier;
	post_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	post_barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	post_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	post_barrier.dstAccessMask = vk::AccessFlagBits2::eShaderWrite;

	const vk::DescriptorBufferInfo infos[] {
	    {m_fault_buffer.Handle(), bitmap_offset, bitmap_size},
	    {reader.download.Handle(), offset, area_size},
	};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t index = 0; index < writes.size(); ++index) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}

	m_scheduler.EndRendering();
	auto command = m_scheduler.Current().Handle();
	vk::DependencyInfo dependency {};
	dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
	dependency.bufferMemoryBarrierCount = 1;
	dependency.pBufferMemoryBarriers    = &pre_barrier;
	command.pipelineBarrier2(dependency);
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_fault_process_pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,
	                             m_fault_process_pipeline_layout, 0, writes);
	const auto num_threads    = BufferCache::CACHING_NUMPAGES / 32;
	const auto num_workgroups = (num_threads + 63) / 64;
	command.dispatch(static_cast<uint32_t>(num_workgroups), 1, 1);
	dependency.pBufferMemoryBarriers = &post_barrier;
	command.pipelineBarrier2(dependency);

	const auto area = reader.current;
	m_scheduler.DeferOperation(
	    [&reader, mapped, offset, area, area_size, handler = std::move(handler)] {
		    reader.download.Invalidate(offset, area_size);
		    const auto* entries = std::bit_cast<const uint64_t*>(mapped);
		    // The parser counts every set bit but stores only the entries the area holds; it leaves
		    // the others set for the next read.
		    const auto capacity = area_size / sizeof(uint64_t) - 1;
		    const auto count    = std::min<uint64_t>(static_cast<uint32_t>(entries[0]), capacity);
		    std::vector<uint64_t> pages;
		    pages.reserve(count);
		    for (uint64_t index = 1; index <= count; ++index) {
			    pages.push_back(BufferCache::GuestAddress(entries[index]));
		    }
		    if (!pages.empty()) {
			    handler(pages);
		    }
		    reader.ticks[area] = 0;
	    });

	reader.ticks[reader.current++] = m_scheduler.CurrentTick();
	reader.current %= MaxPendingFaults;
}

} // namespace Libs::Graphics
