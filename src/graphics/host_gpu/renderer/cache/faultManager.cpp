#include "graphics/host_gpu/renderer/cache/faultManager.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "gpu_tiler_shaders/fault_buffer_process_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cstring>
#include <limits>

namespace Libs::Graphics {

namespace {

constexpr size_t MaxPageFaults    = 1024;
constexpr size_t PageFaultAreaSize = MaxPageFaults * sizeof(uint64_t);
constexpr size_t DownloadAreaSize  = PageFaultAreaSize + BufferCache::BDA_USE_BITMAP_SIZE;

} // namespace

FaultManager::FaultManager(GraphicContext& graphics, CommandScheduler& scheduler,
                           BufferCache& buffer_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_buffer_cache(buffer_cache),
      m_fault_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                     BufferCache::FAULT_BUFFER_SIZE),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 0, AllFlags,
                        MaxPendingFaults * DownloadAreaSize) {
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

	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount = 1;
	pipeline_layout_info.pSetLayouts    = &m_fault_process_desc_layout;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                           &m_fault_process_pipeline_layout),
	    "create fault-buffer pipeline layout");

	m_fault_process_pipeline =
	    CreateComputePipeline(m_graphics.device, FAULT_BUFFER_PROCESS_SPV,
		                      m_fault_process_pipeline_layout, "create fault-buffer pipeline");
	SetVulkanObjectNameF(m_graphics.device, m_fault_process_pipeline, "Fault Buffer Parser");
}

FaultManager::~FaultManager() {
	m_graphics.device.destroyPipeline(m_fault_process_pipeline, nullptr);
	m_graphics.device.destroyPipelineLayout(m_fault_process_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_fault_process_desc_layout, nullptr);
}

void FaultManager::ProcessFaultBuffer() {
	if (const auto wait_tick = m_fault_areas[m_current_area]; wait_tick != 0) {
		m_scheduler.Wait(wait_tick);
		m_scheduler.PopPendingOperations();
	}

	const auto offset = m_current_area * DownloadAreaSize;
	auto*      mapped = m_download_buffer.Mapped().data() + offset;
	std::memset(mapped, 0, PageFaultAreaSize);
	m_download_buffer.Flush(offset, PageFaultAreaSize);

	vk::BufferMemoryBarrier2 pre_barrier {};
	pre_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	pre_barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	pre_barrier.dstStageMask =
	    vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eCopy;
	pre_barrier.dstAccessMask  = vk::AccessFlagBits2::eShaderRead |
	                             vk::AccessFlagBits2::eShaderWrite |
	                             vk::AccessFlagBits2::eTransferRead;
	pre_barrier.buffer        = m_fault_buffer.Handle();
	pre_barrier.offset        = 0;
	pre_barrier.size           = m_fault_buffer.Size();
	auto post_barrier         = pre_barrier;
	post_barrier.srcStageMask =
	    vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eClear;
	post_barrier.srcAccessMask =
	    vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite;
	post_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	post_barrier.dstAccessMask =
	    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
	vk::BufferMemoryBarrier2 use_barrier {};
	use_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eCopy;
	use_barrier.srcAccessMask = vk::AccessFlagBits2::eTransferRead;
	use_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eClear;
	use_barrier.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
	use_barrier.buffer        = m_fault_buffer.Handle();
	use_barrier.offset        = BufferCache::FAULT_BITMAP_SIZE;
	use_barrier.size          = BufferCache::BDA_USE_BITMAP_SIZE;
	vk::BufferMemoryBarrier2 host_barrier {};
	host_barrier.srcStageMask =
	    vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eCopy;
	host_barrier.srcAccessMask =
	    vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite;
	host_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eHost;
	host_barrier.dstAccessMask = vk::AccessFlagBits2::eHostRead;
	host_barrier.buffer        = m_download_buffer.Handle();
	host_barrier.offset        = offset;
	host_barrier.size          = DownloadAreaSize;

	const vk::DescriptorBufferInfo infos[] {
	    {m_fault_buffer.Handle(), 0, m_fault_buffer.Size()},
	    {m_download_buffer.Handle(), offset, PageFaultAreaSize},
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
	const auto num_threads    = BufferCache::CACHING_NUMPAGES / 128; // Four words per invocation.
	const auto num_workgroups = (num_threads + 63) / 64;
	command.dispatch(static_cast<uint32_t>(num_workgroups), 1, 1);
	const vk::BufferCopy use_copy {BufferCache::FAULT_BITMAP_SIZE, offset + PageFaultAreaSize,
	                               BufferCache::BDA_USE_BITMAP_SIZE};
	command.copyBuffer(m_fault_buffer.Handle(), m_download_buffer.Handle(), 1, &use_copy);
	dependency.pBufferMemoryBarriers = &use_barrier;
	command.pipelineBarrier2(dependency);
	command.fillBuffer(m_fault_buffer.Handle(), BufferCache::FAULT_BITMAP_SIZE,
	                   BufferCache::BDA_USE_BITMAP_SIZE, 0);
	const vk::BufferMemoryBarrier2 after[] {post_barrier, host_barrier};
	dependency.bufferMemoryBarrierCount = std::size(after);
	dependency.pBufferMemoryBarriers    = after;
	command.pipelineBarrier2(dependency);

	const auto area = m_current_area;
	m_scheduler.DeferOperation([this, mapped, offset, area] {
		m_download_buffer.Invalidate(offset, DownloadAreaSize);
		ApplyUseBitmap(mapped + PageFaultAreaSize);
		RangeSet    fault_ranges;
		const auto* faults = std::bit_cast<const uint64_t*>(mapped);
		// An overflowing scan keeps counting past the MaxPageFaults - 1 records the area holds.
		const auto count = std::min<uint64_t>(faults[0], MaxPageFaults - 1);
		for (uint32_t index = 1; index <= count; ++index) {
			const auto address = BufferCache::GuestAddress(faults[index]);
			fault_ranges.Add(address, BufferCache::CACHING_PAGESIZE);
			LOGF("Accessed non-GPU cached memory at 0x%016" PRIx64 "\n", address);
		}
		fault_ranges.ForEach([this](uint64_t start, uint64_t end) {
			EXIT_IF(end - start > std::numeric_limits<uint32_t>::max());
			(void)m_buffer_cache.FindBuffer(start, end - start);
			m_buffer_cache.MarkGpuUse(start, end - start);
		});
		m_fault_areas[area] = 0;
	});

	m_fault_areas[m_current_area++] = m_scheduler.CurrentTick();
	m_current_area %= MaxPendingFaults;
}

void FaultManager::ApplyUseBitmap(const uint8_t* bitmap) {
	constexpr auto words     = BufferCache::BDA_USE_BITMAP_SIZE / sizeof(uint32_t);
	uint64_t       run_begin = 0;
	uint64_t       run_end   = 0;
	const auto     flush     = [&] {
		if (run_end != run_begin) {
			m_buffer_cache.MarkGpuUse(BufferCache::GuestAddress(run_begin), run_end - run_begin);
		}
	};
	for (uint64_t index = 0; index < words; ++index) {
		uint32_t word = 0;
		std::memcpy(&word, bitmap + index * sizeof(word), sizeof(word));
		while (word != 0) {
			const auto bit   = static_cast<uint64_t>(std::countr_zero(word));
			const auto begin = (index * 32 + bit) << BufferCache::BDA_USE_REGIONBITS;
			word &= word - 1;
			// Runs stop at the lower-memory end, where guest addresses jump.
			if (begin != run_end || begin == LOWER_ADDRESS_SIZE) {
				flush();
				run_begin = begin;
			}
			run_end = begin + BufferCache::BDA_USE_REGIONSIZE;
		}
	}
	flush();
}

} // namespace Libs::Graphics
