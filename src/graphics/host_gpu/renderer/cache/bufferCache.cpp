#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/gpuBreadcrumbs.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/logging/log.h"
#include "common/perfCounters.h"
#include "common/profiler.h"
#include "common/trace.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/pendingLabels.h"
#include "graphics/host_gpu/bufferChunk.h"
#include "graphics/host_gpu/cacheCollection.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/readbackPlan.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {

constexpr uint64_t MiB           = 1024 * 1024;
constexpr uint64_t GdsBufferSize = 64 * 1024;

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
		if (!buffer.IsGuestBacked()) {
			m_total_used_memory += buffer.Size();
		}
		buffer.lru_id = m_lru_cache.Insert(id, m_gc_tick);
		std::vector<vk::DeviceAddress> addresses;
		addresses.reserve(size_pages);
		for (uint64_t i = 0; i < size_pages; ++i) {
			addresses.push_back(buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS));
		}
		WriteDataBuffer(m_bda_pagetable_buffer, table_offset,
		                addresses.data(), addresses.size() * sizeof(vk::DeviceAddress));
	} else {
		const auto found = m_buffers.find(buffer.CpuAddress());
		EXIT_IF(found == m_buffers.end() || found->second != id);
		m_buffers.erase(found);
		if (!buffer.IsGuestBacked()) {
			EXIT_IF(buffer.Size() > m_total_used_memory);
			m_total_used_memory -= buffer.Size();
		}
		m_lru_cache.Free(buffer.lru_id);
		m_bda_pagetable_buffer.Fill(table_offset,
		                            size_pages * sizeof(vk::DeviceAddress), 0);
		buffer.is_deleted = true;
	}
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
	if (!buffer.is_deleted) {
		m_lru_cache.Touch(buffer.lru_id, m_gc_tick);
	}
}

void BufferCache::DeleteBuffer(BufferId id) {
	if (IsBufferInvalid(id)) {
		return;
	}
	Unregister(id);
	m_scheduler.DeferDestruction([this, id] { m_slot_buffers.erase(id); });
}

template <bool async>
bool BufferCache::DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	KYTY_TRACE_SCOPE("gpu", "DownloadBufferMemory");
	if (buffer.IsGuestBacked()) {
		// The GPU wrote guest memory itself; once its work completed there is nothing to copy.
		return ForgetGuestBackedWrites(vaddr, size);
	}
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
		    std::lock_guard lock(m_gpu_modified_mutex);
		    m_gpu_modified_ranges.Subtract(address, bytes);
		    ForgetColorMetadataChecks(address, bytes);
	    });
	if (copies.empty()) {
		return false;
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferDownloadBytes, total_size);

	PerfCounters::Add(PerfCounters::Counter::ReadbackBytes, total_size);
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
	InsertDebugLabel(native, "Download {} ranges, {} bytes, from buffer 0x{:x}+0x{:x}",
	                 copies.size(), total_size, buffer_address, buffer.Size());
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
	auto publish = [this, mapped, offset, total_size, buffer_address,
	                copies = std::move(copies), owner = std::move(temporary)] {
		(owner ? *owner : m_download_buffer).Invalidate(offset, total_size);
		for (const auto& copy: copies) {
			Libs::LibKernel::Memory::WriteBacking(buffer_address + copy.srcOffset,
			                                      mapped + (copy.dstOffset - offset), copy.size);
		}
	};
	if constexpr (async) {
		m_scheduler.DeferPriorityOperation(std::move(publish));
	} else {
		const auto tick = m_scheduler.CurrentTick();
		m_scheduler.Wait(tick);
		m_scheduler.WaitPriorityOperations(tick);
		publish();
		// A temporary download buffer is held by every command buffer committed while it lived,
		// also by the other submitters' (see queueCommits.h).
		m_scheduler.DeferDestruction([held = std::move(publish)] {});
	}
	return true;
}

BufferCache::BufferCache(GraphicContext& graphics, CommandScheduler& scheduler,
                         PageManager& page_manager, TextureCache& texture_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_import(graphics),
      m_fault_manager(graphics, scheduler, *this),
      m_gds_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, GdsBufferSize),
      m_bda_pagetable_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                             BDA_PAGETABLE_SIZE),
      m_memory_tracker(page_manager),
      m_staging_buffer(graphics, scheduler, MemoryUsage::Upload, 512 * MiB),
      m_stream_buffer(graphics, scheduler, MemoryUsage::Stream, 64 * MiB),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 64 * MiB),
      m_device_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 128 * MiB),
      m_texture_cache(texture_cache) {
	// Writes to open hot pages fault nowhere: a scan finds them (UffdWriteWatch). They are CPU
	// writes for the write watches and the device-address passes like the faulting ones.
	m_memory_tracker.SetArmedWriteHook([this](uint64_t vaddr, uint64_t size) {
		m_write_watches.NotifyWrite(vaddr, size);
		RecordGuestWrite(vaddr, size);
	});
	std::memset(m_gds_buffer.Mapped().data(), 0, static_cast<size_t>(m_gds_buffer.Size()));
	m_gds_buffer.Flush(0, m_gds_buffer.Size());
	GpuBreadcrumbs::Watch("GDS", reinterpret_cast<const uint32_t*>(m_gds_buffer.Mapped().data()),
	                      static_cast<uint32_t>(std::min<uint64_t>(m_gds_buffer.Size(), 4096u) / 4u));
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
	m_write_watches.NotifyWrite(vaddr, size);
	m_memory_tracker.InvalidateRegion(vaddr, size,
	                                  [this, vaddr, size] { ReadMemory(vaddr, size, true); });
	// After the dirty state is published: whoever takes this entry also sees the dirty bytes.
	RecordGuestWrite(vaddr, size);
}

void BufferCache::UnmapMemory(uint64_t vaddr, uint64_t size) {
	std::vector<BufferId> stale;
	const auto            end = vaddr + size;
	auto                  it  = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		--it;
	}
	for (; it != m_buffers.end() && it->first < end; ++it) {
		const auto& buffer = m_slot_buffers[it->second];
		if (buffer.IsGuestBacked() && buffer.CpuAddress() + buffer.Size() > vaddr) {
			stale.push_back(it->second);
		}
	}
	// As a demotion (DemoteGuestBackedBuffers): the GPU's writes are in guest memory already.
	for (const auto id: stale) {
		const auto& buffer  = m_slot_buffers[id];
		const auto  address = buffer.CpuAddress();
		const auto  bytes   = buffer.Size();
		(void)ForgetGuestBackedWrites(address, bytes);
		m_memory_tracker.UnmarkRegionAsGpuModified(address, bytes);
		m_memory_tracker.UntrackMemory(address, bytes);
		DeleteBuffer(id);
	}
}

void BufferCache::ReplaceMemory(uint64_t vaddr, const void* data, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid memory-replacement range\n");
	}
	// Guest memory that is a buffer itself takes each GPU write in place when the write runs: one
	// still queued would land over the new bytes, so it is waited for (as ReadBack waits).
	if (IsGuestBacked(vaddr, size)) {
		const auto current   = m_scheduler.CurrentTick();
		const bool unsettled = m_unsettled_everywhere ||
		                       std::ranges::any_of(m_unsettled_writers, [&](BufferId id) {
			                       if (IsBufferInvalid(id) || m_slot_buffers[id].is_deleted) {
				                       return false;
			                       }
			                       const auto& buffer = m_slot_buffers[id];
			                       return buffer.CpuAddress() < vaddr + size &&
			                              vaddr < buffer.CpuAddress() + buffer.Size();
		                       });
		const auto last_write =
		    unsettled ? current
		              : std::max(m_guest_backed_writes.LastWrite(vaddr, size), m_address_write_tick);
		if (last_write != 0 && !(last_write < current && m_scheduler.IsFree(last_write))) {
			if (last_write >= current) {
				m_scheduler.Flush();
			}
			m_scheduler.Wait(last_write);
			m_scheduler.WaitPriorityOperations(last_write);
		}
	}
	m_write_watches.NotifyWrite(vaddr, size);
	// What the GPU wrote in the range is about to be overwritten in full, so it is dropped: a
	// read-back of it would only make the caller wait for bytes nobody reads.
	{
		std::lock_guard lock(m_gpu_modified_mutex);
		m_gpu_modified_ranges.Subtract(vaddr, size);
	}
	m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, size);
	m_memory_tracker.InvalidateRegion(vaddr, size, [] {});
	LibKernel::Memory::WriteBacking(vaddr, data, size);
	// After the dirty state is published: whoever takes this entry also sees the dirty bytes.
	RecordGuestWrite(vaddr, size);
}

void BufferCache::InvalidateWrittenMemory(uint64_t fault_vaddr, bool window_is_mapped) {
	// Each fault costs a signal and a protection change, which is slow under Rosetta. A game that
	// rewrites buffers every frame faults on every page it touches, so one fault covers a window.
	// On a native host a fault is cheap next to what the window costs: every page of it is then
	// uploaded at its next use, on the GPU thread (Sonic Racing CrossWorlds uploaded four times the
	// bytes it wrote, and ran ~40% slower in races).
#if defined(__APPLE__)
	constexpr bool UseWindow = true;
#else
	constexpr bool UseWindow = false;
#endif
	constexpr uint64_t WindowSize = 64 * 1024;
	const auto         begin      = Common::AlignDown(fault_vaddr, WindowSize);
	if (UseWindow && window_is_mapped && GuestRange {begin, WindowSize}.Valid() &&
	    !m_memory_tracker.IsRegionGpuModified(begin, WindowSize)) {
		InvalidateMemory(begin, WindowSize);
		return;
	}
	InvalidateMemory(fault_vaddr, 1);
}

void BufferCache::NoteAddressWrites() noexcept {
	m_unsettled_everywhere = true;
}

void BufferCache::SettleGpuWrites() noexcept {
	for (const auto id: m_unsettled_writers) {
		if (!IsBufferInvalid(id) && !m_slot_buffers[id].is_deleted) {
			m_slot_buffers[id].NoteGpuWrite();
		}
	}
	m_unsettled_writers.clear();
	if (m_unsettled_everywhere) {
		m_unsettled_everywhere = false;
		m_address_write_tick   = m_scheduler.CurrentTick();
		// No buffer was noted for these writes, so the barrier is asked for here.
		m_scheduler.NoteWriteTheCpuReads();
	}
}

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write) {
	KYTY_PROFILER_FUNCTION();
	if (CommandScheduler::InCompletionOffGpuThread()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	// See ReadbackCallerCanWait: after a few waits the GPU thread does the waiting.
	const bool    on_other_thread = !GuestGpu::IsGpuThread();
	auto&         gpu             = m_scheduler.Context().GetGpu();
	for (int waits = 0;; ++waits) {
		std::optional<uint64_t> wait_tick;
		auto round = [&] {
			wait_tick = ReadBack(vaddr, size, is_write, ReadbackCallerCanWait(on_other_thread, waits));
		};
		// The GPU thread no longer takes work, or the device no longer answers: the emulator is
		// shutting down. This thread cannot go on without the page, and must not stop the
		// shutdown with an error, which would also lose the caches that are saved at exit. It
		// stays here until the process ends.
		const auto wait_for_exit = [] {
			for (;;) {
				std::this_thread::sleep_for(std::chrono::hours(1));
			}
		};
		bool sent = false;
		{
			KYTY_PROFILER_BLOCK("ReadMemory: on the GPU thread");
			sent = gpu.TrySendCommandSync(round);
		}
		if (!sent) {
			wait_for_exit();
		}
		if (!wait_tick) {
			return;
		}
		KYTY_PROFILER_BLOCK("ReadMemory: wait for the GPU");
		if (!m_scheduler.WaitSubmitted(*wait_tick)) {
			wait_for_exit();
		}
		// And for what earlier submissions write to guest memory when they finish, so that the
		// GPU thread finds nothing left to wait for when it publishes.
		m_scheduler.WaitPriorityOperations(*wait_tick);
	}
}

std::optional<uint64_t> BufferCache::ReadBack(uint64_t vaddr, uint64_t size, bool is_write,
                                              bool caller_can_wait) {
	if (is_write && !IsRegionRegistered(vaddr, size)) {
		return std::nullopt;
	}
	const auto id     = FindBuffer(vaddr, size);
	auto&      buffer = m_slot_buffers[id];

	const auto current = m_scheduler.CurrentTick();
	// A write that is prepared but not recorded yet counts as one in the open submission.
	const bool unsettled = m_unsettled_everywhere ||
	                       std::ranges::find(m_unsettled_writers, id) != m_unsettled_writers.end();
	const auto last_write =
	    unsettled ? current : std::max(buffer.LastGpuWriteTick(), m_address_write_tick);
	const auto plan       = PlanReadback({
	          .cpu_readable    = buffer.IsGuestBacked() ||
	                             (!m_read_back_through_gpu && !buffer.Mapped().empty()),
	          .last_write_tick = last_write,
	          .current_tick    = current,
	          .last_write_done = last_write < current && m_scheduler.IsFree(last_write),
	          .caller_can_wait = caller_can_wait,
    });
	if (plan.step == ReadbackStep::CopyThroughGpu) {
		if (DownloadBufferMemory<false>(buffer, vaddr, size)) {
			m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, size);
		}
	} else {
		if (plan.submit_first) {
			m_scheduler.Flush();
		}
		if (plan.step == ReadbackStep::CallerWaits) {
			return plan.tick;
		}
		m_scheduler.Wait(plan.tick);
		// What earlier submissions write to guest memory when they finish comes first.
		m_scheduler.WaitPriorityOperations(plan.tick);
		if (PublishFromBufferMemory(buffer, vaddr, size)) {
			m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, size);
		}
	}
	if (is_write) {
		m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
	}
	return std::nullopt;
}

bool BufferCache::PublishFromBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	if (buffer.IsGuestBacked()) {
		return ForgetGuestBackedWrites(vaddr, size);
	}
	bool       published = false;
	const auto memory    = buffer.Mapped();
	m_memory_tracker.ForEachDownloadRange<false>(
	    vaddr, size, [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "buffer read-back");
		    m_gpu_modified_ranges.ForEachInRange(address, bytes, [&](uint64_t start, uint64_t end) {
			    const auto offset = buffer.Offset(start);
			    buffer.Invalidate(offset, end - start);
			    Libs::LibKernel::Memory::WriteBacking(start, memory.data() + offset, end - start);
			    published = true;
		    });
		    std::lock_guard lock(m_gpu_modified_mutex);
		    m_gpu_modified_ranges.Subtract(address, bytes);
	    });
	return published;
}

bool BufferCache::MarkCopyOnly(uint64_t vaddr, uint64_t size) {
	const auto begin = Common::AlignDown(vaddr, CACHING_PAGESIZE);
	const auto end   = Common::AlignUp(vaddr + size, CACHING_PAGESIZE);
	m_copy_only_marked.Add(begin, end - begin);
	if (m_copy_only.Contains(begin, end - begin)) {
		return false;
	}
	m_copy_only.Add(begin, end - begin);
	return true;
}

void BufferCache::ExpireCopyOnly() {
	if (++m_copy_only_age < CopyOnlyGeneration) {
		return;
	}
	m_copy_only_age = 0;
	RangeSet expired;
	m_copy_only.ForEach([&](uint64_t begin, uint64_t end) { expired.Add(begin, end - begin); });
	m_copy_only_marked.ForEach(
	    [&](uint64_t begin, uint64_t end) { expired.Subtract(begin, end - begin); });
	m_copy_only = std::move(m_copy_only_marked);
	m_copy_only_marked.Clear();
	if (expired.Empty() || !m_import.Enabled()) {
		return;
	}
	// The copies over expired pages that hold nothing the GPU wrote: guest memory has their
	// bytes, and their next use makes them guest memory itself (CreateBuffer). As the garbage
	// collector, at a frame boundary, when no draw being prepared refers to them.
	std::vector<BufferId> promote;
	expired.ForEach([&](uint64_t begin, uint64_t end) {
		auto it = m_buffers.upper_bound(begin);
		if (it != m_buffers.begin()) {
			--it;
		}
		for (; it != m_buffers.end() && it->first < end; ++it) {
			const auto& buffer = m_slot_buffers[it->second];
			if (buffer.CpuAddress() + buffer.Size() > begin) {
				promote.push_back(it->second);
			}
		}
	});
	PromoteCopies(promote);
}

bool BufferCache::CanPromote(BufferId id) {
	if (IsBufferInvalid(id)) {
		return false;
	}
	const auto& buffer  = m_slot_buffers[id];
	const auto  address = buffer.CpuAddress();
	const auto  bytes   = buffer.Size();
	return !buffer.IsGuestBacked() && !buffer.is_deleted && !m_copy_only.Intersects(address, bytes) &&
	       !m_unsettled_everywhere &&
	       std::ranges::find(m_unsettled_writers, id) == m_unsettled_writers.end() &&
	       m_import.CanPlace(address, bytes);
}

void BufferCache::PromoteCopies(std::vector<BufferId>& ids) {
	std::ranges::sort(ids);
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	for (const auto id: ids) {
		if (!CanPromote(id)) {
			continue;
		}
		const auto& buffer  = m_slot_buffers[id];
		const auto  address = buffer.CpuAddress();
		const auto  bytes   = buffer.Size();
		// Only when the buffer its next use makes can be guest memory itself: a copy next to one
		// that can't be would be made again.
		const auto plan = PlanBuffer(address, bytes);
		if (m_copy_only.Intersects(plan.begin, plan.end - plan.begin) ||
		    !m_import.CanPlace(plan.begin, plan.end - plan.begin)) {
			continue;
		}
		if (!m_memory_tracker.IsRegionGpuModified(address, bytes)) {
			m_memory_tracker.UntrackMemory(address, bytes);
			DeleteBuffer(id);
			continue;
		}
		// The copy holds what the GPU wrote: a new buffer over it that is guest memory itself
		// takes those bytes over on the GPU (JoinOverlap).
		(void)CreateBuffer(address, bytes);
	}
	ids.clear();
}

void BufferCache::ClipToCopyOnly(uint64_t request_begin, uint64_t request_end, uint64_t& begin,
                                 uint64_t& end) const {
	if (m_copy_only.Empty()) {
		return;
	}
	bool     touches = false;
	uint64_t low     = request_begin;
	uint64_t high    = request_end;
	m_copy_only.ForEachInRange(begin, end - begin, [&](uint64_t from, uint64_t to) {
		if (from < request_end && request_begin < to) {
			touches = true;
			low     = std::min(low, from);
			high    = std::max(high, to);
		}
	});
	if (touches) {
		begin = std::max(begin, Common::AlignDown(low, CACHING_PAGESIZE));
		end   = std::min(end, Common::AlignUp(high, CACHING_PAGESIZE));
		return;
	}
	// Copy-only ranges are whole pages, and the request is outside them.
	m_copy_only.ForEachInRange(begin, end - begin, [&](uint64_t from, uint64_t to) {
		if (to <= request_begin) {
			begin = std::max(begin, to);
		} else if (from >= request_end) {
			end = std::min(end, from);
		}
	});
}

void BufferCache::DemoteGuestBackedBuffers() {
	if (m_demote.empty()) {
		return;
	}
	std::vector<BufferId> demote;
	for (const auto id: m_demote) {
		if (!IsBufferInvalid(id) && !m_slot_buffers[id].is_deleted &&
		    m_slot_buffers[id].IsGuestBacked()) {
			demote.push_back(id);
		}
	}
	m_demote.clear();
	if (demote.empty()) {
		return;
	}
	// The GPU's writes to them are in guest memory once the work so far completes; the copies
	// that take their place start from guest memory, as new buffers do.
	m_scheduler.Finish();
	for (const auto id: demote) {
		const auto& buffer  = m_slot_buffers[id];
		const auto  address = buffer.CpuAddress();
		const auto  bytes   = buffer.Size();
		(void)ForgetGuestBackedWrites(address, bytes);
		m_memory_tracker.UnmarkRegionAsGpuModified(address, bytes);
		m_memory_tracker.UntrackMemory(address, bytes);
		DeleteBuffer(id);
	}
}

bool BufferCache::ForgetGuestBackedWrites(uint64_t vaddr, uint64_t size) {
	// By pages, as a read-back of a copy publishes them: the tracker owns whole pages.
	bool written = false;
	m_memory_tracker.ForEachDownloadRange<false>(
	    vaddr, size, [&](uint64_t address, uint64_t bytes) noexcept {
		    {
			    std::lock_guard lock(m_gpu_modified_mutex);
			    m_gpu_modified_ranges.Subtract(address, bytes);
		    }
		    written = true;
	    });
	return written;
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
	if (new_buffer.IsGuestBacked()) {
		// No copy notes a write here: the GPU writes the old buffer still has in flight are the new
		// buffer's, or a CPU access of their pages would not wait for them (ReadBack). A write that
		// is prepared but not recorded yet stays unsettled under the new buffer.
		new_buffer.InheritGpuWrite(overlap.LastGpuWriteTick());
		if (std::ranges::find(m_unsettled_writers, overlap_id) != m_unsettled_writers.end()) {
			if (m_unsettled_writers.size() < MaxUnsettledWriters) {
				m_unsettled_writers.push_back(new_id);
			} else {
				m_unsettled_everywhere = true;
			}
		}
		// Guest memory holds the bytes already, except those only a copy of it holds: what the
		// GPU wrote to a buffer that was a copy.
		if (!overlap.IsGuestBacked()) {
			m_gpu_modified_ranges.ForEachInRange(
			    overlap.CpuAddress(), overlap.Size(), [&](uint64_t start, uint64_t end) {
				    new_buffer.CopyFrom(m_scheduler.Current(), overlap, overlap.Offset(start),
				                        new_buffer.Offset(start), end - start);
			    });
		}
	} else {
		new_buffer.CopyFrom(m_scheduler.Current(), overlap, 0,
		                    overlap.CpuAddress() - new_buffer.CpuAddress(), overlap.Size());
	}
	DeleteBuffer(overlap_id);
}

BufferCache::OverlapResult BufferCache::PlanBuffer(uint64_t vaddr, uint64_t size) {
	// The whole chunk around the first use (see bufferChunk.h).
	const auto chunk =
	    ChunkRange({vaddr, vaddr + size}, m_scheduler.Context().MappingAt(vaddr), s_chunk_size);
	auto begin = Common::AlignDown(chunk.begin, CACHING_PAGESIZE);
	auto end   = Common::AlignUp(chunk.end, CACHING_PAGESIZE);
	// A buffer that is guest memory itself must be one piece of direct memory: the part of the
	// chunk mapped in one piece around the request.
	uint64_t piece_begin = 0;
	uint64_t piece_end   = 0;
	if (m_import.Enabled() &&
	    LibKernel::Memory::ContiguousDirectMemory(vaddr, size, begin, end, &piece_begin,
	                                              &piece_end)) {
		piece_begin = Common::AlignUp(piece_begin, CACHING_PAGESIZE);
		piece_end   = Common::AlignDown(piece_end, CACHING_PAGESIZE);
		if (piece_begin <= vaddr && vaddr + size <= piece_end) {
			begin = piece_begin;
			end   = piece_end;
		}
	}
	ClipToCopyOnly(vaddr, vaddr + size, begin, end);
	return ResolveOverlaps(begin, end - begin);
}

BufferId BufferCache::CreateBuffer(uint64_t vaddr, uint64_t size) {
	EXIT_IF(m_scheduler.Current().IsInvalid());
	const auto overlap = PlanBuffer(vaddr, size);

	const auto flags     = AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress;
	const auto placement = m_copy_only.Intersects(overlap.begin, overlap.end - overlap.begin)
	                           ? std::nullopt
	                           : m_import.Place(overlap.begin, overlap.end - overlap.begin);
	const auto id =
	    placement ? m_slot_buffers.insert(m_graphics, m_scheduler, overlap.begin, flags,
	                                      overlap.end - overlap.begin, placement->memory,
	                                      placement->offset)
	              : m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::Guest,
	                                      overlap.begin, flags, overlap.end - overlap.begin);
	const auto& buffer = m_slot_buffers[id];
	SetVulkanObjectNameF(m_graphics.device, buffer.Handle(),
	                     "Kyty.GameBuffer[guest=0x{:016x} size=0x{:x}]", overlap.begin,
	                     overlap.end - overlap.begin);
	for (auto it = overlap.first; it != overlap.last;) {
		const auto old_id = (it++)->second;
		JoinOverlap(id, old_id, !overlap.has_stream_leap);
	}
	Register(id);
	if (!placement) {
		// A new buffer starts with guest bytes the GPU has not seen: it counts as a CPU write.
		RecordGuestWrite(overlap.begin, overlap.end - overlap.begin);
	}
	return id;
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_written,
                                    bool is_texel_buffer) {
	KYTY_PROFILER_FUNCTION();
	if (buffer.IsGuestBacked()) {
		if (is_written) {
			// Nothing to upload, but the pages become the GPU's: a CPU access waits for the
			// writes to complete (ReadBack), and then finds them in guest memory.
			m_memory_tracker.ForEachUploadRange(
			    vaddr, size, true, [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
			return false;
		}
		return is_texel_buffer && SynchronizeBufferFromImage(buffer, vaddr, size);
	}
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
	if (total_size != 0 && m_import.Enabled() && m_uploading_copies.size() < MaxUploadingCopies &&
	    !m_copy_only.Intersects(buffer.CpuAddress(), buffer.Size()) &&
	    m_import.CanPlace(buffer.CpuAddress(), buffer.Size())) {
		if (const auto* owner = m_page_table.Find(buffer.CpuAddress() >> PageTable::kPageBits);
		    owner != nullptr && *owner && &m_slot_buffers[*owner] == &buffer) {
			m_uploading_copies.push_back(*owner);
		}
	}
	if (!is_written) {
		// Hot pages stay dirty and open after an upload (see writeHeat.h), so the CPU's next
		// writes to them neither fault nor reach the log. The uploaded ranges that are still
		// dirty hold them: uploaded again through device addresses in the next guest submission.
		for (const auto& copy: copies) {
			const auto address = buffer.CpuAddress() + copy.dstOffset;
			if (m_memory_tracker.IsRegionCpuModified(address, copy.size)) {
				m_guest_write_log.RecordHot({address, copy.size});
			}
		}
	}
	if (source) {
		buffer.NoteGpuWrite();
		auto& command = m_scheduler.Current();
		command.EndRendering();
		const auto native = command.Handle();
		InsertDebugLabel(native, "Upload {} ranges, {} bytes, to buffer 0x{:x}+0x{:x}",
		                 copies.size(), total_size, buffer.CpuAddress(), buffer.Size());
		if (m_upload_batch_open) {
			if (!m_upload_batch_started) {
				m_upload_batch_started = true;
				vk::MemoryBarrier before_all {};
				before_all.srcAccessMask =
				    vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
				    vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite;
				before_all.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
				native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
				                       vk::PipelineStageFlagBits::eTransfer, {}, 1, &before_all, 0,
				                       nullptr, 0, nullptr);
			}
			native.copyBuffer(source, buffer.Handle(), static_cast<uint32_t>(copies.size()),
			                  copies.data());
		} else {
			vk::BufferMemoryBarrier before {};
			before.srcAccessMask =
			    vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
			    vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite;
			before.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
			before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			before.buffer              = buffer.Handle();
			before.offset              = 0;
			before.size                = buffer.Size();
			native.pipelineBarrier(
			    vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
			    vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &before, 0, nullptr);
			native.copyBuffer(source, buffer.Handle(), static_cast<uint32_t>(copies.size()),
			                  copies.data());
			auto after          = before;
			after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
			after.dstAccessMask =
			    vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
			native.pipelineBarrier(
			    vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
			    vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr);
		}
	}
	if (is_texel_buffer && !is_written) {
		return SynchronizeBufferFromImage(buffer, vaddr, size);
	}
	return false;
}

void BufferCache::BeginUploadBatch() {
	EXIT_IF(m_upload_batch_open);
	m_upload_batch_open    = true;
	m_upload_batch_started = false;
}

void BufferCache::EndUploadBatch() {
	EXIT_IF(!m_upload_batch_open);
	m_upload_batch_open = false;
	RunBatchCopies();
	if (!m_upload_batch_started) {
		return;
	}
	// A wrap of the staging buffer can submit in the middle of a batch. A barrier orders all
	// earlier commands of the queue, so the one recorded here still covers the copies before it.
	vk::MemoryBarrier after_all {};
	after_all.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after_all.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	m_scheduler.Current().Handle().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                                               vk::PipelineStageFlagBits::eAllCommands, {}, 1,
	                                               &after_all, 0, nullptr, 0, nullptr);
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     uint64_t total_size) {
	Trace::AddKeyed("upload_buffer", buffer.CpuAddress(), total_size);
	if (copies.empty()) {
		return nullptr;
	}
	PerfCounters::Add(PerfCounters::Counter::UploadBytes, total_size);
	PerfCounters::Add(PerfCounters::Counter::UploadCopies, copies.size());
	Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferUploadBytes, total_size);

	auto [mapped, base_offset] = MapStaging(total_size);
	if (mapped != nullptr) {
		for (auto& copy: copies) {
			const auto* source = reinterpret_cast<const void*>(buffer.CpuAddress() + copy.dstOffset);
			if (m_upload_batch_open) {
				m_batch_copies.push_back({mapped + copy.srcOffset, source, copy.size});
			} else {
				CopyGuestMemory(mapped + copy.srcOffset, source, copy.size);
			}
			copy.srcOffset += base_offset;
		}
		if (m_upload_batch_open && !m_staging_buffer.IsCoherent()) {
			m_batch_flushes.push_back({.offset = base_offset, .size = total_size});
		}
		m_staging_buffer.Commit();
		return m_staging_buffer.Handle();
	}

	auto temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Upload, 0,
	                                         vk::BufferUsageFlagBits::eTransferSrc, total_size);
	for (const auto& copy: copies) {
		CopyGuestMemory(temporary->Mapped().data() + copy.srcOffset,
		                reinterpret_cast<const void*>(buffer.CpuAddress() + copy.dstOffset),
		                copy.size);
	}
	temporary->Flush(0, total_size);
	const auto handle = temporary->Handle();
	m_scheduler.DeferOperation([owner = std::move(temporary)]() mutable { owner.reset(); });
	return handle;
}

void BufferCache::CopyGuestMemory(void* destination, const void* source, size_t size) {
	if (!LibKernel::Memory::CopySparseMemory(reinterpret_cast<uint64_t>(source), destination,
	                                         size)) {
		EXIT("BufferCache: failed to read sparse guest buffer memory\n");
	}
}

std::pair<uint8_t*, uint64_t> BufferCache::MapStaging(uint64_t size) {
	// A map can wait for the GPU, and a wait for the open command buffer submits it. The batch's
	// GPU copies would then read staging bytes not copied yet, so a map that has to wait runs the
	// batch's copies first.
	if (!m_batch_copies.empty()) {
		if (const auto mapped = m_staging_buffer.Map(size, 4, false); mapped.first != nullptr) {
			return mapped;
		}
		RunBatchCopies();
	}
	return m_staging_buffer.Map(size, 4);
}

void BufferCache::RunBatchCopies() {
	if (m_batch_copies.empty()) {
		return;
	}
	KYTY_PROFILER_BLOCK("UploadCopies: batch copy");
	m_copy_workers.Run(m_batch_copies);
	m_batch_copies.clear();
	for (const auto& flush: m_batch_flushes) {
		m_staging_buffer.Flush(flush.offset, flush.size);
	}
	m_batch_flushes.clear();
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer(uint64_t vaddr, uint64_t size,
                                                       bool is_written, bool is_texel_buffer,
                                                       BufferId id) {
	// A batch copies guest bytes when it ends. A written buffer protects its pages before that,
	// and the copy would fault on its own page (see BeginUploadBatch).
	EXIT_IF(m_upload_batch_open);
	auto& command = m_scheduler.Current();
	if (command.IsInvalid() || !GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: buffer request requires a recording command buffer\n");
	}

	// A read-only range the CPU wrote and the GPU has not: copy it into the stream buffer on the
	// CPU instead of recording a GPU copy. A GPU copy ends the current render pass, and on a
	// tile-based GPU every new pass loads and stores its whole targets. A draw-heavy frame can
	// issue thousands of such copies, nearly all under 64 KiB. A formatted buffer over one page
	// keeps the full path, which takes its bytes from an image when an image holds them.
	constexpr uint64_t StreamUploadLimit = 4 * CACHING_PAGESIZE;
	// Most ranges a draw reads are clean: asked once, first, it saves the other queries and the
	// synchronization below, which would find nothing to upload.
	const bool cpu_modified = !is_written && m_memory_tracker.IsRegionCpuModified(vaddr, size);
	if (cpu_modified &&
	    (size <= CACHING_PAGESIZE || (!is_texel_buffer && size <= StreamUploadLimit)) &&
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size) && !IsGuestBacked(vaddr, size)) {
		const auto alignment = std::max<uint64_t>(
		    m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 1);
		auto [mapped, offset] = m_stream_buffer.Map(size, alignment, false);
		Trace::AddKeyed("upload_stream", vaddr, size);
		if (mapped != nullptr) {
			if (!LibKernel::Memory::CopySparseMemory(vaddr, mapped, size)) {
				EXIT("BufferCache: failed to read sparse guest buffer memory\n");
			}
			if (m_labels != nullptr) {
				for (const auto& label: m_labels->PendingForGpu(vaddr, vaddr + size)) {
					const auto* bytes = reinterpret_cast<const uint8_t*>(&label.value);
					for (uint32_t i = 0; i < label.bytes; i++) {
						if (label.address + i >= vaddr && label.address + i < vaddr + size) {
							static_cast<uint8_t*>(mapped)[label.address + i - vaddr] = bytes[i];
						}
					}
				}
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
	// A clean range read by a plain buffer has nothing to upload (a texel buffer can take its
	// bytes from an image).
	if (is_written || cpu_modified || is_texel_buffer) {
		(void)SynchronizeBuffer(buffer, vaddr, size, is_written, is_texel_buffer);
	}
	if (m_labels != nullptr) {
		ApplyPendingLabels(buffer, vaddr, size);
		if (is_written) {
			m_labels->WrittenByGpu(vaddr, vaddr + size);
		}
	}
	if (is_written && buffer.IsGuestBacked()) {
		m_guest_backed_writes.Add(vaddr, size, m_scheduler.CurrentTick());
		if (is_texel_buffer && MarkCopyOnly(vaddr, size)) {
			m_demote.push_back(id);
		}
	}
	if (is_written) {
		if (m_unsettled_writers.size() < MaxUnsettledWriters) {
			m_unsettled_writers.push_back(id);
		} else {
			m_unsettled_everywhere = true;
		}
		m_write_watches.NotifyWrite(vaddr, size);
		// A draw that writes the same buffer as the last one adds nothing: clean pages stay known.
		if (!m_gpu_modified_ranges.Contains(vaddr, size)) {
			std::lock_guard lock(m_gpu_modified_mutex);
			m_gpu_modified_ranges.Add(vaddr, size);
			m_gpu_modified_version++;
		}
		ForgetColorMetadataChecks(vaddr, size);
	}
	return {&buffer, buffer.Offset(vaddr)};
}

void BufferCache::ApplyPendingLabels(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	// A label reaches guest memory when the host GPU completed the work before it, which can be
	// after commands recorded later read the memory: FC 25 resets an append counter with a label
	// and loads it into GDS in the next submission, and read the stale count (or bytes another
	// use left there) until a draw of billions of vertices hung the GPU.
	const auto labels = m_labels->PendingForGpu(vaddr, vaddr + size);
	if (labels.empty()) {
		return;
	}
	m_scheduler.Current().EndRendering();
	for (const auto& label: labels) {
		for (uint32_t at = 0; at < label.bytes; at += sizeof(uint32_t)) {
			const auto address = label.address + at;
			if ((address & 3u) != 0 || !buffer.IsInBounds(address, sizeof(uint32_t))) {
				continue;
			}
			uint32_t word = 0;
			std::memcpy(&word, reinterpret_cast<const uint8_t*>(&label.value) + at, sizeof(word));
			buffer.Fill(buffer.Offset(address), sizeof(word), word);
		}
	}
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size,
                                                               uint64_t skip_prefix) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid image source\n");
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
		skip_prefix = std::min(skip_prefix, size);
		if (!Libs::LibKernel::Memory::TryReadSparseBacking(vaddr + skip_prefix, mapped + skip_prefix,
		                                                   size - skip_prefix)) {
			EXIT("BufferCache: failed to read mapped guest image backing\n");
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
	(void)m_texture_cache.ClearMeta(vaddr);
	// Guest memory that is a buffer itself is filled in GPU order: the work recorded before the
	// fill has not run yet, and must not see the new bytes.
	if (!IsRegionGpuModified(vaddr, size) && !IsGuestBacked(vaddr, size)) {
		// Access the guest mapping so write faults invalidate cached buffers and images.
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
	// As a fill: a copy to or from guest memory that is a buffer itself runs in GPU order.
	if (src_memory && dst_memory && !IsRegionGpuModified(dst_vaddr, size) &&
	    !IsRegionGpuModified(src_vaddr, size) && !IsGuestBacked(dst_vaddr, size) &&
	    !IsGuestBacked(src_vaddr, size) && !m_texture_cache.FindImageFromRange(src_vaddr, size) &&
	    (m_labels == nullptr || (m_labels->PendingForGpu(src_vaddr, src_vaddr + size).empty() &&
	                             m_labels->PendingForGpu(dst_vaddr, dst_vaddr + size).empty()))) {
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
	// Memory that GDS is saved to, the counters of append buffers, is also reset by the CPU
	// before the next submission loads it back (FC 25). Where the buffer is guest memory itself,
	// the CPU's zero lands at once and the save, still queued on a GPU that runs further behind
	// than the PS5's, overwrites it: the counters grew every frame until a draw or dispatch of
	// millions of elements hung the GPU. As a copy, a CPU write to it waits for the save and
	// reaches the GPU in GPU order.
	if (src_gds && dst->IsGuestBacked() && MarkCopyOnly(dst_vaddr, size)) {
		m_demote.push_back(FindBuffer(dst_vaddr, size)); // obtaining it can have joined buffers
	}
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

bool BufferCache::IsGuestBacked(uint64_t vaddr, uint64_t size) {
	if (!m_import.Enabled() || vaddr == 0) {
		return false;
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		const auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			return buffer.IsGuestBacked();
		}
	}
	return !m_copy_only.Intersects(vaddr, size) && m_import.CanPlace(vaddr, size);
}

bool BufferCache::ReadGuestBacked(uint64_t vaddr, void* data, uint64_t size) {
	if (!m_import.Enabled() || vaddr == 0) {
		return false;
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner == nullptr || !*owner) {
		return false;
	}
	const auto& buffer = m_slot_buffers[*owner];
	if (!buffer.IsGuestBacked() || !buffer.IsInBounds(vaddr, size)) {
		return false;
	}
	// Only the writes of these bytes: a command processor that misses a write it waits for
	// (through device addresses) waits again.
	const auto last_write = m_guest_backed_writes.LastWrite(vaddr, size);
	if (last_write != 0 && !m_scheduler.IsFree(last_write)) {
		if (last_write >= m_scheduler.CurrentTick()) {
			m_scheduler.Flush();
		}
		m_scheduler.Wait(last_write);
	}
	return LibKernel::Memory::TryReadBacking(vaddr, data, size);
}

bool BufferCache::WriteGuestBacked(uint64_t vaddr, const void* data, uint64_t size) {
	if (!m_import.Enabled() || vaddr == 0) {
		return false;
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner == nullptr || !*owner) {
		return false;
	}
	const auto& buffer = m_slot_buffers[*owner];
	if (!buffer.IsGuestBacked() || !buffer.IsInBounds(vaddr, size)) {
		return false;
	}
	m_write_watches.NotifyWrite(vaddr, size);
	return LibKernel::Memory::TryWriteBacking(vaddr, data, size);
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

void BufferCache::RecordGuestWrite(uint64_t vaddr, uint64_t size) {
	m_guest_write_log.Record({vaddr, size});
}

void BufferCache::RecordGpuThreadWrite(uint64_t vaddr, uint64_t size) {
	RecordGuestWrite(vaddr, size);
}

void BufferCache::RecordMappingChange(uint64_t vaddr, uint64_t size) {
	RecordGuestWrite(vaddr, size);
}

void BufferCache::BeginSubmission() {
	m_guest_write_log.BeginSubmission();
	// The open hot pages are scanned again where their dirty state is next read.
	RegionManager::NoteSubmission();
}

bool BufferCache::TakeGuestWrites(std::vector<GuestRange>& ranges) {
	// Device addresses can reach any buffer: every due armed page is scanned first.
	m_memory_tracker.HarvestArmedWrites();
	return m_guest_write_log.Take(ranges);
}

void BufferCache::RequestFullSynchronization() {
	m_guest_write_log.RequestFullPass();
}

bool BufferCache::HasGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	// Shader resource walks ask word by word, mostly within a few pages. A page found clean stays
	// clean until a GPU write is added: removals only clean more.
	const uint64_t page     = vaddr >> CleanPageBits;
	const bool     one_page = ((vaddr + std::max<uint64_t>(size, 1) - 1) >> CleanPageBits) == page;
	auto&          known    = m_clean_pages[page % m_clean_pages.size()];
	if (one_page && known.page == page + 1 && known.version == m_gpu_modified_version) {
		return false;
	}
	const bool dirty = m_gpu_modified_ranges.Intersects(vaddr, size);
	// The ranges are bytes: a clean range does not make its page clean.
	if (one_page && !dirty &&
	    !m_gpu_modified_ranges.Intersects(page << CleanPageBits, uint64_t {1} << CleanPageBits)) {
		known = {page + 1, m_gpu_modified_version};
	}
	return dirty;
}

bool BufferCache::HasGpuDirtyBytesShared(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_gpu_modified_mutex);
	return m_gpu_modified_ranges.Intersects(vaddr, size);
}

bool BufferCache::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionCpuModified(vaddr, size);
}

void BufferCache::RunGarbageCollector() {
	KYTY_PROFILER_FUNCTION();
	DemoteGuestBackedBuffers();
	ExpireCopyOnly();
	PromoteCopies(m_uploading_copies);
	m_guest_backed_writes.ForgetCompleted(m_scheduler.GetMasterSemaphore().KnownGpuTick());
	const auto tick = m_gc_tick++;
	if (m_graphics.CanReportMemoryUsage()) {
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
	}
	PerfCounters::Set(PerfCounters::Gauge::LiveBuffers, static_cast<int64_t>(m_buffers.size()));
	PerfCounters::Set(PerfCounters::Gauge::DeviceMemoryMiB,
	                  static_cast<int64_t>(m_total_used_memory >> 20u));
	// Buffers are collected only for memory, never because they are idle (see cacheCollection.h):
	// the plan is asked as for a GPU with memory of its own.
	const auto kind = PlanCollection({.used_memory    = m_total_used_memory,
	                                  .trigger_memory = m_trigger_gc_memory,
	                                  .system_memory  = false,
	                                  .passes         = tick});
	if (kind == CollectionKind::None) {
		return;
	}

	const bool     aggressive = m_total_used_memory >= m_critical_gc_memory;
	const uint64_t age        = CollectionAge(kind, aggressive ? 80 : 160, tick);
	const size_t   limit      = aggressive ? 64 : 32;

	std::vector<BufferId> dirty_buffers;
	size_t                retire_count = 0;
	m_lru_cache.ForEachItemBelow(tick - age, [&](BufferId id) {
		auto& buffer = m_slot_buffers[id];
		EXIT_IF(buffer.is_deleted);
		if (buffer.IsGuestBacked()) {
			// Costs no device memory.
			return false;
		}
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, buffer.CpuAddress(),
		                                           buffer.Size(), "garbage collection");
		const bool dirty = m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size());
		if (dirty && !aggressive) {
			return false;
		}
		if (dirty) {
			EXIT_NOT_IMPLEMENTED(!DownloadBufferMemory<true>(buffer, buffer.CpuAddress(), buffer.Size()));
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
		DeleteBuffer(id);
	}
}

void BufferCache::ProcessFaultBuffer() {
	m_fault_manager.ProcessFaultBuffer();
}

void BufferCache::SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size) {
	// Most buffers of a range hold nothing the CPU wrote; for them a synchronization uploads
	// nothing and only costs its setup.
	if (!m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
		return;
	}
	const auto end = vaddr + size;
	auto       it  = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		--it;
	}
	for (; it != m_buffers.end() && it->first < end; ++it) {
		auto& buffer = m_slot_buffers[it->second];
		if (buffer.IsGuestBacked()) {
			continue;
		}
		const auto start  = std::max(buffer.CpuAddress(), vaddr);
		const auto finish = std::min(buffer.CpuAddress() + buffer.Size(), end);
		if (start < finish && m_memory_tracker.IsRegionCpuModified(start, finish - start)) {
			(void)SynchronizeBuffer(buffer, start, finish - start, false, false);
		}
	}
}

} // namespace Libs::Graphics
