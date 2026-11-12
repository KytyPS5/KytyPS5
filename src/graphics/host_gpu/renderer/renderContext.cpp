#include "graphics/host_gpu/renderer/renderContext.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/emulatorConfig.h"
#include "common/hackNotice.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/shader/shaderBindings.h"
#include "kernel/memory.h"
#include "libs/errno.h"

#include <algorithm>
#include <cstring>

namespace Libs::Graphics {

RenderContext::RenderContext(GraphicContext& graphics, const std::filesystem::path& shader_store)
    : m_graphics(graphics), m_render_executor(*this),
      m_command_scheduler(*this, graphics,
                          (Config::GpuPipelineStages() & Config::GPU_PIPELINE_RECORDING) != 0),
      m_descriptor_heap(graphics, m_command_scheduler.GetMasterSemaphore()),
      m_pipeline_cache(graphics, &m_image_heap, shader_store), m_sampler_cache(graphics),
      m_buffer_cache(graphics, m_command_scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, m_command_scheduler, m_page_manager, m_buffer_cache),
      m_image_heap(*this, m_page_manager) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	if (graphics.ray_query_enabled) {
		m_ray_tracing = std::make_unique<RayTracing>(*this);
	}
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

void RenderContext::MapMemory(uint64_t vaddr, uint64_t size) {
	{
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Add(vaddr, size);
		m_mapped_ranges_version++;
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
		m_buffer_cache.InvalidateMemory(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		m_image_heap.UnmapMemory(vaddr, size);
		m_occlusion_queries.OnUnmap(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
		m_mapped_ranges_version++;
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
	for (const auto reg: program.info.dma_base_registers) {
		const auto index = static_cast<uint64_t>(reg) - program.user_data_base;
		if (reg < program.user_data_base || index + 1u >= user_data.size()) {
			continue;
		}
		const auto base = user_data[index] | (static_cast<uint64_t>(user_data[index + 1u]) << 32u);
		// The registers of an access that never runs may hold stale data. Mapped memory never
		// reaches the top of the address space, so a window that would overflow is skipped.
		if (base > UINT64_MAX - BufferCache::CACHING_PAGESIZE) {
			continue;
		}
		// DMA reaches only memory with a cached buffer; any other access records a fault and
		// reads zero, and the buffer arrives after the shader has run. That loses data the guest
		// writes for a single dispatch, such as the glyph bitmaps GTA V copies into its font atlas.
		m_mapped_ranges.ForEachInRange(base, BufferCache::CACHING_PAGESIZE,
		                               [this](uint64_t start, uint64_t end) {
			                               (void)m_buffer_cache.FindBuffer(start, end - start);
		                               });
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
	NativeRayTracing::TlasSource source;
	if (!NativeRayTracing::FindGuestTlasSource(program.info, source)) {
		return;
	}
	const auto& resources = *runtime.resources;
	// Memory the CPU wrote last: its current bytes are in guest memory.
	const auto readable = [this](uint64_t address, uint64_t size) {
		return GuestRange {address, size}.Valid() && m_mapped_ranges.Contains(address, size) &&
		       !(m_buffer_cache.MayBeGpuModified(address, size) &&
		         m_buffer_cache.IsRegionGpuModified(address, size));
	};
	// The header address as the walk reads it (see NativeRayTracing::Prepare()).
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
		const auto pointer = NativeRayTracing::UserPointer(
		    source, user_data[index] | (uint64_t {user_data[index + 1u]} << 32u), readable);
		if (pointer == 0 || pointer > UINT64_MAX - source.header_offset ||
		    !readable(pointer + source.header_offset, sizeof(header))) {
			return;
		}
		std::memcpy(&header, reinterpret_cast<const void*>(pointer + source.header_offset), sizeof(header));
	}
	std::vector<std::pair<uint64_t, uint64_t>> ranges;
	const auto tlas = m_native_ray_tracing.ReadGuestTlas(header, source, readable, ranges);
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
	if (program.info.ray_query && m_native_ray_tracing.Enabled()) {
		m_native_ray_tracing.NoteTlasInstances(tlas.instances);
	}
}

bool RenderContext::SynchronizeDmaFootprint(std::span<const ShaderStageRuntime* const> stages) {
	// Larger V# ranges are no buffers a draw reads (see FindBuffers()).
	constexpr uint64_t MaxBufferBytes = uint64_t {256} * 1024 * 1024;
	// An index offset or thread ID added to a V# record stays within a wave of records past it.
	constexpr uint64_t WaveRecords = 64;
	m_dma_spans.clear();
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
		if (!program.info.dma_bounded) {
			return false;
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
	}
	lock.unlock();
	m_buffer_cache.SynchronizeBuffersInSpans(m_dma_spans);
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
	if (full_sync && epochs != m_bda_sync_epochs &&
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
	if (m_ray_tracing) {
		m_ray_tracing->RunGarbageCollector();
	}
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
