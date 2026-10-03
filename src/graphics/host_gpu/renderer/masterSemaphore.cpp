#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"

#include <cinttypes>
#include <cstdio>
#include <vector>

namespace Libs::Graphics {

namespace {

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
		const auto known_tick = m_gpu_tick.load(std::memory_order_acquire);
		EXIT("MasterSemaphore::Refresh failed: %s (gpu tick %" PRIu64 ")\n",
		     vk::to_string(result).c_str(), known_tick);
	}

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
	// Name the result: with an infinite timeout this cannot be eTimeout, so a failure here is a
	// lost device or an allocation failure, and those need completely different investigations.
	if (result != vk::Result::eSuccess) {
		const auto known_tick = m_gpu_tick.load(std::memory_order_acquire);
		EXIT("MasterSemaphore::Wait failed: %s (tick %" PRIu64 ", gpu tick %" PRIu64 ")\n",
		     vk::to_string(result).c_str(), tick, known_tick);
	}
	Refresh();
}

} // namespace Libs::Graphics
