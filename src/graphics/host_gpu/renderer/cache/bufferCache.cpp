#include "graphics/host_gpu/renderer/cache/bufferCache.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/logging/log.h"
#include "common/profiler.h"
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
constexpr uint64_t DownloadWindowSize = 512 * 1024;

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
		m_scheduler.DeferHostOperation([this, id] { m_slot_buffers.erase(id); });
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
			    // Start each packed range on its own 64-byte cache line.
			    total_size += Common::AlignUp(end - start, 64);
		    });
		    m_gpu_modified_ranges.Subtract(address, bytes);
		    ForgetColorMetadataChecks(address, bytes);
	    });
	if (copies.empty()) {
		return false;
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferDownloadBytes, total_size);

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
}

void BufferCache::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	m_memory_tracker.InvalidateRegion(vaddr, size,
	                                  [this, vaddr, size] { ReadMemory(vaddr, size, true); });
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
		InvalidateMemory(page, end - page);
		return;
	}
	InvalidateMemory(vaddr, 1);
}

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write) {
	if (!GuestGpu::IsGpuThread() && CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	// The guest reads (or writes) memory the GPU wrote only after a label the execution thread
	// writes once the work before it executed: the operations queued since cannot be what the
	// access needs, so the readback runs before them instead of after the whole backlog (a
	// frame of draws, 100-200 ms while an area streams in).
	m_scheduler.Context().GetGpu().SendUrgentCommandSync([this, vaddr, size, is_write] {
		if (is_write && !IsRegionRegistered(vaddr, size)) {
			return;
		}
		auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];
		if (IsRegionGpuModified(vaddr, size)) {
			const auto start =
			    std::max(Common::AlignDown(vaddr, DownloadWindowSize), buffer.CpuAddress());
			const auto end = std::min(std::max(start + DownloadWindowSize, vaddr + size),
			                          buffer.CpuAddress() + buffer.Size());
			if (DownloadBufferMemory<false>(buffer, start, end - start)) {
				m_memory_tracker.UnmarkRegionAsGpuModified(start, end - start);
			}
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
	if (source) {
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

void BufferCache::StreamGuestData(uint8_t* destination, uint64_t vaddr, uint64_t size) {
	// The execution thread hands larger copies to the host copy thread. It reads the backing
	// store, which holds the same bytes as the guest range and is never protected, so a copy that
	// runs after a later draw protected the range for GPU writes does not fault.
	constexpr uint64_t MinHostCopy = 256;
	auto*              copies      = m_scheduler.HostCopies();
	if (copies != nullptr && size >= MinHostCopy && m_stream_buffer.IsCoherent() &&
	    GuestGpu::IsGpuThread()) {
		if (const void* source = Libs::LibKernel::Memory::FindBackingPointer(vaddr, size)) {
			copies->Copy(destination, source, size);
			return;
		}
	}
	// Backing reads mostly hit the per-thread mapping cache; sparse ranges with holes or private
	// mappings take the full copy.
	if (!LibKernel::Memory::TryReadBacking(vaddr, destination, size) &&
	    !LibKernel::Memory::CopySparseMemory(vaddr, destination, size)) {
		EXIT("BufferCache: failed to read sparse guest buffer memory\n");
	}
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
		m_gpu_modified_ranges.Add(vaddr, size);
		ForgetColorMetadataChecks(vaddr, size);
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

bool BufferCache::HasOnlyGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	return m_gpu_modified_ranges.Contains(vaddr, size);
}

bool BufferCache::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionCpuModified(vaddr, size);
}

void BufferCache::RunGarbageCollector() {
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
		m_memory_tracker.MarkRegionAsGpuModifiedUnlessCpuDirty(
		    start, finish - start, [&](uint64_t page, uint64_t bytes) {
			    const auto first = std::max(page, start);
			    const auto last  = std::min(page + bytes, finish);
			    m_gpu_modified_ranges.Add(first, last - first);
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
