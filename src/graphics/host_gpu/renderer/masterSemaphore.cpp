#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"

#include <cinttypes>

namespace Libs::Graphics {

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

void MasterSemaphore::RecordSubmission(SubmissionDebugInfo info) {
	std::lock_guard lock(m_submission_mutex);
	m_submissions[info.tick % m_submissions.size()] = info;
}

MasterSemaphore::SubmissionDebugInfo MasterSemaphore::FindSubmission(uint64_t tick) const {
	std::lock_guard lock(m_submission_mutex);
	const auto& info = m_submissions[tick % m_submissions.size()];
	return info.tick == tick ? info : SubmissionDebugInfo {.tick = tick};
}

void MasterSemaphore::Refresh() {
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	if (result != vk::Result::eSuccess) {
		EXIT("vkGetSemaphoreCounterValue failed: %s (%d), completed tick=%" PRIu64
		     ", submitted through=%" PRIu64 "\n",
		     vk::to_string(result).c_str(), static_cast<int>(result),
		     m_gpu_tick.load(std::memory_order_acquire),
		     m_current_tick.load(std::memory_order_acquire));
		return;
	}

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick) {
	KYTY_PROFILER_BLOCK("Vulkan::TimelineSemaphoreWait", profiler::colors::RedA100);
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
		const auto debug = FindSubmission(tick);
		EXIT("vkWaitSemaphores failed: %s (%d), requested tick=%" PRIu64
		     ", completed tick=%" PRIu64 ", submitted through=%" PRIu64
		     ", op=%u, submit=%" PRIu64 ", args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
		     vk::to_string(result).c_str(), static_cast<int>(result), tick,
		     m_gpu_tick.load(std::memory_order_acquire),
		     m_current_tick.load(std::memory_order_acquire), debug.operation, debug.submit_id,
		     debug.arg0, debug.arg1, debug.arg2, debug.arg3, debug.arg4);
		return;
	}
	Refresh();
}

} // namespace Libs::Graphics
