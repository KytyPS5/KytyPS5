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
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstring>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {

thread_local const void* g_cache_lock_owner = nullptr;

constexpr uint64_t MiB           = 1024 * 1024;
constexpr uint64_t GdsBufferSize = 64 * 1024;

class FaultSafeCacheLock final {
public:
	FaultSafeCacheLock(const void* owner, Common::Mutex& mutex): m_mutex(mutex) {
		if (g_cache_lock_owner != nullptr) {
			EXIT("BufferCache: recursive cache lock acquisition\n");
		}
		g_cache_lock_owner = owner;
		m_mutex.Lock();
	}

	~FaultSafeCacheLock() {
		m_mutex.Unlock();
		g_cache_lock_owner = nullptr;
	}

private:
	Common::Mutex& m_mutex;
};

} // namespace

uint64_t BufferCache::AlignDown(uint64_t value) noexcept {
	return value & ~(CACHING_PAGE_SIZE - 1);
}

uint64_t BufferCache::AlignUp(uint64_t value) {
	if (value > UINT64_MAX - (CACHING_PAGE_SIZE - 1)) {
		EXIT("BufferCache: address alignment overflow\n");
	}
	return (value + CACHING_PAGE_SIZE - 1) & ~(CACHING_PAGE_SIZE - 1);
}

void BufferCache::Upload(CommandBuffer& command, Buffer& destination, uint64_t destination_offset,
                         const void* source, uint64_t size) {
	auto* bytes = static_cast<const uint8_t*>(source);
	while (size != 0) {
		const auto chunk        = std::min(size, m_staging_buffer.Size());
		const auto stage_offset = m_staging_buffer.Copy(bytes, chunk, 4);
		destination.CopyFrom(command, m_staging_buffer, stage_offset, destination_offset, chunk,
		                     vk::AccessFlagBits::eHostWrite);
		bytes += chunk;
		destination_offset += chunk;
		size -= chunk;
	}
}

bool BufferCache::ResolveOverlap(CacheRange& merged, CacheRange candidate) noexcept {
	if (merged.address == 0 || merged.size == 0 || candidate.address == 0 || candidate.size == 0 ||
	    merged.size > UINT64_MAX - merged.address ||
	    candidate.size > UINT64_MAX - candidate.address) {
		EXIT("BufferCache: invalid overlap-merge range\n");
	}
	const auto merged_end    = merged.address + merged.size;
	const auto candidate_end = candidate.address + candidate.size;
	if (merged.address >= candidate_end || candidate.address >= merged_end) {
		return false;
	}
	const auto address = std::min(merged.address, candidate.address);
	const auto end     = std::max(merged_end, candidate_end);
	merged             = {.address = address, .size = end - address};
	return true;
}

struct BufferCache::CachedBuffer {
	uint64_t                vaddr = 0;
	uint64_t                size  = 0;
	std::shared_ptr<Buffer> buffer;
	uint64_t                tick_accessed_last = 0;
};

struct BufferCache::DownloadCopy {
	std::shared_ptr<Buffer> owner;
	uint64_t                source_offset = 0;
	uint64_t                address       = 0;
	uint64_t                size          = 0;
};

struct BufferCache::DownloadRange {
	uint64_t address = 0;
	uint64_t size    = 0;
	uint64_t offset  = 0;
};

struct BufferCache::RetiredBuffer {
	uint64_t                address = 0;
	uint64_t                size    = 0;
	std::shared_ptr<Buffer> owner;
};

std::pair<uint64_t, uint64_t> BufferCache::DownloadEnvelope(const DownloadCopy& copy) {
	if (copy.owner == nullptr || copy.size == 0 || copy.source_offset > copy.owner->Size() ||
	    copy.size > copy.owner->Size() - copy.source_offset) {
		EXIT("BufferCache: invalid download copy\n");
	}
	const auto begin = copy.source_offset & ~uint64_t {3};
	if (copy.source_offset > UINT64_MAX - copy.size ||
	    copy.source_offset + copy.size > UINT64_MAX - 3) {
		EXIT("BufferCache: download copy alignment overflow\n");
	}
	const auto end = (copy.source_offset + copy.size + 3) & ~uint64_t {3};
	if (end > copy.owner->Size()) {
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
		download.CopyFrom(m_scheduler.Current(), *copy.owner, source_begin, base_offset + cursor,
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

void BufferCache::QueueGarbageDownload(std::span<const DownloadCopy> copies, RetiredBuffer retire) {
	if (copies.empty()) {
		return;
	}
	auto downloads = RecordDownloads(copies);
	m_scheduler.DeferOperation(
	    [this, downloads = std::move(downloads), retire = std::move(retire)]() mutable {
		    PublishDownloads(downloads);
		    {
			    FaultSafeCacheLock lock(this, m_mutex);
			    for (const auto& range: downloads) {
				    m_gpu_modified_ranges.Subtract(range.address, range.size);
			    }
			    // ForEachDownloadRange reports full tracker pages, and every exact GPU-owned
			    // interval on those pages was downloaded and removed. Clearing the original
			    // query therefore cannot orphan a dirty sibling on an edge page.
			    m_memory_tracker.UnmarkRegionAsGpuModified(retire.address, retire.size);
			    if (m_memory_tracker.IsRegionGpuModified(retire.address, retire.size) ||
			        !m_gpu_modified_ranges.Intersections(retire.address, retire.size).empty()) {
				    EXIT("BufferCache: asynchronous garbage collection retained GPU ownership\n");
			    }
			    m_memory_tracker.UntrackMemory(retire.address, retire.size);
		    }
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
	for (const auto& [vaddr, cached]: m_buffers) {
		(void)vaddr;
		if (m_memory_tracker.IsRegionGpuModified(cached->vaddr, cached->size)) {
			EXIT("BufferCache: destroyed with GPU-modified buffer\n");
		}
	}
	m_buffers.clear();
}

StreamBuffer& BufferCache::GetUtilityBuffer(MemoryUsage usage) noexcept {
	switch (usage) {
		case MemoryUsage::Upload: return m_staging_buffer;
		case MemoryUsage::Stream: return m_stream_buffer;
		case MemoryUsage::Download: return m_download_buffer;
		case MemoryUsage::DeviceLocal: return m_device_buffer;
	}
	EXIT("BufferCache: invalid utility-buffer usage\n");
}

BufferBinding BufferCache::UploadTransient(const void* data, uint64_t size, uint64_t alignment) {
	EXIT_IF(data == nullptr || size == 0);
	if (auto [mapped, offset] = m_stream_buffer.Map(size, alignment, false); mapped != nullptr) {
		std::memcpy(mapped, data, static_cast<size_t>(size));
		m_stream_buffer.Commit();
		return {{}, m_stream_buffer.Handle(), offset};
	}
	auto owner =
	    std::make_shared<Buffer>(m_graphics, m_scheduler, MemoryUsage::Upload, 0, AllFlags, size);
	owner->Write(0, data, size);
	return {owner, owner->Handle(), 0};
}

void BufferCache::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("BufferCache: invalid memory-invalidation range\n");
	}
	if (!IsRegionRegistered(vaddr, size)) {
		return;
	}
	m_memory_tracker.InvalidateRegion(vaddr, size,
	                                  [this, vaddr, size] { ReadMemory(vaddr, size, true); });
}

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write) {
	if (Gpu::IsGpuThread()) {
		ReadMemoryOnGpu(vaddr, size, is_write);
		return;
	}
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	m_scheduler.Context().GetGpu().SendCommandSync(
	    [this, vaddr, size, is_write] { ReadMemoryOnGpu(vaddr, size, is_write); });
}

void BufferCache::ReadMemoryOnGpu(uint64_t vaddr, uint64_t size, bool is_write) {
	std::vector<DownloadCopy> copies;
	{
		FaultSafeCacheLock lock(this, m_mutex);
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
					    auto& cached = *std::prev(owner)->second;
					    if (!cached.buffer->IsInBounds(copy_address, 1)) {
						    EXIT(
						        "BufferCache: invalidation readback is outside its buffer owner\n");
					    }
					    const auto copy_size = std::min(range.size - copied,
					                                    cached.vaddr + cached.size - copy_address);
					    copies.push_back({cached.buffer, cached.buffer->Offset(copy_address),
					                      copy_address, copy_size});
					    copied += copy_size;
				    }
			    }
		    });
		if (copies.empty()) {
			if (!is_write) {
				return;
			}
			// A preceding read fault can consume the last GPU-owned copy after this write
			// invalidation has already chosen to flush. Complete the CPU ownership handoff even
			// though this callback no longer has bytes to download.
			m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
			return;
		}
	}
	auto downloads = RecordDownloads(copies);
	m_scheduler.FinishCurrent();
	PublishDownloads(downloads);
	{
		FaultSafeCacheLock lock(this, m_mutex);
		for (const auto& range: downloads) {
			m_gpu_modified_ranges.Subtract(range.address, range.size);
		}
		// The enumeration above covered whole dirty pages and every exact interval on them.
		m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, size);
		if (is_write) {
			m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
		}
	}
}

void BufferCache::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: invalid unmap range\n");
	}
	std::vector<DownloadCopy>                  copies;
	std::vector<std::pair<uint64_t, uint64_t>> modified_buffers;
	std::vector<std::pair<uint64_t, uint64_t>> retired_buffers;
	{
		FaultSafeCacheLock lock(this, m_mutex);
		for (const auto& [begin, cached]: m_buffers) {
			if (vaddr < begin + cached->size && begin < vaddr + size) {
				retired_buffers.emplace_back(begin, cached->size);
			}
		}
		for (const auto& [begin, cached]: m_buffers) {
			if (vaddr >= begin + cached->size || begin >= vaddr + size ||
			    !m_memory_tracker.IsRegionGpuModified(begin, cached->size)) {
				continue;
			}
			const auto dirty = m_gpu_modified_ranges.Intersections(begin, cached->size);
			if (dirty.empty()) {
				EXIT("BufferCache: GPU-modified buffer has no dirty ranges\n");
			}
			modified_buffers.emplace_back(begin, cached->size);
		}
		for (const auto& [begin, bytes]: modified_buffers) {
			auto owner = m_buffers.find(begin);
			if (owner == m_buffers.end() || owner->second->size != bytes) {
				EXIT("BufferCache: unmap owner changed during collection\n");
			}
			auto& cached = *owner->second;
			m_memory_tracker.ForEachDownloadRange<false>(
			    begin, cached.size,
			    [&](uint64_t address, uint64_t bytes) noexcept {
				    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
				                                           "unmap");
			    },
			    [&](uint64_t address, uint64_t bytes) noexcept {
				    for (const auto& range: m_gpu_modified_ranges.Intersections(address, bytes)) {
					    copies.push_back(
					        {cached.buffer, range.address - begin, range.address, range.size});
				    }
			    });
		}
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
	{
		FaultSafeCacheLock lock(this, m_mutex);
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
		for (auto it = m_buffers.begin(); it != m_buffers.end();) {
			if (vaddr < it->first + it->second->size && it->first < vaddr + size) {
				if (it->second->size > m_total_used_memory) {
					EXIT("BufferCache: allocation accounting underflow\n");
				}
				m_total_used_memory -= it->second->size;
				it = m_buffers.erase(it);
			} else {
				++it;
			}
		}
	}
}

BufferCache::CachedBuffer& BufferCache::GetOrCreateBuffer(CommandBuffer& command, uint64_t vaddr,
                                                          uint64_t size) {
	const auto begin = AlignDown(vaddr);
	const auto end   = AlignUp(vaddr + size);
	auto       it    = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		auto previous = std::prev(it);
		if (previous->second->buffer->IsInBounds(vaddr, size)) {
			it = previous;
		}
	}
	if (it != m_buffers.end() && it->second->buffer->IsInBounds(vaddr, size)) {
		it->second->tick_accessed_last = m_gc_tick;
		return *it->second;
	}

	CacheRange merged {.address = begin, .size = end - begin};
	using Iterator = decltype(m_buffers.begin());
	std::vector<Iterator> overlaps;
	auto                  first = m_buffers.lower_bound(begin);
	if (first != m_buffers.begin()) {
		auto previous = std::prev(first);
		if (ResolveOverlap(merged, {previous->second->vaddr, previous->second->size})) {
			first = previous;
		}
	}
	for (auto candidate = first; candidate != m_buffers.end(); ++candidate) {
		if (candidate->first >= merged.address + merged.size) {
			break;
		}
		if (ResolveOverlap(merged, {candidate->second->vaddr, candidate->second->size})) {
			overlaps.push_back(candidate);
		}
	}
	for (const auto overlap: overlaps) {
		auto&                                      old = *overlap->second;
		std::vector<std::pair<uint64_t, uint64_t>> uploads;
		m_memory_tracker.ForEachUploadRange(
		    old.vaddr, old.size, false,
		    [&](uint64_t address, uint64_t bytes) noexcept {
			    uploads.emplace_back(address, bytes);
		    },
		    [&]() noexcept {
			    for (const auto& [address, bytes]: uploads) {
				    Upload(command, *old.buffer, old.buffer->Offset(address),
				           reinterpret_cast<const void*>(address), bytes);
			    }
		    });
	}

	auto cached                = std::make_unique<CachedBuffer>();
	cached->vaddr              = merged.address;
	cached->size               = merged.size;
	cached->tick_accessed_last = m_gc_tick;
	cached->buffer = std::make_shared<Buffer>(m_graphics, m_scheduler, MemoryUsage::DeviceLocal,
	                                          merged.address, AllFlags, merged.size);
	for (const auto overlap: overlaps) {
		const auto& old = *overlap->second;
		cached->buffer->CopyFrom(command, *old.buffer, 0, old.vaddr - cached->vaddr, old.size);
		command.RetainResourceUntilFence(old.buffer);
	}
	for (const auto overlap: overlaps) {
		if (overlap->second->size > m_total_used_memory) {
			EXIT("BufferCache: allocation accounting underflow\n");
		}
		m_total_used_memory -= overlap->second->size;
		m_buffers.erase(overlap);
	}
	m_total_used_memory += cached->size;
	return *m_buffers.emplace(cached->vaddr, std::move(cached)).first->second;
}

BufferBinding BufferCache::ObtainBuffer(CommandBuffer& command, uint64_t vaddr, uint64_t size,
                                        bool is_written, bool is_read, bool is_formatted) {
	if (command.IsInvalid() || command.IsExecute()) {
		EXIT("BufferCache: buffer request requires a recording command buffer\n");
	}

	if (is_read && !is_written && size <= CACHING_PAGE_SIZE &&
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
	    m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
		const auto alignment = std::max<uint64_t>(
		    m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 1);
		if (auto [mapped, offset] = m_stream_buffer.Map(size, alignment, false);
		    mapped != nullptr) {
			if (Libs::LibKernel::Memory::TryReadBacking(vaddr, mapped, size)) {
				m_stream_buffer.Commit();
				return {{}, m_stream_buffer.Handle(), offset};
			}
		} else {
			auto owner = std::make_shared<Buffer>(m_graphics, m_scheduler, MemoryUsage::Upload, 0,
			                                      AllFlags, size);
			if (Libs::LibKernel::Memory::TryReadBacking(vaddr, owner->Mapped().data(), size)) {
				owner->Flush(0, size);
				return {owner, owner->Handle(), 0};
			}
		}
	}

	if (is_formatted && is_written) {
		(void)m_texture_cache.InvalidateMemoryFromGPU(vaddr, size, true);
	}

	std::shared_ptr<Buffer> owner;
	uint64_t                offset = 0;
	{
		FaultSafeCacheLock                         lock(this, m_mutex);
		auto&                                      cached = GetOrCreateBuffer(command, vaddr, size);
		std::vector<std::pair<uint64_t, uint64_t>> uploads;
		m_memory_tracker.ForEachUploadRange(
		    vaddr, size, is_written,
		    [&](uint64_t address, uint64_t bytes) noexcept {
			    uploads.emplace_back(address, bytes);
		    },
		    [&]() noexcept {
			    for (const auto& [address, bytes]: uploads) {
				    Upload(command, *cached.buffer, cached.buffer->Offset(address),
				           reinterpret_cast<const void*>(address), bytes);
			    }
		    });
		if (is_written) {
			m_gpu_modified_ranges.Add(vaddr, size);
		}
		owner  = cached.buffer;
		offset = cached.buffer->Offset(vaddr);
	}
	if (is_formatted && is_read && !is_written) {
		(void)SynchronizeBufferFromImage(*owner, vaddr, size);
	}
	return {owner, owner->Handle(), offset};
}

std::shared_ptr<Buffer> BufferCache::ObtainNullBuffer() {
	std::shared_ptr<Buffer> buffer;
	{
		FaultSafeCacheLock lock(this, m_mutex);
		if (m_null_buffer != nullptr) {
			return m_null_buffer;
		}
		m_null_buffer = std::make_shared<Buffer>(m_graphics, m_scheduler, MemoryUsage::DeviceLocal,
		                                         0, AllFlags, 16);
		buffer        = m_null_buffer;
	}
	const std::array<uint8_t, 16> zeros {};
	Upload(m_scheduler.Current(), *buffer, 0, zeros.data(), zeros.size());
	return buffer;
}

ImageBufferSource BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("BufferCache: invalid image source\n");
	}
	auto find_owner = [&]() {
		auto owner = m_buffers.upper_bound(vaddr);
		if (owner == m_buffers.begin()) {
			return m_buffers.end();
		}
		--owner;
		return owner->second->buffer->IsInBounds(vaddr, size) ? owner : m_buffers.end();
	};

	{
		FaultSafeCacheLock lock(this, m_mutex);
		const bool         cpu_modified = m_memory_tracker.IsRegionCpuModified(vaddr, size);
		const bool         gpu_modified = m_memory_tracker.IsRegionGpuModified(vaddr, size);
		const auto         dirty        = m_gpu_modified_ranges.Intersections(vaddr, size);
		const bool         has_dirty_buffer_source = !dirty.empty();
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, vaddr, size,
		                                           "image source");

		auto owner = find_owner();
		if (has_dirty_buffer_source && owner == m_buffers.end()) {
			CacheRange merged {.address = AlignDown(vaddr),
			                   .size    = AlignUp(vaddr + size) - AlignDown(vaddr)};
			using Iterator = decltype(m_buffers.begin());
			std::vector<Iterator> overlaps;
			auto                  first = m_buffers.lower_bound(merged.address);
			if (first != m_buffers.begin()) {
				auto previous = std::prev(first);
				if (ResolveOverlap(merged, {previous->second->vaddr, previous->second->size})) {
					first = previous;
				}
			}
			for (auto candidate = first; candidate != m_buffers.end(); ++candidate) {
				if (candidate->first >= merged.address + merged.size) {
					break;
				}
				if (ResolveOverlap(merged, {candidate->second->vaddr, candidate->second->size})) {
					overlaps.push_back(candidate);
				}
			}
			if (overlaps.empty()) {
				EXIT("BufferCache: GPU-dirty image source has no native buffer\n");
			}

			auto cached                = std::make_unique<CachedBuffer>();
			cached->vaddr              = merged.address;
			cached->size               = merged.size;
			cached->tick_accessed_last = m_gc_tick;
			cached->buffer =
			    std::make_shared<Buffer>(m_graphics, m_scheduler, MemoryUsage::DeviceLocal,
			                             merged.address, AllFlags, merged.size);
			for (const auto overlap: overlaps) {
				const auto& old = *overlap->second;
				cached->buffer->CopyFrom(m_scheduler.Current(), *old.buffer, 0,
				                         old.vaddr - cached->vaddr, old.size);
				m_scheduler.Current().RetainResourceUntilFence(old.buffer);
			}
			for (const auto overlap: overlaps) {
				if (overlap->second->size > m_total_used_memory) {
					EXIT("BufferCache: allocation accounting underflow\n");
				}
				m_total_used_memory -= overlap->second->size;
				m_buffers.erase(overlap);
			}
			m_total_used_memory += cached->size;
			owner = m_buffers.emplace(cached->vaddr, std::move(cached)).first;
			if (!owner->second->buffer->IsInBounds(vaddr, size)) {
				EXIT("BufferCache: merged image source does not contain the requested range\n");
			}
		}
		if (owner != m_buffers.end() && !cpu_modified &&
		    (!gpu_modified || has_dirty_buffer_source)) {
			owner->second->tick_accessed_last = m_gc_tick;
			return {owner->second->buffer.get(), owner->second->buffer->Offset(vaddr)};
		}
		if (has_dirty_buffer_source && owner == m_buffers.end()) {
			EXIT("BufferCache: GPU-dirty image source could not resolve its native owner\n");
		}
	}

	auto [staging, stage_offset] = m_staging_buffer.Map(size, 16);
	if (staging == nullptr || !Libs::LibKernel::Memory::TryReadBacking(vaddr, staging, size)) {
		EXIT("BufferCache: failed to read mapped guest image backing\n");
	}
	m_staging_buffer.Commit();

	FaultSafeCacheLock lock(this, m_mutex);
	const auto         dirty                   = m_gpu_modified_ranges.Intersections(vaddr, size);
	const bool         has_dirty_buffer_source = !dirty.empty();
	auto               owner                   = find_owner();
	if (has_dirty_buffer_source && owner == m_buffers.end()) {
		EXIT("BufferCache: GPU-dirty image source lost its native owner\n");
	}
	if (owner == m_buffers.end() ||
	    (m_memory_tracker.IsRegionGpuModified(vaddr, size) && !has_dirty_buffer_source)) {
		return {&m_staging_buffer, stage_offset};
	}

	auto& cached              = *owner->second;
	cached.tick_accessed_last = m_gc_tick;
	std::vector<std::pair<uint64_t, uint64_t>> uploads;
	m_memory_tracker.ForEachUploadRange(
	    vaddr, size, false,
	    [&](uint64_t address, uint64_t upload_size) noexcept {
		    uploads.emplace_back(address, upload_size);
	    },
	    [&]() noexcept {
		    for (const auto& [address, upload_size]: uploads) {
			    cached.buffer->CopyFrom(
			        m_scheduler.Current(), m_staging_buffer, stage_offset + address - vaddr,
			        cached.buffer->Offset(address), upload_size, vk::AccessFlagBits::eHostWrite);
		    }
	    });
	return {cached.buffer.get(), cached.buffer->Offset(vaddr)};
}

void BufferCache::WriteHostMemory(uint64_t vaddr, std::span<const uint8_t> data) {
	if (vaddr == 0 || data.empty() || data.size() > UINT64_MAX - vaddr) {
		EXIT("BufferCache: invalid host DMA write\n");
	}
	Libs::LibKernel::Memory::WriteBacking(vaddr, data.data(), data.size());

	FaultSafeCacheLock lock(this, m_mutex);
	const auto         end = vaddr + data.size();
	for (auto& [address, cached]: m_buffers) {
		const auto cached_end = address + cached->size;
		const auto begin      = std::max(vaddr, address);
		const auto range_end  = std::min(end, cached_end);
		if (begin >= range_end) {
			continue;
		}
		Upload(m_scheduler.Current(), *cached->buffer, cached->buffer->Offset(begin),
		       data.data() + begin - vaddr, range_end - begin);
		cached->tick_accessed_last = m_gc_tick;
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

	auto& command = m_scheduler.Current();
	auto  dst     = ObtainBuffer(command, vaddr, size, true, false, true);
	EXIT_IF(dst.buffer == nullptr || dst.owner == nullptr);
	command.RetainResourceUntilFence(dst.owner);
	auto owner = std::static_pointer_cast<Buffer>(dst.owner);
	owner->Fill(dst.offset, size, value);
}

void BufferCache::CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
                             bool src_gds) {
	const bool dst_memory = !dst_gds;
	const bool src_memory = !src_gds;
	if ((dst_memory && dst_vaddr == 0) || (src_memory && src_vaddr == 0) || size == 0 ||
	    ((dst_vaddr | src_vaddr | size) & 3u) != 0 || size > UINT64_MAX - dst_vaddr ||
	    size > UINT64_MAX - src_vaddr || (dst_gds && src_gds) ||
	    (dst_gds == src_gds && src_vaddr < dst_vaddr + size && dst_vaddr < src_vaddr + size) ||
	    (dst_gds && (dst_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - dst_vaddr)) ||
	    (src_gds && (src_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - src_vaddr))) {
		EXIT("BufferCache: invalid or overlapping copy range\n");
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
	auto  src = src_memory ? ObtainBuffer(command, src_vaddr, size, false, true, true)
	                       : BufferBinding {.buffer = m_gds_buffer.Handle(), .offset = src_vaddr};
	auto  dst = dst_memory ? ObtainBuffer(command, dst_vaddr, size, true, false, true)
	                       : BufferBinding {.buffer = m_gds_buffer.Handle(), .offset = dst_vaddr};
	EXIT_IF(src.buffer == nullptr || dst.buffer == nullptr || (dst_memory && dst.owner == nullptr));
	if (src.owner != nullptr) {
		command.RetainResourceUntilFence(src.owner);
	}
	if (dst.owner != nullptr) {
		command.RetainResourceUntilFence(dst.owner);
	}
	if (src.buffer == dst.buffer && src.offset < dst.offset + size &&
	    dst.offset < src.offset + size) {
		EXIT("BufferCache: resolved Vulkan copy ranges overlap\n");
	}
	auto& source = src.owner != nullptr ? *std::static_pointer_cast<Buffer>(src.owner)
	               : src_gds            ? m_gds_buffer
	                                    : m_stream_buffer;
	auto& destination =
	    dst.owner != nullptr ? *std::static_pointer_cast<Buffer>(dst.owner) : m_gds_buffer;
	if (source.Handle() != src.buffer || destination.Handle() != dst.buffer) {
		EXIT("BufferCache: resolved copy owner does not match its Vulkan handle\n");
	}
	destination.CopyFrom(command, source, src.offset, dst.offset, size);
}

bool BufferCache::IsRegionRegistered(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("BufferCache: invalid registered-region query\n");
	}
	FaultSafeCacheLock lock(this, m_mutex);
	// Cached buffers are ordered and non-overlapping. The last buffer beginning before the query
	// end is therefore the only possible intersection.
	const auto candidate = m_buffers.lower_bound(vaddr + size);
	if (candidate == m_buffers.begin()) {
		return false;
	}
	const auto& [address, cached] = *std::prev(candidate);
	return address + cached->size > vaddr;
}

bool BufferCache::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionGpuModified(vaddr, size);
}

bool BufferCache::HasGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	FaultSafeCacheLock lock(this, m_mutex);
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

	std::vector<RetiredBuffer>                                       retires;
	std::vector<std::pair<RetiredBuffer, std::vector<DownloadCopy>>> dirty_retires;
	{
		FaultSafeCacheLock    lock(this, m_mutex);
		std::vector<uint64_t> candidates;
		for (const auto& [address, owner]: m_buffers) {
			const auto& cached = *owner;
			if (tick - std::min(tick, cached.tick_accessed_last) < age) {
				continue;
			}
			candidates.push_back(address);
		}
		std::ranges::sort(candidates, [&](uint64_t left, uint64_t right) {
			return m_buffers.at(left)->tick_accessed_last < m_buffers.at(right)->tick_accessed_last;
		});
		if (candidates.size() > limit) {
			candidates.resize(limit);
		}
		for (const auto address: candidates) {
			auto& cached = *m_buffers.at(address);
			m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, cached.vaddr,
			                                           cached.size, "garbage collection");
			retires.push_back({address, cached.size, cached.buffer});
			// GC runs immediately before submission. Preserve every source referenced by commands
			// already recorded in the active batch.
			m_scheduler.Current().RetainResourceUntilFence(cached.buffer);
		}
		for (const auto& retire: retires) {
			if (!m_memory_tracker.IsRegionGpuModified(retire.address, retire.size)) {
				continue;
			}
			auto& copies = dirty_retires.emplace_back(retire, std::vector<DownloadCopy> {}).second;
			m_memory_tracker.ForEachDownloadRange<false>(
			    retire.address, retire.size,
			    [&](uint64_t address, uint64_t size) noexcept {
				    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, size,
				                                           "garbage collection");
			    },
			    [&](uint64_t address, uint64_t size) noexcept {
				    for (const auto range: m_gpu_modified_ranges.Intersections(address, size)) {
					    copies.push_back({retire.owner, range.address - retire.address,
					                      range.address, range.size});
				    }
			    });
		}
	}

	for (auto& [retire, copies]: dirty_retires) {
		QueueGarbageDownload(copies, std::move(retire));
	}

	FaultSafeCacheLock lock(this, m_mutex);
	for (const auto& retire: retires) {
		auto found = m_buffers.find(retire.address);
		if (found == m_buffers.end() || found->second->size != retire.size ||
		    found->second->buffer != retire.owner) {
			EXIT("BufferCache: garbage-collection owner changed during download\n");
		}
		if (!m_memory_tracker.IsRegionGpuModified(retire.address, retire.size)) {
			m_memory_tracker.UntrackMemory(retire.address, retire.size);
		}
		if (retire.size > m_total_used_memory) {
			EXIT("BufferCache: allocation accounting underflow\n");
		}
		m_total_used_memory -= retire.size;
		m_buffers.erase(found);
	}
}

} // namespace Libs::Graphics
