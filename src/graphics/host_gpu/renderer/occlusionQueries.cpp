#include "graphics/host_gpu/renderer/occlusionQueries.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>

namespace Libs::Graphics {

namespace {

// Host queries waiting for their results; GTA V keeps a few dozen per frame in flight.
constexpr uint32_t PoolCapacity = 16384;

} // namespace

OcclusionQueries::OcclusionQueries(RenderContext& context)
    : m_context(context), m_device(context.GetGraphics().device), m_segments(PoolCapacity) {
	const auto& graphics = context.GetGraphics();
	if (!graphics.host_query_reset_enabled) {
		Log::WriteToConsoleAndLog("Occlusion queries: the Vulkan device lacks hostQueryReset; "
		                          "they read always visible\n");
		return;
	}
	vk::QueryPoolCreateInfo info {};
	info.queryType  = vk::QueryType::eOcclusion;
	info.queryCount = PoolCapacity;
	if (m_device.createQueryPool(&info, nullptr, &m_pool) != vk::Result::eSuccess) {
		m_pool = nullptr;
		Log::WriteToConsoleAndLog("Occlusion queries: cannot create the query pool; they read "
		                          "always visible\n");
		return;
	}
	SetVulkanObjectNameF(m_device, m_pool, "Kyty.OcclusionQueries");
	m_device.resetQueryPool(m_pool, 0, PoolCapacity);
	if (graphics.occlusion_query_precise) {
		m_control = vk::QueryControlFlagBits::ePrecise;
	}
	m_results.resize(PoolCapacity);
}

OcclusionQueries::~OcclusionQueries() {
	if (m_pool != nullptr) {
		m_device.destroyQueryPool(m_pool, nullptr);
	}
}

void OcclusionQueries::BeginQuery(vk::CommandBuffer recorder) {
	EXIT_IF(m_query_active || !m_rendering);
	if (!m_segments.AllocateSlot(m_active_slot)) {
		return;
	}
	recorder.beginQuery(m_pool, m_active_slot, m_control);
	m_query_active = true;
}

void OcclusionQueries::EndQuery(vk::CommandBuffer recorder) {
	EXIT_IF(!m_query_active);
	recorder.endQuery(m_pool, m_active_slot);
	m_query_active = false;
}

void OcclusionQueries::OnBeginRendering(vk::CommandBuffer recorder) {
	m_rendering = true;
	if (m_pool != nullptr && m_segments.Counting()) {
		BeginQuery(recorder);
	}
}

void OcclusionQueries::OnEndRendering(vk::CommandBuffer recorder) {
	if (m_query_active) {
		EndQuery(recorder);
	}
	m_rendering = false;
}

void OcclusionQueries::OnFrame() {
	m_segments.OnFrame();
	// Dumps are only published when the GPU's progress is checked, so a counter can reach guest
	// memory later than the labels and flips around it. GTA V (PPSA04263) takes the counters of
	// a frame a few frames later, and on the console they are there by then: when a counter of
	// its water was published a flip later than usual, the game drew a frame of the water
	// without reflections and waves. Wait for the GPU to finish the dumps of earlier frames
	// before this frame's flip (it usually already has).
	uint64_t tick = 0;
	for (const auto& pending: m_pending) {
		if (pending.frame < m_frame) {
			tick = std::max(tick, pending.tick);
		}
	}
	auto& scheduler = m_context.GetCommandScheduler();
	if (tick != 0 && !scheduler.GetMasterSemaphore().IsFree(tick)) {
		scheduler.Wait(tick);
	}
	m_frame++;
	PublishCompleted(true);
}

void OcclusionQueries::PublishCompleted(bool refresh) {
	auto& master = m_context.GetCommandScheduler().GetMasterSemaphore();
	while (!m_pending.empty()) {
		if (!master.IsFree(m_pending.front().tick)) {
			if (!refresh) {
				return;
			}
			// Draws do not wait on dumps: the GPU's progress is only queried here.
			master.Refresh();
			refresh = false;
			continue;
		}
		Publish(m_pending.front());
		m_pending.pop_front();
	}
}

void OcclusionQueries::OnUnmap(uint64_t address, uint64_t size) {
	for (auto& pending: m_pending) {
		if (pending.dump.address < address + size &&
		    address < pending.dump.address + OcclusionCounters::DumpSize) {
			pending.write = false;
		}
	}
}

void OcclusionQueries::Dump(CommandScheduler& scheduler, uint64_t address) {
	EXIT_IF(m_pool == nullptr);
	auto&      buffer   = scheduler.Current();
	const auto recorder = buffer.Handle();
	if (m_query_active) {
		EndQuery(recorder);
	}
	m_pending.push_back(
	    {.dump = m_segments.OnDump(address), .tick = scheduler.CurrentTick(), .frame = m_frame});
	PublishCompleted(false);
	if (m_segments.Counting() && buffer.IsRendering()) {
		EXIT_IF(!m_rendering);
		BeginQuery(recorder);
	}
}

void OcclusionQueries::ResetSlots(uint64_t first_slot, uint64_t end_slot) {
	const auto capacity = m_segments.Capacity();
	while (first_slot < end_slot) {
		const auto slot  = static_cast<uint32_t>(first_slot % capacity);
		const auto count = static_cast<uint32_t>(
		    std::min<uint64_t>(end_slot - first_slot, uint64_t {capacity} - slot));
		m_device.resetQueryPool(m_pool, slot, count);
		first_slot += count;
	}
}

uint64_t OcclusionQueries::ReadSamples(uint64_t first_slot, uint64_t end_slot) {
	const auto capacity = m_segments.Capacity();
	uint64_t   samples  = 0;
	while (first_slot < end_slot) {
		const auto slot  = static_cast<uint32_t>(first_slot % capacity);
		const auto count = static_cast<uint32_t>(
		    std::min<uint64_t>(end_slot - first_slot, uint64_t {capacity} - slot));
		// The command buffers that ended these queries completed: nothing waits here.
		const auto result = m_device.getQueryPoolResults(
		    m_pool, slot, count, count * sizeof(uint64_t), m_results.data(), sizeof(uint64_t),
		    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
		if (result != vk::Result::eSuccess) {
			Log::WriteToConsoleAndLog(fmt::format(
			    "Occlusion queries: vkGetQueryPoolResults failed ({}); the query reads visible\n",
			    vk::to_string(result)));
			samples += OcclusionCounters::UncountedSamples;
		} else {
			for (uint32_t i = 0; i < count; i++) {
				samples += m_results[i];
			}
		}
		first_slot += count;
	}
	return samples;
}

void OcclusionQueries::Publish(const PendingDump& pending) {
	const auto& dump = pending.dump;
	m_total += ReadSamples(dump.first_slot, dump.end_slot);
	if (dump.uncounted) {
		m_total += OcclusionCounters::UncountedSamples;
	}
	ResetSlots(dump.first_slot, dump.end_slot);
	m_segments.Release(dump.end_slot);
	if (!pending.write) {
		return;
	}
	// DB 0 last: once its ready bit is visible, the other DBs are too.
	auto* counters = reinterpret_cast<volatile uint64_t*>(dump.address);
	for (uint32_t db = OcclusionCounters::DbCount; db-- > 0;) {
		counters[db * (OcclusionCounters::DbStride / sizeof(uint64_t))] =
		    OcclusionCounters::DbValue(db, m_total);
	}
}

} // namespace Libs::Graphics
