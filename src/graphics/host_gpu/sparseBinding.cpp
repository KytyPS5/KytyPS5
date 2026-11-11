#include "graphics/host_gpu/sparseBinding.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>

namespace Libs::Graphics {

SparseBinding::SparseBinding(GraphicContext& graphics): m_graphics(graphics) {
	EXIT_IF(graphics.device == nullptr || !graphics.sparse_residency_image2d_enabled);
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;
	vk::SemaphoreCreateInfo semaphore_info {};
	semaphore_info.pNext = &type_info;
	EXIT_NOT_IMPLEMENTED(graphics.device.createSemaphore(&semaphore_info, nullptr, &m_timeline) !=
	                     vk::Result::eSuccess);
}

SparseBinding::~SparseBinding() {
	WaitIdle();
	m_graphics.device.destroySemaphore(m_timeline, nullptr);
}

void SparseBinding::BindImage(SparseImageBinds&& binds) {
	EXIT_IF(binds.image == nullptr);
	if (binds.levels.empty() && binds.opaque.empty()) {
		return;
	}
	std::lock_guard lock(m_mutex);
	m_images.push_back(std::move(binds));
}

void SparseBinding::Forget(vk::Image image) {
	std::lock_guard lock(m_mutex);
	std::erase_if(m_images,
	              [image](const SparseImageBinds& binds) { return binds.image == image; });
}

SparseWait SparseBinding::Flush() {
	std::lock_guard lock(m_mutex);
	if (m_images.empty()) {
		return {m_value != 0 ? m_timeline : vk::Semaphore {}, m_value};
	}
	std::vector<vk::SparseImageMemoryBindInfo>       image_infos;
	std::vector<vk::SparseImageOpaqueMemoryBindInfo> opaque_infos;
	for (const auto& binds: m_images) {
		if (!binds.levels.empty()) {
			image_infos.push_back(
			    {binds.image, static_cast<uint32_t>(binds.levels.size()), binds.levels.data()});
		}
		if (!binds.opaque.empty()) {
			opaque_infos.push_back(
			    {binds.image, static_cast<uint32_t>(binds.opaque.size()), binds.opaque.data()});
		}
	}

	const uint64_t                  wait_value   = m_value;
	const uint64_t                  signal_value = m_value + 1;
	vk::TimelineSemaphoreSubmitInfo timeline {};
	timeline.waitSemaphoreValueCount   = wait_value != 0 ? 1 : 0;
	timeline.pWaitSemaphoreValues      = &wait_value;
	timeline.signalSemaphoreValueCount = 1;
	timeline.pSignalSemaphoreValues    = &signal_value;
	vk::BindSparseInfo info {};
	info.pNext                = &timeline;
	info.waitSemaphoreCount   = wait_value != 0 ? 1 : 0;
	info.pWaitSemaphores      = &m_timeline;
	info.imageOpaqueBindCount = static_cast<uint32_t>(opaque_infos.size());
	info.pImageOpaqueBinds    = opaque_infos.data();
	info.imageBindCount       = static_cast<uint32_t>(image_infos.size());
	info.pImageBinds          = image_infos.data();
	info.signalSemaphoreCount = 1;
	info.pSignalSemaphores    = &m_timeline;
	const auto result         = m_graphics.queue.bindSparse(1, &info, nullptr);
	if (result != vk::Result::eSuccess) {
		EXIT("vkQueueBindSparse failed: %s, images=%zu\n", vk::to_string(result).c_str(),
		     m_images.size());
	}
	m_value = signal_value;
	m_images.clear();
	return {m_timeline, m_value};
}

void SparseBinding::WaitIdle() {
	uint64_t value = 0;
	{
		std::lock_guard lock(m_mutex);
		value = m_value;
	}
	if (value == 0) {
		return;
	}
	vk::SemaphoreWaitInfo wait {};
	wait.semaphoreCount = 1;
	wait.pSemaphores    = &m_timeline;
	wait.pValues        = &value;
	EXIT_NOT_IMPLEMENTED(m_graphics.device.waitSemaphores(&wait, UINT64_MAX) !=
	                     vk::Result::eSuccess);
}

} // namespace Libs::Graphics
