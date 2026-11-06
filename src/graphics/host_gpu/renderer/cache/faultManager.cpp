#include "graphics/host_gpu/renderer/cache/faultManager.h"

#include "common/assert.h"
#include "common/hackNotice.h"
#include "common/logging/log.h"
#include "gpu_tiler_shaders/fault_buffer_process_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cinttypes>
#include <cstring>
#include <limits>
#include <vector>

namespace Libs::Graphics {

namespace {

constexpr size_t MaxPageFaults = 1024;
// Pages V#-table stores may write between two reads; more stay in the bitmap for the next read.
constexpr size_t MaxPageWrites = 16384;
// Pages device-address accesses found cached between two reads; more stay for the next read.
constexpr size_t MaxPageUses = 65536;

// What each HackMark changed, for its notice.
struct HackMarkNotice {
	const char* hack;
	const char* site;
	const char* what;
};
constexpr std::array<HackMarkNotice, static_cast<size_t>(HackMark::Count)> HackMarkNotices {};

} // namespace

FaultManager::Reader::Reader(GraphicContext& graphics, CommandScheduler& scheduler,
                             size_t capacity)
    : download(graphics, scheduler, MemoryUsage::Download, 0, AllFlags,
               MaxPendingFaults * capacity * sizeof(uint64_t)),
      area_size(capacity * sizeof(uint64_t)) {}

FaultManager::FaultManager(GraphicContext& graphics, CommandScheduler& scheduler,
                           BufferCache& buffer_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_buffer_cache(buffer_cache),
      m_fault_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                     BufferCache::FAULT_BUFFER_SIZE),
      m_faults(graphics, scheduler, MaxPageFaults), m_writes(graphics, scheduler, MaxPageWrites),
      m_uses(graphics, scheduler, MaxPageUses),
      m_marks_download(graphics, scheduler, MemoryUsage::Download, 0, AllFlags,
                       BufferCache::HACK_MARKS_SIZE) {
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

void FaultManager::PrepareFaultBuffer() {
	if (!m_cleared) {
		// Shaders only set bits: the bitmaps must start clear.
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
			// The guest may have unmapped the range since the GPU accessed it: an unmap does not
			// wait for this completion.
			if (m_scheduler.Context().IsMapped(start, end - start)) {
				m_buffer_cache.CacheFaultedMemory(start, end - start);
			}
		});
	});
	ProcessHackMarks();
}

void FaultManager::ProcessHackMarks() {
	// Never waits: while the last read is pending the marks stay in the buffer and add up.
	if (m_marks_tick != 0 || ++m_marks_countdown < HackMarksPeriod) {
		return;
	}
	m_marks_countdown     = 0;
	constexpr auto offset = BufferCache::HACK_MARKS_OFFSET;
	constexpr auto size   = BufferCache::HACK_MARKS_SIZE;

	std::array<vk::BufferMemoryBarrier2, 2> barriers {};
	barriers[0].srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	barriers[0].srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	barriers[0].dstStageMask  = vk::PipelineStageFlagBits2::eTransfer;
	barriers[0].dstAccessMask = vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite;
	barriers[0].buffer        = m_fault_buffer.Handle();
	barriers[0].offset        = offset;
	barriers[0].size          = size;
	vk::DependencyInfo dependency {};
	dependency.bufferMemoryBarrierCount = 1;
	dependency.pBufferMemoryBarriers    = barriers.data();

	m_scheduler.EndRendering();
	auto command = m_scheduler.Current().Handle();
	command.pipelineBarrier2(dependency);
	const vk::BufferCopy region {offset, 0, size};
	command.copyBuffer(m_fault_buffer.Handle(), m_marks_download.Handle(), 1, &region);
	// The clear after the copy's read.
	barriers[0].srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
	barriers[0].srcAccessMask = vk::AccessFlagBits2::eNone;
	barriers[0].dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
	command.pipelineBarrier2(dependency);
	command.fillBuffer(m_fault_buffer.Handle(), offset, size, 0);
	barriers[0].srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
	barriers[0].dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	barriers[0].dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
	barriers[1]               = barriers[0];
	barriers[1].dstStageMask  = vk::PipelineStageFlagBits2::eHost;
	barriers[1].dstAccessMask = vk::AccessFlagBits2::eHostRead;
	barriers[1].buffer        = m_marks_download.Handle();
	barriers[1].offset        = 0;
	dependency.bufferMemoryBarrierCount = 2;
	command.pipelineBarrier2(dependency);

	m_scheduler.DeferHostOperation([this] {
		m_marks_download.Invalidate(0, BufferCache::HACK_MARKS_SIZE);
		const auto* words = std::bit_cast<const uint32_t*>(m_marks_download.Mapped().data());
		// The marks name their shader: not the one of the draw that runs this completion.
		Common::HackNotice::ScopedShader no_shader(0);
		for (uint32_t entry = 0; entry < BufferCache::HACK_MARK_ENTRIES; entry++) {
			const auto* mark = words + uint64_t {entry} * BufferCache::HACK_MARK_DWORDS;
			// Two shaders that marked the entry leave a bit set in a dword and its complement.
			const bool alone = (mark[0] ^ mark[2]) == ~0u && (mark[1] ^ mark[3]) == ~0u;
			const auto hash  = alone ? (uint64_t {mark[1]} << 32u) | mark[0] : 0;
			if (const auto rays = mark[BufferCache::HACK_MARK_GUEST_RAYS]; rays != 0) {
				NativeRayTracing::NoteGuestRays(hash, rays);
			}
			for (size_t kind = 0; kind < HackMarkNotices.size(); kind++) {
				const auto count = mark[BufferCache::HACK_MARK_COUNTS + kind];
				if (count == 0) {
					continue;
				}
				const auto& notice = HackMarkNotices[kind];
				Common::HackNotice::Report(notice.hack, notice.site, hash, 0,
				                           fmt::format("{} {}{}", count, notice.what,
				                                       alone ? "" : " (several shaders in the entry)"));
			}
		}
		m_marks_tick = 0;
	});
	m_marks_tick = m_scheduler.CurrentTick();
}

void FaultManager::ProcessWriteBuffer(PagesHandler&& handler) {
	Process(m_writes, BufferCache::WRITE_BITMAP_OFFSET, std::move(handler));
}

void FaultManager::ProcessUseBuffer(PagesHandler&& handler) {
	Process(m_uses, BufferCache::USE_BITMAP_OFFSET, std::move(handler));
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

	constexpr auto bitmap_size = BufferCache::CACHING_NUMPAGES / 8;
	vk::BufferMemoryBarrier2 pre_barrier {};
	pre_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	pre_barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	pre_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	pre_barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
	pre_barrier.buffer        = m_fault_buffer.Handle();
	pre_barrier.offset        = bitmap_offset;
	pre_barrier.size          = bitmap_size;
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
	const auto num_threads    = BufferCache::CACHING_NUMPAGES / 128; // Four words per invocation.
	const auto num_workgroups = (num_threads + 63) / 64;
	command.dispatch(static_cast<uint32_t>(num_workgroups), 1, 1);
	dependency.pBufferMemoryBarriers = &post_barrier;
	command.pipelineBarrier2(dependency);

	const auto area = reader.current;
	// The write scan marks the pages the GPU wrote as GPU-owned: an unmap waits for it, or a remap
	// of the range would inherit the marks. The fault scan only caches pages still mapped.
	Common::UniqueFunction<void> scan = [&reader, mapped, offset, area, area_size,
	                                     handler = std::move(handler)] {
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
	};
	if (&reader == &m_writes) {
		m_scheduler.DeferOperation(std::move(scan));
	} else {
		m_scheduler.DeferHostOperation(std::move(scan));
	}

	reader.ticks[reader.current++] = m_scheduler.CurrentTick();
	reader.current %= MaxPendingFaults;
}

} // namespace Libs::Graphics
