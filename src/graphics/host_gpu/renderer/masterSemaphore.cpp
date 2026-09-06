#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"

#include <bit>
#include <cinttypes>

namespace Libs::Graphics {

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.sType         = vk::StructureType::eSemaphoreTypeCreateInfo;
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.sType = vk::StructureType::eSemaphoreCreateInfo;
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
	// NextTick advances before submission, so even concurrent submissions cannot
	// complete a value at or beyond a freshly observed CurrentTick. Reject an
	// anomalous driver counter before it makes every pending resource appear free.
	const auto current = CurrentTick();
	if (result != vk::Result::eSuccess || counter >= current) {
		EXIT("vkGetSemaphoreCounterValue %s: semaphore=0x%016" PRIx64
		     " result=%s (%d) counter=%" PRIu64 " current_tick=%" PRIu64 "\n",
		     result == vk::Result::eSuccess ? "returned an unissued timeline value" : "failed",
		     std::bit_cast<uint64_t>(static_cast<VkSemaphore>(m_semaphore)),
		     VulkanToString(result).c_str(), static_cast<int>(result), counter, current);
		return;
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
	wait_info.sType          = vk::StructureType::eSemaphoreWaitInfo;
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	const auto result = m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX);
	if (result != vk::Result::eSuccess) {
		EXIT("vkWaitSemaphores failed: semaphore=0x%016" PRIx64
		     " result=%s (%d) wait_tick=%" PRIu64 " current_tick=%" PRIu64
		     " known_gpu_tick=%" PRIu64 "\n",
		     std::bit_cast<uint64_t>(static_cast<VkSemaphore>(m_semaphore)),
		     VulkanToString(result).c_str(), static_cast<int>(result), tick, CurrentTick(),
		     KnownGpuTick());
		return;
	}
	Refresh();
}

} // namespace Libs::Graphics
