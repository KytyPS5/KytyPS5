#include "graphics/host_gpu/renderer/cache/bufferCache.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/refusalReport.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {

constexpr uint64_t MiB           = 1024 * 1024;
constexpr uint64_t GdsBufferSize = 64 * 1024;
constexpr uint64_t MaxFaultMappingSize = 16 * MiB;

RefusalReporter g_image_backing_reports;

void ReportImageBackingSubstitute(uint64_t vaddr, uint64_t size, const char* substitute) {
	const auto occurrences = g_image_backing_reports.Observe(vaddr, size, 0);
	if (occurrences == 0) {
		return;
	}
	char repeat[32] = "";
	LOGF("BufferCache: image backing unavailable addr=0x%016" PRIx64 " size=0x%016" PRIx64
	     " bound=%s%s\n",
	     vaddr, size, substitute, RefusalReporter::RepeatSuffix(repeat, occurrences));
}

// Walks the tracked set, which is far smaller than the mapped set.
template <typename Mark>
void ForEachBdaMarkRange(const RangeSet& tracked, const RangeSet& mapped, Mark&& mark) {
	tracked.ForEach([&](uint64_t begin, uint64_t end) {
		mapped.ForEachInRange(begin, end - begin,
		                      [&](uint64_t start, uint64_t finish) { mark(start, finish); });
	});
}


std::pair<Buffer*, uint64_t> UploadMemory(GraphicContext& graphics, CommandScheduler& scheduler,
                                        StreamBuffer& staging, uint64_t size, uint64_t alignment,
                                        vk::BufferUsageFlags usage, const auto& write) {
	const auto [mapped, offset] = staging.Map(size, alignment);
	if (mapped != nullptr) {
		write(mapped, offset);
		staging.Commit();
		return {&staging, offset};
	}

	// Large transfers use the same one-time host allocation as ordinary buffer uploads.
	auto temporary = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Upload, 0,
	                                         usage, size);
	write(temporary->Mapped().data(), 0);
	temporary->Flush(0, size);
	auto* buffer = temporary.get();
	scheduler.DeferOperation([owner = std::move(temporary)]() mutable { owner.reset(); });
	return {buffer, 0};
}

} // namespace

void BufferCache::WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source,
                                  uint64_t size) {
	auto* bytes = static_cast<const uint8_t*>(source);
	while (size != 0) {
		StreamHold hold(m_staging_buffer);
		const auto chunk  = std::min(size, m_staging_buffer.Size());
		// Copy waits for ring reuse and flushes the fresh host data before submission.
		const auto offset = m_staging_buffer.Copy(bytes, chunk, 4);
		const auto destination_offset = buffer.Offset(address);
		EXIT_IF(destination_offset > buffer.Size() || chunk > buffer.Size() - destination_offset);
		m_scheduler.EndRendering();
		const auto command = m_scheduler.Current().Handle();
		vk::BufferMemoryBarrier2 before {};
		before.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		before.srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		before.dstStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		before.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer        = buffer.Handle();
		before.offset        = destination_offset;
		before.size          = chunk;
		vk::DependencyInfo dependency {};
		dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
		dependency.bufferMemoryBarrierCount = 1;
		dependency.pBufferMemoryBarriers    = &before;
		command.pipelineBarrier2(dependency);
		const vk::BufferCopy copy {offset, destination_offset, chunk};
		command.copyBuffer(m_staging_buffer.Handle(), buffer.Handle(), 1, &copy);
		auto after          = before;
		after.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		after.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
		after.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		after.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		dependency.pBufferMemoryBarriers = &after;
		command.pipelineBarrier2(dependency);
		bytes += chunk;
		address += chunk;
		size -= chunk;
	}
}

void BufferCache::Register(BufferId id) {
	ChangeRegister<true>(id);
}

void BufferCache::Unregister(BufferId id) {
	ChangeRegister<false>(id);
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId id) {
	auto& buffer = m_slot_buffers[id];
	PageTable::PageRange pages {};
	EXIT_IF(!(GuestRange {buffer.CpuAddress(), buffer.Size()}.Valid()) ||
	        !PageTable::TryGetPageRange(buffer.CpuAddress(), buffer.Size(), pages));
	for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
		if constexpr (insert) {
			m_page_table[page] = id;
		} else {
			m_page_table[page] = {};
		}
	}
	const auto size_pages = pages.last_exclusive - pages.first;
	const auto table_offset = PageIndex(buffer.CpuAddress()) * sizeof(vk::DeviceAddress);
	if constexpr (insert) {
		const auto [it, inserted] = m_buffers.emplace(buffer.CpuAddress(), id);
		(void)it;
		EXIT_IF(!inserted);
		m_total_used_memory += buffer.Size();
		buffer.lru_id = m_lru_cache.Insert(id, m_gc_tick);
		std::vector<vk::DeviceAddress> addresses;
		addresses.reserve(size_pages);
		for (uint64_t i = 0; i < size_pages; ++i) {
			addresses.push_back(buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS));
		}
		m_bda_tracked_ranges.ForEachInRange(
		    buffer.CpuAddress(), buffer.Size(), [&](uint64_t begin, uint64_t end) {
			    for (auto page = begin; page < end; page += CACHING_PAGESIZE) {
				    addresses[(page - buffer.CpuAddress()) >> CACHING_PAGEBITS] |=
				        BDA_STORE_TRACKED_BIT;
			    }
		    });
		MakePageTableResident(table_offset, addresses.size() * sizeof(vk::DeviceAddress));
		WriteDataBuffer(m_bda_pagetable_buffer, table_offset, addresses.data(),
		                addresses.size() * sizeof(vk::DeviceAddress));
		// Queue after the registration, so a drain racing this sees the buffer it must fill.
		m_memory_tracker.QueueBdaSync(buffer.CpuAddress(), buffer.Size());
	} else {
		const auto found = m_buffers.find(buffer.CpuAddress());
		EXIT_IF(found == m_buffers.end() || found->second != id);
		m_buffers.erase(found);
		EXIT_IF(buffer.Size() > m_total_used_memory);
		m_total_used_memory -= buffer.Size();
		m_lru_cache.Free(buffer.lru_id);
		m_bda_pagetable_buffer.Fill(table_offset,
		                            size_pages * sizeof(vk::DeviceAddress), 0);
		buffer.is_deleted = true;
		// Whatever covers these pages next inherits them, and they may still be CPU-dirty.
		m_memory_tracker.QueueBdaSync(buffer.CpuAddress(), buffer.Size());
	}
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
void BufferCache::MakePageTableResident(uint64_t table_offset, uint64_t size) {
	if (!m_bda_pagetable_buffer.IsSparse() || size == 0) {
		return;
	}
	const auto first = table_offset / m_bda_table_chunk;
	const auto last  = (table_offset + size - 1) / m_bda_table_chunk;
	EXIT_IF(last >= m_bda_table_chunks.size());
	std::vector<size_t> fresh;
	for (auto chunk = first; chunk <= last; chunk++) {
		if (m_bda_table_chunks[chunk] == nullptr) {
			fresh.push_back(static_cast<size_t>(chunk));
		}
	}
	if (fresh.empty()) {
		return;
	}
	VmaAllocationCreateInfo create {};
	create.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	VkMemoryRequirements requirements {};
	requirements.size           = m_bda_table_chunk;
	requirements.alignment      = m_bda_table_chunk;
	requirements.memoryTypeBits = m_bda_table_memory_types;
	std::vector<vk::SparseMemoryBind> binds;
	std::vector<vk::Buffer>           zeroing;
	binds.reserve(fresh.size());
	zeroing.reserve(fresh.size());
	for (const auto chunk: fresh) {
		VmaAllocation     allocation = nullptr;
		VmaAllocationInfo info {};
		EXIT_NOT_IMPLEMENTED(vmaAllocateMemory(m_graphics.allocator, &requirements, &create,
		                                       &allocation, &info) != VK_SUCCESS);
		m_bda_table_chunks[chunk] = allocation;
		vk::SparseMemoryBind bind {};
		bind.resourceOffset = chunk * m_bda_table_chunk;
		bind.size           = m_bda_table_chunk;
		bind.memory         = info.deviceMemory;
		bind.memoryOffset   = info.offset;
		binds.push_back(bind);
		VkBufferCreateInfo alias_info {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		alias_info.size  = m_bda_table_chunk;
		alias_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		VkBuffer alias   = VK_NULL_HANDLE;
		EXIT_NOT_IMPLEMENTED(vmaCreateAliasingBuffer(m_graphics.allocator, allocation, &alias_info,
		                                             &alias) != VK_SUCCESS);
		zeroing.push_back(alias);
	}
	m_scheduler.RunDetached(0, [&](vk::CommandBuffer command) {
		for (const auto alias: zeroing) {
			command.fillBuffer(alias, 0, VK_WHOLE_SIZE, 0);
		}
		vk::MemoryBarrier barrier {};
		barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                        vk::PipelineStageFlagBits::eAllCommands, {}, 1, &barrier, 0,
		                        nullptr, 0, nullptr);
	});
	for (const auto alias: zeroing) {
		m_graphics.device.destroyBuffer(alias, nullptr);
	}
	vk::SparseBufferMemoryBindInfo buffer_bind {};
	buffer_bind.buffer    = m_bda_pagetable_buffer.Handle();
	buffer_bind.bindCount = static_cast<uint32_t>(binds.size());
	buffer_bind.pBinds    = binds.data();
	vk::BindSparseInfo bind_info {};
	bind_info.bufferBindCount = 1;
	bind_info.pBufferBinds    = &buffer_bind;
	const auto queue =
	    m_graphics.readback_queue != nullptr ? m_graphics.readback_queue : m_graphics.queue;
	vk::Result result;
	{
		Common::LockGuard lock(m_graphics.queue_mutex);
		result = queue.bindSparse(1, &bind_info, m_bda_table_fence);
	}
	if (result == vk::Result::eSuccess) {
		result = m_graphics.device.waitForFences(1, &m_bda_table_fence, VK_TRUE, UINT64_MAX);
	}
	EXIT_IF(result != vk::Result::eSuccess);
	EXIT_IF(m_graphics.device.resetFences(1, &m_bda_table_fence) != vk::Result::eSuccess);
}

	if (!buffer.is_deleted) {
		m_lru_cache.Touch(buffer.lru_id, m_gc_tick);
	}
}

void BufferCache::DeleteBuffer(BufferId id) {
	if (IsBufferInvalid(id)) {
		return;
	}
	Unregister(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_buffers.erase(id); });
	} else {
		m_slot_buffers.erase(id);
	}
}

template <bool async>
bool BufferCache::DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size     = 0;
	const auto                  buffer_address = buffer.CpuAddress();
	m_memory_tracker.ForEachDownloadRange<false>(
	    vaddr, size, [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "buffer download");
		    m_gpu_modified_ranges.ForEachInRange(address, bytes, [&](uint64_t start, uint64_t end) {
			    copies.emplace_back(start - buffer_address, total_size, end - start);
			    // Keep packed ranges on separate cache lines, as in shadPS4.
			    total_size += Common::AlignUp(end - start, 64);
		    });
		    m_gpu_modified_ranges.Subtract(address, bytes);
		    ForgetColorMetadataChecks(address, bytes);
	    });
	if (copies.empty()) {
		return false;
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferDownloadBytes, total_size);

	std::optional<StreamHold> hold(std::in_place, m_download_buffer);
	auto [mapped, offset] = m_download_buffer.Map(total_size, 64);
	std::unique_ptr<Buffer> temporary;
	if (mapped == nullptr) {
		temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Download, 0,
		                                     vk::BufferUsageFlagBits::eTransferDst, total_size);
		mapped = temporary->Mapped().data();
	} else {
		m_download_buffer.Commit();
	}
	const auto& download = temporary ? *temporary : m_download_buffer;
	for (auto& copy: copies) {
		copy.dstOffset += offset;
	}

	auto& command = m_scheduler.Current();
	command.EndRendering();
	const auto              native = command.Handle();
	vk::BufferMemoryBarrier before {};
	before.srcAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.buffer              = buffer.Handle();
	before.offset              = 0;
	before.size                = buffer.Size();
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &before, 0,
	                       nullptr);
	native.copyBuffer(buffer.Handle(), download.Handle(),
	                  static_cast<uint32_t>(copies.size()), copies.data());

	auto after          = before;
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eHostRead;
	after.buffer        = download.Handle();
	after.offset        = offset;
	after.size          = total_size;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands |
	                           vk::PipelineStageFlagBits::eHost,
	                       {}, 0, nullptr, 1, &after, 0, nullptr);
	// The copy consuming the reservation is recorded; a synchronous wait must not carry it on.
	hold.reset();
	uint64_t write_begin = UINT64_MAX;
	uint64_t write_end   = 0;
	for (const auto& copy: copies) {
		write_begin = std::min(write_begin, copy.srcOffset);
		write_end   = std::max(write_end, copy.srcOffset + copy.size);
	}
	auto publish = [this, mapped, offset, total_size, buffer_address, copies = std::move(copies),
	                owner = std::move(temporary)] {
		(owner ? *owner : m_download_buffer).Invalidate(offset, total_size);
		for (const auto& copy: copies) {
			Libs::LibKernel::Memory::WriteBacking(buffer_address + copy.srcOffset,
			                                      mapped + (copy.dstOffset - offset), copy.size);
		}
	};
	if constexpr (async) {
		m_scheduler.DeferPriorityOperation(std::move(publish), buffer_address + write_begin,
		                                   write_end - write_begin);
	} else {
		const auto tick = m_scheduler.CurrentTick();
		m_scheduler.Wait(tick);
		m_scheduler.WaitPriorityOperations(tick);
		publish();
	}
	return true;
}

void BufferCache::MarkGpuWrite(uint64_t vaddr, uint64_t size) {
	if (size == 0) {
		return;
	}
	const auto tick = m_scheduler.CurrentTick();
	auto       it   = m_gpu_write_marks.lower_bound(vaddr);
	// A binding re-marked with its previous extent, the common case, only moves its tick.
	if (it != m_gpu_write_marks.end() && it->first == vaddr && it->second.end == end) {
		it->second = {end, tick};
		return;
	}
	if (it != m_gpu_write_marks.begin()) {
		auto& previous = std::prev(it)->second;
		if (previous.end > vaddr) {
			const auto tail = previous;
			previous.end    = vaddr;
			if (tail.end > end) {
				m_gpu_write_marks.emplace_hint(it, end, tail);
			}
		}
	}
	while (it != m_gpu_write_marks.end() && it->first < end) {
		if (it->second.end > end) {
			auto node  = m_gpu_write_marks.extract(it);
			node.key() = end;
			m_gpu_write_marks.insert(std::move(node));
			break;
		}
		it = m_gpu_write_marks.erase(it);
	}
	m_gpu_write_marks.emplace(vaddr, GpuWriteMark {end, tick});
}

uint64_t BufferCache::NewestGpuWriteTick(uint64_t vaddr, uint64_t size) const {
	const auto end    = vaddr + size;
	auto       it     = m_gpu_write_marks.upper_bound(vaddr);
	uint64_t   newest = 0;
	if (it != m_gpu_write_marks.begin()) {
		--it;
	}
	for (; it != m_gpu_write_marks.end() && it->first < end; ++it) {
		if (it->second.end > vaddr) {
			newest = std::max(newest, it->second.tick);
		}
	}
	return newest;
}

void BufferCache::PruneGpuWriteMarks() {
	const auto completed = m_scheduler.GetMasterSemaphore().KnownGpuTick();
	std::erase_if(m_gpu_write_marks,
	              [completed](const auto& entry) { return entry.second.tick <= completed; });
}

bool BufferCache::TryDownloadDetached(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	const auto                  buffer_address = buffer.CpuAddress();
	std::vector<vk::BufferCopy> copies;
	RangeSet                    pages;
	m_memory_tracker.ForEachDownloadRange<false>(
	    vaddr, size, [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "buffer download");
		    pages.Add(address, bytes);
		    m_gpu_modified_ranges.ForEachInRange(address, bytes, [&](uint64_t start, uint64_t end) {
			    copies.emplace_back(start - buffer_address, 0, end - start);
		    });
	    });
	if (copies.empty()) {
		return false;
	}
	uint64_t total_size  = 0;
	uint64_t newest_tick = 0;
	for (auto& copy: copies) {
		copy.dstOffset = total_size;
		total_size += Common::AlignUp(copy.size, 64);
		newest_tick =
		    std::max(newest_tick, NewestGpuWriteTick(buffer_address + copy.srcOffset, copy.size));
	}

	const auto current = m_scheduler.CurrentTick();
	// Only a semaphore wait makes a completed writer's stores visible to the copy.
	const auto wait = std::max(newest_tick, m_scheduler.GetMasterSemaphore().KnownGpuTick());
	// The recording in progress wrote it, or a BDA store shader in it may have.
	if (newest_tick >= current || m_bda_store_tick >= current) {
		return false;
	}
	// Queued write-backs publish in order; one still waiting for a later tick would land on top.
	if (m_scheduler.PendingGuestWriteTick(vaddr, size) > wait) {
		return false;
	}
	StreamHold hold(m_download_buffer);
	const auto [mapped, offset] = m_download_buffer.Map(total_size, 64, false);
	if (mapped == nullptr) {
		return false;
	}
	m_download_buffer.Commit();
	for (auto& copy: copies) {
		copy.dstOffset += offset;
	}
	const auto source = buffer.Handle();
	const auto target = m_download_buffer.Handle();
	m_scheduler.RunDetached(wait, [&](vk::CommandBuffer command) {
		command.copyBuffer(source, target, static_cast<uint32_t>(copies.size()), copies.data());
		vk::BufferMemoryBarrier after {};
		after.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
		after.dstAccessMask       = vk::AccessFlagBits::eHostRead;
		after.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		after.buffer              = target;
		after.offset              = offset;
		after.size                = total_size;
		command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                        vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1, &after, 0,
		                        nullptr);
	});
	// Write-backs queued up to the writer's tick come first, exactly as in the drain.
	m_scheduler.WaitPriorityOperations(wait);
	m_download_buffer.Invalidate(offset, total_size);
	for (const auto& copy: copies) {
		Libs::LibKernel::Memory::WriteBacking(buffer_address + copy.srcOffset,
		                                      mapped + (copy.dstOffset - offset), copy.size);
	}
	pages.ForEach([this](uint64_t begin, uint64_t end) {
		m_gpu_modified_ranges.Subtract(begin, end - begin);
		ForgetColorMetadataChecks(begin, end - begin);
	});
	return true;
}

BufferCache::BufferCache(GraphicContext& graphics, CommandScheduler& scheduler,
                         PageManager& page_manager, TextureCache& texture_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_fault_manager(graphics, scheduler, *this),
      m_gds_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, GdsBufferSize),
      m_bda_pagetable_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                             BDA_PAGETABLE_SIZE, graphics.sparse_residency_buffer_enabled),
      m_memory_tracker(page_manager),
      m_staging_buffer(graphics, scheduler, MemoryUsage::Upload, 512 * MiB),
      m_stream_buffer(graphics, scheduler, MemoryUsage::Stream, 64 * MiB),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 64 * MiB),
      m_device_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 128 * MiB),
      m_texture_cache(texture_cache) {
	std::memset(m_gds_buffer.Mapped().data(), 0, static_cast<size_t>(m_gds_buffer.Size()));
	m_gds_buffer.Flush(0, m_gds_buffer.Size());
	SetVulkanObjectNameF(m_graphics.device, m_bda_pagetable_buffer.Handle(),
	                     "BDA Page Table Buffer");
	if (m_bda_pagetable_buffer.IsSparse()) {
		vk::MemoryRequirements requirements {};
		m_graphics.device.getBufferMemoryRequirements(m_bda_pagetable_buffer.Handle(),
		                                              &requirements);
		m_bda_table_chunk        = std::max<uint64_t>(requirements.alignment, 64 * 1024);
		m_bda_table_memory_types = requirements.memoryTypeBits;
		m_bda_table_chunks.assign(
		    static_cast<size_t>((BDA_PAGETABLE_SIZE + m_bda_table_chunk - 1) / m_bda_table_chunk),
		    nullptr);
		vk::FenceCreateInfo fence_info {};
		EXIT_NOT_IMPLEMENTED(m_graphics.device.createFence(
		                         &fence_info, nullptr, &m_bda_table_fence) != vk::Result::eSuccess);
	}
	m_scheduler.BeginCommand();
	for (auto* buffer: {&m_bda_pagetable_buffer, m_fault_manager.GetFaultBuffer()}) {
		// A sparse page table is zeroed chunk by chunk as MakePageTableResident binds it.
		if (!buffer->IsSparse()) {
			buffer->Fill(0, buffer->Size(), 0);
		}
	}
	const auto null_id =
	    m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
	EXIT_IF(null_id != NULL_BUFFER_ID);
	auto& null_buffer = m_slot_buffers[null_id];
	SetVulkanObjectNameF(m_graphics.device, null_buffer.Handle(), "Kyty.NullBuffer");
	// Bound in place of a missing guest buffer, so a read must see zeros.
	null_buffer.Fill(0, null_buffer.Size(), 0);
	if (!m_graphics.CanReportMemoryUsage()) {
		return;
	}
	constexpr int64_t GiB              = 1024ll * 1024 * 1024;
	constexpr int64_t target_threshold = 8 * GiB;
	const auto        budget =
	    static_cast<int64_t>(std::min<uint64_t>(m_graphics.GetTotalMemoryBudget(), INT64_MAX));
	const auto threshold = std::min(budget, target_threshold);
	const auto expected  = std::min(budget - 6 * threshold / 10, budget - GiB);
	const auto critical  = std::min(budget - 2 * threshold / 10, budget - GiB / 2);
	m_trigger_gc_memory  = static_cast<uint64_t>(std::max<int64_t>(expected, GiB));
	m_critical_gc_memory = static_cast<uint64_t>(std::max<int64_t>(critical, 2 * GiB));
}

BufferCache::~BufferCache() {
	if (m_bda_table_fence != nullptr) {
		m_graphics.device.destroyFence(m_bda_table_fence, nullptr);
	}
	for (auto& chunk: m_bda_table_chunks) {
		if (chunk != nullptr) {
			vmaFreeMemory(m_graphics.allocator, chunk);
			chunk = nullptr;
		}
	}
	if (!m_gpu_modified_ranges.Empty()) {
		EXIT("BufferCache: destroyed with pending GPU-modified ranges\n");
	}
	for (const auto& [vaddr, id]: m_buffers) {
		(void)vaddr;
		const auto& buffer = m_slot_buffers[id];
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: destroyed with GPU-modified buffer\n");
		}
	}
	m_buffers.clear();
}

void BufferCache::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid memory-invalidation range\n");
	}
	m_memory_tracker.InvalidateRegion(vaddr, size,
	                                  [this, vaddr, size] { ReadMemory(vaddr, size, true); });
}

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write) {
	if (!GuestGpu::IsGpuThread() && CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const bool    guest_access = !GuestGpu::IsGpuThread();
		    if (TryDownloadDetached(buffer, vaddr, size)) {
			    m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, size);
		    } else if (DownloadBufferMemory<false>(buffer, vaddr, size)) {
			return;
		}
		auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];

			m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, size);
		}
		if (is_write) {
			m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
		}
	});
}

BufferId BufferCache::FindBuffer(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0) {
		return NULL_BUFFER_ID;
	}
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid buffer discovery request\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			return *owner;
		}
	}
	return CreateBuffer(vaddr, size);
}

BufferCache::OverlapResult BufferCache::ResolveOverlaps(uint64_t vaddr, uint64_t size) {
	static constexpr int      StreamLeapThreshold = 16;
	static constexpr uint64_t StreamLeapSize      = CACHING_PAGESIZE * 128;

	auto       begin      = vaddr;
	auto       end        = vaddr + size;
	const auto find_first = [&](uint64_t address) {
		auto first = m_buffers.lower_bound(address);
		if (first != m_buffers.begin()) {
			const auto  previous = std::prev(first);
			const auto& buffer   = m_slot_buffers[previous->second];
			if (buffer.CpuAddress() + buffer.Size() > address) {
				first = previous;
			}
		}
		return first;
	};
	auto first           = find_first(begin);
	auto last            = first;
	int  stream_score    = 0;
	bool has_stream_leap = false;
	for (; last != m_buffers.end() && last->first < end; ++last) {
		const auto& buffer        = m_slot_buffers[last->second];
		const auto  buffer_begin  = buffer.CpuAddress();
		const auto  buffer_end    = buffer_begin + buffer.Size();
		const bool  expands_left  = buffer_begin < begin;
		const bool  expands_right = buffer_end > end;
		begin                     = std::min(begin, buffer_begin);
		end                       = std::max(end, buffer_end);
		if (!has_stream_leap && (stream_score += buffer.StreamScore()) > StreamLeapThreshold) {
			has_stream_leap = true;
			// Reserve space in the incoming stream's direction of growth.
			// The old buffer extending left of the request predicts growth to the right, and vice versa.
			if (expands_left) {
				end += std::min(StreamLeapSize, (vaddr < LOWER_ADDRESS_SIZE ? LOWER_ADDRESS_SIZE
				                                       : LibKernel::Memory::kExtendedMemoryBase +
				                                             LibKernel::Memory::kExtendedMemorySize) - end);
			}
			if (expands_right) {
				const auto minimum = vaddr < LOWER_ADDRESS_SIZE
				                         ? CACHING_PAGESIZE * 2
				                         : LibKernel::Memory::kExtendedMemoryBase;
				if (begin > minimum) {
					begin -= std::min(StreamLeapSize, begin - minimum);
				}
				first = find_first(begin);
				begin = std::min(begin, first->first);
			}
		}
	}
	return {first, last, begin, end, has_stream_leap};
}

void BufferCache::JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score) {
	auto& new_buffer = m_slot_buffers[new_id];
	auto& overlap    = m_slot_buffers[overlap_id];
	if (accumulate_stream_score) {
		new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
	}
	new_buffer.CopyFrom(m_scheduler.Current(), overlap, 0,
	                    overlap.CpuAddress() - new_buffer.CpuAddress(), overlap.Size());
	DeleteBuffer(overlap_id);
}

BufferId BufferCache::CreateBuffer(uint64_t vaddr, uint64_t size) {
	EXIT_IF(m_scheduler.Current().IsInvalid());
	const auto end = Common::AlignUp(vaddr + size, CACHING_PAGESIZE);
	vaddr = Common::AlignDown(vaddr, CACHING_PAGESIZE);
	size               = end - vaddr;
	const auto overlap = ResolveOverlaps(vaddr, size);

	const auto id = m_slot_buffers.insert(
	    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, overlap.begin,
	    AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, overlap.end - overlap.begin);
	const auto& buffer = m_slot_buffers[id];
	SetVulkanObjectNameF(m_graphics.device, buffer.Handle(),
	                     "Kyty.GameBuffer[guest=0x{:016x} size=0x{:x}]", overlap.begin,
	                     overlap.end - overlap.begin);
	for (auto it = overlap.first; it != overlap.last;) {
		const auto  old_id = (it++)->second;
		const auto& joined = m_slot_buffers[old_id];
		// The joined bytes only reach the new buffer when the recording in progress runs.
		MarkGpuWrite(joined.CpuAddress(), joined.Size());
		JoinOverlap(id, old_id, !overlap.has_stream_leap);
	}
	Register(id);
	return id;
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_written,
                                    bool is_texel_buffer) {
                                    bool is_texel_buffer, bool raw_image_read) {
	StreamHold                  hold(m_staging_buffer);
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size = 0;
	vk::Buffer                  source;
	m_memory_tracker.ForEachUploadRange(
	    vaddr, size, is_written,
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    copies.emplace_back(total_size, buffer.Offset(address), bytes);
		    total_size += bytes;
	    },
	    [&]() noexcept { source = UploadCopies(buffer, copies, total_size); });
	if (source) {
		auto& command = m_scheduler.Current();
		command.EndRendering();
		vk::BufferMemoryBarrier before {};
		before.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
		                       vk::AccessFlagBits::eTransferRead |
		                       vk::AccessFlagBits::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer              = buffer.Handle();
		before.offset              = 0;
		before.size                = buffer.Size();
		native.copyBuffer(source, buffer.Handle(), static_cast<uint32_t>(copies.size()),
		                  copies.data());
		auto after          = before;
		after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	}
	if ((is_texel_buffer || raw_image_read) && !is_written) {
		return SynchronizeBufferFromImage(buffer, vaddr, size, !is_texel_buffer);
	}
	return false;
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     uint64_t total_size) {
	if (copies.empty()) {
		return nullptr;
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferUploadBytes, total_size);

	const auto source = UploadMemory(m_graphics, m_scheduler, m_staging_buffer, total_size, 4,
	    vk::BufferUsageFlagBits::eTransferSrc, [&](uint8_t* mapped, uint64_t base_offset) {
		for (auto& copy: copies) {
			const auto address = buffer.CpuAddress() + copy.dstOffset;
			if (!LibKernel::Memory::CopySparseMemory(address, mapped + copy.srcOffset, copy.size)) {
				EXIT("BufferCache: failed to read sparse guest buffer memory\n");
			}
			copy.srcOffset += base_offset;
		}
	});
	return source.first->Handle();
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer(uint64_t vaddr, uint64_t size,
                                                       bool is_written, bool is_texel_buffer,
                                                       BufferId id, bool raw_image_read) {
	auto& command = m_scheduler.Current();
	if (command.IsInvalid() || !GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: buffer request requires a recording command buffer\n");
	}

	if (!is_written && size <= CACHING_PAGESIZE &&
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
	    m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
		const auto alignment = std::max<uint64_t>(
		    m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 1);
		auto [mapped, offset] = m_stream_buffer.Map(size, alignment, false);
		if (mapped != nullptr) {
			if (!LibKernel::Memory::CopySparseMemory(vaddr, mapped, size)) {
				EXIT("BufferCache: failed to read sparse guest buffer memory\n");
			}
			m_stream_buffer.Commit();
			return {&m_stream_buffer, offset};
		}
	}

	if (IsBufferInvalid(id) || !m_slot_buffers[id].IsInBounds(vaddr, size)) {
		id = FindBuffer(vaddr, size);
	}
	auto& buffer = m_slot_buffers[id];
	TouchBuffer(buffer);
	(void)SynchronizeBuffer(buffer, vaddr, size, is_written, is_texel_buffer, raw_image_read);
	if (is_written) {
		m_gpu_modified_ranges.Add(vaddr, size);
		ForgetColorMetadataChecks(vaddr, size);
	}
	return {&buffer, buffer.Offset(vaddr)};
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		ReportImageBackingSubstitute(vaddr, size, "nothing");
		return {nullptr, 0};
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			TouchBuffer(buffer);
			(void)SynchronizeBuffer(buffer, vaddr, size, false, false);
			return {&buffer, buffer.Offset(vaddr)};
		}
	}
	if (IsRegionGpuModified(vaddr, size)) {
		return ObtainBuffer(vaddr, size, false, false);
	}

	return UploadMemory(m_graphics, m_scheduler, m_staging_buffer, size, 16,
	    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eStorageBuffer,
	    [&](uint8_t* mapped, uint64_t) {
		if (!Libs::LibKernel::Memory::TryReadSparseBacking(vaddr, mapped, size)) {
			// A texture descriptor is guest data: it may overlap private memory or leave the arena.
			if (!Libs::LibKernel::Memory::CopySparseMemory(vaddr, mapped, size)) {
				std::memset(mapped, 0, size);
				ReportImageBackingSubstitute(vaddr, size, "zeros");
			}
		}
	});
}

void BufferCache::FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds) {
	if ((vaddr & 3u) != 0 || size == 0 || (size & 3u) != 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: fill range must be dword aligned\n");
	}
	if (is_gds) {
		if (vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - vaddr) {
			EXIT("BufferCache: GDS fill range is out of bounds\n");
		}
		m_gds_buffer.Fill(vaddr, size, value);
		return;
	}
	if (vaddr == 0) {
		EXIT("BufferCache: invalid fill memory address\n");
	}
	(void)m_texture_cache.ClearMetaSlices(vaddr, size);
	if (!IsRegionGpuModified(vaddr, size)) {
		// Access the guest mapping so write faults invalidate cached buffers and images.
		auto* destination = reinterpret_cast<uint32_t*>(vaddr);
		std::fill(destination, destination + size / sizeof(uint32_t), value);
		return;
	}

	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true);
	dst->Fill(dst_offset, size, value);
}

void BufferCache::FillBufferPattern(uint64_t vaddr, uint64_t size, const uint32_t* pattern,
                                    uint32_t words) {
	const uint64_t pattern_size = uint64_t {words} * sizeof(uint32_t);
	if (pattern == nullptr || words == 0 || words > 4 || vaddr == 0 || (vaddr & 3u) != 0 ||
	    size == 0 || size % pattern_size != 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: invalid pattern fill\n");
	}
	if (std::all_of(pattern, pattern + words, [&](uint32_t word) { return word == pattern[0]; })) {
		FillBuffer(vaddr, size, pattern[0], false);
		return;
	}
	std::vector<uint32_t> data(size / sizeof(uint32_t));
	for (size_t i = 0; i < data.size(); i++) {
		data[i] = pattern[i % words];
	}
	WriteMemory(vaddr, data.data(), size);
}

void BufferCache::WriteMemory(uint64_t vaddr, const void* data, uint64_t size) {
	if (data == nullptr || vaddr == 0 || size == 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: invalid memory write\n");
	}
	if (!GuestRange {vaddr, size}.Valid() || !IsRegionGpuModified(vaddr, size)) {
		// Access the guest mapping so write faults invalidate cached buffers and images.
		if (((vaddr | size) & 3u) != 0) {
			std::memcpy(reinterpret_cast<void*>(vaddr), data, size);
			return;
		}
		// Completion labels can be reused immediately; memcpy may write a destination twice.
		auto*       output = reinterpret_cast<volatile uint32_t*>(vaddr);
		const auto* input  = static_cast<const uint8_t*>(data);
		for (uint64_t offset = 0; offset < size; offset += sizeof(uint32_t)) {
			uint32_t word = 0;
			std::memcpy(&word, input + offset, sizeof(word));
			*output++ = word;
		}
		return;
	}
	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true);
	(void)dst_offset;
	WriteDataBuffer(*dst, vaddr, data, size);
}

void BufferCache::CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
                             bool src_gds) {
	const bool dst_memory = !dst_gds;
	const bool src_memory = !src_gds;
	if ((dst_memory && dst_vaddr == 0) || (src_memory && src_vaddr == 0) || size == 0 ||
	    ((dst_gds || src_gds) && ((dst_vaddr | src_vaddr | size) & 3u) != 0) ||
	    size > UINT64_MAX - dst_vaddr || size > UINT64_MAX - src_vaddr || (dst_gds && src_gds) ||
	    (dst_gds && (dst_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - dst_vaddr)) ||
	    (src_gds && (src_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - src_vaddr))) {
		EXIT("BufferCache: invalid copy range, src=0x%016" PRIx64 " dst=0x%016" PRIx64
		     " size=0x%016" PRIx64 " src_gds=%d dst_gds=%d\n",
		     src_vaddr, dst_vaddr, size, static_cast<int>(src_gds), static_cast<int>(dst_gds));
	}
	if (src_memory && dst_memory && !IsRegionGpuModified(dst_vaddr, size) &&
	    !IsRegionGpuModified(src_vaddr, size) && !m_texture_cache.FindImageFromRange(src_vaddr, size)) {
		std::memcpy(reinterpret_cast<void*>(dst_vaddr), reinterpret_cast<const void*>(src_vaddr),
		            size);
		return;
	}

	auto& command = m_scheduler.Current();
	if (dst_memory) {
		m_texture_cache.InvalidateMemoryFromGPU(dst_vaddr, size);
	}
	StreamHold hold(m_stream_buffer);
	const auto src_id      = src_memory ? FindBuffer(src_vaddr, size) : BufferId {};
	const auto dst_id      = dst_memory ? FindBuffer(dst_vaddr, size) : BufferId {};
	auto [src, src_offset] = src_memory ? ObtainBuffer(src_vaddr, size, false, true, src_id)
	                                    : std::pair {&m_gds_buffer, src_vaddr};
	auto [dst, dst_offset] = dst_memory ? ObtainBuffer(dst_vaddr, size, true, true, dst_id)
	                                    : std::pair {&m_gds_buffer, dst_vaddr};
	dst->CopyFrom(command, *src, src_offset, dst_offset, size);
}

bool BufferCache::IsRegionRegistered(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid registered-region query\n");
	}
	// Cached buffers are ordered and non-overlapping. The last buffer beginning before the query
	// end is therefore the only possible intersection.
	const auto candidate = m_buffers.lower_bound(vaddr + size);
	if (candidate == m_buffers.begin()) {
		return false;
	}
	const auto& [address, id] = *std::prev(candidate);
	return address + m_slot_buffers[id].Size() > vaddr;
}

void BufferCache::MarkColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys) {
	for (uint32_t slot = 0; slot < m_checked_color_metadata.size(); slot++) {
		if ((keys & (1u << slot)) != 0) {
			m_checked_color_metadata[slot].Add(vaddr, size);
		}
	}
}

bool BufferCache::IsColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys) const {
	for (uint32_t slot = 0; slot < m_checked_color_metadata.size(); slot++) {
		if ((keys & (1u << slot)) != 0 && !m_checked_color_metadata[slot].Contains(vaddr, size)) {
			return false;
		}
	}
	return true;
}

void BufferCache::ForgetColorMetadataChecks(uint64_t vaddr, uint64_t size) {
	for (auto& checked: m_checked_color_metadata) {
		checked.Subtract(vaddr, size);
	}
}

bool BufferCache::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionGpuModified(vaddr, size);
}

bool BufferCache::HasGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	return m_gpu_modified_ranges.Intersects(vaddr, size);
}

bool BufferCache::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionCpuModified(vaddr, size);
}

void BufferCache::RunGarbageCollector() {
	PruneGpuWriteMarks();
	const auto tick = m_gc_tick++;
	if (m_graphics.CanReportMemoryUsage()) {
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
	}
	if (m_total_used_memory < m_trigger_gc_memory) {
		return;
	}

	const bool     aggressive = m_total_used_memory >= m_critical_gc_memory;
	const uint64_t age        = std::min<uint64_t>(aggressive ? 80 : 160, tick);
	const size_t   limit      = aggressive ? 64 : 32;

	std::vector<BufferId> dirty_buffers;
	size_t                retire_count = 0;
	m_lru_cache.ForEachItemBelow(tick - age, [&](BufferId id) {
		auto& buffer = m_slot_buffers[id];
		EXIT_IF(buffer.is_deleted);
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, buffer.CpuAddress(),
		                                           buffer.Size(), "garbage collection");
		if (m_bda_pinned_ranges.Intersects(buffer.CpuAddress(), buffer.Size())) {
			TouchBuffer(buffer);
			return false;
		}
		const bool dirty = m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size());
		if (dirty && !aggressive) {
			return false;
		}
		if (dirty) {
			EXIT_NOT_IMPLEMENTED(
			    !DownloadBufferMemory<true>(buffer, buffer.CpuAddress(), buffer.Size()));
			dirty_buffers.push_back(id);
		} else {
			m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
			DeleteBuffer(id);
			Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferEvictions);
		}
		return ++retire_count == limit;
	});
	if (dirty_buffers.empty()) {
		return;
	}

	// Publish all queued downloads before releasing their tracked pages and owners.
	const auto completion_tick = m_scheduler.CurrentTick();
	m_scheduler.Wait(completion_tick);
	m_scheduler.WaitPriorityOperations(completion_tick);
	for (const auto id: dirty_buffers) {
		auto& buffer = m_slot_buffers[id];
		m_memory_tracker.UnmarkRegionAsGpuModified(buffer.CpuAddress(), buffer.Size());
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size()) ||
		    m_gpu_modified_ranges.Intersects(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: garbage collection retained GPU ownership\n");
		}
		m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
		Unregister(id);
		m_slot_buffers.erase(id);
	}
}

void BufferCache::ProcessFaultBuffer() {
	m_fault_manager.ProcessFaultBuffer();
}

void BufferCache::ResolveBdaFault(uint64_t vaddr, uint64_t size) {
	const auto aligned_begin = vaddr & ~(CACHING_PAGESIZE - 1);
	const auto aligned_end   = (vaddr + size + CACHING_PAGESIZE - 1) & ~(CACHING_PAGESIZE - 1);
	vaddr                    = aligned_begin;
	size                     = aligned_end - aligned_begin;
	if (size == 0 || !GuestRange {vaddr, size}.Valid()) {
		// A page that really faulted faults again on the next pass.
		return;
	}
	RangeSet stored;
	RangeSet missing;
	for (auto page = vaddr; page < vaddr + size; page += CACHING_PAGESIZE) {
		const auto* owner = m_page_table.Find(page >> PageTable::kPageBits);
		(owner != nullptr && *owner ? stored : missing).Add(page, CACHING_PAGESIZE);
	}
	// A pointer walk over a partly resident mapping can follow a zero link forever.
	RangeSet widened;
	missing.ForEach([&](uint64_t begin, uint64_t end) {
		widened.Add(begin, end - begin);
		uint64_t start  = 0;
		uint64_t length = 0;
		if (!Libs::LibKernel::Memory::QueryCommittedRange(begin, &start, &length) ||
		    length > MaxFaultMappingSize) {
			return;
		}
		const auto first = start & ~(CACHING_PAGESIZE - 1);
		const auto last  = (start + length + CACHING_PAGESIZE - 1) & ~(CACHING_PAGESIZE - 1);
		if (!GuestRange {first, last - first}.Valid()) {
			return;
		}
		for (auto page = first; page < last; page += CACHING_PAGESIZE) {
			const auto* owner = m_page_table.Find(page >> PageTable::kPageBits);
			if (owner == nullptr || !*owner) {
				widened.Add(page, CACHING_PAGESIZE);
			}
		}
	});
	missing = std::move(widened);
	stored.ForEach([this](uint64_t begin, uint64_t end) {
		m_bda_tracked_ranges.Add(begin, end - begin);
		m_bda_unmarked_ranges.Add(begin, end - begin);
	});
	missing.ForEach([this](uint64_t begin, uint64_t end) {
		m_bda_pinned_ranges.Add(begin, end - begin);
		(void)FindBuffer(begin, end - begin);
	});
	stored.ForEach([this](uint64_t begin, uint64_t end) {
		for (auto page = begin; page < end; page += CACHING_PAGESIZE) {
			const auto* owner = m_page_table.Find(page >> PageTable::kPageBits);
			if (owner == nullptr || !*owner) {
				continue;
			}
			const auto&             buffer = m_slot_buffers[*owner];
			const vk::DeviceAddress entry =
			    (buffer.BufferDeviceAddress() + (page - buffer.CpuAddress())) |
			    BDA_STORE_TRACKED_BIT;
			WriteDataBuffer(m_bda_pagetable_buffer, PageIndex(page) * sizeof(vk::DeviceAddress),
			                &entry, sizeof(entry));
		}
	});
}

	const auto begin = vaddr & ~(CACHING_PAGESIZE - 1);
	const auto end   = (vaddr + size + CACHING_PAGESIZE - 1) & ~(CACHING_PAGESIZE - 1);
	if (begin >= end || !GuestRange {begin, end - begin}.Valid()) {
		return;
	}
	RangeSet missing;
	for (auto page = begin; page < end; page += CACHING_PAGESIZE) {
		const auto* owner = m_page_table.Find(page >> PageTable::kPageBits);
		if (owner == nullptr || !*owner) {
			missing.Add(page, CACHING_PAGESIZE);
		} else {
			TouchBuffer(m_slot_buffers[*owner]);
		}
	}
	missing.ForEach(
	    [this](uint64_t first, uint64_t last) { (void)FindBuffer(first, last - first); });
	if (!store) {
		return;
	}
	for (auto page = begin; page < tracked_end; page += CACHING_PAGESIZE) {
		if (!m_bda_tracked_ranges.Contains(page, CACHING_PAGESIZE)) {
			ResolveBdaFault(page, CACHING_PAGESIZE);
		}
	}
}

void BufferCache::ForgetBdaResidency(uint64_t vaddr, uint64_t size) {
	m_bda_pinned_ranges.Subtract(vaddr, size);
}

void BufferCache::MarkBdaStoresInMapped(const RangeSet& mapped, bool all_tracked) {
	if (all_tracked) {
		m_bda_store_tick = m_scheduler.CurrentTick();
	}
	const auto& ranges = all_tracked ? m_bda_tracked_ranges : m_bda_unmarked_ranges;
	ForEachBdaMarkRange(ranges, mapped, [this](uint64_t begin, uint64_t end) {
		MarkBdaStores(begin, end - begin);
	});
}

void BufferCache::MarkBdaStores(uint64_t vaddr, uint64_t size) {
	const auto end = vaddr + size;
	auto       it  = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		--it;
	}
	for (; it != m_buffers.end() && it->first < end; ++it) {
		auto&      buffer = m_slot_buffers[it->second];
		const auto start  = std::max(buffer.CpuAddress(), vaddr);
		const auto finish = std::min(buffer.CpuAddress() + buffer.Size(), end);
		if (start >= finish) {
			continue;
		}
		TouchBuffer(buffer);
		(void)SynchronizeBuffer(buffer, start, finish - start, true, false);
		m_gpu_modified_ranges.Add(start, finish - start);
		MarkGpuWrite(start, finish - start);
		ForgetColorMetadataChecks(start, finish - start);
	}
}

void BufferCache::SynchronizeBda(const RangeSet& mapped) {
	// Take before walking: writes queued during synchronization remain pending.
	const auto pending = m_memory_tracker.TakeBdaSync();
	pending.ForEach([&](uint64_t begin, uint64_t end) {
		mapped.ForEachInRange(begin, end - begin, [this](uint64_t start, uint64_t finish) {
			SynchronizeBuffersInRange(start, finish - start);
		});
	});
}

void BufferCache::SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size) {
	const auto end = vaddr + size;
	auto       it  = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		--it;
	}
	for (; it != m_buffers.end() && it->first < end; ++it) {
		auto&      buffer = m_slot_buffers[it->second];
		const auto start  = std::max(buffer.CpuAddress(), vaddr);
		const auto finish = std::min(buffer.CpuAddress() + buffer.Size(), end);
		if (start < finish) {
			(void)SynchronizeBuffer(buffer, start, finish - start, false, false);
		}
	}
}

} // namespace Libs::Graphics
