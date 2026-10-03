#include "graphics/host_gpu/renderer/cache/faultManager.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "gpu_tiler_shaders/fault_buffer_process_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cinttypes>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace Libs::Graphics {

namespace {

constexpr size_t MaxPageFaults    = 1024;
constexpr size_t PageFaultAreaSize = MaxPageFaults * sizeof(uint64_t);

} // namespace

FaultManager::FaultManager(GraphicContext& graphics, CommandScheduler& scheduler,
                           BufferCache& buffer_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_buffer_cache(buffer_cache),
      m_fault_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                     BufferCache::CACHING_NUMPAGES / 8),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 0, AllFlags,
                        MaxPendingFaults * PageFaultAreaSize),
      m_fault_list(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, PageFaultAreaSize) {
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
	// RenderContext declares the pipeline cache before the BufferCache that owns this manager.
	const auto result = m_graphics.device.createComputePipelines(
	    m_scheduler.Context().GetPipelineCache().DriverCache(), 1, &pipeline_info, nullptr,
	    &m_fault_process_pipeline);
	m_graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create fault-buffer pipeline");
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

	const auto offset = m_current_area * PageFaultAreaSize;
	auto*      mapped = m_download_buffer.Mapped().data() + offset;
	std::memset(mapped, 0, PageFaultAreaSize);
	m_download_buffer.Flush(offset, PageFaultAreaSize);

	vk::BufferMemoryBarrier2 pre_barrier {};
	pre_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	pre_barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	pre_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	pre_barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
	pre_barrier.buffer        = m_fault_buffer.Handle();
	pre_barrier.offset        = 0;
	pre_barrier.size           = m_fault_buffer.Size();
	auto post_barrier         = pre_barrier;
	post_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	post_barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	post_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	post_barrier.dstAccessMask = vk::AccessFlagBits2::eShaderWrite;

	const vk::DescriptorBufferInfo infos[] {
	    {m_fault_buffer.Handle(), 0, m_fault_buffer.Size()},
	    {m_fault_list.Handle(), 0, PageFaultAreaSize},
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
	vk::BufferMemoryBarrier2 list_barrier {};
	list_barrier.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
	list_barrier.srcAccessMask =
	    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	list_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eClear;
	list_barrier.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
	list_barrier.buffer        = m_fault_list.Handle();
	list_barrier.offset        = 0;
	list_barrier.size          = PageFaultAreaSize;
	vk::DependencyInfo list_dependency {};
	list_dependency.bufferMemoryBarrierCount = 1;
	list_dependency.pBufferMemoryBarriers    = &list_barrier;
	command.pipelineBarrier2(list_dependency);
	command.fillBuffer(m_fault_list.Handle(), 0, PageFaultAreaSize, 0);
	list_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eClear;
	list_barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
	list_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	list_barrier.dstAccessMask =
	    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
	command.pipelineBarrier2(list_dependency);
	vk::DependencyInfo dependency {};
	dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
	dependency.bufferMemoryBarrierCount = 1;
	dependency.pBufferMemoryBarriers    = &pre_barrier;
	command.pipelineBarrier2(dependency);
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_fault_process_pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,
	                             m_fault_process_pipeline_layout, 0, writes);
	static_assert(BufferCache::CACHING_NUMPAGES % 128 == 0);
	const auto num_threads    = BufferCache::CACHING_NUMPAGES / 128;
	const auto num_workgroups = (num_threads + 63) / 64;
	command.dispatch(static_cast<uint32_t>(num_workgroups), 1, 1);
	dependency.pBufferMemoryBarriers = &post_barrier;
	command.pipelineBarrier2(dependency);
	list_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	list_barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	list_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eCopy;
	list_barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
	command.pipelineBarrier2(list_dependency);
	const vk::BufferCopy copy {0, offset, PageFaultAreaSize};
	command.copyBuffer(m_fault_list.Handle(), m_download_buffer.Handle(), 1, &copy);
	vk::BufferMemoryBarrier2 host_barrier {};
	host_barrier.srcStageMask             = vk::PipelineStageFlagBits2::eCopy;
	host_barrier.srcAccessMask            = vk::AccessFlagBits2::eTransferWrite;
	host_barrier.dstStageMask             = vk::PipelineStageFlagBits2::eHost;
	host_barrier.dstAccessMask            = vk::AccessFlagBits2::eHostRead;
	host_barrier.buffer                   = m_download_buffer.Handle();
	host_barrier.offset                   = offset;
	host_barrier.size                     = PageFaultAreaSize;
	list_dependency.pBufferMemoryBarriers = &host_barrier;
	command.pipelineBarrier2(list_dependency);

	const auto area = m_current_area;
	m_scheduler.DeferOperation([this, mapped, offset, area] {
		m_download_buffer.Invalidate(offset, PageFaultAreaSize);
		RangeSet    fault_ranges;
		const auto* faults = std::bit_cast<const uint64_t*>(mapped);
		// `fault_buffer_process.comp` increments the counter before checking that the slot it won
		// is inside the area, so the counter counts every faulting page while the area holds at
		// most MaxPageFaults - 1. Reading `count` entries walks off the end of the area.
		const auto  reported = static_cast<uint32_t>(faults[0]);
		const auto  count    = std::min<uint32_t>(reported, MaxPageFaults - 1u);
		if (reported > count) {
			// Dropped faults are self-healing: an unresolved page faults again on the next pass.
			static std::atomic<uint64_t> storms {0};
			const auto total = storms.fetch_add(1, std::memory_order_relaxed) + 1u;
			if ((total & (total - 1u)) == 0u) {
				LOGF("BDA fault storm: %" PRIu32 " pages faulted, %zu fit this pass, %" PRIu32
				     " deferred to the next; occurrence %" PRIu64 "\n",
				     reported, MaxPageFaults - 1u, reported - count, total);
			}
		}
		// The same page faults on every pass that dereferences it, so report the first sighting
		// and then every doubling.
		static std::mutex                             fault_mutex;
		static std::unordered_map<uint64_t, uint64_t> fault_counts;
		for (uint32_t index = 1; index <= count; ++index) {
			const auto address = BufferCache::GuestAddress(faults[index]);
			fault_ranges.Add(address, BufferCache::CACHING_PAGESIZE);
			uint64_t seen = 0;
			{
				const std::lock_guard<std::mutex> lock(fault_mutex);
				seen = ++fault_counts[address];
			}
			if ((seen & (seen - 1u)) == 0u) {
				LOGF("Accessed non-GPU cached memory at 0x%016" PRIx64 ", occurrence %" PRIu64
				     "\n",
				     address, seen);
			}
		}
		fault_ranges.ForEach([this](uint64_t start, uint64_t end) {
			EXIT_IF(end - start > std::numeric_limits<uint32_t>::max());
			m_buffer_cache.ResolveBdaFault(start, end - start);
		});
		m_fault_areas[area] = 0;
	});

	m_fault_areas[m_current_area++] = m_scheduler.CurrentTick();
	m_current_area %= MaxPendingFaults;
}

} // namespace Libs::Graphics
