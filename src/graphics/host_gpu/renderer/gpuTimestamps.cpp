#include "graphics/host_gpu/renderer/gpuTimestamps.h"

#include "common/assert.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/sync.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace Libs::Graphics {

namespace {

constexpr double   FixedPointOne       = 4294967296.0;
constexpr uint64_t ReferenceFrequency  = 100000000;
constexpr uint64_t CalibrationInterval = 2 * ReferenceFrequency;
// A new segment starts this long after its sample: a batch submitted before the sample and run
// within this delay converts with the segment the previous batches used.
constexpr uint64_t SegmentDelay = ReferenceFrequency;

uint64_t Scale(uint64_t delta, uint64_t rate) {
	const uint64_t delta_lo = delta & 0xffffffffu;
	return (delta >> 32u) * rate + delta_lo * (rate >> 32u) +
	       ((delta_lo * (rate & 0xffffffffu)) >> 32u);
}

double RateOf(const GpuTimestamps::Segment& segment) {
	return static_cast<double>(segment.rate) / FixedPointOne;
}

uint64_t FixedRate(double rate) {
	return static_cast<uint64_t>(rate * FixedPointOne);
}

uint64_t ValidMask(GraphicContext& graphics) {
	uint32_t count = 0;
	graphics.physical_device.getQueueFamilyProperties(&count, nullptr);
	std::vector<vk::QueueFamilyProperties> families(count);
	graphics.physical_device.getQueueFamilyProperties(&count, families.data());
	const auto bits = graphics.queue_family < families.size()
	                      ? families[graphics.queue_family].timestampValidBits
	                      : 0u;
	return bits == 0 ? 0 : bits >= 64 ? UINT64_MAX : (uint64_t {1} << bits) - 1u;
}

double NominalRate(const GraphicContext& graphics) {
	// Reference ticks (10 ns) per host GPU tick.
	return graphics.physical_device_properties.limits.timestampPeriod / 10.0;
}

} // namespace

GpuTimestamps::GpuTimestamps(GraphicContext& graphics, CommandScheduler& scheduler,
                             RenderContext& context)
    : m_graphics(graphics), m_scheduler(scheduler), m_context(context), m_mask(ValidMask(graphics)),
      m_readback(graphics, scheduler, MemoryUsage::Download, 0,
                 vk::BufferUsageFlagBits::eTransferDst, QueryCount * sizeof(uint64_t)),
      m_clock(m_mask, NominalRate(graphics)) {
	SetVulkanObjectNameF(m_graphics.device, m_readback.Handle(), "GPU Timestamp Readback");
	if (m_mask == 0) {
		return;
	}
	vk::QueryPoolCreateInfo pool_info {};
	pool_info.queryType  = vk::QueryType::eTimestamp;
	pool_info.queryCount = QueryCount;
	RequireVulkanSuccess(m_graphics.device.createQueryPool(&pool_info, nullptr, &m_pool),
	                     "create GPU timestamp query pool");
	// A first sample here gives the first segment a measured rate.
	uint64_t   ticks     = 0;
	uint64_t   reference = 0;
	const bool sampled   = SampleClocks(ticks, reference);
	if (sampled) {
		m_clock.Sample(ticks, reference);
	}
	InitializeWithSubmission(!sampled);
	m_scheduler.SetBeforeSubmit([this] { Resolve(); });
}

GpuTimestamps::~GpuTimestamps() {
	m_scheduler.SetBeforeSubmit({});
	if (m_pool != nullptr) {
		m_graphics.device.destroyQueryPool(m_pool, nullptr);
	}
}

uint64_t GpuTimestamps::ToReference(const Segment& previous, const Segment& current,
                                    uint64_t ticks) {
	return Clock::Convert(previous, current, ticks, UINT64_MAX);
}

uint64_t GpuTimestamps::Clock::Convert(const Segment& previous, const Segment& current,
                                       uint64_t ticks, uint64_t mask) {
	const bool  after_current = ((ticks - current.device_base) & mask) <= (mask >> 1u);
	const auto& segment       = after_current ? current : previous;
	const auto  forward       = (ticks - segment.device_base) & mask;
	return forward <= (mask >> 1u)
	           ? segment.reference_base + Scale(forward, segment.rate)
	           : segment.reference_base - Scale((segment.device_base - ticks) & mask, segment.rate);
}

uint64_t GpuTimestamps::Clock::Convert(uint64_t ticks) const {
	return Convert(m_previous, m_current, ticks & m_mask, m_mask);
}

void GpuTimestamps::Clock::Restart(uint64_t ticks, uint64_t reference) {
	m_measure_reference = reference;
	m_measure_ticks     = 0;
	m_last_ticks        = ticks;
	m_last_reference    = reference;
}

void GpuTimestamps::Clock::Sample(uint64_t ticks, uint64_t reference) {
	ticks &= m_mask;
	if (!m_sampled) {
		m_sampled = true;
		Restart(ticks, reference);
		return;
	}
	// A masked interval is only known while it is shorter than half a counter period: past a
	// quarter, by the current rate, measure again from this sample.
	const double rate_before = m_anchored ? RateOf(m_current) : m_nominal_rate;
	const auto   interval    = (ticks - m_last_ticks) & m_mask;
	if (static_cast<double>(reference - m_last_reference) / rate_before >=
	    static_cast<double>(m_mask >> 2u)) {
		Restart(ticks, reference);
		if (m_anchored) {
			m_current  = {ticks, reference, m_current.rate};
			m_previous = m_current;
		}
		return;
	}
	if (interval == 0) {
		return;
	}
	m_measure_ticks += interval;
	// Host periods can be off: RADV reports 10.019 ns for a 10 ns counter on Strix Halo.
	const auto   measured_reference = reference - m_measure_reference;
	const double rate =
	    measured_reference >= ReferenceFrequency / 20
	        ? static_cast<double>(measured_reference) / static_cast<double>(m_measure_ticks)
	        : rate_before;
	if (!m_anchored) {
		m_current  = {ticks, reference, FixedRate(rate)};
		m_previous = m_current;
		m_anchored = true;
	} else {
		// Remove the offset error over the next interval.
		const double error = static_cast<double>(static_cast<int64_t>(reference - Convert(ticks)));
		const double correction =
		    std::clamp(error / static_cast<double>(interval), -rate / 1000.0, rate / 1000.0);
		const auto knot =
		    (ticks + static_cast<uint64_t>(static_cast<double>(SegmentDelay) / rate)) & m_mask;
		const auto knot_reference = Convert(knot);
		m_previous                = m_current;
		m_current                 = {knot, knot_reference, FixedRate(rate + correction)};
	}
	m_last_ticks     = ticks;
	m_last_reference = reference;
}

void GpuTimestamps::Clock::Anchor(uint64_t ticks, uint64_t reference) {
	m_anchor   = {ticks & m_mask, reference, FixedRate(m_nominal_rate)};
	m_current  = m_anchor;
	m_previous = m_current;
	m_anchored = true;
	Restart(ticks & m_mask, reference);
}

void GpuTimestamps::Clock::Advance(uint64_t reference) {
	// The anchor line from a base at this reference time, counted from the anchor so that
	// rounding does not accumulate.
	const auto elapsed =
	    reference > m_anchor.reference_base ? reference - m_anchor.reference_base : uint64_t {0};
	const auto ticks = static_cast<uint64_t>(static_cast<double>(elapsed) / RateOf(m_anchor));
	m_current        = {(m_anchor.device_base + ticks) & m_mask,
	                    m_anchor.reference_base + Scale(ticks, m_anchor.rate), m_anchor.rate};
	m_previous       = m_current;
	m_last_reference = reference;
}

bool GpuTimestamps::SampleClocks(uint64_t& ticks, uint64_t& reference) const {
	const auto get = m_graphics.get_calibrated_timestamps;
	if (get == nullptr) {
		return false;
	}
	VkCalibratedTimestampInfoKHR info {};
	info.sType      = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
	info.timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
	// The device sample lies between the two reference reads; keep the tightest bracket.
	uint64_t best = UINT64_MAX;
	for (int attempt = 0; attempt < 4; ++attempt) {
		uint64_t   device    = 0;
		uint64_t   deviation = 0;
		const auto before    = Sync::ReadReferenceClock();
		const auto result    = get(m_graphics.device, 1, &info, &device, &deviation);
		const auto after     = Sync::ReadReferenceClock();
		RequireVulkanSuccess(static_cast<vk::Result>(result), "vkGetCalibratedTimestamps");
		if (after - before < best) {
			best      = after - before;
			ticks     = device;
			reference = before + (after - before) / 2;
		}
	}
	return true;
}

void GpuTimestamps::InitializeWithSubmission(bool anchor) {
	// Queries start reset. Without calibrated timestamps, one timestamp read back right after its
	// batch anchors the clock.
	vk::CommandPoolCreateInfo pool_info {};
	pool_info.flags            = vk::CommandPoolCreateFlagBits::eTransient;
	pool_info.queueFamilyIndex = m_graphics.queue_family;
	vk::CommandPool pool       = nullptr;
	RequireVulkanSuccess(m_graphics.device.createCommandPool(&pool_info, nullptr, &pool),
	                     "create GPU clock setup pool");
	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = 1;
	vk::CommandBuffer command   = nullptr;
	RequireVulkanSuccess(m_graphics.device.allocateCommandBuffers(&allocate, &command),
	                     "allocate GPU clock setup commands");
	vk::CommandBufferBeginInfo begin {};
	begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	RequireVulkanSuccess(command.begin(&begin), "begin GPU clock setup commands");
	command.resetQueryPool(m_pool, 0, QueryCount);
	vk::QueryPool anchor_pool = nullptr;
	if (anchor) {
		vk::QueryPoolCreateInfo anchor_info {};
		anchor_info.queryType  = vk::QueryType::eTimestamp;
		anchor_info.queryCount = 1;
		RequireVulkanSuccess(m_graphics.device.createQueryPool(&anchor_info, nullptr, &anchor_pool),
		                     "create GPU clock anchor query");
		command.resetQueryPool(anchor_pool, 0, 1);
		command.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, anchor_pool, 0);
	}
	RequireVulkanSuccess(command.end(), "end GPU clock setup commands");
	vk::Fence           fence = nullptr;
	vk::FenceCreateInfo fence_info {};
	RequireVulkanSuccess(m_graphics.device.createFence(&fence_info, nullptr, &fence),
	                     "create GPU clock setup fence");
	{
		Common::LockGuard lock(m_graphics.queue_mutex);
		vk::SubmitInfo    submit {};
		submit.commandBufferCount = 1;
		submit.pCommandBuffers    = &command;
		RequireVulkanSuccess(m_graphics.queue.submit(1, &submit, fence), "submit GPU clock setup");
	}
	RequireVulkanSuccess(m_graphics.device.waitForFences(1, &fence, VK_TRUE, UINT64_MAX),
	                     "wait GPU clock setup");
	const auto reference = Sync::ReadReferenceClock();
	if (anchor) {
		uint64_t ticks = 0;
		RequireVulkanSuccess(m_graphics.device.getQueryPoolResults(anchor_pool, 0, 1, sizeof(ticks),
		                                                           &ticks, sizeof(ticks),
		                                                           vk::QueryResultFlagBits::e64),
		                     "read GPU clock anchor");
		m_graphics.device.destroyQueryPool(anchor_pool, nullptr);
		m_clock.Anchor(ticks, reference);
	}
	m_graphics.device.destroyFence(fence, nullptr);
	m_graphics.device.destroyCommandPool(pool, nullptr);
}

void GpuTimestamps::Write(uint64_t vaddr, uint32_t size, bool end_of_pipe) {
	if ((size != sizeof(uint32_t) && size != sizeof(uint64_t)) || vaddr == 0 ||
	    (vaddr & (sizeof(uint32_t) - 1u)) != 0) {
		EXIT("invalid GPU clock write, dst=0x%016" PRIx64 " size=%u\n", vaddr, size);
	}
	if (m_mask == 0) {
		// The host GPU has no timestamps here: the parse time stands in.
		const auto value = Sync::ReadReferenceClock();
		std::memcpy(reinterpret_cast<void*>(vaddr), &value, size);
		return;
	}
	StoreRetries();
	const auto now = Sync::ReadReferenceClock();
	if (m_graphics.get_calibrated_timestamps != nullptr) {
		if (!m_clock.Anchored() || now - m_clock.LastReference() >= CalibrationInterval) {
			uint64_t ticks     = 0;
			uint64_t reference = 0;
			EXIT_IF(!SampleClocks(ticks, reference));
			m_clock.Sample(ticks, reference);
		}
	} else if (now - m_clock.LastReference() >= CalibrationInterval) {
		m_clock.Advance(now);
	}
	if (m_issued - m_retired.load(std::memory_order_acquire) >= QueryCount) {
		// Every query waits for its batch to complete.
		m_scheduler.Finish();
		m_scheduler.WaitPriorityOperations(m_scheduler.CurrentTick() - 1);
	}
	if (m_batch == nullptr) {
		m_batch              = std::make_shared<Batch>();
		m_batch->first_query = static_cast<uint32_t>(m_issued % QueryCount);
		// Queued before the interrupts recorded after this write, so it runs first.
		m_scheduler.DeferPriorityOperation([this, batch = m_batch] { Complete(*batch); });
	}
	const auto query = static_cast<uint32_t>(m_issued % QueryCount);
	m_issued++;
	m_batch->writes.push_back({vaddr, query, size});
	// Timestamps may be written inside a render pass; their readback waits for the batch end.
	m_scheduler.Current().Handle().writeTimestamp2(end_of_pipe
	                                                   ? vk::PipelineStageFlagBits2::eAllCommands
	                                                   : vk::PipelineStageFlagBits2::eTopOfPipe,
	                                               m_pool, query);
}

void GpuTimestamps::Resolve() {
	if (m_batch == nullptr) {
		return;
	}
	const auto batch = std::move(m_batch);
	m_batch          = nullptr;

	auto& command_buffer = m_scheduler.Current();
	command_buffer.EndRendering();
	auto       command = command_buffer.Handle();
	const auto count   = static_cast<uint32_t>(batch->writes.size());
	const auto first   = std::min(count, QueryCount - batch->first_query);
	const auto copy    = [&](uint32_t query, uint32_t queries) {
		command.copyQueryPoolResults(m_pool, query, queries, m_readback.Handle(),
		                             query * sizeof(uint64_t), sizeof(uint64_t),
		                             vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
		// Query commands execute in submission order: the reset follows the copy.
		command.resetQueryPool(m_pool, query, queries);
	};
	copy(batch->first_query, first);
	if (count > first) {
		copy(0, count - first);
	}
	vk::MemoryBarrier2 copied {};
	copied.srcStageMask  = vk::PipelineStageFlagBits2::eCopy;
	copied.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
	copied.dstStageMask  = vk::PipelineStageFlagBits2::eHost;
	copied.dstAccessMask = vk::AccessFlagBits2::eHostRead;
	vk::DependencyInfo dependency {};
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers    = &copied;
	command.pipelineBarrier2(dependency);

	std::lock_guard lock(m_mutex);
	batch->previous = m_clock.Previous();
	batch->current  = m_clock.Current();
	batch->resolved = true;
}

void GpuTimestamps::Complete(const Batch& batch) {
	const auto count = static_cast<uint32_t>(batch.writes.size());
	Segment    previous;
	Segment    current;
	bool       deferred = false;
	{
		std::lock_guard lock(m_mutex);
		if (!batch.resolved) {
			m_retired.fetch_add(count, std::memory_order_release);
			return;
		}
		previous = batch.previous;
		current  = batch.current;
		deferred = m_retrying || !m_retries.empty();
	}
	const auto first = std::min(count, QueryCount - batch.first_query);
	m_readback.Invalidate(batch.first_query * sizeof(uint64_t), first * sizeof(uint64_t));
	m_readback.Invalidate(0, (count - first) * sizeof(uint64_t));
	for (const auto& write: batch.writes) {
		uint64_t ticks = 0;
		std::memcpy(&ticks, m_readback.Mapped().data() + write.query * sizeof(uint64_t),
		            sizeof(ticks));
		const auto value = Clock::Convert(previous, current, ticks & m_mask, m_mask);
		if (!deferred && m_context.StoreAtCompletion(write.vaddr, &value, write.size)) {
			continue;
		}
		// Later values must not land before this one.
		deferred = true;
		std::lock_guard lock(m_mutex);
		m_retries.push_back({write.vaddr, value, write.size});
	}
	m_retired.fetch_add(count, std::memory_order_release);
	if (deferred) {
		QueueRetries();
	}
}

void GpuTimestamps::QueueRetries() {
	// A guest may wait for a deferred value without another GPU clock write: the GPU thread
	// stores it as soon as it is between packets. One queued store covers all retries so far.
	{
		std::lock_guard lock(m_mutex);
		if (m_retries.empty() || m_retry_queued) {
			return;
		}
		m_retry_queued = true;
	}
	const bool queued = m_context.PostGpuCommand([this] {
		{
			std::lock_guard lock(m_mutex);
			m_retry_queued = false;
		}
		StoreRetries();
	});
	if (!queued) {
		// The GPU is shutting down: the unmaps of the teardown store what is left.
		std::lock_guard lock(m_mutex);
		m_retry_queued = false;
	}
}

void GpuTimestamps::StoreRetries() {
	for (;;) {
		std::vector<Retry> retries;
		{
			std::lock_guard lock(m_mutex);
			if (m_retries.empty()) {
				m_retrying = false;
				return;
			}
			retries.swap(m_retries);
			m_retrying = true;
		}
		// Plain stores fault here like other GPU-thread writes, so the watching caches see them.
		// Unmapping a range stores its retries first, so every destination is still mapped.
		for (const auto& retry: retries) {
			std::memcpy(reinterpret_cast<void*>(retry.vaddr), &retry.value, retry.size);
		}
	}
}

} // namespace Libs::Graphics
