#include "graphics/host_gpu/renderer/renderContext.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/presentation/videoOut.h"
#include "libs/errno.h"

#include <algorithm>

namespace Libs::Graphics {

RenderContext::RenderContext(GraphicContext& graphics, const std::filesystem::path& shader_store)
    : m_graphics(graphics), m_render_executor(*this), m_command_scheduler(*this, graphics),
      m_descriptor_heap(graphics, m_command_scheduler.GetMasterSemaphore()),
      m_pipeline_cache(graphics, &m_image_heap, &m_parameter_keys, shader_store),
      m_sampler_cache(graphics),
      m_buffer_cache(graphics, m_command_scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, m_command_scheduler, m_page_manager, m_buffer_cache),
      m_image_heap(*this, m_page_manager), m_parameter_keys(*this, m_page_manager),
      m_occlusion_queries(graphics, m_command_scheduler) {
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
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}
	// The access retries once the image write-backs over its page land.
	if (WaitWriteBacks(fault_vaddr, fault_size)) {
		return true;
	}
	if (access == PageFaultAccess::Write) {
		m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_image_heap.InvalidateMemory(fault_vaddr, fault_size);
		m_parameter_keys.InvalidateMemory(fault_vaddr, fault_size);
	} else {
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
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
	m_parameter_keys.InvalidateMemory(vaddr, size);
	return true;
}

bool RenderContext::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

namespace {

using AddressRange            = ShaderRecompiler::IR::AddressRange;
constexpr uint64_t ValueLimit = uint64_t {1} << 32u;

// A word of the stage's user data or flattened SRT, as recorded for this draw.
std::optional<uint64_t> StageWord(const ShaderStageRuntime& stage, AddressRange::Source source,
                                  uint32_t index) {
	const auto& resources      = *stage.resources;
	const auto  user_data_base = stage.module->info.user_data_base;
	if (source == AddressRange::Source::UserData) {
		if (index < user_data_base || index - user_data_base >= resources.user_data.size()) {
			return std::nullopt;
		}
		return resources.user_data[index - user_data_base];
	}
	return index < resources.flattened_srt.size()
	           ? std::optional<uint64_t>(resources.flattened_srt[index])
	           : std::nullopt;
}

// One past the largest index a pixel input gives the range; 0 when unknown.
uint64_t PixelParameterExtent(const AddressRange& range, const AddressRangeExtents& extents) {
	if (extents.exporter == nullptr || !*extents.exporter || extents.pixel_inputs == nullptr) {
		return 0;
	}
	const auto& pixel = *extents.pixel_inputs;
	const auto  input = range.parameter / 4u;
	if (!ShaderPixelParameterIsExported(pixel, input) ||
	    ShaderPixelParameterIsCustom(pixel, input)) {
		return 0;
	}
	const auto exported =
	    ShaderPixelParameterMappedLocation(pixel, input) * 4u + range.parameter % 4u;
	const auto& bounds = extents.exporter->module->info.info.export_bounds;
	const auto  bound  = std::ranges::find_if(
	    bounds, [&](const auto& known) { return known.parameter == exported; });
	if (bound == bounds.end() || bound->as_float != range.parameter_float) {
		return 0;
	}
	// Flat inputs and P0 are one vertex's word; interpolation keeps only a per-primitive float.
	const bool exact = range.parameter_vertex || ShaderPixelParameterIsFlat(pixel, input);
	if (!exact && (!range.parameter_float || !bound->per_primitive)) {
		return 0;
	}
	uint64_t count = 0;
	if (bound->term == AddressRange::Term::InstanceIndex) {
		if (extents.instance_end == 0) {
			return 0;
		}
		count = extents.instance_end - 1;
	}
	uint64_t value = 0;
	if (bound->value_source != AddressRange::Source::None) {
		const auto known = StageWord(*extents.exporter, bound->value_source, bound->value_word);
		if (!known) {
			return 0;
		}
		value = *known;
	}
	if ((bound->scale != 0 && count >= ValueLimit / bound->scale) ||
	    (bound->value_scale != 0 && value >= ValueLimit / bound->value_scale)) {
		return 0;
	}
	const auto largest = bound->hi + bound->scale * count + bound->value_scale * value;
	// The float of an integer below 2^24 is exact; above, rounding could pass the bound.
	if (largest >= (bound->as_float ? uint64_t {1} << 24u : ValueLimit)) {
		return 0;
	}
	// Host interpolation of equal values is only near exact: allow one more.
	return largest + (exact ? 1u : 2u);
}

} // namespace

void RenderContext::BackAddressRanges(const ShaderStageRuntime&  stage,
                                      const AddressRangeExtents& extents) {
	using Range                 = AddressRange;
	using Source                = Range::Source;
	using Term                  = Range::Term;
	constexpr uint64_t MaxBytes = 512ull * 1024 * 1024;
	if (!stage) {
		return;
	}
	const auto word = [&](Source source, uint32_t index) {
		return StageWord(stage, source, index);
	};
	const auto extent = [&](const Range& range) -> uint64_t {
		const auto term = range.term;
		switch (term) {
			case Term::None: return 1;
			case Term::PixelParameter: return PixelParameterExtent(range, extents);
			case Term::InstanceIndex: return extents.instance_end;
			case Term::GlobalInvocationX:
			case Term::GlobalInvocationY:
			case Term::GlobalInvocationZ:
				return extents.invocations[static_cast<uint32_t>(term) -
				                           static_cast<uint32_t>(Term::GlobalInvocationX)];
			case Term::WorkgroupX:
			case Term::WorkgroupY:
			case Term::WorkgroupZ:
				return extents.workgroups[static_cast<uint32_t>(term) -
				                          static_cast<uint32_t>(Term::WorkgroupX)];
		}
		return 0;
	};
	for (const auto& range: stage.module->info.info.address_ranges) {
		uint64_t base = range.base;
		if (range.base_source != Source::Constant) {
			const auto low  = word(range.base_source, range.base_words[0]);
			const auto high = word(range.base_source, range.base_words[1]);
			if (!low || !high) {
				continue;
			}
			base = *low | (*high << 32u);
		}
		if (range.align_base) {
			base &= ~uint64_t {3};
		}
		const auto count = extent(range);
		if (count == 0) {
			continue;
		}
		uint64_t value = 0;
		if (range.value_source != Source::None) {
			const auto known = word(range.value_source, range.value_word);
			if (!known) {
				continue;
			}
			value = *known;
		}
		// A 32-bit offset that could pass 2^32 wraps and proves nothing.
		const auto growth = range.scale * (count - 1) + range.value_scale * value;
		if (range.end < 0 || growth >= ValueLimit || range.offset_max + growth >= ValueLimit) {
			continue;
		}
		const auto first = base + static_cast<uint64_t>(range.begin) +
		                   (range.value_exact ? range.value_scale * value : 0);
		const auto last  = base + static_cast<uint64_t>(range.end) + growth;
		if (last <= first || last - first > MaxBytes) {
			continue;
		}
		std::shared_lock lock(m_mapped_ranges_mutex);
		m_mapped_ranges.ForEachInRange(first, last - first,
		                               [this](uint64_t start, uint64_t finish) {
			                               (void)m_buffer_cache.FindBuffer(start, finish - start);
		                               });
	}
}

void RenderContext::MapMemory(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mapped_ranges_mutex);
	m_mapped_ranges.Add(vaddr, size);
	m_buffer_cache.QueueBdaSync(vaddr, size);
}

void RenderContext::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto unmap = [this, vaddr, size] {
		// Check cache ownership on the GPU thread. Guest-memory callbacks can still
		// access a range with no cached data, so they must finish before it is unmapped.
		if (m_command_scheduler.Active() &&
		    (m_buffer_cache.IsRegionRegistered(vaddr, size) ||
		     m_texture_cache.IsRegionRegistered(vaddr, size) ||
		     m_command_scheduler.HasPendingPriorityOperations())) {
			const auto tick = m_command_scheduler.CurrentTick();
			m_command_scheduler.Finish();
			m_command_scheduler.WaitPriorityOperations(tick);
		}
		m_buffer_cache.InvalidateMemory(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		m_image_heap.UnmapMemory(vaddr, size);
		m_parameter_keys.InvalidateMemory(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
	};
	// Shutdown still owns the GPU while queued rendering drains, but its command lane no
	// longer accepts external work. Use the guest GPU's state for the teardown route.
	if (m_gpu == nullptr || m_gpu->IsStopping()) {
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
}

void RenderContext::PrepareBda() {
	[[maybe_unused]] static const bool logged = [] {
		Log::WriteToConsoleAndLog("GPU: using buffer device address (BDA) shader memory access.\n");
		return true;
	}();
	std::shared_lock lock(m_mapped_ranges_mutex);
	m_buffer_cache.SynchronizeBda(m_mapped_ranges);
	m_fault_process_pending = true;
}

void RenderContext::RunGarbageCollector() {
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	m_image_heap.RunGarbageCollector();
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
	// What allocations took past the budget comes back from idle memory, whatever its age:
	// pages before images, which cost more to make again.
	if (auto past = m_graphics.device_past_budget.exchange(0, std::memory_order_relaxed);
	    past != 0) {
		past -= std::min(past, m_buffer_cache.ReclaimIdle(past));
		if (past != 0) {
			(void)m_texture_cache.ReclaimIdle(past);
		}
	}
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
