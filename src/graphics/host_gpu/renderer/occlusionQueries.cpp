#include "graphics/host_gpu/renderer/occlusionQueries.h"

#include "common/assert.h"
#include "common/hackNotice.h"
#include "common/logging/log.h"
#include "gpu_tiler_shaders/occlusion_counters_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "kernel/memory.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>

namespace Libs::Graphics {

namespace {

// Host queries waiting for their results; GTA V keeps a few dozen per frame in flight.
constexpr uint32_t PoolCapacity = 16384;

// Dumps of one command buffer whose running totals WriteForReaders() computes on the GPU.
constexpr uint32_t MaxGpuDumps = 65536;

constexpr uint64_t PageBits  = 12;
constexpr uint64_t ChunkBits = 6;

[[nodiscard]] size_t PageSlot(uint64_t page, size_t count) {
	return static_cast<size_t>((page ^ (page >> 12u)) % count);
}

// The 64-byte chunks of `page` that [address, address + size) covers.
[[nodiscard]] uint64_t PageChunks(uint64_t page, uint64_t address, uint64_t size) {
	const auto page_begin = page << PageBits;
	const auto begin      = std::max(address, page_begin);
	const auto end        = std::min(address + size, page_begin + (uint64_t {1} << PageBits));
	if (begin >= end) {
		return 0;
	}
	const auto first = (begin - page_begin) >> ChunkBits;
	const auto count = ((end - 1u - page_begin) >> ChunkBits) - first + 1u;
	return (count >= 64u ? ~uint64_t {0} : (uint64_t {1} << count) - 1u) << first;
}

// The layouts of occlusion_counters.comp.
struct CounterDump {
	uint32_t first_slot = 0;
	uint32_t slot_count = 0;
	uint32_t uncounted  = 0;
	uint32_t pad        = 0;
};
static_assert(sizeof(CounterDump) == 16);

struct CounterWrite {
	uint64_t address  = 0;
	uint64_t known    = 0;
	uint32_t dump     = 0;
	uint32_t db_first = 0;
	uint32_t db_end   = 0;
	uint32_t pad      = 0;
};
static_assert(sizeof(CounterWrite) == 32);

struct CounterPush {
	uint64_t dumps       = 0;
	uint64_t writes      = 0;
	uint64_t results     = 0;
	uint64_t totals      = 0;
	uint64_t base        = 0;
	uint32_t dump_count  = 0;
	uint32_t write_count = 0;
	uint32_t capacity    = 0;
	uint32_t pad         = 0;
};
static_assert(sizeof(CounterPush) == 56);

} // namespace

OcclusionQueries::OcclusionQueries(RenderContext& context)
    : m_context(context), m_device(context.GetGraphics().device), m_segments(PoolCapacity) {
	const auto& graphics = context.GetGraphics();
	if (!graphics.occlusion_queries_enabled) {
		Log::WriteToConsoleAndLog("Occlusion queries: off (--no-occlusion-queries or no Vulkan "
		                          "hostQueryReset); they read always visible\n");
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
	if (graphics.occlusion_query_precise_enabled) {
		m_control = vk::QueryControlFlagBits::ePrecise;
	}
	m_results.resize(PoolCapacity);
	m_pending_pages.resize(PendingPageCount);
}

OcclusionQueries::~OcclusionQueries() {
	if (m_counter_pipeline) {
		m_device.destroyPipeline(m_counter_pipeline);
	}
	if (m_counter_layout) {
		m_device.destroyPipelineLayout(m_counter_layout);
	}
	if (m_pool != nullptr) {
		m_device.destroyQueryPool(m_pool, nullptr);
	}
}

void OcclusionQueries::BeginQuery(const CommandRecorder& recorder) {
	EXIT_IF(m_query_active || !m_rendering);
	{
		std::lock_guard lock(m_mutex);
		if (!m_segments.AllocateSlot(m_active_slot)) {
			return;
		}
	}
	recorder.beginQuery(m_pool, m_active_slot, m_control);
	m_query_active = true;
}

void OcclusionQueries::EndQuery(const CommandRecorder& recorder) {
	EXIT_IF(!m_query_active);
	recorder.endQuery(m_pool, m_active_slot);
	m_query_active = false;
}

void OcclusionQueries::OnBeginRendering(const CommandRecorder& recorder) {
	m_rendering = true;
	if (m_pool != nullptr && m_segments.Counting()) {
		BeginQuery(recorder);
	}
}

void OcclusionQueries::OnEndRendering(const CommandRecorder& recorder) {
	if (m_query_active) {
		EndQuery(recorder);
	}
	m_rendering = false;
}

void OcclusionQueries::OnFrame() {
	const auto open_before = m_segments.OpenQueries();
	m_segments.OnFrame();
	if (m_segments.OpenQueries() < open_before) {
		Common::HackNotice::Report("occlusion query begin without end dropped after 8 frames",
		                           "OcclusionQueries::OnFrame", 0, 0);
	}
	// The execution thread writes the end-of-pipe labels and flips that pace the guest before
	// the GPU runs the work, so the GPU can fall more than a frame behind them. GTA V (PPSA04263)
	// takes the counters of a frame a few frames later, and on the console they are there by
	// then: when a counter of its water was published a flip later than usual, the game drew a
	// frame of the water without reflections and waves. Wait for the GPU to finish the
	// dumps of earlier frames before this frame's flip (it usually already has).
	uint64_t tick = 0;
	{
		std::lock_guard lock(m_mutex);
		for (const auto& pending: m_pending) {
			if (pending.frame < m_frame) {
				tick = std::max(tick, pending.tick);
			}
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
	auto&           master = m_context.GetCommandScheduler().GetMasterSemaphore();
	std::lock_guard lock(m_mutex);
	size_t          count = 0;
	while (count < m_pending.size()) {
		if (!master.IsFree(m_pending[count].tick)) {
			if (!refresh) {
				break;
			}
			// Draws do not wait on dumps: the GPU's progress is only queried here.
			master.Refresh();
			refresh = false;
			continue;
		}
		count++;
	}
	if (count != 0) {
		PublishFront(count);
	}
	if (!m_holds.empty()) {
		ReleaseSlots(m_published_slot);
	}
}

void OcclusionQueries::OnUnmap(uint64_t address, uint64_t size) {
	std::lock_guard lock(m_mutex);
	for (auto& pending: m_pending) {
		if (pending.dump.address < address + size &&
		    address < pending.dump.address + OcclusionCounters::DumpSize) {
			pending.write = false;
		}
	}
}

void OcclusionQueries::Dump(CommandScheduler& scheduler, uint64_t address) {
	EXIT_IF(m_pool == nullptr);
	// The counters reach memory in a completion, which cannot read memory back: bytes earlier GPU
	// work wrote under them are read back now, as for a label (see ExecuteReleaseMem()). The
	// readback may end the render pass, and with it the active query, before the code below.
	if (GuestRange {address, OcclusionCounters::DumpSize}.Valid() &&
	    m_context.GetBufferCache().HasGpuDirtyBytes(address, OcclusionCounters::DumpSize)) {
		m_context.PrepareGpuWrite(reinterpret_cast<const void*>(address), OcclusionCounters::DumpSize);
	}
	auto&      buffer   = scheduler.Current();
	const auto recorder = buffer.Handle();
	if (m_query_active) {
		EndQuery(recorder);
	}
	const auto tick = scheduler.CurrentTick();
	const auto seq  = ++m_dump_seq;
	{
		std::lock_guard lock(m_mutex);
		m_pending.push_back(
		    {.dump = m_segments.OnDump(address), .tick = tick, .frame = m_frame, .seq = seq});
	}
	NotePendingPages(address, seq);
	// The GPU writes the counters when it gets past the dump. Queued now, the completion runs
	// before those of any later operation: a guest that learns the GPU finished this work (a
	// suspend point, a flip) finds them in memory, as on the console. One completion per command
	// buffer publishes all of its dumps (ASTRO BOT dumps ~10,000 times a frame, one per object):
	// a completion per dump made the execution thread queue and track 10,000 operations a frame.
	// An unmap of counters a later dump of the command buffer wrote does not wait for it: the
	// unmap drops their publication instead (OnUnmap()).
	if (tick != m_completion_tick) {
		m_completion_tick = tick;
		scheduler.DeferPriorityOperation([this] { PublishCompleted(true); }, address,
		                                 OcclusionCounters::DumpSize);
	}
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

bool OcclusionQueries::ReadResults(uint64_t first_slot, uint64_t end_slot) {
	const auto capacity = m_segments.Capacity();
	size_t     offset   = 0;
	while (first_slot < end_slot) {
		const auto slot  = static_cast<uint32_t>(first_slot % capacity);
		const auto count = static_cast<uint32_t>(
		    std::min<uint64_t>(end_slot - first_slot, uint64_t {capacity} - slot));
		// The command buffers that ended these queries completed: nothing waits here.
		const auto result = m_device.getQueryPoolResults(
		    m_pool, slot, count, count * sizeof(uint64_t), m_results.data() + offset,
		    sizeof(uint64_t), vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
		if (result != vk::Result::eSuccess) {
			Log::WriteToConsoleAndLog(fmt::format(
			    "Occlusion queries: vkGetQueryPoolResults failed ({}); the queries read visible\n",
			    vk::to_string(result)));
			return false;
		}
		offset += count;
		first_slot += count;
	}
	return true;
}

void OcclusionQueries::WriteCounters(uint64_t address) {
	// DB 0 last: once its ready bit is visible, the other DBs are too. Dump() read back the
	// GPU-written bytes under the counters; later GPU work may own the pages again. One lookup of
	// the backing store writes the 16 of them.
	std::array<uint64_t, OcclusionCounters::DbCount> values {};
	for (uint32_t db = 0; db < OcclusionCounters::DbCount; db++) {
		values[db] = OcclusionCounters::DbValue(db, m_total);
	}
	m_context.WriteQwordsFromCompletion(reinterpret_cast<void*>(address), values.data(),
	                                    OcclusionCounters::DbCount, OcclusionCounters::DbStride);
}

void OcclusionQueries::PublishFront(size_t count) {
	// The segments of consecutive dumps are consecutive slots: their results come in one call and
	// their slots are reset in one. ASTRO BOT (PPSA21564) publishes ~13,000 dumps a frame.
	const auto first_slot = m_pending.front().dump.first_slot;
	const auto end_slot   = m_pending[count - 1u].dump.end_slot;
	const bool counted    = ReadResults(first_slot, end_slot);
	for (size_t i = 0; i < count; i++) {
		const auto& pending = m_pending[i];
		const auto& dump    = pending.dump;
		if (!counted && dump.end_slot > dump.first_slot) {
			m_total += OcclusionCounters::UncountedSamples;
		} else {
			for (auto slot = dump.first_slot; slot < dump.end_slot; slot++) {
				m_total += m_results[slot - first_slot];
			}
		}
		if (dump.uncounted) {
			Common::HackNotice::Report("occlusion segment without a free host query slot reads visible",
			                           "OcclusionQueries::Publish", 0, 0);
			m_total += OcclusionCounters::UncountedSamples;
		}
		if (pending.write) {
			WriteCounters(dump.address);
		}
	}
	m_published_seq.store(m_pending[count - 1u].seq, std::memory_order_release);
	ReleaseSlots(end_slot);
	m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<ptrdiff_t>(count));
}

void OcclusionQueries::ReleaseSlots(uint64_t end_slot) {
	m_published_slot = std::max(m_published_slot, end_slot);
	auto        end    = m_published_slot;
	const auto& master = m_context.GetCommandScheduler().GetMasterSemaphore();
	while (!m_holds.empty() && master.IsFree(m_holds.front().tick)) {
		m_holds.pop_front();
	}
	if (!m_holds.empty()) {
		end = std::min(end, m_holds.front().slot);
	}
	if (end > m_reset_slot) {
		ResetSlots(m_reset_slot, end);
		m_segments.Release(end);
		m_reset_slot = end;
	}
}

void OcclusionQueries::NotePendingPages(uint64_t address, uint64_t seq) {
	const auto published = m_published_seq.load(std::memory_order_acquire);
	const auto end       = address + OcclusionCounters::DumpSize;
	if (published + 1u >= seq) {
		// The only unpublished dump.
		m_pending_low  = address;
		m_pending_high = end;
	} else {
		m_pending_low  = std::min(m_pending_low, address);
		m_pending_high = std::max(m_pending_high, end);
	}
	for (auto page = address >> PageBits; page <= (end - 1u) >> PageBits; page++) {
		auto& entry = m_pending_pages[PageSlot(page, m_pending_pages.size())];
		if (entry.page != page) {
			if (entry.last_seq > published) {
				m_evicted_seq = std::max(m_evicted_seq, entry.last_seq);
			}
			entry = {.page = page};
		}
		if (entry.last_seq <= published) {
			entry.chunks = 0;
		}
		entry.last_seq = seq;
		entry.chunks |= PageChunks(page, address, OcclusionCounters::DumpSize);
	}
}

bool OcclusionQueries::MayReadPendingDump(uint64_t address, uint64_t size) const {
	const auto published = m_published_seq.load(std::memory_order_acquire);
	if (m_dump_seq <= published || size == 0 || address >= m_pending_high ||
	    address + size <= m_pending_low) {
		return false;
	}
	if (m_evicted_seq > published) {
		return true;
	}
	const auto first = address >> PageBits;
	const auto last  = (address + size - 1u) >> PageBits;
	if (last - first >= m_pending_pages.size()) {
		return true;
	}
	for (auto page = first; page <= last; page++) {
		const auto& entry = m_pending_pages[PageSlot(page, m_pending_pages.size())];
		if (entry.page == page && entry.last_seq > published &&
		    (entry.chunks & PageChunks(page, address, size)) != 0) {
			return true;
		}
	}
	return false;
}

void OcclusionQueries::WaitAndPublish(uint64_t tick) {
	auto& scheduler = m_context.GetCommandScheduler();
	if (!scheduler.GetMasterSemaphore().IsFree(tick)) {
		scheduler.Wait(tick);
	}
	PublishCompleted(true);
}

bool OcclusionQueries::PrepareRead(uint64_t address, uint64_t size) {
	if (m_pool == nullptr || !MayReadPendingDump(address, size)) {
		return false;
	}
	const auto first_dump = m_read_dumps.size();
	{
		std::lock_guard lock(m_mutex);
		for (const auto& pending: m_pending) {
			const auto dump_address = pending.dump.address;
			if (pending.write && dump_address < address + size &&
			    address < dump_address + OcclusionCounters::DumpSize) {
				m_read_dumps.push_back({dump_address, pending.seq});
			}
		}
	}
	if (m_read_dumps.size() == first_dump) {
		return false;
	}
	m_read_targets.push_back({.address    = address,
	                          .size       = size,
	                          .first_dump = first_dump,
	                          .end_dump   = m_read_dumps.size()});
	return true;
}

void OcclusionQueries::NoteReadTarget(uint64_t address, uint64_t size, const Buffer& buffer,
                                      uint64_t offset) {
	EXIT_IF(m_read_targets.empty());
	auto& target = m_read_targets.back();
	EXIT_IF(target.address != address || target.size != size || target.device_address != 0);
	const auto device_address = buffer.BufferDeviceAddress();
	// The shader stores dwords: the copy keeps the guest's dword alignment, which a stream copy of
	// an unaligned range does not.
	if (device_address == 0 || ((device_address + offset - address) & 3u) != 0) {
		Common::HackNotice::Report("occlusion counters read through an unaligned copy keep its bytes",
		                           "OcclusionQueries::NoteReadTarget", 0, address);
		m_read_dumps.resize(target.first_dump);
		m_read_targets.pop_back();
		return;
	}
	target.device_address = device_address + offset;
}

void OcclusionQueries::WaitForCounters(uint64_t address, uint64_t size) {
	if (m_pool == nullptr || !MayReadPendingDump(address, size)) {
		return;
	}
	uint64_t tick = 0;
	{
		std::lock_guard lock(m_mutex);
		for (const auto& pending: m_pending) {
			if (pending.dump.address < address + size &&
			    address < pending.dump.address + OcclusionCounters::DumpSize) {
				tick = std::max(tick, pending.tick);
			}
		}
	}
	if (tick != 0) {
		WaitAndPublish(tick);
	}
}

void OcclusionQueries::CreateCounterPipeline() {
	auto&       graphics = m_context.GetGraphics();
	auto&       scheduler = m_context.GetCommandScheduler();
	const auto  flags    = AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress;
	m_slot_results = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::DeviceLocal, 0, flags,
	                                          uint64_t {PoolCapacity} * sizeof(uint64_t));
	m_totals       = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::DeviceLocal, 0, flags,
	                                          uint64_t {MaxGpuDumps} * sizeof(uint64_t));
	const vk::PushConstantRange  push {vk::ShaderStageFlagBits::eCompute, 0, sizeof(CounterPush)};
	vk::PipelineLayoutCreateInfo layout_info {};
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &push;
	RequireVulkanSuccess(m_device.createPipelineLayout(&layout_info, nullptr, &m_counter_layout),
	                     "create occlusion counter pipeline layout");
	const auto                        module = CompileSPV(OCCLUSION_COUNTERS_SPV, m_device);
	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage  = vk::ShaderStageFlagBits::eCompute;
	stage.module = module;
	stage.pName  = "main";
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage  = stage;
	pipeline_info.layout = m_counter_layout;
	const auto result =
	    m_device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr, &m_counter_pipeline);
	m_device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create occlusion counter pipeline");
	SetVulkanObjectNameF(m_device, m_counter_pipeline, "Kyty.OcclusionCounters");
}

void OcclusionQueries::WriteForReaders() {
	if (m_read_targets.empty()) {
		return;
	}
	auto&    scheduler = m_context.GetCommandScheduler();
	uint64_t last_seq  = 0;
	for (const auto& target: m_read_targets) {
		if (target.device_address != 0) {
			for (size_t i = target.first_dump; i < target.end_dump; i++) {
				last_seq = std::max(last_seq, m_read_dumps[i].seq);
			}
		}
	}
	// The running total after each unpublished dump up to the last one read: the total of the
	// published ones plus the results of the host queries of each segment, which the GPU copies
	// once it has them. Their slots are reset only after it did (ReleaseSlots()).
	std::vector<CounterDump> dumps;
	std::vector<CounterWrite> writes;
	uint64_t                  base       = 0;
	uint64_t                  first_seq  = 0;
	uint64_t                  first_slot = 0;
	uint64_t                  end_slot   = 0;
	uint64_t                  wait_tick  = 0;
	{
		std::lock_guard lock(m_mutex);
		if (!m_pending.empty() && m_pending.front().seq <= last_seq) {
			const auto count = static_cast<size_t>(last_seq - m_pending.front().seq + 1u);
			if (count > MaxGpuDumps) {
				wait_tick = m_pending[count - 1u].tick;
			} else {
				base       = m_total;
				first_seq  = m_pending.front().seq;
				first_slot = m_pending.front().dump.first_slot;
				end_slot   = m_pending[count - 1u].dump.end_slot;
				dumps.reserve(count);
				for (size_t i = 0; i < count; i++) {
					const auto& dump = m_pending[i].dump;
					dumps.push_back(
					    {.first_slot = static_cast<uint32_t>(dump.first_slot % PoolCapacity),
					     .slot_count = static_cast<uint32_t>(dump.end_slot - dump.first_slot),
					     .uncounted  = dump.uncounted ? 1u : 0u});
				}
				// The first slot of the holds only grows: the front one bounds the release.
				const auto tick = scheduler.CurrentTick();
				if (m_holds.empty() || m_holds.back().tick != tick) {
					m_holds.push_back({.tick = tick, .slot = first_slot});
				}
			}
		}
	}
	if (wait_tick != 0) {
		// More dumps than the GPU side keeps totals for: their counters come from memory.
		WaitAndPublish(wait_tick);
	}
	for (const auto& target: m_read_targets) {
		if (target.device_address == 0) {
			continue;
		}
		const auto target_end = target.address + target.size;
		for (size_t i = target.first_dump; i < target.end_dump; i++) {
			const auto& read = m_read_dumps[i];
			// The DBs whose counter lies inside the range.
			const uint64_t db_first =
			    read.address >= target.address
			        ? 0
			        : (target.address - read.address + OcclusionCounters::DbStride - 1u) /
			              OcclusionCounters::DbStride;
			const uint64_t db_end =
			    target_end < read.address + sizeof(uint64_t)
			        ? 0
			        : std::min<uint64_t>(OcclusionCounters::DbCount,
			                             (target_end - read.address - sizeof(uint64_t)) /
			                                     OcclusionCounters::DbStride +
			                                 1u);
			if (db_first >= db_end) {
				continue;
			}
			CounterWrite write {.address  = target.device_address + (read.address - target.address),
			                    .db_first = static_cast<uint32_t>(db_first),
			                    .db_end   = static_cast<uint32_t>(db_end)};
			if (first_seq != 0 && read.seq >= first_seq && read.seq - first_seq < dumps.size()) {
				write.dump = static_cast<uint32_t>(read.seq - first_seq);
			} else {
				// Published since PrepareRead(): DB 0 in memory holds its total.
				uint64_t value = 0;
				if (!LibKernel::Memory::TryReadBacking(read.address, &value, sizeof(value)) ||
				    (value & OcclusionCounters::ReadyBit) == 0) {
					Common::HackNotice::Report(
					    "occlusion counters of a copy keep its bytes while DB 0 is unread or not ready",
					    "OcclusionQueries::WriteForReaders", 0, read.address);
					continue;
				}
				write.known = value & OcclusionCounters::ValueMask;
				write.dump  = UINT32_MAX;
			}
			writes.push_back(write);
		}
	}
	DropReadTargets();
	if (writes.empty()) {
		return;
	}
	if (!m_counter_pipeline) {
		CreateCounterPipeline();
	}
	auto& command = scheduler.Current();
	command.EndRendering();
	const auto native = command.Handle();
	const bool gpu_totals = std::ranges::any_of(writes, [](const CounterWrite& write) {
		return write.dump != UINT32_MAX;
	});
	if (gpu_totals && end_slot > first_slot) {
		// The previous dispatch reads the results this copy replaces.
		vk::MemoryBarrier before {};
		before.srcAccessMask = vk::AccessFlagBits::eShaderRead;
		before.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
		native.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
		                       vk::PipelineStageFlagBits::eTransfer, {}, 1, &before, 0, nullptr, 0,
		                       nullptr);
		// The queries ended earlier in the stream; the copy waits for their results on the GPU.
		const auto slot  = static_cast<uint32_t>(first_slot % PoolCapacity);
		const auto count = static_cast<uint32_t>(end_slot - first_slot);
		const auto head  = std::min(count, PoolCapacity - slot);
		const auto flags = vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait;
		native.copyQueryPoolResults(m_pool, slot, head, m_slot_results->Handle(),
		                            uint64_t {slot} * sizeof(uint64_t), sizeof(uint64_t), flags);
		if (count > head) {
			native.copyQueryPoolResults(m_pool, 0, count - head, m_slot_results->Handle(), 0,
			                            sizeof(uint64_t), flags);
		}
	}
	auto&      cache  = m_context.GetBufferCache();
	auto&      stream = cache.GetUtilityBuffer(MemoryUsage::Stream);
	const auto dumps_offset =
	    dumps.empty() ? 0 : cache.StreamUpload(dumps.data(), dumps.size() * sizeof(CounterDump), 16);
	const auto writes_offset =
	    cache.StreamUpload(writes.data(), writes.size() * sizeof(CounterWrite), 16);
	const CounterPush push {.dumps       = stream.BufferDeviceAddress() + dumps_offset,
	                        .writes      = stream.BufferDeviceAddress() + writes_offset,
	                        .results     = m_slot_results->BufferDeviceAddress(),
	                        .totals      = m_totals->BufferDeviceAddress(),
	                        .base        = base,
	                        .dump_count  = gpu_totals ? static_cast<uint32_t>(dumps.size()) : 0u,
	                        .write_count = static_cast<uint32_t>(writes.size()),
	                        .capacity    = PoolCapacity};
	// The copies were just uploaded or written by earlier work; the queries' results were copied.
	vk::MemoryBarrier before {};
	before.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eComputeShader, {}, 1, &before, 0, nullptr, 0,
	                       nullptr);
	native.bindPipeline(vk::PipelineBindPoint::eCompute, m_counter_pipeline);
	native.pushConstants(m_counter_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
	                     &push);
	native.dispatch(1, 1, 1);
	vk::MemoryBarrier after {};
	after.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                       vk::PipelineStageFlagBits::eAllCommands, {}, 1, &after, 0, nullptr, 0,
	                       nullptr);
}

} // namespace Libs::Graphics
