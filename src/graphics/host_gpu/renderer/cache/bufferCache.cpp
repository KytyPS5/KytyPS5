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
// Two regions of 4 GiB, the longest range a storage buffer binds.
constexpr uint64_t MaxWindowSize = uint64_t {8} << 30;

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

// A sparse table leaves the entries of unused address space without memory; they must read zero.
bool SparsePageTable(const GraphicContext& graphics) {
	return graphics.sparse_residency_buffer_enabled &&
	       graphics.physical_device_properties.sparseProperties.residencyNonResidentStrict;
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

void BufferCache::BindPageTable(uint64_t offset, uint64_t size) {
	if (m_table_pages.empty() || size == 0) {
		return;
	}
	auto&                             sparse = *m_graphics.sparse_binding;
	const auto                        page   = sparse.PageSize();
	std::vector<vk::SparseMemoryBind> binds;
	for (auto index = offset / page; index <= (offset + size - 1) / page; index++) {
		auto& memory = m_table_pages[index];
		if (memory.allocation != nullptr) {
			continue;
		}
		if (!sparse.AllocatePages({&memory, 1}, false)) {
			m_graphics.LogMemoryBudget();
			EXIT("out of memory for the BDA page table\n");
		}
		binds.push_back({index * page, page, memory.memory, memory.offset});
	}
	if (binds.empty()) {
		return;
	}
	// A bind takes effect ahead of the recording it is queued in: the work recorded so far, which
	// reads these entries as zero, finishes before memory not yet zeroed takes their place.
	m_scheduler.Wait(m_scheduler.CurrentTick());
	sparse.BindBuffer(m_bda_pagetable_buffer.Handle(), binds);
	for (const auto& bind: binds) {
		m_bda_pagetable_buffer.Fill(bind.resourceOffset, bind.size, 0);
	}
}

void BufferCache::WritePageTable(uint64_t offset, std::span<const vk::DeviceAddress> entries) {
	const auto size = entries.size() * sizeof(vk::DeviceAddress);
	BindPageTable(offset, size);
	WriteDataBuffer(m_bda_pagetable_buffer, offset, entries.data(), size);
}

void BufferCache::WritePageEntries(const Buffer& buffer, uint64_t begin, uint64_t end) {
	std::vector<vk::DeviceAddress> addresses;
	addresses.reserve((end - begin) >> CACHING_PAGEBITS);
	for (auto address = begin; address < end; address += CACHING_PAGESIZE) {
		addresses.push_back(buffer.BufferDeviceAddress() + buffer.Offset(address));
	}
	WritePageTable(PageIndex(begin) * sizeof(vk::DeviceAddress), addresses);
	QueueBdaSync(begin, end - begin);
}

template <typename Func>
void BufferCache::ForEachPageRun(std::span<const uint64_t> pages, Func&& func) const {
	for (size_t first = 0; first < pages.size();) {
		size_t last = first + 1;
		while (last < pages.size() && pages[last] == pages[last - 1] + m_page_size &&
		       Region(pages[last]) == Region(pages[first])) {
			last++;
		}
		func(pages[first], pages[last - 1] + m_page_size);
		first = last;
	}
}

template <typename Func>
void BufferCache::ForEachResidentPiece(uint64_t vaddr, uint64_t size, Func&& func) {
	m_resident.ForEachInRange(vaddr, size, [&](uint64_t begin, uint64_t end) {
		while (begin < end) {
			const auto piece = std::min(end, (Region(begin) + 1) << m_region_bits);
			func(m_slot_buffers[m_windows.at(Region(begin))], begin, piece - begin);
			begin = piece;
		}
	});
}

BufferId BufferCache::EnsureWindow(uint64_t region) {
	if (const auto found = m_windows.find(region); found != m_windows.end()) {
		return found->second;
	}
	const auto  base   = region << m_region_bits;
	const auto  stride = uint64_t {1} << m_region_bits;
	const auto  id = m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, base,
	                                       AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress,
	                                       2 * stride, Buffer::Sparse::Aliased);
	const auto& window = m_slot_buffers[id];
	SetVulkanObjectNameF(m_graphics.device, window.Handle(), "Kyty.GuestWindow[guest=0x{:016x}]",
	                     base);
	m_windows.emplace(region, id);
	// Its upper half is the region above, whose resident pages it shares.
	std::vector<vk::SparseMemoryBind> binds;
	m_resident.ForEachInRange(base + stride, stride, [&](uint64_t begin, uint64_t end) {
		for (auto address = begin; address < end; address += m_page_size) {
			auto& page = m_pages.at(address);
			page.lower = true;
			binds.push_back(
			    {window.Offset(address), m_page_size, page.memory.memory, page.memory.offset});
		}
	});
	if (!binds.empty()) {
		m_graphics.sparse_binding->BindBuffer(window.Handle(), binds);
	}
	return id;
}

void BufferCache::AllocatePages(std::span<SparsePage> pages) {
	auto& sparse = *m_graphics.sparse_binding;
	if (sparse.AllocatePages(pages, true)) {
		return;
	}
	// Sparse buffers take no host memory: idle pages make room, or the pages pass the budget.
	const auto bytes = pages.size() * m_page_size;
	if (ReclaimPages(bytes) && sparse.AllocatePages(pages, true)) {
		return;
	}
	if (!sparse.AllocatePages(pages, false)) {
		m_graphics.LogMemoryBudget();
		EXIT("out of device memory for %" PRIu64 " bytes of guest buffer pages\n", bytes);
	}
	m_graphics.NotePastBudget(bytes);
}

void BufferCache::CommitPages(uint64_t begin, uint64_t end) {
	// The pages to make resident: new ones, and released ones that are still bound.
	std::vector<uint64_t> pages;
	std::vector<uint64_t> fresh;
	for (auto address = begin; address < end; address += m_page_size) {
		const auto found = m_pages.find(address);
		if (found == m_pages.end()) {
			fresh.push_back(address);
		} else if (found->second.release == 0) {
			continue;
		}
		pages.push_back(address);
	}
	std::vector<SparsePage> memory(fresh.size());
	AllocatePages(memory);
	std::map<BufferId, std::vector<vk::SparseMemoryBind>> binds;
	for (size_t index = 0; index < fresh.size(); index++) {
		const auto region            = Region(fresh[index]);
		const auto home              = EnsureWindow(region);
		m_pages[fresh[index]].memory = memory[index];
		m_region_pages[region]++;
		binds[home].push_back({m_slot_buffers[home].Offset(fresh[index]), m_page_size,
		                       memory[index].memory, memory[index].offset});
	}
	for (const auto address: pages) {
		auto&      page   = m_pages.at(address);
		const auto region = Region(address);
		// A released page whose unbind has not run is kept as it is.
		page.release     = 0;
		page.lru_id      = m_page_lru.Insert(address, m_gc_tick);
		const auto below = region != 0 ? m_windows.find(region - 1) : m_windows.end();
		if (!page.lower && below != m_windows.end()) {
			page.lower = true;
			binds[below->second].push_back({m_slot_buffers[below->second].Offset(address),
			                                m_page_size, page.memory.memory, page.memory.offset});
		}
	}
	for (const auto& [id, list]: binds) {
		m_graphics.sparse_binding->BindBuffer(m_slot_buffers[id].Handle(), list);
	}
	ForEachPageRun(pages, [this](uint64_t first, uint64_t last) {
		const auto id = m_windows.at(Region(first));
		for (auto address = first; address < last; address += CACHING_PAGESIZE) {
			m_page_table[address >> PageTable::kPageBits] = id;
		}
		m_resident.Add(first, last - first);
		m_total_used_memory += last - first;
		WritePageEntries(m_slot_buffers[id], first, last);
	});
}

void BufferCache::TouchPages(uint64_t vaddr, uint64_t size) {
	if (size == 0) {
		return;
	}
	const auto begin = Common::AlignDown(vaddr, m_page_size);
	const auto end   = Common::AlignUp(vaddr + size, m_page_size);
	if (m_touched_tick != m_gc_tick) {
		m_touched      = {};
		m_touched_tick = m_gc_tick;
	}
	if (m_touched.Contains(begin, end - begin)) {
		return;
	}
	m_resident.ForEachInRange(begin, end - begin, [this](uint64_t first, uint64_t last) {
		for (auto address = first; address < last; address += m_page_size) {
			m_page_lru.Touch(m_pages.at(address).lru_id, m_gc_tick);
		}
	});
	m_touched.Add(begin, end - begin);
}

uint64_t BufferCache::HeldBytes(const Buffer& buffer, uint64_t address, uint64_t size) const {
	if (size == 0 || !buffer.IsInBounds(address, 1)) {
		return 0;
	}
	const auto bytes = std::min(size, buffer.CpuAddress() + buffer.Size() - address);
	if (!buffer.IsSparse()) {
		return bytes;
	}
	uint64_t held = 0;
	m_resident.ForEachInRange(address, bytes, [&](uint64_t begin, uint64_t end) {
		if (begin == address + held) {
			held = end - address;
		}
	});
	return held;
}

std::vector<BufferCache::PageRelease> BufferCache::ReleasePages(std::span<const uint64_t> pages) {
	std::vector<PageRelease> releases;
	releases.reserve(pages.size());
	for (const auto address: pages) {
		auto& page = m_pages.at(address);
		EXIT_IF(page.release != 0);
		page.release = ++m_releases;
		m_page_lru.Free(page.lru_id);
		releases.push_back({address, page.release});
	}
	ForEachPageRun(pages, [this](uint64_t first, uint64_t last) {
		// Guest memory holds the bytes: the next use of the pages uploads them again.
		m_memory_tracker.MarkRegionAsCpuModified(first, last - first);
		for (auto address = first; address < last; address += CACHING_PAGESIZE) {
			m_page_table[address >> PageTable::kPageBits] = {};
		}
		m_resident.Subtract(first, last - first);
		m_total_used_memory -= last - first;
		m_bda_pagetable_buffer.Fill(
		    PageIndex(first) * sizeof(vk::DeviceAddress),
		    ((last - first) >> CACHING_PAGEBITS) * sizeof(vk::DeviceAddress), 0);
	});
	Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferEvictions, pages.size());
	return releases;
}

void BufferCache::UnbindReleased(std::span<const PageRelease> releases) {
	// A page used again since then, or unbound already, has another release or none.
	std::vector<uint64_t> pages;
	for (const auto& [address, release]: releases) {
		const auto found = m_pages.find(address);
		if (found != m_pages.end() && found->second.release == release) {
			pages.push_back(address);
		}
	}
	auto& sparse = *m_graphics.sparse_binding;
	ForEachPageRun(pages, [&](uint64_t first, uint64_t last) {
		const auto              region = Region(first);
		const auto&             home   = m_slot_buffers[m_windows.at(region)];
		std::vector<SparsePage> memory;
		for (auto address = first; address < last; address += m_page_size) {
			const auto found = m_pages.find(address);
			if (found->second.lower) {
				const auto& below = m_slot_buffers[m_windows.at(region - 1)];
				sparse.UnbindBuffer(below.Handle(), below.Offset(address), m_page_size, {});
			}
			memory.push_back(found->second.memory);
			m_pages.erase(found);
		}
		sparse.UnbindBuffer(home.Handle(), home.Offset(first), last - first, memory);
		if ((m_region_pages[region] -= memory.size()) == 0) {
			m_region_pages.erase(region);
		}
	});
}

bool BufferCache::ReclaimPages(uint64_t bytes) {
	if (m_gc_tick == 0 || !m_scheduler.Active()) {
		return false;
	}
	// Pages no work of this frame bound. A DMA shader reaches a page without a bind, so the
	// pages one was reported to use leave only by age.
	std::vector<uint64_t> idle;
	m_page_lru.ForEachItemBelow(m_gc_tick - 1, [&](uint64_t address) {
		if (!m_dma_accessed.Intersects(address, m_page_size) &&
		    !m_memory_tracker.IsRegionGpuModified(address, m_page_size)) {
			idle.push_back(address);
		}
		return idle.size() * m_page_size >= bytes;
	});
	if (idle.size() * m_page_size < bytes) {
		return false;
	}
	std::ranges::sort(idle);
	const auto releases = ReleasePages(idle);
	// The work recorded so far may use the pages: it runs before they are unbound.
	m_scheduler.Wait(m_scheduler.CurrentTick());
	UnbindReleased(releases);
	m_graphics.sparse_binding->FlushNow();
	m_graphics.sparse_binding->WaitIdle();
	return true;
}

void BufferCache::WriteBackPages(std::span<const uint64_t> pages) {
	ForEachPageRun(pages, [this](uint64_t first, uint64_t last) {
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, first, last - first,
		                                           "garbage collection");
		EXIT_NOT_IMPLEMENTED(!DownloadBufferMemory(m_slot_buffers[m_windows.at(Region(first))],
		                                           first, last - first));
	});
	// Publish all queued downloads before releasing their tracked pages.
	const auto completion_tick = m_scheduler.CurrentTick();
	m_scheduler.Wait(completion_tick);
	m_scheduler.WaitPriorityOperations(completion_tick);
	ForEachPageRun(pages, [this](uint64_t first, uint64_t last) {
		m_memory_tracker.UnmarkRegionAsGpuModified(first, last - first);
	});
}

void BufferCache::CollectPages(uint64_t before, bool aggressive) {
	// Pages the GPU wrote go only under critical pressure, written back first.
	std::vector<uint64_t> idle;
	std::vector<uint64_t> written;
	m_page_lru.ForEachItemBelow(before, [&](uint64_t address) {
		if (!m_memory_tracker.IsRegionGpuModified(address, m_page_size)) {
			idle.push_back(address);
		} else if (aggressive) {
			written.push_back(address);
		}
	});
	ReleaseIdle(std::move(idle), std::move(written));
}

void BufferCache::ReleaseIdle(std::vector<uint64_t> idle, std::vector<uint64_t> written) {
	if (!written.empty()) {
		std::ranges::sort(written);
		WriteBackPages(written);
		idle.insert(idle.end(), written.begin(), written.end());
	}
	if (idle.empty()) {
		return;
	}
	std::ranges::sort(idle);
	auto releases = ReleasePages(idle);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation(
		    [this, releases = std::move(releases)] { UnbindReleased(releases); });
	} else {
		UnbindReleased(releases);
	}
}

uint64_t BufferCache::ReclaimIdle(uint64_t bytes) {
	if (!Windowed() || m_gc_tick < 2) {
		return 0;
	}
	std::vector<uint64_t> idle;
	std::vector<uint64_t> written;
	const auto enough = [&] { return (idle.size() + written.size()) * m_page_size >= bytes; };
	// Clean pages need no write-back. A DMA shader reaches a page without a bind, so the pages
	// one was reported to use leave only by age.
	for (const bool modified: {false, true}) {
		if (enough()) {
			break;
		}
		m_page_lru.ForEachItemBelow(m_gc_tick - 2, [&](uint64_t address) {
			if (!m_dma_accessed.Intersects(address, m_page_size) &&
			    m_memory_tracker.IsRegionGpuModified(address, m_page_size) == modified) {
				(modified ? written : idle).push_back(address);
			}
			return enough();
		});
	}
	const auto released = (idle.size() + written.size()) * m_page_size;
	ReleaseIdle(std::move(idle), std::move(written));
	return released;
}

void BufferCache::RetireIdleWindows() {
	for (auto it = m_windows.begin(); it != m_windows.end();) {
		const auto [region, id] = *it;
		if (m_region_pages.contains(region)) {
			++it;
			continue;
		}
		// The pages of the region above lose their place in the window.
		for (auto& [address, page]: m_pages) {
			if (Region(address) == region + 1) {
				page.lower = false;
			}
		}
		it = m_windows.erase(it);
		if (m_scheduler.Active()) {
			m_scheduler.DeferOperation([this, id] { m_slot_buffers.erase(id); });
		} else {
			m_slot_buffers.erase(id);
		}
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
		m_total_used_memory += buffer.Size();
		buffer.lru_id = m_lru_cache.Insert(id, m_gc_tick);
		WritePageEntries(buffer, buffer.CpuAddress(), buffer.CpuAddress() + buffer.Size());
	} else {
		m_buffers.erase(m_buffers.find(buffer.CpuAddress()));
		m_total_used_memory -= buffer.Size();
		m_lru_cache.Free(buffer.lru_id);
		m_bda_pagetable_buffer.Fill(table_offset,
		                            size_pages * sizeof(vk::DeviceAddress), 0);
		buffer.is_deleted = true;
	}
}

void BufferCache::TouchBuffer(const Buffer& buffer, uint64_t vaddr, uint64_t size) {
	if (buffer.IsSparse()) {
		TouchPages(vaddr, size);
	} else {
		m_lru_cache.Touch(buffer.lru_id, m_gc_tick);
	}
}

void BufferCache::DeleteBuffer(BufferId id) {
	Unregister(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_buffers.erase(id); });
	} else {
		m_slot_buffers.erase(id);
	}
}

bool BufferCache::DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size,
                                       RangeSet* carried) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size     = 0;
	const auto                  buffer_address = buffer.CpuAddress();
	m_memory_tracker.ForEachDownloadRange<false>(
	    vaddr, size, [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "buffer download");
		    const auto copy = [&](uint64_t start, uint64_t end) {
			    copies.emplace_back(start - buffer_address, total_size, end - start);
			    // Start each packed range on its own 64-byte cache line.
			    total_size += Common::AlignUp(end - start, 64);
		    };
		    if (carried != nullptr) {
			    // One readback in flight per byte: none publishes it after it has landed.
			    UncarriedBytes(address, bytes).ForEach([&](uint64_t start, uint64_t end) {
				    copy(start, end);
				    carried->Add(start, end - start);
			    });
		    } else {
			    m_gpu_modified_ranges.ForEachInRange(address, bytes, copy);
			    m_gpu_modified_ranges.Subtract(address, bytes);
		    }
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
	auto publish = [total_size, buffer_address, copies = std::move(copies),
	                download = std::move(download)] {
		download.buffer->Invalidate(download.offset, total_size);
		for (const auto& copy: copies) {
			Libs::LibKernel::Memory::WriteBacking(
			    buffer_address + copy.srcOffset,
			    download.mapped + (copy.dstOffset - download.offset), copy.size);
		}
	};
	m_scheduler.DeferPriorityOperation(std::move(publish));
	return true;
}

BufferCache::BufferCache(GraphicContext& graphics, CommandScheduler& scheduler,
                         PageManager& page_manager, TextureCache& texture_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_fault_manager(graphics, scheduler, *this),
      m_gds_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, GdsBufferSize),
      m_bda_pagetable_buffer(
          graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, BDA_PAGETABLE_SIZE,
          SparsePageTable(graphics) ? Buffer::Sparse::Residency : Buffer::Sparse::None),
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
		const auto page = m_graphics.sparse_binding->PageSize();
		EXIT_IF(BDA_PAGETABLE_SIZE % page != 0);
		m_table_pages.resize(BDA_PAGETABLE_SIZE / page);
	}
	m_scheduler.BeginCommand();
	for (auto* buffer: {&m_bda_pagetable_buffer, m_fault_manager.GetFaultBuffer()}) {
		// A sparse table has no pages yet; each is zeroed as it is bound.
		if (!buffer->IsSparse()) {
			buffer->Fill(0, buffer->Size(), 0);
		}
	}
	const auto null_id =
	    m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
	EXIT_IF(null_id != NULL_BUFFER_ID);
	SetVulkanObjectNameF(m_graphics.device, GetBuffer(null_id).Handle(), "Kyty.NullBuffer");
	if (m_graphics.sparse_residency_aliased_enabled) {
		// A page holds whole BDA pages, and a window is the largest buffer the device creates.
		const auto page   = m_graphics.sparse_binding->PageSize();
		const auto window = std::min(std::bit_floor(m_graphics.max_buffer_size), MaxWindowSize);
		if (page % CACHING_PAGESIZE == 0 && window >= 2 * page) {
			m_page_size   = page;
			m_region_bits = static_cast<uint32_t>(std::countr_zero(window)) - 1;
		}
	}
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
	if (Windowed()) {
		m_resident.ForEach([this](uint64_t begin, uint64_t end) {
			if (m_memory_tracker.IsRegionGpuModified(begin, end - begin)) {
				EXIT("BufferCache: destroyed with GPU-modified buffer\n");
			}
		});
		// The windows go before the memory bound into them.
		for (const auto& [region, id]: m_windows) {
			(void)region;
			m_slot_buffers.erase(id);
		}
		auto& sparse = *m_graphics.sparse_binding;
		sparse.WaitIdle();
		for (const auto& [address, page]: m_pages) {
			(void)address;
			sparse.FreePages({&page.memory, 1});
		}
	}
	if (!m_table_pages.empty()) {
		// No bind, queued or executing, may name the pages once they are freed.
		auto& sparse = *m_graphics.sparse_binding;
		sparse.Forget(m_bda_pagetable_buffer.Handle());
		sparse.WaitIdle();
		sparse.FreePages(m_table_pages);
	}
}

void BufferCache::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	m_memory_tracker.InvalidateRegion(vaddr, size,
	                                  [this, vaddr, size] { ReadMemory(vaddr, size, true); });
	QueueBdaSync(vaddr, size);
}

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write) {
	if (!GuestGpu::IsGpuThread() && CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	auto&    gpu  = m_scheduler.Context().GetGpu();
	uint64_t tick = 0;
	gpu.SendCommandSync([&] { tick = BeginReadback(vaddr, size, is_write); });
	if (tick != 0) {
		// The thread that asked waits for the copies; the GPU thread goes on recording.
		m_scheduler.WaitPriorityOperations(tick);
		gpu.SendCommandSync([&] { EndReadback(vaddr, size, is_write, tick); });
	}
}

uint64_t BufferCache::BeginReadback(uint64_t vaddr, uint64_t size, bool is_write) {
	if (is_write && !IsRegionRegistered(vaddr, size)) {
		return 0;
	}
	auto* joined = Windowed() ? nullptr : &m_slot_buffers[FindBuffer(vaddr, size)];
	if (IsRegionGpuModified(vaddr, size)) {
		if (const auto tick = PendingReadback(vaddr, size); tick != 0) {
			return tick;
		}
		// In the list before its copies are recorded: a GPU write among them is not carried.
		const auto readback = m_readbacks.emplace(m_readbacks.end());
		auto       begin    = Common::AlignDown(vaddr, DownloadWindowSize);
		auto       end      = begin;
		if (joined != nullptr) {
			begin = std::max(begin, joined->CpuAddress());
			end   = std::min(std::max(begin + DownloadWindowSize, vaddr + size),
			                 joined->CpuAddress() + joined->Size());
			(void)DownloadBufferMemory(*joined, begin, end - begin, &readback->bytes);
		} else {
			// GPU-written bytes are in resident pages, so reading them back takes no new ones.
			const auto limit = vaddr < LOWER_ADDRESS_SIZE
			                       ? LOWER_ADDRESS_SIZE
			                       : LibKernel::Memory::kExtendedMemoryBase +
			                             LibKernel::Memory::kExtendedMemorySize;
			end              = std::min(std::max(begin + DownloadWindowSize, vaddr + size), limit);
			for (auto address = begin; address < end;) {
				const auto piece = std::min(end, (Region(address) + 1) << m_region_bits);
				if (const auto window = m_windows.find(Region(address));
				    window != m_windows.end()) {
					(void)DownloadBufferMemory(m_slot_buffers[window->second], address,
					                           piece - address, &readback->bytes);
				}
				address = piece;
			}
		}
		if (!readback->bytes.Empty()) {
			readback->begin = begin;
			readback->end   = end;
			// Recording a copy can submit: the last one's tick publishes them all.
			readback->tick = m_scheduler.CurrentTick();
			m_scheduler.Flush();
			return readback->tick;
		}
		m_readbacks.erase(readback);
	}
	if (is_write) {
		m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
		QueueBdaSync(vaddr, size);
	}
	return 0;
}

uint64_t BufferCache::PendingReadback(uint64_t vaddr, uint64_t size) const {
	const auto begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	if (!UncarriedBytes(begin, end - begin).Empty()) {
		return 0;
	}
	// The last one recorded publishes after the others.
	for (auto readback = m_readbacks.rbegin(); readback != m_readbacks.rend(); ++readback) {
		if (readback->bytes.Intersects(begin, end - begin)) {
			return readback->tick;
		}
	}
	return 0;
}

RangeSet BufferCache::UncarriedBytes(uint64_t vaddr, uint64_t size) const {
	RangeSet bytes;
	m_gpu_modified_ranges.ForEachInRange(
	    vaddr, size, [&](uint64_t begin, uint64_t end) { bytes.Add(begin, end - begin); });
	for (const auto& readback: m_readbacks) {
		readback.bytes.ForEachInRange(
		    vaddr, size, [&](uint64_t begin, uint64_t end) { bytes.Subtract(begin, end - begin); });
	}
	return bytes;
}

void BufferCache::EndReadback(uint64_t vaddr, uint64_t size, bool is_write, uint64_t tick) {
	LandReadbacks(tick);
	// Written again before it landed: the GPU thread waits for this readback itself.
	if (IsRegionGpuModified(vaddr, size)) {
		if (const auto again = BeginReadback(vaddr, size, false); again != 0) {
			m_scheduler.WaitPriorityOperations(again);
			LandReadbacks(again);
		}
	}
	if (is_write) {
		m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
		QueueBdaSync(vaddr, size);
	}
}

void BufferCache::LandReadbacks(uint64_t tick) {
	std::erase_if(m_readbacks, [&](const Readback& readback) {
		if (readback.tick > tick) {
			return false;
		}
		readback.bytes.ForEach([this](uint64_t begin, uint64_t end) {
			m_gpu_modified_ranges.Subtract(begin, end - begin);
		});
		// Guest memory is current for the pages left without GPU-written bytes.
		const auto last  = Common::AlignUp(readback.end, TRACKER_PAGE_SIZE);
		auto       clean = Common::AlignDown(readback.begin, TRACKER_PAGE_SIZE);
		m_gpu_modified_ranges.ForEachInRange(
		    clean, last - clean, [&](uint64_t begin, uint64_t end) {
			    if (const auto written = Common::AlignDown(begin, TRACKER_PAGE_SIZE);
			        written > clean) {
				    m_memory_tracker.UnmarkRegionAsGpuModified(clean, written - clean);
			    }
			    clean = std::max(clean, Common::AlignUp(end, TRACKER_PAGE_SIZE));
		    });
		if (clean < last) {
			m_memory_tracker.UnmarkRegionAsGpuModified(clean, last - clean);
		}
		return true;
	});
}

BufferId BufferCache::FindBuffer(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0) {
		return NULL_BUFFER_ID;
	}
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid buffer discovery request\n");
	}
	if (Windowed()) {
		if (size > uint64_t {1} << m_region_bits) {
			EXIT("BufferCache: a range of %" PRIu64 " bytes is longer than a window region\n",
			     size);
		}
		if (m_found_tick != m_gc_tick) {
			m_found.fill({});
			m_found_tick = m_gc_tick;
		}
		auto& found = m_found[(vaddr * 0x9e3779b97f4a7c15ull) >> 56];
		if (found.vaddr == vaddr && found.size == size) {
			return found.id;
		}
		const auto id    = EnsureWindow(Region(vaddr));
		const auto begin = Common::AlignDown(vaddr, m_page_size);
		const auto end   = Common::AlignUp(vaddr + size, m_page_size);
		// Touched first: making room for new pages takes none this frame used.
		TouchPages(vaddr, size);
		if (!m_resident.Contains(begin, end - begin)) {
			CommitPages(begin, end);
		}
		found = {vaddr, size, id};
		return id;
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
	m_memory_tracker.ForEachUploadRange(
	    vaddr, size, is_written,
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    copies.emplace_back(total_size, buffer.Offset(address), bytes);
		    total_size += bytes;
	    },
	    [&]() noexcept {
		    const BufferUpload upload {&buffer, copies.size()};
		    UploadCopies({&upload, 1}, copies, total_size);
	    });
	if (is_texel_buffer && !is_written) {
		return SynchronizeBufferFromImage(buffer, vaddr, size);
	}
	return false;
}

void BufferCache::UploadCopies(std::span<const BufferUpload> uploads,
                               std::span<vk::BufferCopy> copies, uint64_t total_size) {
	if (copies.empty()) {
		return;
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferUploadBytes, total_size);

	const auto source = UploadMemory(
	    m_graphics, m_scheduler, m_staging_buffer, total_size, 4,
	    vk::BufferUsageFlagBits::eTransferSrc, [&](uint8_t* mapped, uint64_t base_offset) {
		    size_t first = 0;
		    for (const auto& upload: uploads) {
			    for (auto& copy: copies.subspan(first, upload.end - first)) {
				    const auto address = upload.buffer->CpuAddress() + copy.dstOffset;
				    if (!LibKernel::Memory::CopySparseMemory(address, mapped + copy.srcOffset,
					                                         copy.size)) {
					    EXIT("BufferCache: failed to read sparse guest buffer memory\n");
				    }
				    copy.srcOffset += base_offset;
			    }
			    first = upload.end;
		    }
	    });
	auto& command = m_scheduler.Current();
	command.EndRendering();
	const auto              native = command.Handle();
	const vk::MemoryBarrier before {
	    .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
		                 vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite,
	    .dstAccessMask = vk::AccessFlagBits::eTransferWrite};
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
	                       1, &before, 0, nullptr, 0, nullptr);
	size_t first = 0;
	for (const auto& upload: uploads) {
		native.copyBuffer(source.first->Handle(), upload.buffer->Handle(),
		                  static_cast<uint32_t>(upload.end - first), copies.data() + first);
		first = upload.end;
	}
	const vk::MemoryBarrier after {.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
	                               .dstAccessMask = vk::AccessFlagBits::eMemoryRead |
	                                                vk::AccessFlagBits::eMemoryWrite};
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands,
	                       vk::DependencyFlagBits::eByRegion, 1, &after, 0, nullptr, 0, nullptr);
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer(uint64_t vaddr, uint64_t size,
                                                       bool is_written, bool is_texel_buffer,
                                                       BufferId id) {
	EXIT_IF(!(GuestRange {vaddr, size}.Valid()));

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

	// A window holds any range, but only FindBuffer makes the pages of one resident and used.
	if (Windowed() || IsBufferInvalid(id) || !m_slot_buffers[id].IsInBounds(vaddr, size)) {
		id = FindBuffer(vaddr, size);
	}
	auto& buffer = m_slot_buffers[id];
	if (!Windowed()) {
		TouchBuffer(buffer, vaddr, size);
	}
	(void)SynchronizeBuffer(buffer, vaddr, size, is_written, is_texel_buffer);
	if (is_written) {
		m_gpu_modified_ranges.Add(vaddr, size);
		// A readback recorded earlier does not carry these bytes.
		for (auto& readback: m_readbacks) {
			readback.bytes.Subtract(vaddr, size);
		}
		ForgetColorMetadataChecks(vaddr, size);
	}
	return {&buffer, buffer.Offset(vaddr)};
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size) {
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (HeldBytes(buffer, vaddr, size) == size) {
			TouchBuffer(buffer, vaddr, size);
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
		// Access the guest mapping so write faults invalidate cached buffers and images.
		auto* destination = reinterpret_cast<uint32_t*>(vaddr);
		std::fill(destination, destination + size / sizeof(uint32_t), value);
		return;
	}

	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true);
	dst->Fill(dst_offset, size, value);
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

	// Two windows can hold the same bytes, so the ranges are compared, not the buffers.
	if (Windowed() && src_memory && dst_memory && src_vaddr < dst_vaddr + size &&
	    dst_vaddr < src_vaddr + size) {
		EXIT("BufferCache: overlapping copy\n");
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
	if (Windowed()) {
		return m_resident.Intersects(vaddr, size);
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
	if (Windowed()) {
		m_graphics.sparse_binding->Collect();
		RetireIdleWindows();
	}
	if (m_graphics.CanReportMemoryUsage()) {
		// Memory released stays in the allocator's blocks, where the next allocations take it.
		m_total_used_memory = m_graphics.GetDeviceMemoryInUse();
	}
	if (m_total_used_memory < m_trigger_gc_memory) {
		return;
	}

	const bool     aggressive = m_total_used_memory >= m_critical_gc_memory;
	const uint64_t age        = std::min<uint64_t>(aggressive ? 80 : 160, tick);
	if (Windowed()) {
		CollectPages(tick - age, aggressive);
		return;
	}
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
			EXIT_NOT_IMPLEMENTED(!DownloadBufferMemory(buffer, buffer.CpuAddress(), buffer.Size()));
			dirty_buffers.push_back(id);
		} else {
			m_memory_tracker.MarkRegionAsCpuModified(buffer.CpuAddress(), buffer.Size());
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
		Unregister(id);
		m_slot_buffers.erase(id);
	}
}

void BufferCache::ProcessFaultBuffer() {
	m_fault_manager.ProcessFaultBuffer();
}

void BufferCache::MarkGpuUse(uint64_t vaddr, uint64_t size) {
	if (Windowed()) {
		m_dma_accessed.Add(vaddr, size);
		TouchPages(vaddr, size);
		return;
	}
	const auto end = vaddr + size;
	// Buffers never overlap, so only the one before vaddr can reach into the range.
	auto it = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		--it;
	}
	for (; it != m_buffers.end() && it->first < end; ++it) {
		const auto& buffer = m_slot_buffers[it->second];
		if (buffer.CpuAddress() + buffer.Size() > vaddr) {
			TouchBuffer(buffer, vaddr, size);
		}
	}
}

void BufferCache::QueueBdaSync(uint64_t vaddr, uint64_t size) {
	const auto begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	std::lock_guard lock(m_bda_sync_mutex);
	m_bda_sync.Add(begin, end - begin);
}

void BufferCache::SynchronizeBda(const RangeSet& mapped) {
	const auto queued = [this] {
		std::lock_guard lock(m_bda_sync_mutex);
		return std::exchange(m_bda_sync, {});
	}();
	std::vector<vk::BufferCopy> copies;
	std::vector<BufferUpload>   uploads;
	uint64_t                    total_size = 0;
	const auto                  collect    = [&](Buffer& buffer, uint64_t first, uint64_t last) {
		// Reads follow every range's protection; a write in between re-queues its page.
		m_memory_tracker.ForEachUploadRange(
		    first, last - first, false,
		    [&](uint64_t address, uint64_t bytes) noexcept {
			    if (uploads.empty() || uploads.back().buffer != &buffer) {
				    uploads.push_back({&buffer, 0});
			    }
			    copies.emplace_back(total_size, buffer.Offset(address), bytes);
			    uploads.back().end = copies.size();
			    total_size += bytes;
		    },
		    []() noexcept {});
	};
	queued.ForEach([&](uint64_t begin, uint64_t end) {
		mapped.ForEachInRange(begin, end - begin, [&](uint64_t start, uint64_t finish) {
			if (Windowed()) {
				ForEachResidentPiece(start, finish - start,
				                     [&](Buffer& window, uint64_t address, uint64_t bytes) {
					                     collect(window, address, address + bytes);
				                     });
				return;
			}
			auto it = m_buffers.upper_bound(start);
			if (it != m_buffers.begin()) {
				--it;
			}
			for (; it != m_buffers.end() && it->first < finish; ++it) {
				auto&      buffer = m_slot_buffers[it->second];
				const auto first  = std::max(buffer.CpuAddress(), start);
				const auto last   = std::min(buffer.CpuAddress() + buffer.Size(), finish);
				if (first < last) {
					collect(buffer, first, last);
				}
			}
		});
	});
	UploadCopies(uploads, copies, total_size);
}

} // namespace Libs::Graphics
