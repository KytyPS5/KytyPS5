#include "graphics/host_gpu/renderer/cache/bufferCache.h"

#include "common/alignment.h"
#include "gpu_tiler_shaders/buffer_scatter_spv.h"
#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/hackNotice.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/spscQueue.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {

constexpr uint64_t MiB           = 1024 * 1024;
constexpr uint64_t GdsBufferSize = 64 * 1024;
// GPU-modified ranges come as many small scattered islands, so a readback widens to a window.

std::pair<Buffer*, uint64_t> UploadMemory(GraphicContext& graphics, CommandScheduler& scheduler,
                                        StreamBuffer& staging, uint64_t size, uint64_t alignment,
                                        vk::BufferUsageFlags usage, const auto& write,
                                        bool wait = true) {
	const auto [mapped, offset] = staging.Map(size, alignment, wait);
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
	scheduler.DeferHostOperation([owner = std::move(temporary)]() mutable { owner.reset(); });
	return {buffer, 0};
}

} // namespace

void BufferCache::WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source,
                                  uint64_t size) {
	auto* bytes = static_cast<const uint8_t*>(source);
	while (size != 0) {
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
		m_buffers.emplace(buffer.CpuAddress(), id);
		m_register_epoch++;
		m_buffer_set_epoch++;
		NoteBufferRegions(buffer, true);
		m_total_used_memory += buffer.Size();
		buffer.lru_id = m_lru_cache.Insert(id, m_gc_tick);
		std::vector<vk::DeviceAddress> addresses;
		addresses.reserve(size_pages);
		for (uint64_t i = 0; i < size_pages; ++i) {
			addresses.push_back(buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS));
		}
		WriteDataBuffer(m_bda_pagetable_buffer, table_offset,
		                addresses.data(), addresses.size() * sizeof(vk::DeviceAddress));
	} else {
		m_buffers.erase(m_buffers.find(buffer.CpuAddress()));
		m_buffer_set_epoch++;
		NoteBufferRegions(buffer, false);
		m_total_used_memory -= buffer.Size();
		m_lru_cache.Free(buffer.lru_id);
		m_bda_pagetable_buffer.Fill(table_offset,
		                            size_pages * sizeof(vk::DeviceAddress), 0);
		buffer.is_deleted = true;
	}
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
	m_lru_cache.Touch(buffer.lru_id, m_gc_tick);
}

void BufferCache::DeleteBuffer(BufferId id) {
	Unregister(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferHostOperation([this, id] {
			WaitReadbackQueueCopies(m_slot_buffers[id].Handle());
			m_slot_buffers.erase(id);
		});
	} else {
		m_slot_buffers.erase(id);
	}
}

void BufferCache::CollectDownloadCopies(const Buffer& buffer, uint64_t vaddr, uint64_t size,
                                        std::vector<vk::BufferCopy>& copies,
                                        uint64_t&                    total_size) {
	const auto buffer_address = buffer.CpuAddress();
	m_memory_tracker.ForEachDownloadRange<false>(
	    vaddr, size, [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "buffer download");
		    m_gpu_modified_ranges.ForEachInRange(address, bytes, [&](uint64_t start, uint64_t end) {
			    copies.emplace_back(start - buffer_address, total_size, end - start);
			    // Start each packed range on its own 64-byte cache line.
			    total_size += Common::AlignUp(end - start, 64);
		    });
		    if (m_gpu_modified_ranges.Subtract(address, bytes)) {
			    NoteGpuModifiedChange(address, bytes, false);
		    }
		    ForgetColorMetadataChecks(address, bytes);
	    });
	if (!copies.empty()) {
		Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferDownloadBytes, total_size);
	}
}

template <bool async>
bool BufferCache::DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size     = 0;
	const auto                  buffer_address = buffer.CpuAddress();
	CollectDownloadCopies(buffer, vaddr, size, copies, total_size);
	if (copies.empty()) {
		return false;
	}

	if constexpr (!async) {
		if (DownloadOnReadbackQueue(buffer, copies, total_size)) {
			return true;
		}
	}
	auto download = m_download_buffer.MapDownload(total_size, 64);
	for (auto& copy: copies) {
		copy.dstOffset += download.offset;
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
	native.copyBuffer(buffer.Handle(), download.buffer->Handle(),
	                  static_cast<uint32_t>(copies.size()), copies.data());

	auto after          = before;
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eHostRead;
	after.buffer        = download.buffer->Handle();
	after.offset        = download.offset;
	after.size          = total_size;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands |
	                           vk::PipelineStageFlagBits::eHost,
	                       {}, 0, nullptr, 1, &after, 0, nullptr);
	uint64_t written_begin = UINT64_MAX;
	uint64_t written_end   = 0;
	for (const auto& copy: copies) {
		written_begin = std::min(written_begin, buffer_address + copy.srcOffset);
		written_end   = std::max(written_end, buffer_address + copy.srcOffset + copy.size);
	}
	auto publish = [total_size, buffer_address, copies = std::move(copies),
	                download = std::move(download)] {
		download.buffer->Invalidate(download.offset, total_size);
		for (const auto& copy: copies) {
			Libs::LibKernel::Memory::WriteBacking(
			    buffer_address + copy.srcOffset,
			    download.mapped + (copy.dstOffset - download.offset), copy.size);
		}
	};
	if constexpr (async) {
		m_scheduler.DeferPriorityOperation(std::move(publish), written_begin,
		                                   written_end - written_begin);
	} else {
		const auto tick = m_scheduler.CurrentTick();
		m_scheduler.Wait(tick);
		m_scheduler.WaitPriorityOperations(tick);
		publish();
	}
	return true;
}

BufferCache::BufferCache(GraphicContext& graphics, CommandScheduler& scheduler,
                         PageManager& page_manager, TextureCache& texture_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_fault_manager(graphics, scheduler, *this),
      m_gds_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, GdsBufferSize),
      m_bda_pagetable_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                             BDA_PAGETABLE_SIZE),
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
	m_scheduler.BeginCommand();
	for (auto* buffer: {&m_bda_pagetable_buffer, m_fault_manager.GetFaultBuffer()}) {
		buffer->Fill(0, buffer->Size(), 0);
	}
	const auto null_id =
	    m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
	EXIT_IF(null_id != NULL_BUFFER_ID);
	SetVulkanObjectNameF(m_graphics.device, GetBuffer(null_id).Handle(), "Kyty.NullBuffer");
	// Bindings of empty buffers read this one, so it must hold zeros, not leftover memory.
	GetBuffer(null_id).Fill(0, 16, 0);
	m_scheduler.SetSubmitHook(
	    [](void* self, CommandBuffer& command) {
		    static_cast<BufferCache*>(self)->RecordMirrorCopies(command);
	    },
	    this);
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
	m_scheduler.SetSubmitHook(nullptr, nullptr);
	if (m_scatter_pipeline) {
		m_graphics.device.destroyPipeline(m_scatter_pipeline);
	}
	if (m_scatter_layout) {
		m_graphics.device.destroyPipelineLayout(m_scatter_layout);
	}
	if (m_readback.pool) {
		m_graphics.device.destroyFence(m_readback.fence);
		m_graphics.device.destroyCommandPool(m_readback.pool);
	}
	// A slot's fence is signaled while no copy of it is in flight.
	for (auto& slot: m_readback_slots) {
		if (slot.queue.pool) {
			(void)m_graphics.device.waitForFences(1, &slot.queue.fence, VK_TRUE, UINT64_MAX);
			m_graphics.device.destroyFence(slot.queue.fence);
			m_graphics.device.destroyCommandPool(slot.queue.pool);
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
}

void BufferCache::InvalidateMemory(uint64_t vaddr, uint64_t size, bool guest_fault) {
	// A CPU write needs the whole range CPU-modified. GPU-modified pages are read back first; a
	// GPU write recorded while another thread's readback waits keeps a page GPU-modified, so
	// repeat until every page is CPU-modified.
	for (;;) {
		bool gpu_modified = false;
		m_memory_tracker.InvalidateRegion(vaddr, size, [&gpu_modified] { gpu_modified = true; });
		if (!gpu_modified) {
			return;
		}
		ReadMemory(vaddr, size, true, guest_fault);
	}
}

void BufferCache::InvalidateUnlessGpuModified(uint64_t vaddr, uint64_t size) noexcept {
	if (GuestRange {vaddr, size}.Valid()) {
		m_memory_tracker.InvalidateRegion(vaddr, size, [] {});
	}
}

void BufferCache::InvalidateFaultedPage(uint64_t vaddr) {
	// The guest fills its streaming memory (vertex and constant rings) upwards, and every page it
	// writes under a cached buffer takes a fault: thousands per frame, each a system call to drop
	// the protection. When the page below is already writable the guest is filling the range
	// upwards, so release the pages ahead up to the next 64 KB boundary with this fault; they are
	// only uploaded again at their next synchronization. A range with pages the GPU wrote must be
	// read back first, so it keeps to the faulting page.
	static constexpr uint64_t FaultBlockSize = 64 * 1024;
	static_assert(TRACKER_REGION_SIZE % FaultBlockSize == 0);
	const auto page = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto end  = Common::AlignDown(page, FaultBlockSize) + FaultBlockSize;
	if (page >= TRACKER_PAGE_SIZE && GuestRange {page - TRACKER_PAGE_SIZE, end - page}.Valid() &&
	    m_memory_tracker.IsRegionCpuModified(page - TRACKER_PAGE_SIZE, TRACKER_PAGE_SIZE) &&
	    !m_memory_tracker.IsRegionGpuModified(page, end - page)) {
		InvalidateMemory(page, end - page, true);
		return;
	}
	InvalidateMemory(vaddr, 1, true);
}

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write, bool guest_fault) {
	if (!GuestGpu::IsGpuThread() && CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	if (!GuestGpu::IsGpuThread()) {
		ReadMemoryAsync(vaddr, size, is_write, guest_fault);
		return;
	}
	// The guest reads (or writes) memory the GPU wrote only after a label the execution thread
	// writes once the work before it executed: the operations queued since cannot be what the
	// access needs, so the readback runs before them instead of after the whole backlog (a
	// frame of draws, 100-200 ms while an area streams in).
	m_scheduler.Context().GetGpu().SendUrgentCommandSync([this, vaddr, size, is_write, guest_fault] {
		if (is_write && !IsRegionRegistered(vaddr, size)) {
			return;
		}
		auto&      buffer    = m_slot_buffers[FindBuffer(vaddr, size)];
		const auto read_size = guest_fault ? ReadAheadSize(buffer, vaddr, size) : size;

		// Pages whose GPU-written bytes were all read through the mirror stay GPU-dirty with
		// nothing left to download: they are released all the same.
		const auto pages_begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
		const auto pages_end   = Common::AlignUp(vaddr + read_size, TRACKER_PAGE_SIZE);
		// Bytes another thread's readback downloaded are in the backing once it published them.
		if (const auto inflight = InFlightReadbackTick(pages_begin, pages_end); inflight != 0) {
			m_scheduler.Wait(inflight);
			m_scheduler.WaitPriorityOperations(inflight);
			std::erase_if(m_inflight_readbacks, [inflight](const InFlightReadback& readback) {
				return readback.tick <= inflight;
			});
		}
		if (DownloadBufferMemory<false>(buffer, vaddr, read_size) ||
		    !m_gpu_modified_ranges.Intersects(pages_begin, pages_end - pages_begin)) {
			m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, read_size);
		}
		if (is_write) {
			m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
		}
	});
}

void BufferCache::ReadMemoryAsync(uint64_t vaddr, uint64_t size, bool is_write, bool guest_fault) {
	// The execution thread records the download right away (see ReadMemory()) and submits it, then
	// goes on with its operations while this thread waits for the GPU and the publication: on
	// hardware only the reading thread waits for the memory, not the GPU front end.
	auto&             gpu         = m_scheduler.Context().GetGpu();
	uint64_t          tick        = 0;
	uint64_t          read_size   = size;
	uint64_t          pages_begin = 0;
	uint64_t          pages_end   = 0;
	ReadbackQueueRead queue_read;
	// The command processor thread's commands run as operations, whose completion the execution
	// thread reports only now and then: until then that thread could not publish its copy, and the
	// execution thread can wait for it (an unmap waits for the priority operations).
	const bool        readback_queue = !GuestGpu::IsCommandProcessorThread();
	gpu.SendUrgentCommandSync([&] {
		if (is_write && !IsRegionRegistered(vaddr, size)) {
			return;
		}
		auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];
		read_size    = guest_fault ? ReadAheadSize(buffer, vaddr, size) : size;
		pages_begin  = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
		pages_end    = Common::AlignUp(vaddr + read_size, TRACKER_PAGE_SIZE);
		tick         = InFlightReadbackTick(pages_begin, pages_end);
		// Pages read back while their writer is still being recorded make operations that write
		// them submit at once (see NoteWrite()), so the GPU is done sooner next time.
		NoteReadbackWait(pages_begin, pages_end, false,
		                 m_write_ticks.Latest(pages_begin, pages_end) >= m_scheduler.CurrentTick());
		if (tick == 0 && readback_queue &&
		    StartReadbackQueueRead(buffer, vaddr, read_size, pages_begin, pages_end, queue_read)) {
			tick = queue_read.recorded;
		} else if (DownloadBufferMemory<true>(buffer, vaddr, read_size)) {
			const auto recorded = m_scheduler.CurrentTick();
			m_inflight_readbacks.push_back({pages_begin, pages_end, recorded});
			tick = std::max(tick, recorded);
		}
		if (tick == 0) {
			// Nothing to download or in flight: the pages leave GPU ownership at once.
			ReleaseReadback(vaddr, size, pages_begin, pages_end, 0, is_write);
			return;
		}
		// This thread may only wait for submitted work; a copy on the readback queue waits for
		// submitted work only.
		if (queue_read.slot == nullptr && tick >= m_scheduler.CurrentTick()) {
			m_scheduler.Flush();
		}
	});
	if (tick == 0) {
		return;
	}
	if (queue_read.slot != nullptr) {
		FinishReadbackQueueRead(queue_read);
		gpu.SendUrgentCommandSync([&] {
			// Another thread's readback that waited for the tick took it out already.
			const auto own = std::find_if(m_inflight_readbacks.begin(), m_inflight_readbacks.end(),
			                              [&](const InFlightReadback& readback) {
				                              return readback.begin == pages_begin &&
				                                     readback.end == pages_end &&
				                                     readback.tick == queue_read.recorded;
			                              });
			if (own != m_inflight_readbacks.end()) {
				m_inflight_readbacks.erase(own);
			}
			ReleaseReadback(vaddr, size, pages_begin, pages_end, 0, is_write);
		});
	} else {
		m_scheduler.GetMasterSemaphore().Wait(tick);
		m_scheduler.WaitPriorityOperations(tick);
		gpu.SendUrgentCommandSync(
		    [&] { ReleaseReadback(vaddr, size, pages_begin, pages_end, tick, is_write); });
	}
}

bool BufferCache::StartReadbackQueueRead(const Buffer& buffer, uint64_t vaddr, uint64_t size,
                                         uint64_t pages_begin, uint64_t pages_end,
                                         ReadbackQueueRead& read) {
	if (!m_graphics.readback_queue || m_device_address_pending) {
		return false;
	}
	// As DownloadOnReadbackQueue(): the submissions that last wrote the bytes, and those that
	// wrote through device addresses (which name no range). Writes of the operation being
	// recorded have no tick yet. The download takes whole pages.
	uint64_t needed  = m_device_address_tick;
	bool     unknown = false;
	bool     written = false;
	m_gpu_modified_ranges.ForEachInRange(pages_begin, pages_end - pages_begin, [&](uint64_t start,
	                                                                              uint64_t end) {
		const auto latest = m_write_ticks.Latest(start, end);
		unknown |= latest == UINT64_MAX;
		needed  = std::max(needed, latest);
		written = true;
		for (const auto& [pending, bytes]: m_pending_writes) {
			unknown |= pending < end && start < pending + bytes;
		}
	});
	// The copy must not be submitted before the submission it waits for (see
	// DownloadOnReadbackQueue()).
	if (!written || unknown || needed > m_scheduler.SubmittedTick()) {
		return false;
	}
	ReadbackQueueSlot* slot = nullptr;
	for (auto& candidate: m_readback_slots) {
		if (!candidate.busy.load(std::memory_order_acquire)) {
			slot = &candidate;
			break;
		}
	}
	if (slot == nullptr) {
		return false;
	}
	read.copies.clear();
	read.total_size = 0;
	CollectDownloadCopies(buffer, vaddr, size, read.copies, read.total_size);
	if (read.copies.empty()) {
		return false;
	}
	slot->source = buffer.Handle();
	slot->busy.store(true, std::memory_order_release);
	auto& queue  = slot->queue;
	auto& device = m_graphics.device;
	if (!queue.pool) {
		vk::CommandPoolCreateInfo pool_info {};
		pool_info.flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer |
		                             vk::CommandPoolCreateFlagBits::eTransient;
		pool_info.queueFamilyIndex = m_graphics.readback_family;
		vk::CommandBufferAllocateInfo allocate {};
		allocate.level              = vk::CommandBufferLevel::ePrimary;
		allocate.commandBufferCount = 1;
		vk::FenceCreateInfo fence_info {};
		fence_info.flags = vk::FenceCreateFlagBits::eSignaled;
		if (device.createCommandPool(&pool_info, nullptr, &queue.pool) != vk::Result::eSuccess ||
		    (allocate.commandPool = queue.pool,
		     device.allocateCommandBuffers(&allocate, &queue.command) != vk::Result::eSuccess) ||
		    device.createFence(&fence_info, nullptr, &queue.fence) != vk::Result::eSuccess) {
			EXIT("BufferCache: could not create the readback queue resources\n");
		}
	}
	if (!queue.staging || queue.staging->Size() < read.total_size) {
		queue.staging = std::make_unique<Buffer>(
		    m_graphics, m_scheduler, MemoryUsage::Download, 0,
		    vk::BufferUsageFlagBits::eTransferDst,
		    std::max<uint64_t>(Common::AlignUp(read.total_size, 1024 * 1024), 1024 * 1024));
	}
	const auto command = queue.command;
	(void)device.resetFences(1, &queue.fence);
	(void)command.reset({});
	vk::CommandBufferBeginInfo begin_info {};
	begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	(void)command.begin(&begin_info);
	command.copyBuffer(buffer.Handle(), queue.staging->Handle(),
	                   static_cast<uint32_t>(read.copies.size()), read.copies.data());
	vk::BufferMemoryBarrier after {};
	after.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask       = vk::AccessFlagBits::eHostRead;
	after.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	after.buffer              = queue.staging->Handle();
	after.offset              = 0;
	after.size                = read.total_size;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
	                        {}, 0, nullptr, 1, &after, 0, nullptr);
	(void)command.end();
	// The semaphore wait makes the writes of the needed submissions visible to the copy.
	const auto                      semaphore  = m_scheduler.GetMasterSemaphore().Handle();
	const vk::PipelineStageFlags    wait_stage = vk::PipelineStageFlagBits::eTransfer;
	vk::TimelineSemaphoreSubmitInfo timeline {};
	timeline.waitSemaphoreValueCount = 1;
	timeline.pWaitSemaphoreValues    = &needed;
	vk::SubmitInfo submit {};
	submit.pNext              = &timeline;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores    = &semaphore;
	submit.pWaitDstStageMask  = &wait_stage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &command;
	if (m_graphics.readback_queue.submit(1, &submit, queue.fence) != vk::Result::eSuccess) {
		EXIT("BufferCache: readback queue submission failed\n");
	}
	read.slot           = slot;
	read.buffer_address = buffer.CpuAddress();
	read.needed         = needed;
	read.recorded       = m_scheduler.CurrentTick();
	read.published      = std::make_shared<std::atomic<bool>>(false);
	// Other readers of these pages (the execution thread, another guest thread, an unmap) wait
	// for the downloads in flight over them up to their tick: this one is published by then.
	m_inflight_readbacks.push_back({pages_begin, pages_end, read.recorded});
	m_scheduler.DeferPriorityOperation(
	    [published = read.published] { published->wait(false, std::memory_order_acquire); },
	    pages_begin, pages_end - pages_begin);
	return true;
}

void BufferCache::WaitReadbackQueueCopies(vk::Buffer buffer) {
	for (auto& slot: m_readback_slots) {
		if (slot.busy.load(std::memory_order_acquire) && slot.source == buffer) {
			(void)m_graphics.device.waitForFences(1, &slot.queue.fence, VK_TRUE, UINT64_MAX);
		}
	}
}

void BufferCache::FinishReadbackQueueRead(ReadbackQueueRead& read) {
	auto& queue = read.slot->queue;
	if (m_graphics.device.waitForFences(1, &queue.fence, VK_TRUE, UINT64_MAX) !=
	    vk::Result::eSuccess) {
		EXIT("BufferCache: readback queue wait failed\n");
	}
	// Earlier asynchronous publications of these bytes run first, as in the queued path.
	m_scheduler.WaitPriorityOperations(read.needed);
	queue.staging->Invalidate(0, read.total_size);
	const auto* mapped = queue.staging->Mapped().data();
	for (const auto& copy: read.copies) {
		Libs::LibKernel::Memory::WriteBacking(read.buffer_address + copy.srcOffset,
		                                      mapped + copy.dstOffset, copy.size);
	}
	read.slot->busy.store(false, std::memory_order_release);
	read.published->store(true, std::memory_order_release);
	read.published->notify_all();
}

uint64_t BufferCache::InFlightReadbackTick(uint64_t begin, uint64_t end) const {
	uint64_t tick = 0;
	for (const auto& readback: m_inflight_readbacks) {
		if (readback.begin < end && begin < readback.end) {
			tick = std::max(tick, readback.tick);
		}
	}
	return tick;
}

void BufferCache::ReleaseReadback(uint64_t vaddr, uint64_t size, uint64_t pages_begin,
                                  uint64_t pages_end, uint64_t tick, bool is_write) {
	// Downloads up to `tick` are published.
	std::erase_if(m_inflight_readbacks,
	              [tick](const InFlightReadback& readback) { return readback.tick <= tick; });
	// GPU writes recorded since the download keep their pages GPU-owned for the next readback, as
	// do pages another download in flight covers.
	if (InFlightReadbackTick(pages_begin, pages_end) != 0 ||
	    m_gpu_modified_ranges.Intersects(pages_begin, pages_end - pages_begin)) {
		// The faulting access runs again and reads (or writes) after the next readback.
		return;
	}
	m_memory_tracker.UnmarkRegionAsGpuModified(pages_begin, pages_end - pages_begin);
	if (is_write) {
		m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
	}
}

uint64_t BufferCache::ReadAheadSize(const Buffer& buffer, uint64_t vaddr, uint64_t size) const {
	// The guest reads (and overwrites) GPU results page by page, each page a fault and a
	// readback (Demon's Souls: ~100 consecutive pages a frame). Read the GPU-written pages that
	// follow along when their writes were already submitted, or when the readback drains the GPU
	// anyway (the faulting bytes were written by work still being recorded).
	static constexpr uint64_t MaxReadAhead = 256 * 1024;
	const auto                faulting_begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto                faulting_end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	const auto                limit =
	    std::min(buffer.CpuAddress() + buffer.Size(), faulting_end + MaxReadAhead);
	const auto latest_write = [&](uint64_t begin, uint64_t end) {
		uint64_t latest = 0;
		m_gpu_modified_ranges.ForEachInRange(begin, end - begin, [&](uint64_t first, uint64_t last) {
			latest = std::max(latest, m_write_ticks.Latest(first, last));
			for (const auto& [pending, bytes]: m_pending_writes) {
				if (pending < last && first < pending + bytes) {
					latest = UINT64_MAX;
				}
			}
		});
		return latest;
	};
	// The pages decide, not the faulting bytes: GTA V reads the CPU-written fields between the
	// GPU-written ones of a few hundred pages a frame in order, every page a fault of its own.
	const auto needed = latest_write(faulting_begin, faulting_end);
	if (needed == 0) {
		return size;
	}
	const auto current = m_scheduler.CurrentTick();
	const auto bound   = needed >= current ? UINT64_MAX : current - 1;
	// Pages without GPU-written bytes in between have nothing to download: skip them.
	auto end    = faulting_end;
	auto cursor = faulting_end;
	while (cursor + TRACKER_PAGE_SIZE <= limit &&
	       !m_texture_cache.IsRegionGpuModified(cursor, TRACKER_PAGE_SIZE)) {
		if (!m_gpu_modified_ranges.Intersects(cursor, TRACKER_PAGE_SIZE)) {
			cursor += TRACKER_PAGE_SIZE;
			continue;
		}
		const auto tick = latest_write(cursor, cursor + TRACKER_PAGE_SIZE);
		if (tick == 0 || tick > bound) {
			break;
		}
		cursor += TRACKER_PAGE_SIZE;
		end = cursor;
	}
	return end > faulting_end ? end - vaddr : size;
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

void BufferCache::CacheFaultedMemory(uint64_t vaddr, uint64_t size) {
	for (size_t i = 0; i < StaleRingSize; i++) {
		const auto address = m_stale_ring[i * 2].load(std::memory_order_relaxed);
		const auto length  = m_stale_ring[i * 2 + 1].load(std::memory_order_relaxed);
		if (length != 0 && address < vaddr + size && vaddr < address + length) {
			Common::HackNotice::Report(
			    "buffer released by age reached again by a device-address fault",
			    "BufferCache::CacheFaultedMemory", 0, 0,
			    "a shader read that memory before the buffer existed again (first read may be zero)");
			break;
		}
	}
	if (const auto id = FindBuffer(vaddr, size)) {
		m_slot_buffers[id].device_address_pinned = true;
	}
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
	new_buffer.device_address_pinned |= overlap.device_address_pinned;
	new_buffer.CopyFrom(m_scheduler.Current(), overlap, 0,
	                    overlap.CpuAddress() - new_buffer.CpuAddress(), overlap.Size());
	NoteWrite(overlap.CpuAddress(), overlap.Size());
	DeleteBuffer(overlap_id);
}

BufferId BufferCache::CreateBuffer(uint64_t vaddr, uint64_t size) {
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
		const auto old_id = (it++)->second;
		JoinOverlap(id, old_id, !overlap.has_stream_leap);
	}
	Register(id);
	return id;
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_written,
                                    bool is_texel_buffer) {
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
	if (source && m_upload_batching && source == m_staging_buffer.Handle()) {
		m_upload_batch_buffers.push_back(
		    {buffer.Handle(), buffer.BufferDeviceAddress(), static_cast<uint32_t>(copies.size())});
		m_upload_batch_copies.insert(m_upload_batch_copies.end(), copies.begin(), copies.end());
	} else if (source) {
		auto& command = m_scheduler.Current();
		command.EndRendering();
		const auto native = command.Handle();
		vk::BufferMemoryBarrier before {};
		before.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
		                       vk::AccessFlagBits::eTransferRead |
		                       vk::AccessFlagBits::eTransferWrite;
		before.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer              = buffer.Handle();
		before.offset              = 0;
		before.size                = buffer.Size();
		native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                       vk::PipelineStageFlagBits::eTransfer,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &before, 0, nullptr);
		native.copyBuffer(source, buffer.Handle(), static_cast<uint32_t>(copies.size()),
		                  copies.data());
		auto after          = before;
		after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                       vk::PipelineStageFlagBits::eAllCommands,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr);
	}
	if (is_texel_buffer && !is_written) {
		return SynchronizeBufferFromImage(buffer, vaddr, size);
	}
	return false;
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     uint64_t total_size) {
	if (copies.empty()) {
		return nullptr;
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferUploadBytes, total_size);

	// Within a batch of uploads (see FlushUploadBatch()) the staging buffer must not wait for
	// the current command buffer: that would submit it without the batched copies. A copy that
	// does not fit then goes through a buffer of its own.
	const auto source = UploadMemory(m_graphics, m_scheduler, m_staging_buffer, total_size, 16,
	    vk::BufferUsageFlagBits::eTransferSrc, [&](uint8_t* mapped, uint64_t base_offset) {
		for (auto& copy: copies) {
			const auto address = buffer.CpuAddress() + copy.dstOffset;
			if (!LibKernel::Memory::CopySparseMemory(address, mapped + copy.srcOffset, copy.size)) {
				EXIT("BufferCache: failed to read sparse guest buffer memory\n");
			}
			copy.srcOffset += base_offset;
		}
	}, !m_upload_batching);
	return source.first->Handle();
}

void BufferCache::StreamGuestData(uint8_t* destination, uint64_t vaddr, uint64_t size) {
	// The execution thread hands its copies to the host copy thread (see HostCopyQueue: writing
	// the stream buffer itself would stall it on its next fence). The copy reads the backing
	// store, which holds the same bytes as the guest range and is never protected, so a copy that
	// runs after a later draw protected the range for GPU writes does not fault.
	auto* copies = m_scheduler.HostCopies();
	if (copies != nullptr && m_stream_buffer.IsCoherent() && GuestGpu::IsGpuThread()) {
		if (const void* source = Libs::LibKernel::Memory::FindBackingPointer(vaddr, size)) {
			copies->Copy(destination, source, size);
			return;
		}
	}
	// Backing reads mostly hit the per-thread mapping cache; sparse ranges with holes or private
	// mappings take the full copy.
	if (!LibKernel::Memory::TryReadBacking(vaddr, destination, size) &&
	    !LibKernel::Memory::CopySparseMemory(vaddr, destination, size)) {
		EXIT("BufferCache: failed to read sparse guest buffer memory at 0x%" PRIx64 " size 0x%" PRIx64
		     "\n",
		     vaddr, size);
	}
}

uint64_t BufferCache::StreamUpload(const void* data, uint64_t size, uint64_t alignment) {
	auto* copies = m_scheduler.HostCopies();
	if (copies == nullptr || !m_stream_buffer.IsCoherent() || !GuestGpu::IsGpuThread()) {
		return m_stream_buffer.Copy(data, size, alignment);
	}
	const auto [mapped, offset] = m_stream_buffer.Map(size, alignment);
	EXIT_IF(mapped == nullptr);
	copies->CopyInline(mapped, data, size);
	m_stream_buffer.Commit();
	return offset;
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer(uint64_t vaddr, uint64_t size,
                                                       bool is_written, bool is_texel_buffer,
                                                       BufferId id) {
	EXIT_IF(!(GuestRange {vaddr, size}.Valid()));

	// Draws stream the same small buffers (frame and pass constants) again and again. Within a
	// draw window the guest cannot have written them since the last copy, so draws share it while
	// it is in the stream buffer and no work wrote cached buffers. A copy made in the window
	// answers before the CPU-dirty checks: it was made from CPU-dirty pages that the guest has not
	// written since, and GPU writes advance the generation (or show in the GPU-dirty check).
	const bool  small_read = !is_written && size <= CACHING_PAGESIZE;
	StreamMemo* stream     = nullptr;
	uint64_t    window     = 0;
	if (small_read && !m_memory_tracker.IsRegionGpuModified(vaddr, size)) {
		window = m_scheduler.Context().GetRenderExecutor().DrawWindow();
		stream = &(*m_stream_memo)[((vaddr ^ (size << 40u)) * 0x9e3779b97f4a7c15ull >> 32u) %
		                           StreamMemoSlots];
		if (stream->vaddr == vaddr && stream->size == size && stream->window == window &&
		    stream->lap == m_stream_buffer.Lap() &&
		    stream->write_generation == m_gpu_write_generation) {
			return {&m_stream_buffer, stream->offset};
		}
	}

	// Draws bind the same read-only ranges over and over. While the range stays in the same
	// buffer and none of its pages became CPU dirty since a lookup found it clean (or uploaded
	// it), the lookup finds the same buffer and has nothing to upload. The epoch is read before
	// the dirty check: a page dirtied after it advances it.
	const bool  plain_read = !is_written && !is_texel_buffer;
	ObtainMemo* memo       = nullptr;
	uint64_t    cpu_epoch  = 0;
	if (plain_read) {
		const auto hash = (vaddr ^ (size << 40u)) * 0x9e3779b97f4a7c15ull;
		memo            = &(*m_obtain_memo)[(hash >> 32u) % ObtainMemoSlots];
		cpu_epoch       = CpuDirtyEpoch(vaddr, size);
		if (memo->vaddr == vaddr && memo->size == size &&
		    memo->buffer_epoch == m_buffer_set_epoch && memo->cpu_epoch == cpu_epoch) {
			auto& buffer = m_slot_buffers[memo->id];
			TouchBuffer(buffer);
			return {&buffer, memo->offset};
		}
	}

	if (stream != nullptr && m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
		const auto alignment = std::max<uint64_t>(
		    m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 1);
		auto [mapped, offset] = m_stream_buffer.Map(size, alignment, false);
		if (mapped != nullptr) {
			StreamGuestData(mapped, vaddr, size);
			m_stream_buffer.Commit();
			*stream = {.vaddr            = vaddr,
			           .size             = size,
			           .window           = window,
			           .lap              = m_stream_buffer.Lap(),
			           .write_generation = m_gpu_write_generation,
			           .offset           = offset};
			return {&m_stream_buffer, offset};
		}
	}

	if (IsBufferInvalid(id) || !m_slot_buffers[id].IsInBounds(vaddr, size)) {
		id = FindBuffer(vaddr, size);
	}
	// Dispatches bind the same written ranges (tens of megabytes) again and again. While the
	// range stays in the same buffer, none of its pages became CPU dirty and none stopped being
	// GPU dirty since the last synchronization marked it written, there is nothing to upload and
	// its pages are GPU dirty already: only the write itself is recorded below.
	WrittenMemo* written = nullptr;
	uint64_t     written_cpu_epoch = 0;
	uint64_t     written_gpu_epoch = 0;
	if (is_written) {
		const auto hash   = (vaddr ^ (size << 40u)) * 0x9e3779b97f4a7c15ull;
		written           = &(*m_written_memo)[(hash >> 32u) % WrittenMemoSlots];
		// Global epochs: two loads, where the ranges' own epochs visit every tracking region of
		// tens of megabytes.
		written_cpu_epoch = RegionManager::CpuDirtyEpoch();
		written_gpu_epoch = RegionManager::GpuCleanEpoch();
		if (written->vaddr == vaddr && written->size == size &&
		    written->buffer_epoch == m_buffer_set_epoch && written->cpu_epoch == written_cpu_epoch &&
		    written->gpu_epoch == written_gpu_epoch &&
		    (IsBufferInvalid(id) || id == written->id)) {
			auto& buffer = m_slot_buffers[written->id];
			TouchBuffer(buffer);
			m_gpu_write_generation++;
			if (m_gpu_modified_ranges.Add(vaddr, size)) {
				NoteGpuModifiedChange(vaddr, size, true);
			}
			ForgetColorMetadataChecks(vaddr, size);
			NoteWrite(vaddr, size);
			return {&buffer, buffer.Offset(vaddr)};
		}
	}

	auto& buffer = m_slot_buffers[id];
	TouchBuffer(buffer);
	(void)SynchronizeBuffer(buffer, vaddr, size, is_written, is_texel_buffer);
	if (plain_read) {
		*memo = {.vaddr        = vaddr,
		         .size         = size,
		         .buffer_epoch = m_buffer_set_epoch,
		         .cpu_epoch    = cpu_epoch,
		         .offset       = buffer.Offset(vaddr),
		         .id           = id};
	}
	if (is_written) {
		m_gpu_write_generation++;
		if (m_gpu_modified_ranges.Add(vaddr, size)) {
			NoteGpuModifiedChange(vaddr, size, true);
		}
		ForgetColorMetadataChecks(vaddr, size);
		NoteWrite(vaddr, size);
	}
	return {&buffer, buffer.Offset(vaddr)};
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size) {
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
			EXIT("BufferCache: failed to read mapped guest image backing\n");
		}
	});
}

void BufferCache::FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds) {
	m_gpu_write_generation++;
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
	(void)m_texture_cache.ClearMeta(vaddr);
	if (!IsRegionGpuModified(vaddr, size)) {
		// Invalidate cached buffers and images for the whole range up front, as the write
		// faults would page by page, so the fill below does not fault on every tracked page.
		InvalidateMemory(vaddr, size);
		m_texture_cache.InvalidateMemory(vaddr, size);
		auto* destination = reinterpret_cast<uint32_t*>(vaddr);
		std::fill(destination, destination + size / sizeof(uint32_t), value);
		return;
	}

	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true);
	dst->Fill(dst_offset, size, value);
}

void BufferCache::CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
                             bool src_gds) {
	m_gpu_write_generation++;
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
	// A copy into whole levels of a GPU-written image lands on its GPU contents, not stale guest
	// bytes, while no buffer holds the range.
	if (src_memory && dst_memory && !IsRegionRegistered(dst_vaddr, size) &&
	    m_texture_cache.CopyToImage(dst_vaddr, src_vaddr, size)) {
		return;
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
	const auto src_id      = src_memory ? FindBuffer(src_vaddr, size) : BufferId {};
	const auto dst_id      = dst_memory ? FindBuffer(dst_vaddr, size) : BufferId {};
	auto [src, src_offset] = src_memory ? ObtainBuffer(src_vaddr, size, false, true, src_id)
	                                    : std::pair {&m_gds_buffer, src_vaddr};
	auto [dst, dst_offset] = dst_memory ? ObtainBuffer(dst_vaddr, size, true, true, dst_id)
	                                    : std::pair {&m_gds_buffer, dst_vaddr};
	dst->CopyFrom(command, *src, src_offset, dst_offset, size);
}

void BufferCache::WriteTicks::Assign(uint64_t begin, uint64_t end, uint64_t tick) {
	if (begin >= end) {
		return;
	}
	auto it = m_ranges.lower_bound(begin);
	// Most writes repeat a range of an earlier operation.
	if (it != m_ranges.end() && it->first == begin && it->second.end == end) {
		it->second.tick = tick;
		return;
	}
	if (it != m_ranges.begin()) {
		const auto previous = std::prev(it);
		if (previous->second.end > begin) {
			const auto right       = previous->second;
			previous->second.end = begin;
			it                     = m_ranges.emplace_hint(it, begin, right);
		}
	}
	while (it != m_ranges.end() && it->first < end) {
		if (it->second.end > end) {
			const auto tail = it->second;
			m_ranges.erase(it);
			m_ranges.emplace(end, tail);
			break;
		}
		it = m_ranges.erase(it);
	}
	m_ranges.emplace(begin, Range {end, tick});
}

uint64_t BufferCache::WriteTicks::Latest(uint64_t begin, uint64_t end) const {
	auto it = m_ranges.upper_bound(begin);
	if (it != m_ranges.begin()) {
		--it;
	}
	uint64_t latest = 0;
	uint64_t cursor = begin;
	for (; it != m_ranges.end() && it->first < end && cursor < end; ++it) {
		if (it->second.end <= cursor) {
			continue;
		}
		if (it->first > cursor) {
			return UINT64_MAX;
		}
		latest = std::max(latest, it->second.tick);
		cursor = it->second.end;
	}
	return cursor >= end ? latest : UINT64_MAX;
}

void BufferCache::NoteReadbackWait(uint64_t begin, uint64_t end, bool device_address,
                                   bool drained) {
	const auto count = ++m_readback_count;
	if (device_address && (drained || m_device_address_hot != 0)) {
		m_device_address_hot = count;
	}
	std::erase_if(m_readback_hot, [count](const HotReadback& hot) {
		return count - hot.last_hit >= HotReadbackLifetime;
	});
	begin = Common::AlignDown(begin, TRACKER_PAGE_SIZE);
	end   = Common::AlignUp(end, TRACKER_PAGE_SIZE);
	for (auto& hot: m_readback_hot) {
		if (begin < hot.end && hot.begin < end) {
			hot.begin    = std::min(hot.begin, begin);
			hot.end      = std::max(hot.end, end);
			hot.last_hit = count;
			return;
		}
	}
	if (drained && !device_address) {
		if (m_readback_hot.size() == MaxHotReadbacks) {
			m_readback_hot.erase(std::ranges::min_element(m_readback_hot, {}, &HotReadback::last_hit));
		}
		m_readback_hot.push_back({begin, end, count});
	}
}

bool BufferCache::TakeFlushRequest() {
	if (!m_flush_requested) {
		return false;
	}
	// The work the request is about went out with a submission made meanwhile.
	if (m_flush_deferred_tick != 0 && m_scheduler.CurrentTick() != m_flush_deferred_tick) {
		m_flush_requested     = false;
		m_flush_deferred_tick = 0;
		return false;
	}
	// A submission costs the recording thread a queue submit. While readbacks keep coming, every
	// operation that may write what they read asks for one (Demon's Souls: ~2,000 a frame, the
	// device-address writers of a game whose SRT walks read GPU-written dwords through the
	// mirror). Submit at most once per interval: a readback then waits at most for the work
	// recorded since; the request stays for a later operation.
	const auto now = std::chrono::steady_clock::now();
	if (now - m_last_requested_flush < MinRequestedFlushInterval) {
		m_flush_deferred_tick = m_scheduler.CurrentTick();
		return false;
	}
	m_last_requested_flush = now;
	m_flush_requested      = false;
	m_flush_deferred_tick  = 0;
	return true;
}

bool BufferCache::CommitWriteTicks() {
	if (m_pending_writes.empty() && !m_device_address_pending) {
		return TakeFlushRequest();
	}
	// The operation's commands are all recorded: they run in the current command buffer or in
	// one submitted while it was recorded.
	const auto tick = m_scheduler.CurrentTick();
	for (const auto& [vaddr, size]: m_pending_writes) {
		m_write_ticks.Assign(vaddr, vaddr + size, tick);
	}
	// Device-address stores name no range: a mirror copy older than them is not used (see
	// TryReadMirroredGpuWrites()).
	if (!m_mirror_lines.empty()) {
		for (const auto& [vaddr, size]: m_pending_writes) {
			MarkMirrorLinesWritten(vaddr, size, tick);
		}
	}
	m_pending_writes.clear();
	if (m_device_address_pending) {
		m_device_address_pending = false;
		m_device_address_tick    = tick;
	}
	return TakeFlushRequest();
}

void BufferCache::MarkMirrorLinesWritten(uint64_t vaddr, uint64_t size, uint64_t tick) {
	const auto end = size > UINT64_MAX - vaddr ? UINT64_MAX : vaddr + size;
	for (auto it = m_mirror_lines.lower_bound(Common::AlignDown(vaddr, MirrorLineSize));
	     it != m_mirror_lines.end() && it->first < end; ++it) {
		auto& line = it->second;
		line.tick  = 0;
		// A line read recently is read again soon: let the GPU start on its writer now.
		if (m_mirror_uses - line.last_use < MirrorFlushLifetime) {
			m_flush_requested = true;
		}
		if (line.dirty_tick != tick) {
			line.dirty_tick = tick;
			m_mirror_dirty.push_back(it->first);
		}
	}
}

void BufferCache::AddMirrorLine(uint64_t line) {
	if (m_mirror_free_slots.empty()) {
		if (m_mirror_lines.size() < MaxMirrorLines) {
			m_mirror_free_slots.push_back(static_cast<uint32_t>(m_mirror_lines.size()));
		} else {
			// Forget the half used least recently; their queued copies are skipped.
			std::vector<uint64_t> uses;
			uses.reserve(m_mirror_lines.size());
			for (const auto& [address, entry]: m_mirror_lines) {
				uses.push_back(entry.last_use);
			}
			std::nth_element(uses.begin(), uses.begin() + uses.size() / 2, uses.end());
			const auto oldest = uses[uses.size() / 2];
			for (auto it = m_mirror_lines.begin(); it != m_mirror_lines.end();) {
				if (it->second.last_use <= oldest) {
					m_mirror_free_slots.push_back(it->second.slot);
					it = m_mirror_lines.erase(it);
				} else {
					++it;
				}
			}
		}
	}
	MirrorLine entry;
	entry.slot = m_mirror_free_slots.back();
	m_mirror_free_slots.pop_back();
	entry.last_use = ++m_mirror_uses;
	// Nothing recorded the line's bytes yet: copy them at the end of this command buffer.
	entry.dirty_tick = m_scheduler.CurrentTick();
	m_mirror_lines.emplace(line, entry);
	m_mirror_dirty.push_back(line);
}

bool BufferCache::TryReadMirroredGpuWrites(uint64_t vaddr, void* data, uint64_t size) {
	if (size == 0 || size > MirrorMaxRead || m_device_address_pending ||
	    !GuestGpu::IsGpuThread()) {
		return false;
	}
	// The bytes of a download in flight left the dirty ranges with their write ticks, so a mirror
	// line older than them would pass (see HasGpuDirtyBytes()).
	if (InFlightReadbackTick(vaddr, vaddr + size) != 0) {
		return false;
	}
	// Writes of the operation being recorded have no tick yet.
	for (const auto& [address, bytes]: m_pending_writes) {
		if (address < vaddr + size && vaddr < address + bytes) {
			return false;
		}
	}
	// As DownloadOnReadbackQueue(): the submissions that last wrote the bytes, and those that
	// wrote through device addresses (which name no range).
	uint64_t needed  = m_device_address_tick;
	bool     unknown = false;
	m_gpu_modified_ranges.ForEachInRange(vaddr, size, [&](uint64_t start, uint64_t end) {
		const auto latest = m_write_ticks.Latest(start, end);
		unknown |= latest == UINT64_MAX;
		needed = std::max(needed, latest);
	});
	if (unknown) {
		return false;
	}
	const auto first = Common::AlignDown(vaddr, MirrorLineSize);
	const auto last  = Common::AlignUp(vaddr + size, MirrorLineSize);
	uint64_t   wait  = 0;
	bool       held  = true;
	// The slots of the lines, for the copy below (MirrorMaxRead bytes span at most this many).
	std::array<uint32_t, MirrorMaxRead / MirrorLineSize + 1> slots {};
	for (auto line = first; line < last; line += MirrorLineSize) {
		const auto it = m_mirror_lines.find(line);
		if (it == m_mirror_lines.end()) {
			AddMirrorLine(line);
			held = false;
			continue;
		}
		auto& entry    = it->second;
		entry.last_use = ++m_mirror_uses;
		slots[(line - first) / MirrorLineSize] = entry.slot;
		if (entry.tick == 0 || entry.tick < needed) {
			// Copy it at the end of this command buffer, for the next read.
			held = false;
			if (entry.dirty_tick != m_scheduler.CurrentTick()) {
				entry.dirty_tick = m_scheduler.CurrentTick();
				m_mirror_dirty.push_back(line);
			}
		}
		wait = std::max(wait, entry.tick);
	}
	if (!held || wait >= m_scheduler.CurrentTick()) {
		return false;
	}
	m_scheduler.GetMasterSemaphore().Wait(wait);
	m_mirror_flush_use = m_mirror_uses;
	// As a readback: earlier asynchronous publications of these bytes run first, and the bytes
	// become clean, so later reads take them from the backing store until the GPU writes them
	// again.
	m_scheduler.WaitPriorityOperations(needed);
	if (!Libs::LibKernel::Memory::TryReadBacking(vaddr, data, size)) {
		return false;
	}
	const auto* mapped = m_mirror->Mapped().data();
	m_gpu_modified_ranges.ForEachInRange(vaddr, size, [&](uint64_t start, uint64_t end) {
		while (start < end) {
			const auto line   = Common::AlignDown(start, MirrorLineSize);
			const auto chunk  = std::min(end, line + MirrorLineSize) - start;
			const auto slot   = slots[(line - first) / MirrorLineSize];
			const auto offset = uint64_t {slot} * MirrorLineSize + (start - line);
			m_mirror->Invalidate(offset, chunk);
			std::memcpy(static_cast<uint8_t*>(data) + (start - vaddr), mapped + offset, chunk);
			Libs::LibKernel::Memory::WriteBacking(start, mapped + offset, chunk);
			start += chunk;
		}
	});
	if (m_gpu_modified_ranges.Subtract(vaddr, size)) {
		NoteGpuModifiedChange(vaddr, size, false);
	}
	return true;
}

void BufferCache::RecordMirrorCopies(CommandBuffer& command) {
	if (m_mirror_dirty.empty() || !GuestGpu::IsGpuThread()) {
		return;
	}
	if (!m_mirror) {
		m_mirror = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Download, 0,
		                                    vk::BufferUsageFlagBits::eTransferDst,
		                                    uint64_t {MaxMirrorLines} * MirrorLineSize);
	}
	std::sort(m_mirror_dirty.begin(), m_mirror_dirty.end());
	m_mirror_dirty.erase(std::unique(m_mirror_dirty.begin(), m_mirror_dirty.end()),
	                     m_mirror_dirty.end());
	const auto tick = m_scheduler.CurrentTick();
	command.EndRendering();
	const auto native = command.Handle();
	// Every earlier write of the command buffer, and the earlier copies into the mirror slots.
	vk::MemoryBarrier before {};
	before.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, {}, 1, &before, 0, nullptr, 0,
	                       nullptr);
	std::vector<vk::BufferCopy> copies;
	const Buffer*               source = nullptr;
	const auto                  flush  = [&] {
		if (!copies.empty()) {
			native.copyBuffer(source->Handle(), m_mirror->Handle(),
			                  static_cast<uint32_t>(copies.size()), copies.data());
			copies.clear();
		}
	};
	for (const auto line: m_mirror_dirty) {
		const auto it = m_mirror_lines.find(line);
		if (it == m_mirror_lines.end()) {
			continue;
		}
		// Lines outside one cached buffer stay unmirrored (their reads take the readback path).
		const auto* owner = m_page_table.Find(line >> PageTable::kPageBits);
		if (owner == nullptr || !*owner) {
			continue;
		}
		const auto& buffer = m_slot_buffers[*owner];
		if (buffer.is_deleted || !buffer.IsInBounds(line, MirrorLineSize)) {
			continue;
		}
		if (&buffer != source) {
			flush();
			source = &buffer;
		}
		copies.push_back({buffer.Offset(line), uint64_t {it->second.slot} * MirrorLineSize,
		                  MirrorLineSize});
		it->second.tick = tick;
	}
	flush();
	m_mirror_dirty.clear();
	vk::MemoryBarrier after {};
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eHostRead;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
	                       {}, 1, &after, 0, nullptr, 0, nullptr);
}

bool BufferCache::DownloadOnReadbackQueue(Buffer& buffer, std::span<const vk::BufferCopy> copies,
                                          uint64_t total_size) {
	if (!m_graphics.readback_queue || m_device_address_pending) {
		return false;
	}
	const auto buffer_address = buffer.CpuAddress();
	// Writes of the operation being recorded, or through device addresses (which name no range)
	// since the needed submissions, may still change the bytes.
	uint64_t needed = m_device_address_tick;
	const auto readback_begin = buffer_address + copies.front().srcOffset;
	const auto readback_end   = buffer_address + copies.back().srcOffset + copies.back().size;
	for (const auto& copy: copies) {
		const auto begin = buffer_address + copy.srcOffset;
		const auto end   = begin + copy.size;
		for (const auto& [vaddr, size]: m_pending_writes) {
			if (vaddr < end && begin < vaddr + size) {
				NoteReadbackWait(readback_begin, readback_end, false, true);
				return false;
			}
		}
		needed = std::max(needed, m_write_ticks.Latest(begin, end));
	}
	const bool device_address = needed != 0 && needed == m_device_address_tick;
	if (needed >= m_scheduler.CurrentTick()) {
		NoteReadbackWait(readback_begin, readback_end, device_address, true);
		return false;
	}
	NoteReadbackWait(readback_begin, readback_end, device_address, false);
	// The copy waits for the needed submission on the GPU, but must not be submitted before it:
	// the driver may then block inside vkQueueSubmit, holding locks a submission of the
	// recording thread (or a present) needs.
	while (m_scheduler.SubmittedTick() < needed) {
		Common::SpinPause();
	}
	auto& device = m_graphics.device;
	if (!m_readback.pool) {
		vk::CommandPoolCreateInfo pool_info {};
		pool_info.flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer |
		                             vk::CommandPoolCreateFlagBits::eTransient;
		pool_info.queueFamilyIndex = m_graphics.readback_family;
		vk::CommandBufferAllocateInfo allocate {};
		allocate.level              = vk::CommandBufferLevel::ePrimary;
		allocate.commandBufferCount = 1;
		const vk::FenceCreateInfo fence_info {};
		if (device.createCommandPool(&pool_info, nullptr, &m_readback.pool) != vk::Result::eSuccess ||
		    (allocate.commandPool = m_readback.pool,
		     device.allocateCommandBuffers(&allocate, &m_readback.command) != vk::Result::eSuccess) ||
		    device.createFence(&fence_info, nullptr, &m_readback.fence) != vk::Result::eSuccess) {
			EXIT("BufferCache: could not create the readback queue resources\n");
		}
	}
	if (!m_readback.staging || m_readback.staging->Size() < total_size) {
		m_readback.staging = std::make_unique<Buffer>(
		    m_graphics, m_scheduler, MemoryUsage::Download, 0,
		    vk::BufferUsageFlagBits::eTransferDst,
		    std::max<uint64_t>(Common::AlignUp(total_size, 1024 * 1024), 4 * MiB));
	}
	auto&                       staging = *m_readback.staging;
	const auto command = m_readback.command;
	(void)command.reset({});
	vk::CommandBufferBeginInfo begin_info {};
	begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	(void)command.begin(&begin_info);
	command.copyBuffer(buffer.Handle(), staging.Handle(), static_cast<uint32_t>(copies.size()),
	                   copies.data());
	vk::BufferMemoryBarrier after {};
	after.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask       = vk::AccessFlagBits::eHostRead;
	after.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	after.buffer              = staging.Handle();
	after.offset              = 0;
	after.size                = total_size;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
	                        {}, 0, nullptr, 1, &after, 0, nullptr);
	(void)command.end();
	// The semaphore wait makes the writes of the needed submissions visible to the copy.
	const auto                      semaphore = m_scheduler.GetMasterSemaphore().Handle();
	const vk::PipelineStageFlags    wait_stage = vk::PipelineStageFlagBits::eTransfer;
	vk::TimelineSemaphoreSubmitInfo timeline {};
	timeline.waitSemaphoreValueCount = 1;
	timeline.pWaitSemaphoreValues    = &needed;
	vk::SubmitInfo submit {};
	submit.pNext              = &timeline;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores    = &semaphore;
	submit.pWaitDstStageMask  = &wait_stage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &command;
	if (m_graphics.readback_queue.submit(1, &submit, m_readback.fence) != vk::Result::eSuccess) {
		EXIT("BufferCache: readback queue submission failed\n");
	}
	for (;;) {
		const auto status = device.getFenceStatus(m_readback.fence);
		if (status == vk::Result::eSuccess) {
			break;
		}
		if (status != vk::Result::eNotReady) {
			EXIT("BufferCache: readback queue wait failed: %s\n", vk::to_string(status).c_str());
		}
		Common::SpinPause();
	}
	(void)device.resetFences(1, &m_readback.fence);
	// Earlier asynchronous publications of these bytes run first, as in the queued path.
	m_scheduler.WaitPriorityOperations(needed);
	staging.Invalidate(0, total_size);
	const auto* mapped = staging.Mapped().data();
	for (const auto& copy: copies) {
		Libs::LibKernel::Memory::WriteBacking(buffer_address + copy.srcOffset,
		                                      mapped + copy.dstOffset, copy.size);
	}
	return true;
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

bool BufferCache::WriteAroundGpuWrites(uint64_t vaddr, const void* data, uint64_t size) {
	constexpr uint64_t MaxBytes = 4096;
	if (size == 0 || size > MaxBytes || (vaddr & 3u) != 0 || (size & 3u) != 0 ||
	    !GuestRange {vaddr, size}.Valid() || !m_memory_tracker.IsRegionGpuModified(vaddr, size)) {
		return false;
	}
	Buffer*    buffer = nullptr;
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner && m_slot_buffers[*owner].IsInBounds(vaddr, size)) {
		buffer = &m_slot_buffers[*owner];
	} else if (IsRegionRegistered(vaddr, size)) {
		// Partly cached: the faulting path handles it.
		return false;
	}
	// Bytes the GPU wrote stay GPU-dirty: the copy below orders the new value after the GPU's
	// writes, and a later read of them reads the buffer back. Without a buffer to copy into,
	// the GPU's bytes would be lost.
	if (buffer == nullptr && HasGpuDirtyBytes(vaddr, size)) {
		return false;
	}
	if (!Libs::LibKernel::Memory::TryWriteBacking(vaddr, data, size)) {
		return false;
	}
	if (buffer == nullptr) {
		return true;
	}
	// The cached buffer is what the GPU and later readbacks of the page use: give it the bytes
	// at this point of the command stream.
	auto&      stream  = GetUtilityBuffer(MemoryUsage::Stream);
	const auto offset  = StreamUpload(data, size, 4);
	auto&      command = m_scheduler.Current();
	command.EndRendering();
	const auto native = command.Handle();
	vk::BufferMemoryBarrier before {};
	before.srcAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
	before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.buffer              = buffer->Handle();
	before.offset              = buffer->Offset(vaddr);
	before.size                = size;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
	                       0, nullptr, 1, &before, 0, nullptr);
	const vk::BufferCopy copy {offset, buffer->Offset(vaddr), size};
	native.copyBuffer(stream.Handle(), buffer->Handle(), 1, &copy);
	NoteWrite(vaddr, size);
	auto after          = before;
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands,
	                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr);
	m_gpu_write_generation++;
	return true;
}

bool BufferCache::HasGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	// Reads around the GPU's writes ask this for every few bytes they read, on a few pages that
	// mix the guest's descriptors with GPU-written data: answer within a page from its mask.
	constexpr uint64_t PageSize = 4096;
	const auto         page     = vaddr / PageSize;
	// Bytes another thread's readback downloads have left the dirty ranges, but the backing holds
	// them only once that download is published.
	if (!m_inflight_readbacks.empty() && size != 0 &&
	    InFlightReadbackTick(vaddr, vaddr + size) != 0) {
		return true;
	}
	if (size == 0 || (vaddr + size - 1) / PageSize != page) {
		return m_gpu_modified_ranges.Intersects(vaddr, size);
	}
	auto& mask = m_dirty_masks[page % m_dirty_masks.size()];
	if (mask.page != page) {
		mask.page = page;
		mask.bits.fill(0);
		const auto base = page * PageSize;
		m_gpu_modified_ranges.ForEachInRange(base, PageSize, [&](uint64_t start, uint64_t end) {
			const auto low  = std::max(start, base) - base;
			const auto high = std::min(end, base + PageSize) - base - 1;
			for (auto word = low / 64; word <= high / 64; word++) {
				const auto from = word == low / 64 ? low % 64 : 0;
				const auto to   = word == high / 64 ? high % 64 : 63;
				mask.bits[word] |= (~uint64_t {0} >> (63 - to)) & (~uint64_t {0} << from);
			}
		});
	}
	const auto first = vaddr % PageSize;
	const auto last  = first + size - 1;
	for (auto word = first / 64; word <= last / 64; word++) {
		const auto low  = word == first / 64 ? first % 64 : 0;
		const auto high = word == last / 64 ? last % 64 : 63;
		const auto bits = (~uint64_t {0} >> (63 - high)) & (~uint64_t {0} << low);
		if ((mask.bits[word] & bits) != 0) {
			return true;
		}
	}
	return false;
}

void BufferCache::NoteGpuModifiedChange(uint64_t vaddr, uint64_t size, bool dirty) {
	// The ranges gained or lost exactly these bytes, so the cached masks of their pages take the
	// change instead of being built again: reads around the GPU's writes publish the bytes they
	// read (a few thousand small ranges a frame), on the pages they keep asking about.
	constexpr uint64_t PageSize = 4096;
	if (size == 0) {
		return;
	}
	const auto first = vaddr / PageSize;
	const auto last  = (vaddr + size - 1) / PageSize;
	const auto apply = [&](DirtyPageMask& mask) {
		const auto base = mask.page * PageSize;
		const auto low  = std::max(vaddr, base) - base;
		const auto high = std::min(vaddr + size, base + PageSize) - base - 1;
		for (auto word = low / 64; word <= high / 64; word++) {
			const auto from = word == low / 64 ? low % 64 : 0;
			const auto to   = word == high / 64 ? high % 64 : 63;
			const auto bits = (~uint64_t {0} >> (63 - to)) & (~uint64_t {0} << from);
			mask.bits[word] = dirty ? mask.bits[word] | bits : mask.bits[word] & ~bits;
		}
	};
	if (last - first >= m_dirty_masks.size()) {
		for (auto& mask: m_dirty_masks) {
			if (mask.page >= first && mask.page <= last) {
				apply(mask);
			}
		}
		return;
	}
	for (auto page = first; page <= last; page++) {
		auto& mask = m_dirty_masks[page % m_dirty_masks.size()];
		if (mask.page == page) {
			apply(mask);
		}
	}
}

bool BufferCache::HasOnlyGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	return m_gpu_modified_ranges.Contains(vaddr, size);
}

bool BufferCache::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionCpuModified(vaddr, size);
}

void BufferCache::CollectStaleBuffers(uint64_t tick) {
	// Buffers outlive the draws that created them, and once the guest moves elsewhere the memory
	// they cover is reused for other data. Every device-address synchronization keeps uploading the
	// guest's writes there and protecting the pages again, so the guest takes write faults on
	// memory no draw reads, more of it the longer the session runs (the collection under memory
	// pressure below rarely runs). Release the buffers no lookup used for a long time; the next use
	// creates them again. Buffers with GPU writes stay, and so do the ones shaders reached through
	// device addresses the host could not see coming.
	static constexpr uint64_t StaleAge      = 2048; // garbage-collection ticks: about half a minute
	static constexpr uint64_t StalePeriod   = 8;
	static constexpr size_t   StaleVisitMax = 64;
	static constexpr size_t   StaleFreeMax  = 16;
	if (tick < StaleAge || tick % StalePeriod != 0) {
		return;
	}
	std::vector<BufferId> stale;
	std::vector<BufferId> kept;
	m_lru_cache.ForEachItemBelow(tick - StaleAge, [&](BufferId id) {
		const auto& buffer = m_slot_buffers[id];
		const bool  keep   = buffer.device_address_pinned ||
		                  m_gpu_modified_ranges.Intersects(buffer.CpuAddress(), buffer.Size()) ||
		                  m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size());
		(keep ? kept : stale).push_back(id);
		return stale.size() == StaleFreeMax || stale.size() + kept.size() == StaleVisitMax;
	});
	// The buffers that stay move to the back of the queue, so the next passes reach the others.
	for (const auto id: kept) {
		m_lru_cache.Touch(m_slot_buffers[id].lru_id, tick);
	}
	for (const auto id: stale) {
		const auto& buffer = m_slot_buffers[id];
		const auto slot =
		    (m_stale_ring_next.fetch_add(1, std::memory_order_relaxed) % StaleRingSize) * 2;
		m_stale_ring[slot].store(buffer.CpuAddress(), std::memory_order_relaxed);
		m_stale_ring[slot + 1].store(buffer.Size(), std::memory_order_relaxed);
		m_memory_tracker.MarkRegionAsCpuModified(buffer.CpuAddress(), buffer.Size());
		if (m_record_released) {
			m_released_ranges.Add(buffer.CpuAddress(), buffer.Size());
		}
		DeleteBuffer(id);
	}
}

void BufferCache::RunGarbageCollector() {
	const auto tick = m_gc_tick++;
	CollectStaleBuffers(tick);
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
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, buffer.CpuAddress(),
		                                           buffer.Size(), "garbage collection");
		const bool dirty = m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size());
		if (dirty && !aggressive) {
			return false;
		}
		if (dirty) {
			// Pages a readback download in flight covers publish with the wait below.
			EXIT_NOT_IMPLEMENTED(
			    !DownloadBufferMemory<true>(buffer, buffer.CpuAddress(), buffer.Size()) &&
			    InFlightReadbackTick(buffer.CpuAddress(), buffer.CpuAddress() + buffer.Size()) == 0);
			dirty_buffers.push_back(id);
		} else {
			m_memory_tracker.MarkRegionAsCpuModified(buffer.CpuAddress(), buffer.Size());
			if (m_record_released) {
				m_released_ranges.Add(buffer.CpuAddress(), buffer.Size());
			}
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
		m_memory_tracker.MarkRegionAsCpuModified(buffer.CpuAddress(), buffer.Size());
		if (m_record_released) {
			m_released_ranges.Add(buffer.CpuAddress(), buffer.Size());
		}
		Unregister(id);
		m_slot_buffers.erase(id);
	}
}

void BufferCache::ProcessFaultBuffer() {
	m_fault_manager.ProcessFaultBuffer();
}

void BufferCache::ProcessUseBuffer() {
	m_fault_manager.ProcessUseBuffer([this](std::span<const uint64_t> pages) {
		for (const auto page: pages) {
			const auto* owner = m_page_table.Find(page >> PageTable::kPageBits);
			if (owner != nullptr && *owner) {
				TouchBuffer(m_slot_buffers[*owner]);
			}
		}
	});
}

void BufferCache::NoteGpuWrites(uint64_t vaddr, uint64_t size) {
	m_gpu_write_generation++;
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid GPU write range\n");
	}
	const auto end = vaddr + size;
	auto       it  = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		--it;
	}
	for (; it != m_buffers.end() && it->first < end; ++it) {
		const auto& buffer = m_slot_buffers[it->second];
		const auto  start  = std::max(buffer.CpuAddress(), vaddr);
		const auto  finish = std::min(buffer.CpuAddress() + buffer.Size(), end);
		if (start >= finish) {
			continue;
		}
		TouchBuffer(buffer);
		NoteWrite(start, finish - start);
		m_memory_tracker.MarkRegionAsGpuModifiedUnlessCpuDirty(
		    start, finish - start, [&](uint64_t page, uint64_t bytes) {
			    const auto first = std::max(page, start);
			    const auto last  = std::min(page + bytes, finish);
			    if (m_gpu_modified_ranges.Add(first, last - first)) {
				    NoteGpuModifiedChange(first, last - first, true);
			    }
		    });
	}
}

void BufferCache::FlushUploadBatch() {
	m_upload_batching = false;
	if (m_upload_batch_buffers.empty()) {
		return;
	}
	// The staging ranges belong to the command buffer they were mapped for.
	EXIT_IF(m_scheduler.CurrentTick() != m_upload_batch_tick);
	auto& command = m_scheduler.Current();
	command.EndRendering();
	const auto native = command.Handle();
	// A batch that copies to several buffers runs as one dispatch: a copy command each costs about
	// as much GPU time as a small dispatch (GTA V: ~200 buffers per batch, several batches a frame).
	if (m_upload_batch_buffers.size() > 1 && ScatterUploadBatch(native)) {
		m_upload_batch_buffers.clear();
		m_upload_batch_copies.clear();
		return;
	}
	// As SynchronizeBuffer() records around each upload, for all the buffers at once.
	vk::MemoryBarrier before {};
	before.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
	                       vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite;
	before.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion, 1,
	                       &before, 0, nullptr, 0, nullptr);
	const auto staging = m_staging_buffer.Handle();
	size_t     first   = 0;
	for (const auto& batch: m_upload_batch_buffers) {
		native.copyBuffer(staging, batch.buffer, batch.count, m_upload_batch_copies.data() + first);
		first += batch.count;
	}
	vk::MemoryBarrier after {};
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands, vk::DependencyFlagBits::eByRegion,
	                       1, &after, 0, nullptr, 0, nullptr);
	m_upload_batch_buffers.clear();
	m_upload_batch_copies.clear();
}

bool BufferCache::ScatterUploadBatch(const CommandRecorder& native) {
	// One job per piece of up to ScatterJobBytes of a copy, by device address: small pieces keep
	// enough workgroups in flight to hide the latency of reads from host memory.
	struct Job {
		uint64_t source;
		uint64_t destination;
		uint32_t size;
		uint32_t pad0;
		uint64_t pad1;
	};
	static_assert(sizeof(Job) == 32);
	constexpr uint64_t ScatterJobBytes = 4 * 1024;
	constexpr uint32_t MaxJobs         = 65535;
	const auto         staging         = m_staging_buffer.BufferDeviceAddress();
	std::vector<Job>   jobs;
	size_t             first = 0;
	for (const auto& batch: m_upload_batch_buffers) {
		if (batch.address == 0 || staging == 0) {
			return false;
		}
		for (uint32_t i = 0; i < batch.count; i++) {
			const auto& copy = m_upload_batch_copies[first + i];
			// The shader copies whole dwords.
			if (((copy.srcOffset | copy.dstOffset | copy.size) & 3u) != 0) {
				return false;
			}
			for (uint64_t done = 0; done < copy.size; done += ScatterJobBytes) {
				jobs.push_back({staging + copy.srcOffset + done, batch.address + copy.dstOffset + done,
				                static_cast<uint32_t>(std::min(ScatterJobBytes, copy.size - done)), 0, 0});
			}
		}
		first += batch.count;
	}
	if (jobs.empty() || jobs.size() > MaxJobs) {
		return false;
	}
	// The job list goes after the staged data, in the same lap of the staging buffer (no wait: that
	// would submit the command buffer without the batch).
	const auto bytes            = jobs.size() * sizeof(Job);
	const auto [mapped, offset] = m_staging_buffer.Map(bytes, 16, false);
	if (mapped == nullptr) {
		return false;
	}
	std::memcpy(mapped, jobs.data(), bytes);
	m_staging_buffer.Commit();
	if (!m_scatter_pipeline) {
		const auto                   device = m_graphics.device;
		const vk::PushConstantRange  push {vk::ShaderStageFlagBits::eCompute, 0, 16};
		vk::PipelineLayoutCreateInfo layout_info {};
		layout_info.pushConstantRangeCount = 1;
		layout_info.pPushConstantRanges    = &push;
		RequireVulkanSuccess(device.createPipelineLayout(&layout_info, nullptr, &m_scatter_layout),
		                     "create buffer scatter pipeline layout");
		const auto                        module = CompileSPV(BUFFER_SCATTER_SPV, device);
		vk::PipelineShaderStageCreateInfo stage {};
		stage.stage  = vk::ShaderStageFlagBits::eCompute;
		stage.module = module;
		stage.pName  = "main";
		vk::ComputePipelineCreateInfo pipeline_info {};
		pipeline_info.stage  = stage;
		pipeline_info.layout = m_scatter_layout;
		const auto result    = device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr, &m_scatter_pipeline);
		device.destroyShaderModule(module, nullptr);
		RequireVulkanSuccess(result, "create buffer scatter pipeline");
		SetVulkanObjectNameF(device, m_scatter_pipeline, "Kyty.BufferScatter");
	}
	vk::MemoryBarrier before {};
	before.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eComputeShader, vk::DependencyFlagBits::eByRegion, 1,
	                       &before, 0, nullptr, 0, nullptr);
	struct Push {
		uint64_t jobs;
		uint32_t job_count;
		uint32_t pad;
	} push {staging + offset, static_cast<uint32_t>(jobs.size()), 0};
	native.bindPipeline(vk::PipelineBindPoint::eCompute, m_scatter_pipeline);
	native.pushConstants(m_scatter_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
	native.dispatch(push.job_count, 1, 1);
	vk::MemoryBarrier after {};
	after.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                       vk::PipelineStageFlagBits::eAllCommands, vk::DependencyFlagBits::eByRegion,
	                       1, &after, 0, nullptr, 0, nullptr);
	return true;
}

// Each buffer's upload would record its own pair of barriers: record them once, around the
// copies of every buffer (hundreds per frame in GTA V).
struct BufferCache::UploadBatch {
	BufferCache& cache;
	explicit UploadBatch(BufferCache& owner): cache(owner) {
		cache.m_upload_batching   = true;
		cache.m_upload_batch_tick = cache.m_scheduler.CurrentTick();
	}
	~UploadBatch() { cache.FlushUploadBatch(); }
	UploadBatch(const UploadBatch&)            = delete;
	UploadBatch& operator=(const UploadBatch&) = delete;
};

void BufferCache::SynchronizeBuffersInSpans(std::span<const std::pair<uint64_t, uint64_t>> spans) {
	const UploadBatch batch {*this};
	for (const auto& [first, last]: spans) {
		SynchronizeBuffersInRange(first, last - first);
	}
}

void BufferCache::GrowRegionTables(uint64_t region) {
	if (region < m_region_buffer_count.size()) {
		return;
	}
	const auto words = region / 64 + 1;
	m_region_buffer_count.resize(words * 64, 0);
	m_synced_region_epochs.resize(words * 64, 0);
	m_buffer_region_bits.resize(words, 0);
	m_forced_region_bits.resize(words, 0);
}

void BufferCache::NoteBufferRegions(const Buffer& buffer, bool insert) {
	const auto first = buffer.CpuAddress() / TRACKER_REGION_SIZE;
	const auto last  = (buffer.CpuAddress() + buffer.Size() - 1) / TRACKER_REGION_SIZE;
	GrowRegionTables(last);
	for (auto index = first; index <= last; index++) {
		const auto bit = uint64_t {1} << (index % 64);
		if (insert) {
			// A new buffer may cover CPU-dirty pages that no earlier synchronization uploaded.
			m_region_buffer_count[index]++;
			m_buffer_region_bits[index / 64] |= bit;
			m_forced_region_bits[index / 64] |= bit;
		} else {
			EXIT_IF(m_region_buffer_count[index] == 0);
			if (--m_region_buffer_count[index] == 0) {
				m_buffer_region_bits[index / 64] &= ~bit;
			}
		}
	}
}

void BufferCache::ForceSynchronization(uint64_t vaddr, uint64_t size) {
	if (size == 0 || m_buffer_region_bits.empty()) {
		return;
	}
	const auto first = vaddr / TRACKER_REGION_SIZE;
	const auto last  = std::min<uint64_t>((vaddr + size - 1) / TRACKER_REGION_SIZE, m_region_buffer_count.size() - 1);
	for (auto index = first; index <= last; index++) {
		m_forced_region_bits[index / 64] |= uint64_t {1} << (index % 64);
	}
}

void BufferCache::SynchronizeBuffersInRanges(const RangeSet& ranges, bool all) {
	const UploadBatch batch {*this};
	// Read each region's epoch before scanning it: pages dirtied during or after the scan advance
	// it again, and the next call visits the region again. Regions without buffers have nothing
	// to upload; a buffer registered later forces its regions.
	m_sync_regions.clear();
	for (size_t word = 0; word < m_buffer_region_bits.size(); word++) {
		const auto forced = m_forced_region_bits[word];
		m_forced_region_bits[word] = 0;
		for (auto bits = m_buffer_region_bits[word]; bits != 0; bits &= bits - 1) {
			const auto index = word * 64 + static_cast<uint64_t>(std::countr_zero(bits));
			const auto epoch = m_memory_tracker.RegionCpuDirtyEpoch(index);
			// Synchronizing a buffer tracks its pages, so an untracked region (epoch 0) has no
			// pending write under a buffer unless the region was forced.
			if (!all && (forced & (uint64_t {1} << (index % 64))) == 0 &&
			    (epoch == 0 || m_synced_region_epochs[index] == epoch)) {
				continue;
			}
			m_sync_regions.emplace_back(index, epoch);
		}
	}
	for (const auto& [index, epoch]: m_sync_regions) {
		ranges.ForEachInRange(index * TRACKER_REGION_SIZE, TRACKER_REGION_SIZE,
		                      [this](uint64_t start, uint64_t end) {
			                      SynchronizeBuffersInRange(start, end - start);
		                      });
		m_synced_region_epochs[index] = epoch;
	}
}

void BufferCache::SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size) {
	// Only tracking regions with pending CPU writes can have anything to upload. Synchronizing a
	// buffer protects the pages it uploads again, and each protection call costs ~2.3 us (with
	// other threads of the process running, flushing the other processors' TLBs) besides ~0.1 us
	// per page. Runs a few clean pages apart are synchronized together: the page tracker then
	// protects them with one call through the clean pages between, which keep their protection.
	constexpr uint64_t MaxGap = 32 * TRACKER_PAGE_SIZE;
	m_memory_tracker.ForEachCpuDirtySpan(vaddr, size, MaxGap, [&](uint64_t span, uint64_t bytes) {
		const auto end = span + bytes;
		auto       it  = m_buffers.upper_bound(span);
		if (it != m_buffers.begin()) {
			--it;
		}
		for (; it != m_buffers.end() && it->first < end; ++it) {
			auto&      buffer = m_slot_buffers[it->second];
			const auto start  = std::max(buffer.CpuAddress(), span);
			const auto finish = std::min(buffer.CpuAddress() + buffer.Size(), end);
			if (start < finish) {
				(void)SynchronizeBuffer(buffer, start, finish - start, false, false);
			}
		}
	});
}

} // namespace Libs::Graphics
