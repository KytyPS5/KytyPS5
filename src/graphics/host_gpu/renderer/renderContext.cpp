#include "graphics/host_gpu/renderer/renderContext.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/emulatorConfig.h"
#include "common/hackNotice.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/scout.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/shader/recompiler/frontend/decode/RecordReach.h"
#include "graphics/shader/shaderBindings.h"
#include "kernel/memory.h"
#include "libs/errno.h"
#include "loader/demonsSoulsPageFree.h"

#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>

namespace Libs::Graphics {

namespace {

// How far CacheDmaBases() caches after the base of a V# the GPU computes: the mapped memory that
// follows it without a gap, up to this.
constexpr uint64_t VsharpBaseReach = uint64_t {32} << 20u;

} // namespace

RenderContext::RenderContext(GraphicContext& graphics, const std::filesystem::path& shader_store)
    : m_graphics(graphics), m_render_executor(*this),
      m_command_scheduler(*this, graphics,
                          (Config::GpuPipelineStages() & Config::GPU_PIPELINE_RECORDING) != 0),
      m_descriptor_heap(graphics, m_command_scheduler.GetMasterSemaphore()),
      m_pipeline_cache(graphics, &m_image_heap, shader_store), m_sampler_cache(graphics),
      m_buffer_cache(graphics, m_command_scheduler, m_page_manager, m_texture_cache, this),
      m_texture_cache(graphics, m_command_scheduler, m_page_manager, m_buffer_cache),
      m_image_heap(*this, m_page_manager) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	m_ray_tracing = std::make_unique<RayTracing>(*this);
}

RenderContext::~RenderContext() {
	ShutdownGpu();
	m_command_scheduler.Shutdown();
}

void RenderContext::InitializeGpu(VideoOut::VideoOutDriver* video_out) {
	EXIT_IF(m_gpu != nullptr);
	m_video_out = video_out;
	m_gpu       = std::make_unique<GuestGpu>(*this);
}

void RenderContext::ShutdownGpu() {
	if (m_gpu != nullptr) {
		m_gpu->Shutdown();
		m_gpu.reset();
	}
	if (m_video_out != nullptr) {
		if (m_command_scheduler.Active()) {
			m_command_scheduler.Finish();
		}
		m_command_scheduler.DrainPriorityOperations();
		m_video_out = nullptr;
	}
}

GuestGpu& RenderContext::GetGpu() const {
	EXIT_IF(m_gpu == nullptr);
	return *m_gpu;
}

VideoOut::VideoOutDriver& RenderContext::GetVideoOut() const {
	EXIT_IF(m_video_out == nullptr);
	return *m_video_out;
}

bool RenderContext::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	// The host reports the faulting byte, not the instruction's access width. Both caches
	// resolve its page; guessing a width can cross the end of a valid guest mapping.
	constexpr uint64_t fault_size = 1;
	// The caches also watch pages of guest code, and their protections carry no execute
	// permission. Once no read watcher holds the page (the read below lets them go), it gets the
	// watchers' access back plus execute.
	if (access == PageFaultAccess::Execute && LibKernel::Memory::IsGuestExecutable(fault_vaddr) &&
	    m_page_manager.AllowExecute(fault_vaddr)) {
		return true;
	}
	if (!IsMapped(fault_vaddr, fault_size)) {
		// Cache tracking may still protect a page whose range left the GPU-mapped set; that
		// fault is ours to resolve, not a guest access violation.
		if (!GuestRange {fault_vaddr, fault_size}.Valid() || !m_page_manager.IsWatched(fault_vaddr)) {
			return false;
		}
	}
	// The access retries once the image write-backs over its page land.
	if (WaitWriteBacks(fault_vaddr, fault_size)) {
		return true;
	}
	if (access == PageFaultAccess::Write && GuestGpu::IsGpuThread()) {
		// A write of the command stream: device-address readers after it must see it.
		m_bda_generation++;
	}
	if (access == PageFaultAccess::Write) {
		m_buffer_cache.InvalidateFaultedPage(fault_vaddr);
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_image_heap.InvalidateMemory(fault_vaddr, fault_size);
	} else {
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size, false, true);
		m_texture_cache.FlushLazyReadback(fault_vaddr, fault_size);
	}
	return true;
}

bool RenderContext::WaitWriteBacks(uint64_t vaddr, uint64_t size) {
	const auto tick = m_texture_cache.WriteBackTick(vaddr, size);
	if (tick == 0) {
		return false;
	}
	// The GPU thread can fault inside a draw, so it submits without running deferred operations.
	if (GuestGpu::IsGpuThread()) {
		m_command_scheduler.Wait(tick);
	} else if (tick == m_command_scheduler.CurrentTick()) {
		GetGpu().SendCommandSync([this, tick] { m_command_scheduler.Wait(tick); });
	}
	m_command_scheduler.WaitPriorityOperations(tick);
	return true;
}

void RenderContext::WriteFromCompletion(void* destination, const void* data,
                                        uint64_t size) noexcept {
	const auto vaddr = reinterpret_cast<uint64_t>(destination);
	// Through the backing store the bytes never fault, even on a page the execution thread
	// hands back to the GPU while the completion runs. The caches then learn of the write as from
	// a write fault, except that pages the GPU owns stay GPU-owned (a readback of them downloads
	// only the bytes the GPU wrote).
	if (GuestRange {vaddr, size}.Valid() && LibKernel::Memory::TryWriteBacking(vaddr, data, size)) {
		const auto page_size = TRACKER_PAGE_SIZE;
		for (auto page = vaddr & ~(page_size - 1); page < vaddr + size; page += page_size) {
			if (m_page_manager.IsWatched(page)) {
				m_buffer_cache.InvalidateUnlessGpuModified(vaddr, size);
				m_texture_cache.InvalidateMemory(vaddr, size);
				m_image_heap.InvalidateMemory(vaddr, size);
				break;
			}
		}
		return;
	}
	if (size == sizeof(uint32_t)) {
		uint32_t value = 0;
		std::memcpy(&value, data, sizeof(value));
		*static_cast<volatile uint32_t*>(destination) = value;
	} else if (size == sizeof(uint64_t)) {
		uint64_t value = 0;
		std::memcpy(&value, data, sizeof(value));
		*static_cast<volatile uint64_t*>(destination) = value;
	} else {
		std::memcpy(destination, data, size);
	}
	LibKernel::Memory::NoteEmulatorWrite(vaddr, size);
}

void RenderContext::WriteQwordsFromCompletion(void* destination, const uint64_t* values,
                                              uint32_t count, uint64_t stride) noexcept {
	const auto vaddr = reinterpret_cast<uint64_t>(destination);
	if (count == 0) {
		return;
	}
	const auto size = uint64_t {count - 1u} * stride + sizeof(uint64_t);
	// One lookup of the backing store for all of them; the caches learn of each write as in
	// WriteFromCompletion().
	if (GuestRange {vaddr, size}.Valid() &&
	    LibKernel::Memory::TryWriteBackingQwords(vaddr, values, count, stride)) {
		for (uint32_t i = 0; i < count; i++) {
			const auto address = vaddr + i * stride;
			if (m_page_manager.IsWatched(address & ~(TRACKER_PAGE_SIZE - 1))) {
				m_buffer_cache.InvalidateUnlessGpuModified(address, sizeof(uint64_t));
				m_texture_cache.InvalidateMemory(address, sizeof(uint64_t));
				m_image_heap.InvalidateMemory(address, sizeof(uint64_t));
			}
		}
		return;
	}
	for (uint32_t i = count; i-- > 0;) {
		WriteFromCompletion(static_cast<uint8_t*>(destination) + i * stride, &values[i],
		                    sizeof(uint64_t));
	}
}

void RenderContext::PrepareGpuWrite(const void* destination, uint64_t size) noexcept {
	const auto vaddr     = reinterpret_cast<uint64_t>(destination);
	const auto page_size = TRACKER_PAGE_SIZE;
	if (size == 0 || !GuestRange {vaddr, size}.Valid()) {
		return;
	}
	// A watched page is protected: the write would fault into HandleFault().
	for (auto page = vaddr & ~(page_size - 1); page < vaddr + size; page += page_size) {
		if (m_page_manager.IsWatched(page)) {
			(void)HandleFault(PageFaultAccess::Write, std::max(page, vaddr));
		}
	}
}

bool RenderContext::WriteAroundGpuWrites(void* destination, const void* data, uint64_t size) {
	const auto vaddr = reinterpret_cast<uint64_t>(destination);
	if (size == 0 || !GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	if (m_texture_cache.IsRegionGpuModified(vaddr, size)) {
		// A write over memory a GPU-written image holds: the faulting path reads the GPU's
		// buffer writes of the page back (draining the GPU) and leaves the image CPU-dirty. The
		// write-around keeps the page GPU-owned instead, which needs no readback, and the image
		// becomes CPU-dirty all the same.
		if (!m_buffer_cache.WriteAroundGpuWrites(vaddr, data, size)) {
			return false;
		}
		m_texture_cache.InvalidateMemory(vaddr, size);
		return true;
	}
	return m_buffer_cache.WriteAroundGpuWrites(vaddr, data, size);
}

bool RenderContext::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	// Host I/O into the range, even one running past its mapping, would fail on its inaccessible
	// pages, and a later write-back would overwrite it.
	(void)WaitWriteBacks(vaddr, size);
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	m_image_heap.InvalidateMemory(vaddr, size);
	return true;
}

bool RenderContext::InvalidateMemoryForWrite(uint64_t vaddr, uint64_t size) {
	(void)WaitWriteBacks(vaddr, size);
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemoryForWrite(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	m_image_heap.InvalidateMemory(vaddr, size);
	return true;
}

void RenderContext::MappedRanges(uint64_t vaddr, uint64_t size, RangeSet& out) const {
	std::shared_lock lock(m_mapped_ranges_mutex);
	m_mapped_ranges.ForEachInRange(vaddr, size, [&](uint64_t begin, uint64_t end) { out.Add(begin, end - begin); });
}

void RenderContext::FreedByGame(uint64_t vaddr, uint64_t size) {
	static const uint32_t mode = [] {
		const char* value = std::getenv("KYTY_ENGINE_FREE_BUFFERS");
		return value != nullptr && (value[0] == '0' || value[0] == '2') ? static_cast<uint32_t>(value[0] - '0') : 1u;
	}();
	if (mode == 0 || m_gpu == nullptr || m_gpu->IsStopping() || !GuestRange {vaddr, size}.Valid()) return;
	m_gpu->SendCommand([this, vaddr, size] {
		// (Unmapped meanwhile: UnmapMemory dealt with its buffers.)
		if (!IsMapped(vaddr, size)) return;
		const auto start = std::chrono::steady_clock::now();
		// What went, reported every 30 s it happened (the GPU thread's own counts: all of its time here, a freed block's read
		// back included when nothing went).
		static BufferCache::FreeBlockSplits splits;
		static uint64_t                     blocks = 0, bytes = 0, let_go = 0, ns = 0, max_ns = 0;
		static auto                         next = start + std::chrono::seconds(30);
		const auto                          lists = splits.lists;
		const auto freed = m_buffer_cache.DropFreedRange(vaddr, size, mode != 2, &Loader::DemonsSoulsPageFree::FreeBlocksIn, &splits);
		const auto took  = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
		ns += took;
		max_ns = std::max(max_ns, took);
		if (freed != 0) {
			blocks++;
			bytes += size;
			let_go += freed;
		}
		if ((freed == 0 && splits.lists == lists) || start < next) return;
		std::printf("Engine frees: %llu freed blocks (%.0f MiB) had buffers over them: %.0f MiB of buffers let go (%.0f MiB cut off "
		            "buffers over other free blocks too, the allocator's lists read %llu times), %.1f ms on the GPU thread (at most "
		            "%.2f ms a block) (KYTY_ENGINE_FREE_BUFFERS)\n",
		            static_cast<unsigned long long>(blocks), static_cast<double>(bytes) / 1048576.0,
		            static_cast<double>(let_go) / 1048576.0, static_cast<double>(splits.cut) / 1048576.0,
		            static_cast<unsigned long long>(splits.lists), static_cast<double>(ns) / 1e6,
		            static_cast<double>(max_ns) / 1e6);
		std::fflush(stdout);
		blocks = bytes = let_go = ns = max_ns = 0;
		splits = {};
		next   = start + std::chrono::seconds(30);
	});
}

bool RenderContext::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

uint64_t RenderContext::MappedRangesVersion() const noexcept {
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges_version;
}

bool RenderContext::UnmappedSince(uint64_t version, uint64_t vaddr, uint64_t size) const {
	std::shared_lock lock(m_mapped_ranges_mutex);
	if (version == m_mapped_ranges_version) {
		return false;
	}
	if (version < m_unmap_log_floor) {
		return true; // unmaps since then left the log
	}
	for (auto it = m_unmap_log.rbegin(); it != m_unmap_log.rend() && it->version > version; ++it) {
		if (vaddr < it->vaddr + it->size && it->vaddr < vaddr + size) {
			return true;
		}
	}
	return false;
}

void RenderContext::MapMemory(uint64_t vaddr, uint64_t size) {
	{
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Add(vaddr, size);
		m_mapped_ranges_version++;
		m_mapping_epoch.store(m_mapped_ranges_version, std::memory_order_release);
		m_bda_mapped_since_sync.emplace_back(vaddr, size);
	}
	// Texture arrays that keep their other layers when one is unmapped learn that memory came
	// back under them. Without such an image there is nothing to tell the GPU thread.
	if (m_texture_cache.HasPartialCandidates()) {
		const auto map = [this, vaddr, size] { m_texture_cache.MapMemory(vaddr, size); };
		if (m_gpu == nullptr || m_gpu->IsStopping()) {
			map();
		} else {
			m_gpu->SendUrgentCommandSync(map);
		}
	}
}

void RenderContext::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto unmap = [this, vaddr, size] {
		// Finishing the GPU protects GPU writes to the range and completions that may write guest
		// memory there (downloads, write scans). Queued GPU work only reads cached copies and
		// host-only completions (cache bookkeeping, flips, interrupts) never touch the range, so
		// without GPU-owned pages, GPU-written images or pending guest completions writing the
		// range the unmap needs no drain. Images that only hold guest data are freed as the
		// garbage collector frees them: their objects live until the queued work is done.
		if (m_command_scheduler.Active()) {
			m_command_scheduler.PopPendingOperations();
		}
		const bool drain = m_command_scheduler.Active() &&
		                   (m_buffer_cache.IsRegionGpuModified(vaddr, size) ||
		                    m_texture_cache.HasGpuWrittenImages(vaddr, size) ||
		                    m_command_scheduler.HasPendingGuestOperations(vaddr, size));
		if (drain) {
			const auto tick = m_command_scheduler.CurrentTick();
			m_command_scheduler.Finish();
			m_command_scheduler.WaitPriorityOperations(tick);
		}
		const bool registered = size != 0 && m_buffer_cache.IsRegionRegistered(vaddr, size);
		m_buffer_cache.InvalidateMemory(vaddr, size);
		if (registered) {
			// The buffers nothing mapped reaches any more go (a buffer over mapped memory too is cut to its mapped parts).
			m_buffer_cache.DeleteUnmappedBuffers(vaddr, size);
			m_buffer_cache.SplitUnmappedBuffers(vaddr, size);
		}
		m_texture_cache.UnmapMemory(vaddr, size);
		m_image_heap.UnmapMemory(vaddr, size);
		m_occlusion_queries.OnUnmap(vaddr, size);
		if (size != 0 && vaddr <= UINT64_MAX - size) {
			m_guest_bvh_memory.erase(m_guest_bvh_memory.lower_bound(vaddr), m_guest_bvh_memory.lower_bound(vaddr + size));
			std::erase_if(m_hle_tlases, [&](const HleTlas& tlas) { return tlas.header >= vaddr && tlas.header - vaddr < size; });
		}
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
		m_mapped_ranges_version++;
		m_mapping_epoch.store(m_mapped_ranges_version, std::memory_order_release);
		if (m_unmap_log.size() >= 4096) {
			m_unmap_log_floor = m_unmap_log.front().version;
			m_unmap_log.pop_front();
		}
		m_unmap_log.push_back({m_mapped_ranges_version, vaddr, size});
	};
	// Shutdown still owns the GPU while queued rendering drains, but its command lane no
	// longer accepts external work. Use the guest GPU's state for the teardown route.
	if (m_gpu == nullptr || m_gpu->IsStopping()) {
		unmap();
		return;
	}
	// The guest unmaps memory once the GPU work that used it executed: it waits on labels the
	// execution thread writes after the operations before them. The operations queued since do not
	// use the range, so the unmap runs before them instead of making the guest wait for them;
	// streaming games unmap many times per frame, holding their own locks.
	m_gpu->SendUrgentCommandSync(unmap);
}

void RenderContext::CacheDmaBases(const ShaderStageRuntime& runtime) {
	const auto&      program   = runtime.module->info;
	const auto&      user_data = runtime.resources->user_data;
	std::shared_lock lock(m_mapped_ranges_mutex);

	const auto cache_after = [&](const std::array<uint32_t, 2>& pair, uint64_t window,
	                             bool contiguous) {
		const auto [low_reg, high_reg] = pair;
		const auto low  = static_cast<uint64_t>(low_reg) - program.user_data_base;
		const auto high = static_cast<uint64_t>(high_reg) - program.user_data_base;
		if (low_reg < program.user_data_base || high_reg < program.user_data_base ||
		    low >= user_data.size() || high >= user_data.size()) {
			return;
		}
		// Guest addresses have 48 bits: a V#'s dword 1 holds the stride above the high 16 of them.
		const auto base =
		    user_data[low] | (static_cast<uint64_t>(user_data[high] & 0xffffu) << 32u);
		// The registers of an access that never runs may hold stale data. Mapped memory never
		// reaches the top of the address space, so a window that would overflow is skipped.
		if (base > UINT64_MAX - window) {
			return;
		}
		m_mapped_ranges.ForEachInRange(base, window, [&](uint64_t start, uint64_t end) {
			// Memory past a gap belongs to another allocation.
			if (!contiguous || start == base) {
				(void)m_buffer_cache.FindBuffer(start, end - start);
			}
		});
	};
	// DMA reaches only memory with a cached buffer; any other access records a fault and reads
	// zero, and the buffer arrives after the shader has run. That loses data the guest writes for a
	// single dispatch, such as the glyph bitmaps GTA V copies into its font atlas.
	for (const auto& pair: program.info.dma_base_registers) {
		cache_after(pair, BufferCache::CACHING_PAGESIZE, false);
	}
	// A V# the GPU computes from a base holds records past offsets the shader reads from memory,
	// anywhere in the allocation that starts there. SAROS's kTrinity BVH passes in PS5 Pro mode
	// reach up to ~10 MiB into their scratch memory, which the guest maps apart for a large BVH:
	// past 16 KiB the stores of one pass were lost and the next read zeros, so the refit pass (cs
	// 0x4a2181e0b7db95a7) looped forever over records with bad indices.
	for (const auto& pair: program.info.vsharp_base_registers) {
		cache_after(pair, VsharpBaseReach, true);
	}
	CacheTlasMemory(runtime);
}

bool RenderContext::CacheSelfSizedBuffers(const ShaderStageRuntime& runtime) {
	// The record count is in GPU-written memory, unknown here. Psr's BVHs in GTA V span 17 to 64
	// KiB; device-address accesses past this find the cached buffers that hold those pages
	// already, or record a fault, read zero and get the page cached for the next dispatch.
	constexpr uint64_t CachedBytes = 4 * BufferCache::CACHING_PAGESIZE;
	const auto&        program     = runtime.module->info;
	const auto&        user_data   = runtime.resources->user_data;
	const auto         value       = [&](uint32_t reg, uint32_t& result) {
		const auto index = static_cast<uint64_t>(reg) - program.user_data_base;
		if (reg < program.user_data_base || index >= user_data.size()) {
			return false;
		}
		result = user_data[index];
		return true;
	};
	bool             written = false;
	std::shared_lock lock(m_mapped_ranges_mutex);
	for (const auto& buffer: program.info.self_sized_buffers) {
		uint32_t low  = 0;
		uint32_t high = 0;
		if (!value(buffer.base_low_register, low) || !value(buffer.base_high_register, high)) {
			continue;
		}
		written |= buffer.written;
		const auto base = low | (uint64_t {high & 0xffffu} << 32u);
		if (base == 0) {
			continue;
		}
		// Pinned, as memory the device-address faults cache: the accesses may be its only uses.
		m_mapped_ranges.ForEachInRange(base, CachedBytes, [this](uint64_t start, uint64_t end) {
			if (const auto id = m_buffer_cache.FindBuffer(start, end - start)) {
				m_buffer_cache.GetBuffer(id).device_address_pinned = true;
			}
		});
	}
	return written;
}

void RenderContext::CacheTlasMemory(const ShaderStageRuntime& runtime) {
	// The guest's traversal, and the walk that builds the host TLAS when the program traces
	// natively, read zeros from TLAS memory without a cached buffer as well. ASTRO BOT writes its
	// TLASes with the CPU into memory that no other GPU work reads, new ones in each level (three
	// that it rotates through, another for a pass of 64 dispatches in a single frame): the first
	// walks of each found no TLAS, or a part of it. The BLASes they reference that the CPU wrote
	// are cached as well.
	const auto&                  program = runtime.module->info;
	RayTracing::TlasSource source;
	if (!RayTracing::FindGuestTlasSource(program.info, source)) {
		return;
	}
	const auto& resources = *runtime.resources;
	// Memory the CPU wrote last: its current bytes are in guest memory.
	const auto readable = [this](uint64_t address, uint64_t size) {
		return GuestRange {address, size}.Valid() && m_mapped_ranges.Contains(address, size) &&
		       !(m_buffer_cache.MayBeGpuModified(address, size) &&
		         m_buffer_cache.IsRegionGpuModified(address, size));
	};
	// The header address as the walk reads it (see RayTracing::Prepare()).
	uint64_t    header = 0;
	const auto& srt    = resources.flattened_srt;
	const auto  in_srt = [&](uint32_t slot) {
		return slot != ShaderRecompiler::IR::ShaderInfo::NoRayQuerySlot && slot < srt.size();
	};
	if (in_srt(source.header_slots[0]) && in_srt(source.header_slots[1])) {
		header = srt[source.header_slots[0]] | (uint64_t {srt[source.header_slots[1]]} << 32u);
	} else {
		const auto& user_data = resources.user_data;
		const auto  index     = uint64_t {source.user_sgpr} - program.user_data_base;
		if (source.user_sgpr < program.user_data_base || index + 1u >= user_data.size()) {
			return;
		}
		const auto pointer = RayTracing::UserPointer(
		    source, user_data[index] | (uint64_t {user_data[index + 1u]} << 32u), readable);
		if (RayTracing::HeaderInUserData(source)) {
			header = RayTracing::UserDataHeader(pointer);
		} else if (pointer == 0 || pointer > UINT64_MAX - source.header_offset ||
		           !readable(pointer + source.header_offset, sizeof(header))) {
			return;
		} else {
			std::memcpy(&header, reinterpret_cast<const void*>(pointer + source.header_offset), sizeof(header));
		}
	}
	std::vector<std::pair<uint64_t, uint64_t>> ranges;
	const auto tlas = m_ray_tracing->ReadGuestTlas(header, source, readable, ranges);
	if (tlas.end != 0) {
		ranges.emplace_back(header, tlas.end);
	}
	// Pinned, as memory the device-address faults cache: the walks, often the only readers, do not
	// record their use of it.
	for (const auto& [first, last]: ranges) {
		m_mapped_ranges.ForEachInRange(first, last - first, [this](uint64_t start, uint64_t stop) {
			if (const auto id = m_buffer_cache.FindBuffer(start, stop - start)) {
				m_buffer_cache.GetBuffer(id).device_address_pinned = true;
			}
		});
	}
	if (program.info.ray_query && m_ray_tracing->Enabled()) {
		m_ray_tracing->NoteTlasInstances(tlas.instances);
	}
}

bool RenderContext::SynchronizeDmaFootprint(std::span<const ShaderStageRuntime* const> stages) {
	// The reach of a program that traces rays is enumerated from guest memory (the BLASes of its
	// TLAS, its buffer tables), and the pass over what its buffer tables describe is one more pass
	// over the pages the guest keeps writing. When other operations of every frame take the
	// synchronization of everything, that one covers the reach as well, once a submission, and
	// enumerating it only adds to it: in GTA V's Fidelity mode, where cs b93ab2ec5c5caf00,
	// 6aeb221de7ff018e and d645a5101b936ce7 are not bounded, the reach of cs 0b41af19bddf132c cost
	// 1.4 ms a frame of the execution thread and 90 ms/s more page protection in the guest's
	// threads. Such a program (one dispatch a frame) joins the synchronization of everything while
	// an operation whose reach is not known ran before most of its last dispatches. One now and then
	// (Performance RT: cs c421273ce118f4a5 or 2adee3faed27c152, a few times a second) leaves it
	// enumerating its reach.
	constexpr int JoinedRuns = 6; // of its last 8 dispatches
	for (const auto* stage: stages) {
		const auto& program = stage->module->info;
		if (!program.info.uses_dma || program.info.dma_bounded || program.info.ray_query_reach.empty()) {
			continue;
		}
		auto& traced  = m_traced_programs[program.shader_hash];
		traced.recent = static_cast<uint8_t>((traced.recent << 1u) |
		                                     (traced.unbounded_ops != m_bda_unbounded_ops ? 1u : 0u));
		traced.unbounded_ops = m_bda_unbounded_ops;
		if (std::popcount(traced.recent) >= JoinedRuns) {
			return false;
		}
	}
	if (!SynchronizeDmaReach(stages)) {
		// (Also a program whose reach could not be enumerated this time.)
		m_bda_unbounded_ops++;
		return false;
	}
	return true;
}

bool RenderContext::SynchronizeDmaReach(std::span<const ShaderStageRuntime* const> stages) {
	// Larger V# ranges are no buffers a draw reads (see FindBuffers()).
	constexpr uint64_t MaxBufferBytes = uint64_t {256} * 1024 * 1024;
	// An index offset or thread ID added to a V# record stays within a wave of records past it.
	constexpr uint64_t WaveRecords = 64;
	m_dma_spans.clear();
	m_dma_sets.clear();
	std::shared_lock lock(m_mapped_ranges_mutex);
	const auto add = [&](uint64_t address, uint64_t size) {
		if (size != 0 && address <= UINT64_MAX - size) {
			m_mapped_ranges.ForEachInRange(address, size, [&](uint64_t start, uint64_t end) {
				m_dma_spans.emplace_back(start, end);
			});
		}
	};
	for (const auto* stage: stages) {
		const auto& program = stage->module->info;
		if (!program.info.uses_dma) {
			continue;
		}
		if (!program.info.dma_bounded && program.info.ray_query_reach.empty()) {
			if (!AddRecordReach(*stage)) {
				return false;
			}
			continue;
		}
		// A program that traces rays: the guest BVH and the buffers of its alpha tests, then its
		// windows and tables.
		if (!program.info.dma_bounded) {
			if (!AddRayTracingReach(*stage)) {
				return false;
			}
		}
		const auto& user_data = stage->resources->user_data;
		const auto  base_of   = [&](uint32_t reg, uint64_t& base) {
			const auto index = static_cast<uint64_t>(reg) - program.user_data_base;
			if (reg < program.user_data_base || index + 1u >= user_data.size()) {
				return false;
			}
			base = user_data[index] | (static_cast<uint64_t>(user_data[index + 1u]) << 32u);
			return true;
		};
		for (const auto& window: program.info.dma_windows) {
			uint64_t base = 0;
			if (!base_of(window.base_register, base)) {
				return false;
			}
			// Scalar address reads clear the low two bits of the base.
			const auto before = static_cast<uint64_t>(-window.first) + 4u;
			const auto start  = base > before ? base - before : 0;
			add(start, std::min(base, UINT64_MAX - window.last) + window.last - start);
		}
		for (const auto& table: program.info.dma_tables) {
			uint64_t base = 0;
			if (!base_of(table.base_register, base)) {
				return false;
			}
			const auto entries = (base & ~uint64_t {3}) + static_cast<uint64_t>(table.immediate);
			Common::HackNotice::Report("DMA upload assumes V# indices stay within 64 records",
			                           "RenderContext::SynchronizeDmaFootprint",
			                           program.shader_hash, table.base_register);
			for (uint32_t offset = table.offset_bits;; offset = (offset - 1u) & table.offset_bits) {
				// The V# the shader would read, from guest memory that the GPU did not write.
				const auto address = entries + offset;
				if (!m_mapped_ranges.Contains(address, 12) ||
				    m_buffer_cache.IsRegionGpuModified(address, 12)) {
					return false;
				}
				ShaderBufferResource descriptor;
				std::memcpy(descriptor.fields, reinterpret_cast<const void*>(address), 12);
				const auto stride = uint64_t {descriptor.Stride()};
				if (descriptor.Base48() != 0 && descriptor.NumRecords() != 0) {
					const auto wanted =
					    descriptor.GetSize() + std::max<uint64_t>(stride, 4u) * WaveRecords;
					if (wanted > MaxBufferBytes) {
						Common::HackNotice::Report("DMA upload of a V# range clipped to 256 MiB",
						                           "RenderContext::SynchronizeDmaFootprint",
						                           program.shader_hash, table.base_register);
					}
					add(descriptor.Base48(), std::min(wanted, MaxBufferBytes));
				}
				if (offset == 0) {
					break;
				}
			}
		}
		// A buffer that takes its size from a header the GPU writes (ShaderInfo::SelfSizedBuffer)
		// lies in the memory sce::Psr captured for the BVH at its base (GTA V's BLAS passes).
		for (const auto& buffer: program.info.self_sized_buffers) {
			uint64_t low  = 0;
			uint64_t high = 0;
			if (!base_of(buffer.base_low_register, low) || !base_of(buffer.base_high_register, high)) {
				return false;
			}
			const auto base = (low & 0xffffffffu) | ((high & 0xffffu) << 32u);
			if (base == 0) {
				continue;
			}
			auto memory = m_guest_bvh_memory.upper_bound(base);
			if (memory == m_guest_bvh_memory.begin() || base - (--memory)->first >= memory->second) {
				return false;
			}
			add(base, memory->first + memory->second - base);
		}
		for (const auto& table: program.info.dma_record_tables) {
			const auto& buffers = stage->resources->buffers;
			if (table.buffer >= buffers.size()) {
				return false;
			}
			const auto records = DecodeNativeDescriptor<ShaderBufferResource>(buffers[table.buffer]);
			const auto first   = records.Base48();
			const auto bytes   = records.GetSize();
			if (first == 0 || bytes == 0) {
				continue;
			}
			// A table of V#s (GTA V's buffer table: 131 072 entries, the same for every frame) is
			// decoded again only when its bytes change.
			if (table.stride == sizeof(records.fields) && table.offset == 0) {
				if (!AddBufferTable(records)) {
					return false;
				}
				continue;
			}
			// The V# of each record, from guest memory that the GPU did not write.
			if (bytes > MaxBufferBytes || !m_mapped_ranges.Contains(first, bytes) ||
			    m_buffer_cache.IsRegionGpuModified(first, bytes)) {
				return false;
			}
			for (uint64_t offset = table.offset; offset + 12u <= bytes; offset += table.stride) {
				ShaderBufferResource descriptor {};
				std::memcpy(descriptor.fields, reinterpret_cast<const void*>(first + offset), 12);
				if (descriptor.Base48() == 0 || descriptor.NumRecords() == 0) {
					continue;
				}
				if (descriptor.GetSize() > MaxBufferBytes) {
					return false;
				}
				add(descriptor.Base48(), descriptor.GetSize());
			}
		}
	}
	const auto reach_layout = ReachLayout();
	lock.unlock();
	m_buffer_cache.SynchronizeBuffersInSpans(m_dma_spans);
	// The buffers that the V#s of the buffer tables describe, once a BDA generation.
	for (auto* table: m_dma_sets) {
		if (table->synced_in == reach_layout) {
			continue;
		}
		table->synced_in = reach_layout;
		m_buffer_cache.SynchronizeBuffersInSet(table->ranges, table->regions);
	}
	return true;
}

bool RenderContext::AddRecordReach(const ShaderStageRuntime& runtime) {
	using Reach = ShaderRecompiler::Decoder::RecordReach;
	// More records, or larger values, are not what the programs this follows work on: the full
	// synchronization covers them.
	constexpr uint64_t MaxRecords = 4096;
	constexpr uint32_t MaxIndex   = 1u << 20u;
	constexpr uint32_t MaxOffset  = 1u << 28u;
	const auto&        program    = runtime.module->info;
	Reach              reach;
	if (program.info.dma_record_reach.empty() || !Reach::Unpack(program.info.dma_record_reach, reach)) {
		return false;
	}
	// Memory the CPU wrote last: its current bytes are in guest memory.
	const auto readable = [this](uint64_t address, uint64_t size) {
		return GuestRange {address, size}.Valid() && m_mapped_ranges.Contains(address, size) &&
		       !(m_buffer_cache.MayBeGpuModified(address, size) &&
		         m_buffer_cache.IsRegionGpuModified(address, size));
	};
	const auto& user_data  = runtime.resources->user_data;
	const auto  descriptor = [&](const Reach::Descriptor& source, ShaderBufferResource& result) {
		const auto index = uint64_t {source.user_sgpr} - program.user_data_base;
		if (source.user_sgpr < program.user_data_base || index + 1u >= user_data.size()) {
			return false;
		}
		const auto pointer = user_data[index] | (uint64_t {user_data[index + 1u]} << 32u);
		if (pointer > UINT64_MAX - source.offset || !readable(pointer + source.offset, sizeof(result.fields))) {
			return false;
		}
		std::memcpy(result.fields, reinterpret_cast<const void*>(pointer + source.offset), sizeof(result.fields));
		return true;
	};
	// What a load of the shader reads at `offset` of a buffer: zero outside it. False when the
	// bytes are not the CPU's.
	const auto load = [&](const ShaderBufferResource& buffer, uint64_t offset, void* data, uint64_t size) {
		std::memset(data, 0, size);
		const auto bytes = buffer.GetSize();
		if (buffer.Base48() == 0 || offset > bytes || size > bytes - offset) {
			return true;
		}
		if (!readable(buffer.Base48() + offset, size)) {
			return false;
		}
		std::memcpy(data, reinterpret_cast<const void*>(buffer.Base48() + offset), size);
		return true;
	};
	// Records share the buffers they write, and often the ones they read: once each.
	const auto first_span = m_dma_spans.size();
	const auto add        = [&](uint64_t address, uint64_t size) {
		if (size != 0 && address <= UINT64_MAX - size) {
			m_mapped_ranges.ForEachInRange(address, size, [&](uint64_t start, uint64_t end) {
				const std::pair span {start, end};
				if (std::find(m_dma_spans.begin() + static_cast<ptrdiff_t>(first_span),
				              m_dma_spans.end(), span) == m_dma_spans.end()) {
					m_dma_spans.push_back(span);
				}
			});
		}
	};
	ShaderBufferResource    header;
	ShaderBufferResource    records;
	ShaderBufferResource    pointers;
	std::array<uint32_t, 2> range {};
	// The shader indexes the records by their size and by the V#'s stride, and the table by its
	// stride and by the size of a V#.
	if (!descriptor(reach.header, header) || !descriptor(reach.records, records) ||
	    !descriptor(reach.pointers, pointers) || records.Stride() != reach.record_size ||
	    pointers.Stride() != sizeof(pointers.fields) || reach.record_size > 0x1000u ||
	    !load(header, reach.header_offset, range.data(), sizeof(range)) || range[1] > MaxRecords ||
	    range[0] > MaxIndex) {
		return false;
	}
	std::array<uint8_t, 0x1000> record {};
	const auto                  field = [&](uint32_t offset) {
        uint32_t value = 0;
        std::memcpy(&value, record.data() + offset, sizeof(value));
        return value;
	};
	for (uint64_t index = range[0]; index < uint64_t {range[0]} + range[1]; index++) {
		if (!load(records, index * reach.record_size, record.data(), reach.record_size)) {
			return false;
		}
		// The threads of the record: first <= thread < first + threads, in 32 bits.
		const auto threads = field(reach.threads_field);
		const auto first   = field(reach.first_field);
		if (static_cast<uint32_t>(first + threads) <= first) {
			continue;
		}
		const auto stride = (field(reach.stride_field) >> (reach.stride_byte * 8u)) & 0xffu;
		const auto last   = uint64_t {threads - 1u} * stride;
		const auto entry  = field(reach.pointer_field);
		std::array<uint32_t, 2> pointer {};
		if (last > MaxOffset || entry > MaxIndex ||
		    !load(pointers, uint64_t {entry} * sizeof(pointers.fields), pointer.data(), sizeof(pointer))) {
			return false;
		}
		// The base as the shader makes it: the low dword and the low word of the next one, sign
		// extended. A negative one is no guest memory.
		const auto high = static_cast<int16_t>(pointer[1]);
		uint64_t   low_offset  = UINT64_MAX;
		uint64_t   high_offset = 0;
		for (const auto& attribute: reach.attributes) {
			uint32_t value = 0;
			if (attribute.field != Reach::None) {
				value = field(attribute.field);
				if (attribute.byte < 4u) {
					value = (value >> (attribute.byte * 8u)) & 0xffu;
				}
			}
			if (value > MaxOffset) {
				return false;
			}
			low_offset  = std::min(low_offset, uint64_t {value} & ~uint64_t {3});
			high_offset = std::max(high_offset, ((value + last) & ~uint64_t {3}) + attribute.end);
		}
		if (high >= 0 && low_offset < high_offset) {
			add((uint64_t {static_cast<uint16_t>(high)} << 32u | pointer[0]) + low_offset,
			    high_offset - low_offset);
		}
		// What its stores write: the buffers of the table entries it names.
		for (const auto store: reach.store_fields) {
			const auto           target_entry = field(store);
			ShaderBufferResource target;
			if (target_entry > MaxIndex ||
			    !load(pointers, uint64_t {target_entry} * sizeof(target.fields), target.fields, sizeof(target.fields))) {
				return false;
			}
			const auto written = IndirectWriteTables::TargetRange(target);
			add(written.address, written.size);
		}
	}
	return true;
}

void RenderContext::NoteHleTlas(uint64_t header, uint64_t size, std::vector<uint64_t> blases) {
	// GTA V builds four TLASes a frame, at addresses it keeps.
	constexpr size_t MaxTlases = 16;
	for (auto& tlas: m_hle_tlases) {
		if (tlas.header == header) {
			tlas.size   = size;
			tlas.blases = std::move(blases);
			return;
		}
	}
	if (m_hle_tlases.size() >= MaxTlases) {
		m_hle_tlases.erase(m_hle_tlases.begin());
	}
	m_hle_tlases.push_back({header, size, std::move(blases)});
}

void RenderContext::ForgetHleTlas(uint64_t header) {
	std::erase_if(m_hle_tlases, [&](const HleTlas& tlas) { return header == 0 || tlas.header == header; });
}

void RenderContext::NoteGuestBvhMemory(uint64_t address, uint64_t size) {
	// The BLASes of a streaming game come and go: forgotten all at once, they are captured again
	// as the game builds them (the dispatches that trace the others synchronize everything meanwhile).
	constexpr size_t MaxCaptures = 65536;
	if (m_guest_bvh_memory.size() >= MaxCaptures) {
		m_guest_bvh_memory.clear();
	}
	m_guest_bvh_memory[address] = size;
}

bool RenderContext::GuestTlasHeader(const ShaderStageRuntime& runtime, const RayTracing::TlasSource& source,
                                    uint64_t& header, uint64_t& header_at) {
	const auto& program   = runtime.module->info;
	const auto& resources = *runtime.resources;
	// Memory the CPU wrote last: its current bytes are in guest memory.
	const auto readable = [this](uint64_t address, uint64_t size) {
		return GuestRange {address, size}.Valid() && m_mapped_ranges.Contains(address, size) &&
		       !(m_buffer_cache.MayBeGpuModified(address, size) &&
		         m_buffer_cache.IsRegionGpuModified(address, size));
	};
	header_at             = 0;
	const auto& user_data = resources.user_data;
	const auto  index     = uint64_t {source.user_sgpr} - program.user_data_base;
	uint64_t    pointer   = 0;
	if (source.user_sgpr >= program.user_data_base && index + 1u < user_data.size()) {
		pointer = RayTracing::UserPointer(
		    source, user_data[index] | (uint64_t {user_data[index + 1u]} << 32u), readable);
	}
	if (RayTracing::HeaderInUserData(source)) {
		header = RayTracing::UserDataHeader(pointer);
		return header != 0;
	}
	if (pointer != 0 && pointer <= UINT64_MAX - source.header_offset) {
		header_at = pointer + source.header_offset;
	}
	// The header address as the walk reads it (see RayTracing::Prepare()).
	const auto& srt    = resources.flattened_srt;
	const auto  in_srt = [&](uint32_t slot) {
		return slot != ShaderRecompiler::IR::ShaderInfo::NoRayQuerySlot && slot < srt.size();
	};
	if (in_srt(source.header_slots[0]) && in_srt(source.header_slots[1])) {
		header = srt[source.header_slots[0]] | (uint64_t {srt[source.header_slots[1]]} << 32u);
		return true;
	}
	if (header_at == 0 || !readable(header_at, sizeof(header))) {
		return false;
	}
	std::memcpy(&header, reinterpret_cast<const void*>(header_at), sizeof(header));
	return true;
}

bool RenderContext::GuestBlasMemory(uint64_t blas, uint64_t& size) {
	// The header of a BLAS of sce::Psr (the magic, then the size of its memory at +16).
	constexpr uint64_t HeaderSize = 256;
	constexpr uint64_t SizeField  = 16;
	// Larger BLASes are not plausible.
	constexpr uint64_t MaxSize    = uint64_t {256} * 1024 * 1024;
	size                          = 0;
	if (!m_mapped_ranges.Contains(blas, HeaderSize)) {
		// Nothing a walk could read.
		return true;
	}
	std::array<uint32_t, 2> magic {};
	uint32_t                field = 0;
	if (!(m_buffer_cache.MayBeGpuModified(blas, HeaderSize) && m_buffer_cache.IsRegionGpuModified(blas, HeaderSize))) {
		// The CPU wrote it (a BLAS loaded as it is).
		std::memcpy(magic.data(), reinterpret_cast<const void*>(blas), sizeof(magic));
		std::memcpy(&field, reinterpret_cast<const void*>(blas + SizeField), sizeof(field));
		if (magic[0] != 0x5f525350u || magic[1] != 0x4c485642u) {
			return false;
		}
	} else if (const auto captured = m_guest_bvh_memory.find(blas); captured != m_guest_bvh_memory.end()) {
		// The GPU built it or decompressed it into memory the library captured.
		field = static_cast<uint32_t>(std::min<uint64_t>(captured->second, UINT32_MAX));
	} else if (!Libs::LibKernel::Memory::TryReadAroundGpuWrites(blas + SizeField, &field, sizeof(field))) {
		// The GPU wrote the size, and the host holds no copy of it.
		return false;
	}
	size = field;
	return field >= HeaderSize && field <= MaxSize;
}

bool RenderContext::AddBufferTable(const ShaderBufferResource& table) {
	// Larger tables, or V#s, are not what an alpha test reads its buffers from.
	constexpr uint64_t MaxTableBytes  = uint64_t {16} * 1024 * 1024;
	constexpr size_t   MaxTables      = 4;
	constexpr uint64_t EntryBytes     = sizeof(table.fields);
	const auto         address        = table.Base48();
	const auto         size           = table.GetSize() / EntryBytes * EntryBytes;
	if (address == 0 || size == 0) {
		return true;
	}
	if (table.Stride() != EntryBytes || size > MaxTableBytes || !m_mapped_ranges.Contains(address, size) ||
	    (m_buffer_cache.MayBeGpuModified(address, size) && m_buffer_cache.IsRegionGpuModified(address, size))) {
		return false;
	}
	auto cached = std::ranges::find_if(m_buffer_tables, [&](const auto& known) { return known->address == address; });
	if (cached == m_buffer_tables.end()) {
		if (m_buffer_tables.size() >= MaxTables) {
			m_buffer_tables.erase(m_buffer_tables.begin());
		}
		m_buffer_tables.push_back(std::make_unique<BufferTable>());
		cached            = std::prev(m_buffer_tables.end());
		(*cached)->address = address;
	}
	auto&       entries = **cached;
	const auto* memory  = reinterpret_cast<const uint8_t*>(address);
	// The table changes when the game streams geometry in or out: its V#s are decoded again then.
	// Only the pages the guest's CPU wrote since the last comparison can differ from the copy
	// (GTA V writes a page of its 2 MB table now and then, and other buffers on its pages). Within
	// the BDA generation of the last comparison, what the CPU writes waits for the next one (the
	// operations of GTA V's Fidelity mode that trace rays take the same table several times in
	// one).
	const auto layout = ReachLayout();
	bool       same   = entries.copy.size() == size;
	if (entries.compared_in != layout || !same) {
		entries.compared_in = layout;
		const auto compare  = [&](uint64_t run, uint64_t bytes) {
			const auto begin = std::max(run, address);
			const auto end   = std::min(run + bytes, address + size);
			if (same && begin < end) {
				same = std::memcmp(entries.copy.data() + (begin - address), memory + (begin - address),
				                   end - begin) == 0;
			}
		};
		m_buffer_cache.ConsumeCpuWrites(CpuWriteConsumer::BufferTables, address, size, [&](uint64_t run, uint64_t bytes) {
			compare(run, bytes);
			// The other tables on these pages compare them at their next comparison.
			const auto first = run & ~(TRACKER_PAGE_SIZE - 1);
			const auto last  = (run + bytes + TRACKER_PAGE_SIZE - 1) & ~(TRACKER_PAGE_SIZE - 1);
			for (const auto& other: m_buffer_tables) {
				if (other.get() != &entries && other->address < last &&
				    first < other->address + other->copy.size()) {
					other->written.emplace_back(first, last - first);
				}
			}
		});
		for (const auto& [run, bytes]: entries.written) {
			compare(run, bytes);
		}
		entries.written.clear();
	}
	if (!same) {
		entries.copy.assign(memory, memory + size);
		entries.ranges.Clear();
		entries.regions   = {};
		entries.synced_in = {};
		for (uint64_t offset = 0; offset < size; offset += EntryBytes) {
			ShaderBufferResource buffer;
			std::memcpy(buffer.fields, entries.copy.data() + offset, EntryBytes);
			// The vertices are read from the base of their V#, at the offsets the indices of the
			// geometry give: within the buffer the V# describes. What is not guest memory (the
			// entries of geometry streamed out) has nothing to upload.
			auto       bytes = buffer.GetSize();
			const auto base  = buffer.Base48();
			if (base < LOWER_ADDRESS_SIZE) {
				bytes = std::min(bytes, LOWER_ADDRESS_SIZE - base);
			} else if (base >= Libs::LibKernel::Memory::kExtendedMemoryBase &&
			           base - Libs::LibKernel::Memory::kExtendedMemoryBase < Libs::LibKernel::Memory::kExtendedMemorySize) {
				bytes = std::min(bytes, Libs::LibKernel::Memory::kExtendedMemorySize -
				                            (base - Libs::LibKernel::Memory::kExtendedMemoryBase));
			} else {
				bytes = 0;
			}
			if (base != 0 && bytes != 0) {
				entries.ranges.Add(base, bytes);
			}
		}
	}
	m_mapped_ranges.ForEachInRange(address, size, [&](uint64_t start, uint64_t end) { m_dma_spans.emplace_back(start, end); });
	if (std::ranges::find(m_dma_sets, &entries) == m_dma_sets.end()) {
		m_dma_sets.push_back(&entries);
	}
	return true;
}

bool RenderContext::RayQueryTableVsharp(const ShaderStageRuntime& runtime, uint32_t table, ShaderBufferResource& vsharp) {
	const auto& buffers = runtime.resources->buffers;
	if ((table & ShaderRecompiler::IR::ShaderInfo::RayQuerySrtVsharp) == 0) {
		if (table >= buffers.size()) {
			return false;
		}
		vsharp = DecodeNativeDescriptor<ShaderBufferResource>(buffers[table]);
		return true;
	}
	const auto& srt   = runtime.resources->flattened_srt;
	const auto  first = table & ~ShaderRecompiler::IR::ShaderInfo::RayQuerySrtVsharp;
	if (uint64_t {first} + 4u > srt.size()) {
		return false;
	}
	for (uint32_t dword = 0; dword < 4u; dword++) {
		vsharp.fields[dword] = srt[first + dword];
	}
	return true;
}

bool RenderContext::AddRayTracingReach(const ShaderStageRuntime& runtime) {
	const auto&                  program = runtime.module->info;
	const auto&                  reach   = program.info.ray_query_reach;
	RayTracing::TlasSource source;
	uint64_t                     header    = 0;
	uint64_t                     header_at = 0;
	if (reach.empty() || reach.size() != 1u + uint64_t {reach[0]} * 2u || !m_ray_tracing->Enabled() ||
	    !RayTracing::FindGuestTlasSource(program.info, source) ||
	    !GuestTlasHeader(runtime, source, header, header_at)) {
		return false;
	}
	// The TLAS the host built from the instance list (its memory and its BLASes are known); the
	// walks of another one (a TLAS the game built, or wrote with the CPU) follow GPU-written data.
	const auto tlas = std::ranges::find_if(m_hle_tlases, [&](const HleTlas& known) { return known.header == header; });
	if (tlas == m_hle_tlases.end()) {
		return false;
	}
	const auto add = [&](uint64_t address, uint64_t size) {
		if (size != 0 && address <= UINT64_MAX - size) {
			m_mapped_ranges.ForEachInRange(address, size,
			                               [&](uint64_t start, uint64_t end) { m_dma_spans.emplace_back(start, end); });
		}
	};
	// The pointer to the header (the host's walk of the TLAS reads it), the TLAS, the BLASes of
	// its instances and the BLASes the preparation of the dispatch may walk to build them.
	if (header_at != 0) {
		add(header_at, sizeof(header));
	}
	add(header, tlas->size);
	m_walk_blases.clear();
	m_ray_tracing->PendingWalks(m_walk_blases);
	for (const auto* list: {&tlas->blases, &m_walk_blases}) {
		for (const auto blas: *list) {
			uint64_t size = 0;
			if (!GuestBlasMemory(blas, size)) {
				return false;
			}
			add(blas, size);
		}
	}
	// The material table and the buffer table of each alpha test, and the index and vertex buffers
	// that the V#s of the buffer table describe.
	const auto& buffers = runtime.resources->buffers;
	for (uint32_t test = 0; test < reach[0]; test++) {
		const auto material = reach[1u + test * 2u];
		const auto table    = reach[2u + test * 2u];
		ShaderBufferResource table_vsharp;
		if (material >= buffers.size() || !RayQueryTableVsharp(runtime, table, table_vsharp)) {
			return false;
		}
		const auto records = DecodeNativeDescriptor<ShaderBufferResource>(buffers[material]);
		add(records.Base48(), records.GetSize());
		if (!AddBufferTable(table_vsharp)) {
			return false;
		}
	}
	m_ray_tracing->AnnounceWalks(m_walk_blases);
	return true;
}

void RenderContext::PrepareBda(bool may_write, bool full_sync) {
	[[maybe_unused]] static const bool logged = [] {
		Log::WriteToConsoleAndLog("GPU: using buffer device address (BDA) shader memory access.\n");
		return true;
	}();
	m_buffer_cache.PrepareFaultBuffer();
	if (may_write) {
		m_buffer_cache.NoteDeviceAddressUse();
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	// Nothing to upload when no page became CPU dirty, no buffer was registered and no range was
	// mapped since the last synchronization started.
	const std::array<uint64_t, 3> epochs {RegionManager::CpuDirtyEpoch(),
	                                      m_buffer_cache.RegisterEpoch(), m_mapped_ranges_version};
	// The guest's CPU writes that the work of a submission may read through device addresses
	// precede the submission, or a WAIT_REG_MEM that the work waited for; the GPU sees later ones
	// only after such a wait. So once a synchronization ran for a submission, pages the CPU
	// dirties afterwards wait for the next submission or wait (they stay dirty meanwhile),
	// unless the execution thread itself wrote them for the command stream (labels, CP writes)
	// or the buffers or mappings changed.
	const bool same_layout = epochs[1] == m_bda_sync_epochs[1] && epochs[2] == m_bda_sync_epochs[2];
	// (Pages the last synchronization left CPU dirty take no fault when the guest writes them:
	// see BufferCache::HasKeptPages().)
	if (full_sync && (epochs != m_bda_sync_epochs || m_buffer_cache.HasKeptPages()) &&
	    (!same_layout || m_bda_synced_generation != m_bda_generation)) {
		m_bda_synced_generation = m_bda_generation;
		// A new buffer or mapped range may cover CPU-dirty pages that earlier passes skipped: the
		// buffer cache forces the regions of new buffers, and of the ranges mapped since. Only this
		// thread takes the ranges out; MapMemory() adds them under the exclusive lock.
		for (const auto& [vaddr, size]: m_bda_mapped_since_sync) {
			m_buffer_cache.ForceSynchronization(vaddr, size);
		}
		m_bda_mapped_since_sync.clear();
		m_buffer_cache.SynchronizeBuffersInRanges(m_mapped_ranges, false);
		m_bda_sync_epochs = epochs;
	}
	m_fault_process_pending = true;
	m_use_scan_pending      = true;
}

void RenderContext::RunGarbageCollector() {
	m_frame_number++;
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	m_image_heap.RunGarbageCollector();
	constexpr uint32_t UseScanPeriod = 256;
	if (m_use_scan_countdown != 0) {
		m_use_scan_countdown--;
	} else if (m_use_scan_pending) {
		m_use_scan_pending   = false;
		m_use_scan_countdown = UseScanPeriod;
		m_buffer_cache.ProcessUseBuffer();
	}
	if (m_indirect_write_tables.TakePendingWrites()) {
		m_buffer_cache.ProcessWriteBuffer([this](std::span<const uint64_t> pages) {
			m_indirect_write_tables.NoteWrites(pages);
		});
	}
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
	m_ray_tracing->RunGarbageCollector();
}

void RenderContext::AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it != m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.push_back({eq, event_id});
}

void RenderContext::DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it == m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.erase(it);
}

void RenderContext::TriggerInterrupt(int event_id, uint32_t context_id) {
	std::vector<InterruptEqRegistration> registrations;
	{
		Common::LockGuard lock(m_interrupt_mutex);
		for (const auto& registration: m_interrupt_eqs) {
			if (registration.event_id == event_id) {
				registrations.push_back(registration);
			}
		}
	}

	for (const auto& registration: registrations) {
		const auto result = LibKernel::EventQueue::KernelTriggerEvent(
		    registration.eq, static_cast<uintptr_t>(registration.event_id),
		    LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS,
		    reinterpret_cast<void*>(static_cast<uintptr_t>(context_id)));
		if (result == LibKernel::KERNEL_ERROR_EBADF || result == LibKernel::KERNEL_ERROR_ENOENT) {
			DeleteInterruptEq(registration.eq, registration.event_id);
			continue;
		}
		EXIT_NOT_IMPLEMENTED(result != OK);
	}
}

} // namespace Libs::Graphics
