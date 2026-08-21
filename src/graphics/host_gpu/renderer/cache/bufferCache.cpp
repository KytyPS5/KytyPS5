#include "graphics/host_gpu/renderer/cache/bufferCache.h"

#include "common/assert.h"
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
#include <array>
#include <cinttypes>
#include <cstring>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {

constexpr uint64_t MiB           = 1024 * 1024;
constexpr uint64_t GdsBufferSize = 64 * 1024;

} // namespace

void BufferCache::WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source,
                                  uint64_t size) {
	auto* bytes = static_cast<const uint8_t*>(source);
	while (size != 0) {
		const auto chunk  = std::min(size, m_staging_buffer.Size());
		const auto offset = m_staging_buffer.Copy(bytes, chunk, 4);
		buffer.CopyFrom(m_scheduler.Current(), m_staging_buffer, offset, buffer.Offset(address),
		                chunk, vk::AccessFlagBits::eHostWrite);
		bytes += chunk;
		address += chunk;
		size -= chunk;
	}
}

struct BufferCache::DownloadCopy {
	Buffer*  buffer        = nullptr;
	uint64_t source_offset = 0;
	uint64_t address       = 0;
	uint64_t size          = 0;
};

struct BufferCache::DownloadRange {
	uint64_t address = 0;
	uint64_t size    = 0;
	uint64_t offset  = 0;
};

void BufferCache::Unregister(BufferId id) {
	auto& buffer = m_slot_buffers[id];
	if (buffer.is_deleted) {
		return;
	}
	const auto found = m_buffers.find(buffer.CpuAddress());
	EXIT_IF(found == m_buffers.end() || found->second != id);
	m_buffers.erase(found);
	if (buffer.Size() > m_total_used_memory) {
		EXIT("BufferCache: allocation accounting underflow\n");
	}
	m_total_used_memory -= buffer.Size();
	buffer.is_deleted = true;
}

void BufferCache::DeleteBuffer(BufferId id) {
	auto* buffer = m_slot_buffers.try_get(id);
	if (buffer == nullptr || buffer->is_deleted) {
		return;
	}
	Unregister(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_buffers.erase(id); });
	} else {
		m_slot_buffers.erase(id);
	}
}

std::pair<uint64_t, uint64_t> BufferCache::DownloadEnvelope(const DownloadCopy& copy) {
	if (copy.buffer == nullptr || copy.size == 0 || copy.source_offset > copy.buffer->Size() ||
	    copy.size > copy.buffer->Size() - copy.source_offset) {
		EXIT("BufferCache: invalid download copy\n");
	}
	const auto begin = copy.source_offset & ~uint64_t {3};
	if (copy.source_offset > UINT64_MAX - copy.size ||
	    copy.source_offset + copy.size > UINT64_MAX - 3) {
		EXIT("BufferCache: download copy alignment overflow\n");
	}
	const auto end = (copy.source_offset + copy.size + 3) & ~uint64_t {3};
	if (end > copy.buffer->Size()) {
		EXIT("BufferCache: aligned download copy exceeds its owner\n");
	}
	return {begin, end - begin};
}

std::vector<BufferCache::DownloadRange>
BufferCache::RecordDownloads(std::span<const DownloadCopy> copies) {
	uint64_t reservation_size = 0;
	for (const auto& copy: copies) {
		const auto [source_begin, envelope_size] = DownloadEnvelope(copy);
		(void)source_begin;
		if (envelope_size > UINT64_MAX - (DOWNLOAD_ALIGNMENT - 1)) {
			EXIT("BufferCache: download batch alignment overflow\n");
		}
		const auto aligned_size = AlignDownload(envelope_size);
		if (aligned_size > UINT64_MAX - reservation_size) {
			EXIT("BufferCache: download batch overflow\n");
		}
		reservation_size += aligned_size;
	}
	if (reservation_size == 0) {
		return {};
	}

	auto& download                   = m_download_buffer;
	const auto [mapped, base_offset] = download.Map(reservation_size, DOWNLOAD_ALIGNMENT);
	if (mapped == nullptr) {
		EXIT("BufferCache: download batch could not reserve the shared stream\n");
	}

	std::vector<DownloadRange> downloads;
	downloads.reserve(copies.size());
	uint64_t cursor = 0;
	for (const auto& copy: copies) {
		const auto [source_begin, envelope_size] = DownloadEnvelope(copy);
		const auto prefix                        = copy.source_offset - source_begin;
		download.CopyFrom(m_scheduler.Current(), *copy.buffer, source_begin, base_offset + cursor,
		                  envelope_size, vk::AccessFlagBits::eMemoryWrite, vk::AccessFlags {},
		                  vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
		                  vk::AccessFlagBits::eHostRead);
		downloads.push_back({copy.address, copy.size, base_offset + cursor + prefix});
		cursor += AlignDownload(envelope_size);
	}
	download.Commit();
	return downloads;
}

void BufferCache::PublishDownloads(std::span<const DownloadRange> downloads) {
	for (const auto& range: downloads) {
		m_download_buffer.Invalidate(range.offset, range.size);
		Libs::LibKernel::Memory::WriteBacking(
		    range.address, m_download_buffer.Mapped().data() + range.offset, range.size);
	}
}

void BufferCache::QueueGarbageDownload(std::span<const DownloadCopy> copies, BufferId id,
                                       uint64_t address, uint64_t size) {
	EXIT_IF(copies.empty());
	auto downloads = RecordDownloads(copies);
	auto retired   = downloads;
	m_scheduler.DeferPriorityOperation(
	    [this, downloads = std::move(downloads)]() mutable { PublishDownloads(downloads); });
	m_scheduler.DeferOperation([this, retired = std::move(retired), id, address, size]() mutable {
		for (const auto& range: retired) {
			m_gpu_modified_ranges.Subtract(range.address, range.size);
		}
		// ForEachDownloadRange reports full tracker pages, and every exact GPU-owned
		// interval on those pages was downloaded and removed. Clearing the original
		// query therefore cannot orphan a dirty sibling on an edge page.
		m_memory_tracker.UnmarkRegionAsGpuModified(address, size);
		if (m_memory_tracker.IsRegionGpuModified(address, size) ||
		    !m_gpu_modified_ranges.Intersections(address, size).empty()) {
			EXIT("BufferCache: asynchronous garbage collection retained GPU ownership\n");
		}
		m_memory_tracker.UntrackMemory(address, size);
		EXIT_IF(!m_slot_buffers[id].is_deleted);
		m_slot_buffers.erase(id);
	});
}

BufferCache::BufferCache(GraphicContext& graphics, CommandScheduler& scheduler,
                         PageManager& page_manager, TextureCache& texture_cache)
    : m_graphics(graphics), m_scheduler(scheduler),
      m_gds_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, GdsBufferSize),
      m_memory_tracker(page_manager),
      m_staging_buffer(graphics, scheduler, MemoryUsage::Upload, 512 * MiB),
      m_stream_buffer(graphics, scheduler, MemoryUsage::Stream, 64 * MiB),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 32 * MiB),
      m_device_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 128 * MiB),
      m_texture_cache(texture_cache) {
	std::memset(m_gds_buffer.Mapped().data(), 0, static_cast<size_t>(m_gds_buffer.Size()));
	m_gds_buffer.Flush(0, m_gds_buffer.Size());
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
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
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
	m_scheduler.Context().GetGpu().SendCommandSync(
	    [this, vaddr, size, is_write] { ReadMemoryOnGpu(vaddr, size, is_write); });
}

void BufferCache::ReadMemoryOnGpu(uint64_t vaddr, uint64_t size, bool is_write) {
	// CPU invalidation reaches this point only for a GPU-owned tracker page. Resolve the exact
	// Buffer owner on the GPU thread so the cache index remains single-thread-owned.
	if (is_write && !IsRegionRegistered(vaddr, size)) {
		return;
	}
	std::vector<DownloadCopy> copies;
	m_memory_tracker.ForEachDownloadRange<false>(
	    vaddr, size,
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "memory invalidation");
	    },
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    for (const auto range: m_gpu_modified_ranges.Intersections(address, bytes)) {
			    for (uint64_t copied = 0; copied < range.size;) {
				    const auto copy_address = range.address + copied;
				    auto       owner        = m_buffers.upper_bound(copy_address);
				    if (owner == m_buffers.begin()) {
					    EXIT("BufferCache: invalidation readback has no buffer owner\n");
				    }
				    auto& buffer = m_slot_buffers[std::prev(owner)->second];
				    if (!buffer.IsInBounds(copy_address, 1)) {
					    EXIT("BufferCache: invalidation readback is outside its buffer owner\n");
				    }
				    const auto copy_size = std::min(
				        range.size - copied, buffer.CpuAddress() + buffer.Size() - copy_address);
				    copies.push_back(
				        {&buffer, buffer.Offset(copy_address), copy_address, copy_size});
				    copied += copy_size;
			    }
		    }
	    });
	if (copies.empty()) {
		if (!is_write) {
			return;
		}
		// A preceding read fault can consume the last GPU-owned copy after this write invalidation
		// has already chosen to flush. Complete the CPU ownership handoff even though this callback
		// no longer has bytes to download.
		m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
		return;
	}
	auto downloads = RecordDownloads(copies);
	m_scheduler.FinishCurrent();
	PublishDownloads(downloads);
	for (const auto& range: downloads) {
		m_gpu_modified_ranges.Subtract(range.address, range.size);
	}
	// The enumeration above covered whole dirty pages and every exact interval on them.
	m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, size);
	if (is_write) {
		m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
	}
}

void BufferCache::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: invalid unmap range\n");
	}
	std::vector<DownloadCopy>                  copies;
	std::vector<std::pair<uint64_t, uint64_t>> modified_buffers;
	std::vector<std::pair<uint64_t, uint64_t>> retired_buffers;
	for (const auto& [begin, id]: m_buffers) {
		const auto& buffer = m_slot_buffers[id];
		if (vaddr < begin + buffer.Size() && begin < vaddr + size) {
			retired_buffers.emplace_back(begin, buffer.Size());
		}
	}
	for (const auto& [begin, id]: m_buffers) {
		const auto& buffer = m_slot_buffers[id];
		if (vaddr >= begin + buffer.Size() || begin >= vaddr + size ||
		    !m_memory_tracker.IsRegionGpuModified(begin, buffer.Size())) {
			continue;
		}
		const auto dirty = m_gpu_modified_ranges.Intersections(begin, buffer.Size());
		if (dirty.empty()) {
			EXIT("BufferCache: GPU-modified buffer has no dirty ranges\n");
		}
		modified_buffers.emplace_back(begin, buffer.Size());
	}
	for (const auto& [begin, bytes]: modified_buffers) {
		auto owner = m_buffers.find(begin);
		if (owner == m_buffers.end() || m_slot_buffers[owner->second].Size() != bytes) {
			EXIT("BufferCache: unmap owner changed during collection\n");
		}
		auto& buffer = m_slot_buffers[owner->second];
		m_memory_tracker.ForEachDownloadRange<false>(
		    begin, buffer.Size(),
		    [&](uint64_t address, uint64_t bytes) noexcept {
			    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
			                                           "unmap");
		    },
		    [&](uint64_t address, uint64_t bytes) noexcept {
			    for (const auto& range: m_gpu_modified_ranges.Intersections(address, bytes)) {
				    copies.push_back({&buffer, range.address - begin, range.address, range.size});
			    }
		    });
	}
	if (!copies.empty()) {
		auto downloads = RecordDownloads(copies);
		m_scheduler.FinishCurrent();
		PublishDownloads(downloads);
	} else if (!retired_buffers.empty()) {
		// Image uploads can reference a clean cached buffer without owning it. Submit the active
		// command stream before removing such backing.
		m_scheduler.FinishCurrent();
	}
	for (const auto& [begin, bytes]: modified_buffers) {
		m_gpu_modified_ranges.Subtract(begin, bytes);
		m_memory_tracker.UnmarkRegionAsGpuModified(begin, bytes);
	}
	for (const auto& [begin, bytes]: retired_buffers) {
		m_memory_tracker.MarkRegionAsCpuModified(begin, bytes);
	}
	if (!m_gpu_modified_ranges.Intersections(vaddr, size).empty()) {
		EXIT("BufferCache: unmap retained dirty byte ranges\n");
	}
	m_memory_tracker.UntrackMemory(vaddr, size);
	for (const auto& [begin, bytes]: retired_buffers) {
		(void)bytes;
		const auto found = m_buffers.find(begin);
		if (found != m_buffers.end()) {
			DeleteBuffer(found->second);
		}
	}
}

BufferId BufferCache::FindBuffer(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0) {
		return NULL_BUFFER_ID;
	}
	if (size == 0 || vaddr >= TRACKER_ADDRESS_SIZE || size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("BufferCache: invalid buffer discovery request\n");
	}
	auto& command = m_scheduler.Current();
	EXIT_IF(command.IsInvalid() || command.IsExecute());
	auto       begin = vaddr & ~(CACHING_PAGE_SIZE - 1);
	auto       end   = (vaddr + size + CACHING_PAGE_SIZE - 1) & ~(CACHING_PAGE_SIZE - 1);
	const auto next  = m_buffers.upper_bound(vaddr);
	if (next != m_buffers.begin()) {
		const auto owner  = std::prev(next);
		auto&      buffer = m_slot_buffers[owner->second];
		if (buffer.IsInBounds(vaddr, size)) {
			buffer.tick_accessed_last = m_gc_tick;
			return owner->second;
		}
	}

	auto merge = [&](const Buffer& buffer) {
		const auto buffer_begin = buffer.CpuAddress();
		const auto buffer_end   = buffer_begin + buffer.Size();
		if (begin >= buffer_end || buffer_begin >= end) {
			return false;
		}
		begin = std::min(begin, buffer_begin);
		end   = std::max(end, buffer_end);
		return true;
	};
	std::vector<BufferId> overlaps;
	auto                  first = m_buffers.lower_bound(begin);
	if (first != m_buffers.begin()) {
		const auto previous = std::prev(first);
		if (merge(m_slot_buffers[previous->second])) {
			first = previous;
		}
	}
	for (auto candidate = first; candidate != m_buffers.end(); ++candidate) {
		if (candidate->first >= end) {
			break;
		}
		if (merge(m_slot_buffers[candidate->second])) {
			overlaps.push_back(candidate->second);
		}
	}

	const auto id = m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, begin,
	                                      AllFlags, end - begin);
	auto&      buffer         = m_slot_buffers[id];
	buffer.tick_accessed_last = m_gc_tick;
	SetVulkanObjectNameF(m_graphics.device, buffer.Handle(),
	                     "Kyty.GameBuffer[guest=0x{:016x} size=0x{:x}]", begin, end - begin);
	for (const auto overlap: overlaps) {
		const auto& old = m_slot_buffers[overlap];
		buffer.CopyFrom(command, old, 0, old.CpuAddress() - begin, old.Size());
	}
	for (const auto overlap: overlaps) {
		DeleteBuffer(overlap);
	}
	m_total_used_memory += buffer.Size();
	const auto [it, inserted] = m_buffers.emplace(begin, id);
	(void)it;
	EXIT_IF(!inserted);
	return id;
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_written,
                                    bool is_texel_buffer) {
	std::vector<std::pair<uint64_t, uint64_t>> uploads;
	m_memory_tracker.ForEachUploadRange(
	    vaddr, size, is_written,
	    [&](uint64_t address, uint64_t bytes) noexcept { uploads.emplace_back(address, bytes); },
	    [&]() noexcept {
		    for (const auto& [address, bytes]: uploads) {
			    WriteDataBuffer(buffer, address, reinterpret_cast<const void*>(address), bytes);
		    }
	    });
	if (is_texel_buffer && !is_written) {
		return SynchronizeBufferFromImage(buffer, vaddr, size);
	}
	return false;
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer(uint64_t vaddr, uint64_t size,
                                                       bool is_written, bool is_texel_buffer,
                                                       BufferId id) {
	auto& command = m_scheduler.Current();
	if (command.IsInvalid() || command.IsExecute() || vaddr == 0 || size == 0 ||
	    vaddr >= TRACKER_ADDRESS_SIZE || size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("BufferCache: buffer request requires a recording command buffer\n");
	}

	if (!is_written && size <= CACHING_PAGE_SIZE &&
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
	    m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
		const auto alignment = std::max<uint64_t>(
		    m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 1);
		auto [mapped, offset] = m_stream_buffer.Map(size, alignment);
		if (mapped != nullptr && Libs::LibKernel::Memory::TryReadBacking(vaddr, mapped, size)) {
			m_stream_buffer.Commit();
			return {&m_stream_buffer, offset};
		}
	}

	auto* buffer = m_slot_buffers.try_get(id);
	if (buffer == nullptr || buffer->is_deleted || !buffer->IsInBounds(vaddr, size)) {
		id     = FindBuffer(vaddr, size);
		buffer = &m_slot_buffers[id];
	}
	(void)SynchronizeBuffer(*buffer, vaddr, size, is_written, is_texel_buffer);
	if (is_written) {
		m_gpu_modified_ranges.Add(vaddr, size);
	}
	return {buffer, buffer->Offset(vaddr)};
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("BufferCache: invalid image source\n");
	}
	auto find_owner = [&]() -> Buffer* {
		auto owner = m_buffers.upper_bound(vaddr);
		if (owner == m_buffers.begin()) {
			return nullptr;
		}
		--owner;
		auto* buffer = m_slot_buffers.try_get(owner->second);
		return buffer != nullptr && buffer->IsInBounds(vaddr, size) ? buffer : nullptr;
	};

	{
		const bool cpu_modified            = m_memory_tracker.IsRegionCpuModified(vaddr, size);
		const bool gpu_modified            = m_memory_tracker.IsRegionGpuModified(vaddr, size);
		const auto dirty                   = m_gpu_modified_ranges.Intersections(vaddr, size);
		const bool has_dirty_buffer_source = !dirty.empty();
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, vaddr, size,
		                                           "image source");

		auto* owner = find_owner();
		if (has_dirty_buffer_source && owner == nullptr) {
			if (!IsRegionRegistered(vaddr, size)) {
				EXIT("BufferCache: GPU-dirty image source has no native buffer\n");
			}
			const auto id       = FindBuffer(vaddr, size);
			auto&      buffer   = m_slot_buffers[id];
			owner               = &buffer;
			const auto resolved = m_buffers.find(buffer.CpuAddress());
			if (resolved == m_buffers.end() || resolved->second != id ||
			    !buffer.IsInBounds(vaddr, size)) {
				EXIT("BufferCache: merged image source does not contain the requested range\n");
			}
		}
		if (owner != nullptr && !cpu_modified && (!gpu_modified || has_dirty_buffer_source)) {
			owner->tick_accessed_last = m_gc_tick;
			return {owner, owner->Offset(vaddr)};
		}
		if (has_dirty_buffer_source && owner == nullptr) {
			EXIT("BufferCache: GPU-dirty image source could not resolve its native owner\n");
		}
	}

	auto [staging, stage_offset] = m_staging_buffer.Map(size, 16);
	if (staging == nullptr || (!Libs::LibKernel::Memory::TryReadBacking(vaddr, staging, size) &&
	                           !Libs::LibKernel::Memory::TryReadPrtBacking(vaddr, staging, size))) {
		EXIT("BufferCache: failed to read mapped guest image backing\n");
	}
	m_staging_buffer.Commit();

	const auto dirty                   = m_gpu_modified_ranges.Intersections(vaddr, size);
	const bool has_dirty_buffer_source = !dirty.empty();
	auto*      owner                   = find_owner();
	if (has_dirty_buffer_source && owner == nullptr) {
		EXIT("BufferCache: GPU-dirty image source lost its native owner\n");
	}
	if (owner == nullptr ||
	    (m_memory_tracker.IsRegionGpuModified(vaddr, size) && !has_dirty_buffer_source)) {
		return {&m_staging_buffer, stage_offset};
	}

	owner->tick_accessed_last = m_gc_tick;
	std::vector<std::pair<uint64_t, uint64_t>> uploads;
	m_memory_tracker.ForEachUploadRange(
	    vaddr, size, false,
	    [&](uint64_t address, uint64_t upload_size) noexcept {
		    uploads.emplace_back(address, upload_size);
	    },
	    [&]() noexcept {
		    for (const auto& [address, upload_size]: uploads) {
			    owner->CopyFrom(m_scheduler.Current(), m_staging_buffer,
			                    stage_offset + address - vaddr, owner->Offset(address), upload_size,
			                    vk::AccessFlagBits::eHostWrite);
		    }
	    });
	return {owner, owner->Offset(vaddr)};
}

void BufferCache::WriteHostMemory(uint64_t vaddr, std::span<const uint8_t> data) {
	if (vaddr == 0 || data.empty() || data.size() > UINT64_MAX - vaddr) {
		EXIT("BufferCache: invalid host DMA write\n");
	}
	Libs::LibKernel::Memory::WriteBacking(vaddr, data.data(), data.size());

	const auto end = vaddr + data.size();
	for (const auto& [address, id]: m_buffers) {
		auto&      buffer     = m_slot_buffers[id];
		const auto buffer_end = address + buffer.Size();
		const auto begin      = std::max(vaddr, address);
		const auto range_end  = std::min(end, buffer_end);
		if (begin >= range_end) {
			continue;
		}
		WriteDataBuffer(buffer, begin, data.data() + begin - vaddr, range_end - begin);
		buffer.tick_accessed_last = m_gc_tick;
	}
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
	{
		const auto region = m_texture_cache.QueryRegion(vaddr, size);
		if (!HasGpuDirtyBytes(vaddr, size) && !region.gpu_image_bytes) {
			if (region.image_bytes) {
				m_texture_cache.InvalidateMemory(vaddr, size);
			}
			std::array<uint32_t, 4096> values;
			values.fill(value);
			const std::span<const uint8_t> bytes {reinterpret_cast<const uint8_t*>(values.data()),
			                                      sizeof(values)};
			for (uint64_t offset = 0; offset < size;) {
				const auto chunk = std::min<uint64_t>(size - offset, bytes.size());
				WriteHostMemory(vaddr + offset, bytes.first(chunk));
				offset += chunk;
			}
			return;
		}
	}

	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	const auto id          = FindBuffer(vaddr, size);
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true, id);
	EXIT_IF(dst == nullptr);
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
	if (src_memory || dst_memory) {
		const auto src_region =
		    src_memory ? m_texture_cache.QueryRegion(src_vaddr, size) : TextureCache::RegionInfo {};
		const auto dst_region =
		    dst_memory ? m_texture_cache.QueryRegion(dst_vaddr, size) : TextureCache::RegionInfo {};
		if (src_memory && dst_memory && !HasGpuDirtyBytes(src_vaddr, size) &&
		    !HasGpuDirtyBytes(dst_vaddr, size) && !src_region.gpu_image_bytes &&
		    !dst_region.gpu_image_bytes) {
			if (dst_region.image_bytes) {
				m_texture_cache.InvalidateMemory(dst_vaddr, size);
			}
			std::array<uint8_t, 64 * 1024> bytes;
			for (uint64_t offset = 0; offset < size;) {
				const auto chunk = std::min<uint64_t>(size - offset, bytes.size());
				if (!Libs::LibKernel::Memory::TryReadBacking(src_vaddr + offset, bytes.data(),
				                                             chunk)) {
					EXIT("BufferCache: host DMA source has no direct backing\n");
				}
				WriteHostMemory(dst_vaddr + offset, std::span {bytes}.first(chunk));
				offset += chunk;
			}
			return;
		}
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
	EXIT_IF(src == nullptr || dst == nullptr);
	if (src == dst && src_offset < dst_offset + size && dst_offset < src_offset + size) {
		EXIT("BufferCache: resolved Vulkan copy ranges overlap\n");
	}
	dst->CopyFrom(command, *src, src_offset, dst_offset, size);
}

bool BufferCache::IsRegionRegistered(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
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

bool BufferCache::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionGpuModified(vaddr, size);
}

bool BufferCache::HasGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	return !m_gpu_modified_ranges.Intersections(vaddr, size).empty();
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

	std::vector<uint64_t> candidates;
	for (const auto& [address, id]: m_buffers) {
		const auto& buffer = m_slot_buffers[id];
		if (tick - std::min(tick, buffer.tick_accessed_last) >= age) {
			candidates.push_back(address);
		}
	}
	std::ranges::sort(candidates, [&](uint64_t left, uint64_t right) {
		return m_slot_buffers[m_buffers.at(left)].tick_accessed_last <
		       m_slot_buffers[m_buffers.at(right)].tick_accessed_last;
	});
	if (candidates.size() > limit) {
		candidates.resize(limit);
	}

	for (const auto address: candidates) {
		auto found = m_buffers.find(address);
		EXIT_IF(found == m_buffers.end());
		const auto id     = found->second;
		auto&      buffer = m_slot_buffers[id];
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, buffer.CpuAddress(),
		                                           buffer.Size(), "garbage collection");
		const bool dirty = m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size());
		std::vector<DownloadCopy> copies;
		if (dirty) {
			m_memory_tracker.ForEachDownloadRange<false>(
			    buffer.CpuAddress(), buffer.Size(),
			    [&](uint64_t dirty_address, uint64_t dirty_size) noexcept {
				    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, dirty_address,
				                                           dirty_size, "garbage collection");
			    },
			    [&](uint64_t dirty_address, uint64_t dirty_size) noexcept {
				    for (const auto range:
				         m_gpu_modified_ranges.Intersections(dirty_address, dirty_size)) {
					    copies.push_back({&buffer, range.address - buffer.CpuAddress(),
					                      range.address, range.size});
				    }
			    });
		}

		const auto retire_address = buffer.CpuAddress();
		const auto retire_size    = buffer.Size();
		if (dirty) {
			EXIT_IF(copies.empty());
			Unregister(id);
			QueueGarbageDownload(copies, id, retire_address, retire_size);
		} else {
			m_memory_tracker.UntrackMemory(retire_address, retire_size);
			DeleteBuffer(id);
		}
	}
}

} // namespace Libs::Graphics
