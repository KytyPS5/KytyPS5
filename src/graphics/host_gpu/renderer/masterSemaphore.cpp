#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"

namespace Libs::Graphics {

MasterSemaphore::MasterSemaphore(GraphicContext& graphics)
    : m_graphics(graphics), m_fences(FenceOps {&graphics}) {
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

vk::Fence MasterSemaphore::FenceOps::Create() const {
	const vk::FenceCreateInfo create {};
	vk::Fence                 fence = nullptr;
	EXIT_NOT_IMPLEMENTED(graphics->device.createFence(&create, nullptr, &fence) !=
	                     vk::Result::eSuccess);
	return fence;
}

bool MasterSemaphore::FenceOps::IsSignaled(vk::Fence fence) const {
	const auto status = graphics->device.getFenceStatus(fence);
	EXIT_NOT_IMPLEMENTED(status != vk::Result::eSuccess && status != vk::Result::eNotReady);
	return status == vk::Result::eSuccess;
}

void MasterSemaphore::FenceOps::Reset(std::span<const vk::Fence> fences) const {
	EXIT_NOT_IMPLEMENTED(graphics->device.resetFences(static_cast<uint32_t>(fences.size()),
	                                                  fences.data()) != vk::Result::eSuccess);
}

void MasterSemaphore::FenceOps::Wait(vk::Fence fence) const {
	// Bounded: the caller checks the retirement state again after each wait, so a wake-up that the
	// driver misses costs one period, not a stall.
	constexpr uint64_t PeriodNs = 1'000'000;
	const auto         result   = graphics->device.waitForFences(1, &fence, VK_TRUE, PeriodNs);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess && result != vk::Result::eTimeout);
}

void MasterSemaphore::FenceOps::Destroy(vk::Fence fence) const {
	graphics->device.destroyFence(fence, nullptr);
}

vk::Fence MasterSemaphore::AcquireFence(uint64_t tick) {
	return m_fences.Acquire(tick);
}

bool MasterSemaphore::IsRetired(uint64_t tick) {
	return m_fences.IsRetired(tick);
}

void MasterSemaphore::WaitRetired(uint64_t tick) {
	KYTY_PROFILER_FUNCTION();
	Wait(tick);
	m_fences.WaitRetired(tick);
}

void MasterSemaphore::Refresh() {
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

bool MasterSemaphore::TryWait(uint64_t tick) noexcept {
	if (IsFree(tick)) {
		return true;
	}
	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;
	return m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX) == vk::Result::eSuccess;
}

void MasterSemaphore::Wait(uint64_t tick) {
	KYTY_PROFILER_FUNCTION();
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
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	Refresh();
}

} // namespace Libs::Graphics
