#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <mutex>

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
	bool     valid     = false;
};

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
	r                 = {tick, submit_id, arg4, debug_op, arg0, arg1, arg2, arg3, true};
	g_history_count++;
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
		            " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
		            r.tick, DebugOpName(r.op), r.op, r.submit_id, r.arg0, r.arg1, r.arg2, r.arg3,
		            r.arg4);
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

	const auto result = m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX);
	if (result != vk::Result::eSuccess) {
		ReportSemaphoreFatal("vkWaitSemaphores", result, tick, KnownGpuTick());
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	Refresh();
}

} // namespace Libs::Graphics
