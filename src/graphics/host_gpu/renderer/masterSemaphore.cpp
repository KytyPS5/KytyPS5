#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/frameStats.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/queueSubmitter.h"

#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <string>

namespace Libs::Graphics {

namespace {

struct SubmitRecord {
	uint64_t tick      = 0;
	uint64_t submit_id = 0;
	uint64_t arg4      = 0;
	uint32_t op        = 0;
	uint32_t arg0      = 0;
	uint32_t arg1      = 0;
	uint32_t arg2      = 0;
	uint32_t arg3      = 0;
	uint64_t vs_hash   = 0;
	uint64_t ps_hash   = 0;
	uint64_t cs_hash   = 0;
	bool     valid     = false;
};

std::atomic<uint64_t> g_bound_vs {0};
std::atomic<uint64_t> g_bound_ps {0};
std::atomic<uint64_t> g_bound_cs {0};

constexpr size_t SubmitHistorySize = 64;

std::mutex                                g_history_mutex;
std::array<SubmitRecord, SubmitHistorySize> g_history;
uint64_t                                  g_history_count = 0;

const char* DebugOpName(uint32_t op) {
	// Keep in sync with CommandBufferDebugOp (render.h).
	static const char* const names[] = {"DispatchDirect", "DrawIndex",  "DrawIndexAuto",
	                                    "EopWrite",       "EopInterrupt", "EopWriteBack",
	                                    "EopFlip",        "EopWriteBackFlip", "EopOnlyFlip",
	                                    "DispatchIndirect", "Unknown"};
	return op < (sizeof(names) / sizeof(names[0])) ? names[op] : "?";
}

} // namespace

void RecordSubmitHistory(uint64_t tick, uint32_t debug_op, uint64_t submit_id, uint32_t arg0,
                         uint32_t arg1, uint32_t arg2, uint32_t arg3, uint64_t arg4) {
	std::lock_guard lock(g_history_mutex);
	auto&           r = g_history[g_history_count % SubmitHistorySize];
	r                 = {tick,
	                     submit_id,
	                     arg4,
	                     debug_op,
	                     arg0,
	                     arg1,
	                     arg2,
	                     arg3,
	                     g_bound_vs.load(std::memory_order_relaxed),
	                     g_bound_ps.load(std::memory_order_relaxed),
	                     g_bound_cs.load(std::memory_order_relaxed),
	                     true};
	g_history_count++;
}

namespace {
std::mutex  g_perf_mutex;
std::string g_perf_lines[12];
size_t      g_perf_count = 0;
} // namespace

void RecordPerfLine(const char* line) {
	std::lock_guard lock(g_perf_mutex);
	g_perf_lines[g_perf_count % 12] = line;
	g_perf_count++;
}

void NoteBoundShader(uint32_t stage_slot, uint64_t hash) {
	// 0 = vertex, 1 = pixel, 2 = compute.
	if (stage_slot == 0) {
		g_bound_vs.store(hash, std::memory_order_relaxed);
	} else if (stage_slot == 1) {
		g_bound_ps.store(hash, std::memory_order_relaxed);
	} else if (stage_slot == 2) {
		g_bound_cs.store(hash, std::memory_order_relaxed);
	}
}

bool CurrentDrawShadersSkipped() {
	static const std::vector<uint64_t> skipped = [] {
		std::vector<uint64_t> list = {0xb3b6b0fc2e9a1de7ull};
		if (const char* env = std::getenv("KYTY_SKIP_SHADERS")) {
			list.clear();
			std::string text(env);
			size_t      pos = 0;
			while (pos < text.size()) {
				const auto end   = text.find(',', pos);
				const auto token = text.substr(pos, end == std::string::npos ? end : end - pos);
				if (!token.empty() && token != "none") {
					list.push_back(std::strtoull(token.c_str(), nullptr, 16));
				}
				if (end == std::string::npos) {
					break;
				}
				pos = end + 1;
			}
		}
		for (const auto hash : list) {
			std::printf("Draws using shader %016" PRIx64 " are skipped (KYTY_SKIP_SHADERS=none to disable)\n", hash);
		}
		return list;
	}();
	if (skipped.empty()) {
		return false;
	}
	const auto ps = g_bound_ps.load(std::memory_order_relaxed);
	const auto vs = g_bound_vs.load(std::memory_order_relaxed);
	for (const auto hash : skipped) {
		if (hash == ps || hash == vs) {
			return true;
		}
	}
	return false;
}

void DumpSubmitHistory() {
	std::lock_guard lock(g_history_mutex);
	std::printf("--- Last GPU submits (oldest first, newest last) ---\n");
	const uint64_t total = g_history_count < SubmitHistorySize ? g_history_count : SubmitHistorySize;
	for (uint64_t i = 0; i < total; i++) {
		const auto& r = g_history[(g_history_count - total + i) % SubmitHistorySize];
		if (!r.valid) {
			continue;
		}
		std::printf("tick=%" PRIu64 " op=%s(%u) submit_id=%" PRIu64
		            " args=%u,%u,%u,%u,0x%016" PRIx64 " vs=%016" PRIx64 " ps=%016" PRIx64
		            " cs=%016" PRIx64 "\n",
		            r.tick, DebugOpName(r.op), r.op, r.submit_id, r.arg0, r.arg1, r.arg2, r.arg3,
		            r.arg4, r.vs_hash, r.ps_hash, r.cs_hash);
	}
	{
		std::lock_guard perf_lock(g_perf_mutex);
		std::printf("--- Recent PERF3 lines (oldest first) ---\n");
		const size_t n     = g_perf_count < 12 ? g_perf_count : 12;
		const size_t start = g_perf_count - n;
		for (size_t i = 0; i < n; i++) {
			std::printf("%s\n", g_perf_lines[(start + i) % 12].c_str());
		}
	}
	std::printf("--- end of submit history ---\n");
	std::fflush(stdout);
}

bool GpuSyncDebugEnabled() {
	static const bool enabled = [] {
		const char* v = std::getenv("KYTY_GPU_SYNC");
		return v != nullptr && v[0] != '\0' && v[0] != '0';
	}();
	return enabled;
}

namespace {

void ReportSemaphoreFatal(const char* what, vk::Result result, uint64_t tick, uint64_t known) {
	LOGF("%s failed: %s (%d), wait_tick=%" PRIu64 " known_gpu_tick=%" PRIu64 "\n", what,
	     vk::to_string(result).c_str(), static_cast<int>(result), tick, known);
	std::printf("%s failed: %s (%d), wait_tick=%" PRIu64 " known_gpu_tick=%" PRIu64 "\n", what,
	            vk::to_string(result).c_str(), static_cast<int>(result), tick, known);
	if (result == vk::Result::eErrorDeviceLost) {
		DumpSubmitHistory();
		std::printf("GPU device lost (hang/TDR or invalid GPU memory access). Try deleting the "
		            "_PipelineCache file for this title and updating the GPU driver.\n");
	}
	std::fflush(stdout);
}

} // namespace

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

MasterSemaphore::~MasterSemaphore() {
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

void MasterSemaphore::Refresh() {
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	if (result != vk::Result::eSuccess) {
		ReportSemaphoreFatal("vkGetSemaphoreCounterValue", result, 0, KnownGpuTick());
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick) {
	if (IsFree(tick)) {
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		return;
	}

	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	// The tick may still sit in the submit queue; make sure the GPU has actually been given it.
	DrainQueueSubmits(m_graphics);
	const auto wait_begin = std::chrono::steady_clock::now();
	const auto result     = m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX);
	g_frame_stats.waits.fetch_add(1, std::memory_order_relaxed);
	g_frame_stats.wait_us.fetch_add(
	    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
	                              std::chrono::steady_clock::now() - wait_begin)
	                              .count()),
	    std::memory_order_relaxed);
	if (result != vk::Result::eSuccess) {
		ReportSemaphoreFatal("vkWaitSemaphores", result, tick, KnownGpuTick());
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	Refresh();
}

} // namespace Libs::Graphics
