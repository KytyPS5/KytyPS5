#include "graphics/host_gpu/renderer/commandScheduler.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace Libs::Graphics {

static thread_local CommandScheduler* g_deferred_callback_scheduler = nullptr;

namespace {

struct BreadcrumbEntry {
	const char* kind   = nullptr;
	uint64_t    hash_a = 0;
	uint64_t    hash_b = 0;
	uint64_t    info   = 0;
};

struct BreadcrumbState {
	std::mutex                              mutex;
	bool                                    initialized = false;
	bool                                    available   = false;
	VkBuffer                                buffer      = VK_NULL_HANDLE;
	VmaAllocation                           allocation  = nullptr;
	volatile uint32_t*                      mapped      = nullptr;
	uint32_t                                next_id     = 1;
	std::array<BreadcrumbEntry, 1u << 16u>  entries {};
};

BreadcrumbState g_breadcrumbs;

bool BreadcrumbInit(GraphicContext& graphics) {
	if (g_breadcrumbs.initialized) {
		return g_breadcrumbs.available;
	}
	g_breadcrumbs.initialized = true;
	const char* enabled       = std::getenv("KYTY_DBG_BREADCRUMBS");
	if (enabled == nullptr || enabled[0] == '\0' || enabled[0] == '0') {
		return false;
	}
	if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdWriteBufferMarkerAMD == nullptr) {
		std::printf("NHL26CRUMB: VK_AMD_buffer_marker not available\n");
		return false;
	}
	VkBufferCreateInfo buffer_info {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	buffer_info.size  = 64;
	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VmaAllocationCreateInfo allocation_info {};
	allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
	                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
	allocation_info.usage         = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
	allocation_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VmaAllocationInfo result_info {};
	if (vmaCreateBuffer(graphics.allocator, &buffer_info, &allocation_info,
	                    &g_breadcrumbs.buffer, &g_breadcrumbs.allocation, &result_info) !=
	        VK_SUCCESS ||
	    result_info.pMappedData == nullptr) {
		std::printf("NHL26CRUMB: breadcrumb buffer allocation failed\n");
		return false;
	}
	g_breadcrumbs.mapped    = static_cast<volatile uint32_t*>(result_info.pMappedData);
	g_breadcrumbs.mapped[0] = 0;
	g_breadcrumbs.mapped[1] = 0;
	g_breadcrumbs.available = true;
	return true;
}

void DumpBreadcrumbs() {
	std::lock_guard lock(g_breadcrumbs.mutex);
	if (!g_breadcrumbs.available) {
		return;
	}
	const uint32_t started  = g_breadcrumbs.mapped[0];
	const uint32_t finished = g_breadcrumbs.mapped[1];
	const uint32_t recorded = g_breadcrumbs.next_id - 1u;
	std::printf("NHL26CRUMB: last started=%u last finished=%u last recorded=%u\n", started,
	            finished, recorded);
	const uint32_t first = finished > 4u ? finished - 4u : 1u;
	const uint32_t last  = std::min(recorded, std::max(started, finished) + 4u);
	for (uint32_t id = first; id <= last && id != 0; id++) {
		const auto& e = g_breadcrumbs.entries[id & (g_breadcrumbs.entries.size() - 1u)];
		const char* tag = id <= finished ? "done   " : (id <= started ? "RUNNING" : "pending");
		std::printf("NHL26CRUMB:   [%s] id=%u %s a=0x%016llx b=0x%016llx info=0x%llx\n", tag, id,
		            e.kind != nullptr ? e.kind : "?", static_cast<unsigned long long>(e.hash_a),
		            static_cast<unsigned long long>(e.hash_b),
		            static_cast<unsigned long long>(e.info));
	}
	std::fflush(stdout);
}

// NHL26 debugging: after a device loss, print what VK_EXT_device_fault reports (faulting GPU
// addresses and vendor fault codes). No-op when the extension is not enabled.
void DumpDeviceFault(vk::Device device) {
	DumpBreadcrumbs();
	const auto get_fault_info = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(
	    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(device, "vkGetDeviceFaultInfoEXT"));
	if (get_fault_info == nullptr) {
		std::printf("NHL26FAULT: VK_EXT_device_fault not available\n");
		return;
	}
	VkDeviceFaultCountsEXT counts {VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
	if (get_fault_info(device, &counts, nullptr) < 0) {
		std::printf("NHL26FAULT: vkGetDeviceFaultInfoEXT (counts) failed\n");
		return;
	}
	std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
	std::vector<VkDeviceFaultVendorInfoEXT>  vendors(counts.vendorInfoCount);
	VkDeviceFaultInfoEXT info {VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
	info.pAddressInfos     = addresses.data();
	info.pVendorInfos      = vendors.data();
	counts.vendorBinarySize = 0;
	const auto result      = get_fault_info(device, &counts, &info);
	std::printf("NHL26FAULT: result=%d description=\"%s\" addresses=%u vendor_infos=%u\n",
	            static_cast<int>(result), info.description, counts.addressInfoCount,
	            counts.vendorInfoCount);
	static const char* const kTypes[] = {"none",       "read-invalid",  "write-invalid",
	                                     "exec-invalid", "ip-unknown",  "ip-invalid",
	                                     "ip-fault"};
	for (const auto& a: addresses) {
		const auto type = static_cast<uint32_t>(a.addressType);
		std::printf("NHL26FAULT:   address type=%s addr=0x%016llx precision=0x%llx\n",
		            type < std::size(kTypes) ? kTypes[type] : "?",
		            static_cast<unsigned long long>(a.reportedAddress),
		            static_cast<unsigned long long>(a.addressPrecision));
	}
	for (const auto& v: vendors) {
		std::printf("NHL26FAULT:   vendor \"%s\" code=0x%llx data=0x%llx\n", v.description,
		            static_cast<unsigned long long>(v.vendorFaultCode),
		            static_cast<unsigned long long>(v.vendorFaultData));
	}
	std::fflush(stdout);
}

// NHL27 profiling (KYTY_PERF_GPU_TIMING=1): per-draw / per-dispatch GPU timing. Timestamps are
// written around each command into a ring of (begin, end) query pairs; completed pairs are read
// back without waiting after every Submit and accumulated per (kind, hash_a, hash_b). Every few
// seconds the top keys by total GPU time are printed. The begin/end timestamps are
// top-of-pipe/bottom-of-pipe, so back-to-back commands that overlap on the GPU are each charged for
// the overlap; the sum can exceed the real GPU-busy time.
constexpr uint32_t GPU_TIMING_PAIRS      = 16384;
constexpr uint32_t GPU_TIMING_READ_CHUNK = 256;
constexpr double   GPU_TIMING_WINDOW_SEC = 5.0;
constexpr size_t   GPU_TIMING_TOP        = 20;

struct GpuTimingSlot {
	const char* kind   = nullptr;
	uint64_t    hash_a = 0;
	uint64_t    hash_b = 0;
	uint64_t    info   = 0;
};

struct GpuTimingAccum {
	double   total_ms = 0.0;
	uint64_t count    = 0;
	uint64_t info     = 0;
};

using GpuTimingKey = std::tuple<std::string, uint64_t, uint64_t>;

struct GpuTimingState {
	std::mutex                            mutex;
	bool                                  initialized = false;
	bool                                  available   = false;
	vk::Device                            device      = nullptr;
	vk::QueryPool                         pool        = nullptr;
	double                                period_ns   = 0.0;
	uint64_t                              valid_mask  = ~0ull;
	uint64_t                              head        = 0;
	uint64_t                              tail        = 0;
	uint64_t                              dropped     = 0;
	uint64_t                              samples     = 0;
	double                                total_ms    = 0.0;
	std::chrono::steady_clock::time_point window_start;
	std::vector<GpuTimingSlot>            slots;
	std::vector<uint64_t>                 results;
	std::map<GpuTimingKey, GpuTimingAccum> accum;
};

GpuTimingState g_timing;

bool GpuTimingInit(GraphicContext& graphics) {
	if (g_timing.initialized) {
		return g_timing.available;
	}
	g_timing.initialized = true;
	const char* enabled  = std::getenv("KYTY_PERF_GPU_TIMING");
	if (enabled == nullptr || enabled[0] == '\0' || enabled[0] == '0') {
		return false;
	}
	if (!graphics.host_query_reset_enabled ||
	    VULKAN_HPP_DEFAULT_DISPATCHER.vkResetQueryPool == nullptr) {
		std::printf("NHL27GPU: hostQueryReset not available, GPU timing disabled\n");
		return false;
	}
	const auto families = graphics.physical_device.getQueueFamilyProperties();
	if (graphics.queue_family >= families.size()) {
		return false;
	}
	const uint32_t valid_bits = families[graphics.queue_family].timestampValidBits;
	if (valid_bits == 0 || graphics.physical_device_properties.limits.timestampPeriod <= 0.0f) {
		return false;
	}
	g_timing.valid_mask = valid_bits >= 64 ? ~0ull : ((1ull << valid_bits) - 1ull);
	g_timing.period_ns  = graphics.physical_device_properties.limits.timestampPeriod;

	vk::QueryPoolCreateInfo create {};
	create.queryType  = vk::QueryType::eTimestamp;
	create.queryCount = GPU_TIMING_PAIRS * 2u;
	if (graphics.device.createQueryPool(&create, nullptr, &g_timing.pool) != vk::Result::eSuccess) {
		std::printf("NHL27GPU: query pool creation failed\n");
		return false;
	}
	g_timing.device = graphics.device;
	g_timing.slots.resize(GPU_TIMING_PAIRS);
	g_timing.results.resize(static_cast<size_t>(GPU_TIMING_READ_CHUNK) * 4u);
	g_timing.window_start = std::chrono::steady_clock::now();
	g_timing.available    = true;
	std::printf("NHL27GPU: GPU timing enabled (%u slots, %.3f ns/tick, %u valid bits)\n",
	            GPU_TIMING_PAIRS, g_timing.period_ns, valid_bits);
	std::fflush(stdout);
	return true;
}

void GpuTimingReport() {
	const auto   now    = std::chrono::steady_clock::now();
	const double window = std::chrono::duration<double>(now - g_timing.window_start).count();
	std::vector<std::pair<const GpuTimingKey*, const GpuTimingAccum*>> sorted;
	for (const auto& [key, acc]: g_timing.accum) {
		sorted.emplace_back(&key, &acc);
	}
	std::sort(sorted.begin(), sorted.end(),
	          [](const auto& l, const auto& r) { return l.second->total_ms > r.second->total_ms; });
	std::printf("NHL27GPU: ---- window=%.2f s measured=%.1f ms (%.1f ms/s) samples=%llu "
	            "dropped=%llu keys=%zu\n",
	            window, g_timing.total_ms, window > 0.0 ? g_timing.total_ms / window : 0.0,
	            static_cast<unsigned long long>(g_timing.samples),
	            static_cast<unsigned long long>(g_timing.dropped), sorted.size());
	for (size_t i = 0; i < sorted.size() && i < GPU_TIMING_TOP; i++) {
		const GpuTimingKey&   key = *sorted[i].first;
		const GpuTimingAccum& acc = *sorted[i].second;
		std::printf("NHL27GPU: %.2f ms %.1f%% n=%llu avg=%.1f us %s a=0x%llx b=0x%llx info=0x%llx\n",
		            acc.total_ms,
		            g_timing.total_ms > 0.0 ? 100.0 * acc.total_ms / g_timing.total_ms : 0.0,
		            static_cast<unsigned long long>(acc.count),
		            acc.count != 0 ? acc.total_ms * 1000.0 / static_cast<double>(acc.count) : 0.0,
		            std::get<0>(key).c_str(), static_cast<unsigned long long>(std::get<1>(key)),
		            static_cast<unsigned long long>(std::get<2>(key)),
		            static_cast<unsigned long long>(acc.info));
	}
	std::fflush(stdout);
	g_timing.accum.clear();
	g_timing.total_ms     = 0.0;
	g_timing.samples      = 0;
	g_timing.dropped      = 0;
	g_timing.window_start = now;
}

// Reads back completed (begin, end) pairs in recording order, without waiting. Caller holds the mutex.
void GpuTimingPollLocked() {
	while (g_timing.tail < g_timing.head) {
		const uint32_t first = static_cast<uint32_t>(g_timing.tail % GPU_TIMING_PAIRS);
		const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(
		    {g_timing.head - g_timing.tail, GPU_TIMING_READ_CHUNK, GPU_TIMING_PAIRS - first}));
		// 64-bit results, each followed by its availability value: 4 uint64 per pair.
		VULKAN_HPP_DEFAULT_DISPATCHER.vkGetQueryPoolResults(
		    static_cast<VkDevice>(g_timing.device), static_cast<VkQueryPool>(g_timing.pool),
		    first * 2u, count * 2u, g_timing.results.size() * sizeof(uint64_t),
		    g_timing.results.data(), 2u * sizeof(uint64_t),
		    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
		uint32_t done = 0;
		for (; done < count; done++) {
			const uint64_t* r = &g_timing.results[static_cast<size_t>(done) * 4u];
			if (r[1] == 0 || r[3] == 0) {
				break;
			}
			const auto&    slot = g_timing.slots[first + done];
			const uint64_t diff = (r[2] - r[0]) & g_timing.valid_mask;
			const double   ms   = static_cast<double>(diff) * g_timing.period_ns * 1e-6;
			auto&          acc  = g_timing.accum[GpuTimingKey {
                slot.kind != nullptr ? slot.kind : "?", slot.hash_a, slot.hash_b}];
			acc.total_ms += ms;
			acc.count++;
			acc.info = slot.info;
			g_timing.total_ms += ms;
			g_timing.samples++;
		}
		g_timing.tail += done;
		if (done < count) {
			break;
		}
	}
	const auto now = std::chrono::steady_clock::now();
	if (std::chrono::duration<double>(now - g_timing.window_start).count() >=
	    GPU_TIMING_WINDOW_SEC) {
		GpuTimingReport();
	}
}

// Returns the slot index + 1, or 0 when timing is off or the sample is dropped.
uint32_t GpuTimingBegin(GraphicContext& graphics, vk::CommandBuffer cmd, const char* kind,
                        uint64_t hash_a, uint64_t hash_b, uint64_t info) {
	if (g_timing.initialized && !g_timing.available) {
		return 0;
	}
	std::lock_guard lock(g_timing.mutex);
	if (!GpuTimingInit(graphics)) {
		return 0;
	}
	if (g_timing.head - g_timing.tail >= GPU_TIMING_PAIRS) {
		GpuTimingPollLocked();
		if (g_timing.head - g_timing.tail >= GPU_TIMING_PAIRS) {
			g_timing.dropped++; // Ring still full: drop the sample rather than stall.
			return 0;
		}
	}
	const uint32_t slot = static_cast<uint32_t>(g_timing.head % GPU_TIMING_PAIRS);
	g_timing.head++;
	g_timing.slots[slot] = {kind, hash_a, hash_b, info};
	// The previous use of this pair has been read back (tail passed it), so the host reset is safe.
	VULKAN_HPP_DEFAULT_DISPATCHER.vkResetQueryPool(static_cast<VkDevice>(g_timing.device),
	                                               static_cast<VkQueryPool>(g_timing.pool), slot * 2u,
	                                               2u);
	cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, g_timing.pool, slot * 2u);
	return slot + 1u;
}

void GpuTimingEnd(vk::CommandBuffer cmd, uint32_t slot_plus_one) {
	if (slot_plus_one == 0) {
		return;
	}
	cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, g_timing.pool,
	                   (slot_plus_one - 1u) * 2u + 1u);
}

} // namespace

uint64_t BreadcrumbBegin(GraphicContext& graphics, vk::CommandBuffer cmd, const char* kind,
                         uint64_t hash_a, uint64_t hash_b, uint64_t info) {
	// Low 32 bits: breadcrumb id (0 = none). High 32 bits: GPU timing slot + 1 (0 = none).
	uint32_t id = 0;
	if (!(g_breadcrumbs.initialized && !g_breadcrumbs.available)) {
		std::lock_guard lock(g_breadcrumbs.mutex);
		if (BreadcrumbInit(graphics)) {
			id = g_breadcrumbs.next_id++;
			g_breadcrumbs.entries[id & (g_breadcrumbs.entries.size() - 1u)] = {kind, hash_a,
			                                                                   hash_b, info};
			cmd.writeBufferMarkerAMD(vk::PipelineStageFlagBits::eTopOfPipe, g_breadcrumbs.buffer, 0,
			                         id);
		}
	}
	const uint32_t timing = GpuTimingBegin(graphics, cmd, kind, hash_a, hash_b, info);
	return (static_cast<uint64_t>(timing) << 32u) | id;
}

void ReportDeviceLost(GraphicContext& graphics) {
	DumpDeviceFault(graphics.device);
}

void BreadcrumbEnd(GraphicContext& graphics, vk::CommandBuffer cmd, uint64_t id) {
	(void)graphics;
	GpuTimingEnd(cmd, static_cast<uint32_t>(id >> 32u));
	const auto crumb = static_cast<uint32_t>(id);
	if (crumb == 0 || !g_breadcrumbs.available) {
		return;
	}
	cmd.writeBufferMarkerAMD(vk::PipelineStageFlagBits::eBottomOfPipe, g_breadcrumbs.buffer, 4,
	                         crumb);
}

void GpuTimingPoll() {
	if (!g_timing.available) {
		return;
	}
	std::lock_guard lock(g_timing.mutex);
	GpuTimingPollLocked();
}

namespace {

void ReportVulkanFatal(const char* what, vk::Result result, uint64_t tick, uint32_t debug_op,
                       uint64_t debug_submit, uint32_t arg0, uint32_t arg1, uint32_t arg2,
                       uint32_t arg3, uint64_t arg4) {
	LOGF("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	     what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	     debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::printf("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	            " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	            what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	            debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::fflush(stdout);
}

} // namespace

CommandScheduler::CommandPool::CommandPool(GraphicContext& graphics, MasterSemaphore& master)
    : m_graphics(graphics), m_master(master) {
	EXIT_IF(graphics.queue_family == static_cast<uint32_t>(-1));
	vk::CommandPoolCreateInfo create {};
	create.queueFamilyIndex = graphics.queue_family;
	create.flags            = vk::CommandPoolCreateFlagBits::eTransient |
	                          vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	const auto result       = graphics.device.createCommandPool(&create, nullptr, &m_pool);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_pool == nullptr);
}

CommandScheduler::CommandPool::~CommandPool() {
	m_graphics.device.destroyCommandPool(m_pool, nullptr);
}

size_t CommandScheduler::CommandPool::Grow() {
	const auto first = m_ticks.size();
	m_ticks.resize(first + GrowStep);
	m_buffers.resize(first + GrowStep);

	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = m_pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = static_cast<uint32_t>(GrowStep);
	EXIT_IF(m_graphics.device.allocateCommandBuffers(&allocate, m_buffers.data() + first) !=
	        vk::Result::eSuccess);
	return first;
}

vk::CommandBuffer CommandScheduler::CommandPool::Commit() {
	auto       gpu_tick = m_master.KnownGpuTick();
	const auto search   = [this, &gpu_tick](size_t begin, size_t end) -> std::optional<size_t> {
		for (size_t index = begin; index < end; ++index) {
			if (gpu_tick >= m_ticks[index]) {
				m_ticks[index] = m_master.CurrentTick();
				return index;
			}
		}
		return std::nullopt;
	};

	auto found = search(m_hint, m_ticks.size());
	if (!found) {
		m_master.Refresh();
		gpu_tick = m_master.KnownGpuTick();
		found    = search(m_hint, m_ticks.size());
	}
	if (!found) {
		found = search(0, m_hint);
	}
	if (!found) {
		found           = Grow();
		m_ticks[*found] = m_master.CurrentTick();
	}

	m_hint = (*found + 1) % m_ticks.size();
	return m_buffers[*found];
}

bool CommandScheduler::InDeferredOperation() noexcept {
	return g_deferred_callback_scheduler != nullptr;
}

CommandScheduler::CommandScheduler(RenderContext& context, GraphicContext& graphics)
    : m_master(graphics), m_context(context), m_graphics(graphics),
      m_command_pool(graphics, m_master), m_command(*this),
      m_priority_thread([this](std::stop_token stop) { PriorityOperationsThread(stop); }) {}

CommandScheduler::~CommandScheduler() {
	Shutdown();
}

void CommandScheduler::Shutdown() {
	{
		std::unique_lock lock(m_operation_mutex);
		if (m_operation_state == OperationState::Closed) {
			return;
		}
		if (g_deferred_callback_scheduler == this) {
			EXIT_IF(m_operation_state == OperationState::Open);
			// A priority callback cannot join its own runner, while a normal callback can be
			// executing inside the shutdown owner's final PopPendingOperations. The owning
			// thread will finish shutdown after this callback returns.
			return;
		}
		if (m_operation_state == OperationState::Draining) {
			m_operation_available.wait(
			    lock, [this] { return m_operation_state == OperationState::Closed; });
			return;
		}
		m_operation_state = OperationState::Draining;
	}
	if (!m_command.IsInvalid()) {
		Submit();
	}
	m_master.Wait(CurrentTick() - 1);
	PopPendingOperations();
	DrainPriorityOperations();
	m_priority_thread.request_stop();
	m_operation_available.notify_all();
	if (m_priority_thread.joinable()) {
		m_priority_thread.join();
	}
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(!m_pending_operations.empty() || !m_priority_operations.empty() ||
		        m_priority_active);
		m_operation_state = OperationState::Closed;
	}
	m_operation_available.notify_all();
}

void CommandScheduler::Begin(HW::Context& registers, HW::UserConfig& user_config,
                             HW::Shader& shaders) {
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(m_operation_state != OperationState::Open);
	}
	m_command.Bind(registers, user_config, shaders);

	if (m_command.IsInvalid()) {
		BeginNext();
	}
}

void CommandScheduler::BeginRendering(const RenderState& state) {
	Current().BeginRendering(state);
}

void CommandScheduler::EndRendering() {
	if (Active() && !m_command.IsInvalid()) {
		Current().EndRendering();
	}
}

void CommandScheduler::Flush() {
	SubmitInfo submit;
	Flush(submit);
}

void CommandScheduler::Flush(SubmitInfo& submit) {
	Submit(submit);
	BeginNext();
}

void CommandScheduler::FlushAndWait() {
	const auto tick = Submit();
	m_master.Wait(tick);
	BeginNext();
}

void CommandScheduler::Finish() {
	CheckActive();
	if (!m_command.IsInvalid()) {
		Submit();
	}
	m_master.Wait(CurrentTick() - 1);
	BeginNext();
	PopPendingOperations();
}

void CommandScheduler::Wait(uint64_t tick) {
	EXIT_IF(tick > CurrentTick());
	if (tick == CurrentTick()) {
		CheckActive();
		// A stream-buffer wrap can wait while a draw is being prepared through a reference to
		// Current(). The wrapper stays stable while its pooled Vulkan buffer is retired. Deferred
		// resources are released only at the next GPU operation boundary.
		const auto submitted_tick = Submit();
		EXIT_IF(submitted_tick != tick);
		m_master.Wait(tick);
		BeginNext();
	} else {
		m_master.Wait(tick);
	}
}

void CommandScheduler::PopPendingOperations() {
	m_master.Refresh();
	for (;;) {
		PendingOperation operation;
		{
			std::lock_guard lock(m_operation_mutex);
			if (m_pending_operations.empty() ||
			    !m_master.IsFree(m_pending_operations.front().tick)) {
				return;
			}
			operation = std::move(m_pending_operations.front());
			m_pending_operations.pop();
		}
		WaitPriorityOperations(operation.tick);
		RunOperation(std::move(operation.callback));
	}
}

void CommandScheduler::DeferOperation(Common::UniqueFunction<void>&& operation) {
	QueueOperation(std::move(operation), false);
}

void CommandScheduler::DeferPriorityOperation(Common::UniqueFunction<void>&& operation) {
	QueueOperation(std::move(operation), true);
}

void CommandScheduler::QueueOperation(Common::UniqueFunction<void>&& operation, bool priority) {
	CheckActive();
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		auto& queue = priority ? m_priority_operations : m_pending_operations;
		queue.push({std::move(operation), CurrentTick()});
		lock.unlock();
		if (priority) {
			m_operation_available.notify_one();
		}
		return;
	}
	if (g_deferred_callback_scheduler != this) {
		m_operation_available.wait(lock,
		                           [this] { return m_operation_state == OperationState::Closed; });
	}
	lock.unlock();
	operation();
}

bool CommandScheduler::HasPendingPriorityOperations() {
	std::lock_guard lock(m_operation_mutex);
	return !m_priority_operations.empty() || m_priority_active;
}

void CommandScheduler::PriorityOperationsThread(std::stop_token stop) {
	while (!stop.stop_requested()) {
		PendingOperation operation;
		{
			std::unique_lock lock(m_operation_mutex);
			m_operation_available.wait(lock, [this, &stop] {
				return stop.stop_requested() || !m_priority_operations.empty();
			});
			if (stop.stop_requested()) {
				return;
			}
			operation = std::move(m_priority_operations.front());
			m_priority_operations.pop();
			m_priority_active      = true;
			m_priority_active_tick = operation.tick;
		}
		m_master.Wait(operation.tick);
		if (!stop.stop_requested()) {
			RunOperation(std::move(operation.callback));
		}
		{
			std::lock_guard lock(m_operation_mutex);
			m_priority_active      = false;
			m_priority_active_tick = 0;
		}
		m_operation_available.notify_all();
	}
}

void CommandScheduler::DrainPriorityOperations() {
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(
	    lock, [this] { return m_priority_operations.empty() && !m_priority_active; });
}

void CommandScheduler::WaitPriorityOperations(uint64_t tick) {
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(lock, [this, tick] {
		const bool active_before_or_at = m_priority_active && m_priority_active_tick <= tick;
		const bool queued_before_or_at =
		    !m_priority_operations.empty() && m_priority_operations.front().tick <= tick;
		return !active_before_or_at && !queued_before_or_at;
	});
}

void CommandScheduler::RunOperation(Common::UniqueFunction<void>&& operation) {
	auto* previous                = g_deferred_callback_scheduler;
	g_deferred_callback_scheduler = this;
	operation();
	g_deferred_callback_scheduler = previous;
}

bool CommandScheduler::IsFree(uint64_t tick) {
	if (m_master.IsFree(tick)) {
		return true;
	}
	m_master.Refresh();
	return m_master.IsFree(tick);
}

void CommandScheduler::CheckActive() const {
	EXIT_IF(!Active());
}

CommandBuffer& CommandScheduler::Current() {
	CheckActive();
	return m_command;
}

CommandBuffer& CommandScheduler::BeginCommand() {
	EXIT_IF(!m_command.IsInvalid());
	m_command.m_buffer = m_command_pool.Commit();
	m_command.Begin();
	return m_command;
}

uint64_t CommandScheduler::Submit(SubmitInfo submit) {
	EXIT_IF(m_command.IsInvalid());
	EXIT_IF(submit.num_wait_semaphores > SubmitInfo::MaxSemaphores ||
	        submit.num_signal_semaphores >= SubmitInfo::MaxSemaphores);

	m_command.End();
	const auto buffer   = m_command.m_buffer;
	auto&      graphics = m_graphics;
	EXIT_IF(graphics.queue == nullptr);

	vk::Result result;
	uint64_t   tick;
	{
		Common::LockGuard lock(graphics.queue_mutex);
		tick = m_master.NextTick();
		submit.AddSignal(m_master.Handle(), tick);

		vk::TimelineSemaphoreSubmitInfo timeline_info {};
		timeline_info.waitSemaphoreValueCount   = submit.num_wait_semaphores;
		timeline_info.pWaitSemaphoreValues      = submit.wait_ticks.data();
		timeline_info.signalSemaphoreValueCount = submit.num_signal_semaphores;
		timeline_info.pSignalSemaphoreValues    = submit.signal_ticks.data();

		vk::SubmitInfo submit_info {};
		submit_info.pNext                = &timeline_info;
		submit_info.waitSemaphoreCount   = submit.num_wait_semaphores;
		submit_info.pWaitSemaphores      = submit.wait_semaphores.data();
		submit_info.pWaitDstStageMask    = submit.wait_stages.data();
		submit_info.commandBufferCount   = 1;
		submit_info.pCommandBuffers      = &buffer;
		submit_info.signalSemaphoreCount = submit.num_signal_semaphores;
		submit_info.pSignalSemaphores    = submit.signal_semaphores.data();

		result = graphics.queue.submit(1, &submit_info, nullptr);
	}

	if (result == vk::Result::eErrorDeviceLost) {
		DumpDeviceFault(graphics.device);
		// NHL26 debugging: GDS is host-visible; print its nonzero dwords as of the hang.
		const auto gds = m_context.GetBufferCache().GetGdsBuffer()->Mapped();
		if (!gds.empty()) {
			const auto* words = reinterpret_cast<const uint32_t*>(gds.data());
			uint32_t    shown = 0;
			for (size_t i = 0; i < gds.size() / 4 && shown < 64; i++) {
				if (words[i] != 0) {
					std::printf("NHL26GDS: [0x%04zx] = 0x%08x (%u)\n", i * 4, words[i], words[i]);
					shown++;
				}
			}
			std::printf("NHL26GDS: %u nonzero dwords shown\n", shown);
			std::fflush(stdout);
		}
	}
	if (result != vk::Result::eSuccess) {
		ReportVulkanFatal("vkQueueSubmit", result, tick, m_command.m_debug_op,
		                  m_command.m_debug_submit_id, m_command.m_debug_arg0,
		                  m_command.m_debug_arg1, m_command.m_debug_arg2, m_command.m_debug_arg3,
		                  m_command.m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	m_command.m_buffer = nullptr;
	GpuTimingPoll();
	return tick;
}

void CommandScheduler::BeginNext() {
	CheckActive();
	BeginCommand();
}

} // namespace Libs::Graphics
