#include "graphics/host_gpu/sparseBinding.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>

namespace Libs::Graphics {

SparseBinding::SparseBinding(GraphicContext& graphics): m_graphics(graphics) {
	EXIT_IF(
	    graphics.device == nullptr || graphics.allocator == nullptr ||
	    !(graphics.sparse_residency_image2d_enabled || graphics.sparse_residency_buffer_enabled));
	if (graphics.sparse_residency_buffer_enabled) {
		ProbeBufferPages();
	}
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
	std::lock_guard lock(m_mutex);
	FreePages(m_queued_release);
	m_queued_release.clear();
	CollectLocked(UINT64_MAX);
	m_graphics.device.destroySemaphore(m_timeline, nullptr);
}

void SparseBinding::ProbeBufferPages() {
	// One page serves every sparse buffer: plain and aliased ones, with any of the usages.
	uint32_t types = UINT32_MAX;
	for (const bool aliased: {false, true}) {
		if (aliased && !m_graphics.sparse_residency_aliased_enabled) {
			continue;
		}
		vk::BufferCreateInfo info {};
		info.size  = 1;
		info.usage = SparseBufferUsage;
		info.flags =
		    vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency;
		if (aliased) {
			info.flags |= vk::BufferCreateFlagBits::eSparseAliased;
		}
		vk::Buffer probe = nullptr;
		EXIT_NOT_IMPLEMENTED(m_graphics.device.createBuffer(&info, nullptr, &probe) !=
		                     vk::Result::eSuccess);
		vk::MemoryRequirements requirements {};
		m_graphics.device.getBufferMemoryRequirements(probe, &requirements);
		m_graphics.device.destroyBuffer(probe, nullptr);
		m_page_size = std::max<uint64_t>(m_page_size, requirements.alignment);
		types &= requirements.memoryTypeBits;
	}
	// Device memory where the buffers take it: NVIDIA takes no other.
	const auto& memory = m_graphics.physical_device_memory_properties;
	for (uint32_t type = 0; type < memory.memoryTypeCount; type++) {
		if ((types & (1u << type)) != 0 &&
		    (memory.memoryTypes[type].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
			m_memory_type_bits |= 1u << type;
		}
	}
	if (m_memory_type_bits == 0) {
		m_memory_type_bits = types;
	}
	EXIT_NOT_IMPLEMENTED(m_page_size == 0 || (m_page_size & (m_page_size - 1)) != 0 ||
	                     m_memory_type_bits == 0);
}

bool SparseBinding::AllocatePages(std::span<SparsePage> pages, bool within_budget) {
	EXIT_IF(m_page_size == 0);
	if (pages.empty()) {
		return true;
	}
	std::vector<VmaAllocation>     allocations(pages.size());
	std::vector<VmaAllocationInfo> infos(pages.size());
	const VkMemoryRequirements     requirements {m_page_size, m_page_size, m_memory_type_bits};
	const auto                     allocate = [&](const VmaAllocationCreateInfo& info) {
		return vmaAllocateMemoryPages(m_graphics.allocator, &requirements, &info, pages.size(),
		                              allocations.data(), infos.data());
	};
	if (within_budget ? !m_graphics.AllocateDeviceLocal(pages.size() * m_page_size, {}, allocate)
	                  : allocate({}) != VK_SUCCESS) {
		return false;
	}
	for (size_t i = 0; i < pages.size(); i++) {
		pages[i] = {allocations[i], infos[i].deviceMemory, infos[i].offset};
	}
	return true;
}

void SparseBinding::FreePages(std::span<const SparsePage> pages) {
	for (const auto& page: pages) {
		if (page.allocation != nullptr) {
			vmaFreeMemory(m_graphics.allocator, page.allocation);
		}
	}
}

void SparseBinding::BindBuffer(vk::Buffer buffer, std::span<const vk::SparseMemoryBind> binds) {
	EXIT_IF(buffer == nullptr || m_page_size == 0);
	std::lock_guard lock(m_mutex);
	auto&           pages = m_buffers[static_cast<VkBuffer>(buffer)];
	for (const auto& bind: binds) {
		EXIT_IF(bind.size == 0 || (bind.resourceOffset | bind.size) % m_page_size != 0 ||
		        (bind.memory != nullptr && bind.memoryOffset % m_page_size != 0) ||
		        bind.flags != vk::SparseMemoryBindFlags {});
		for (uint64_t offset = 0; offset < bind.size; offset += m_page_size) {
			pages[bind.resourceOffset + offset] =
			    bind.memory != nullptr ? PageBind {bind.memory, bind.memoryOffset + offset}
			                           : PageBind {};
		}
	}
}

void SparseBinding::UnbindBuffer(vk::Buffer buffer, uint64_t offset, uint64_t size,
                                 std::span<const SparsePage> release) {
	const vk::SparseMemoryBind unbind {offset, size, nullptr, 0};
	BindBuffer(buffer, {&unbind, 1});
	std::lock_guard lock(m_mutex);
	m_queued_release.insert(m_queued_release.end(), release.begin(), release.end());
}

void SparseBinding::BindImage(SparseImageBinds&& binds) {
	EXIT_IF(binds.image == nullptr);
	if (binds.levels.empty() && binds.opaque.empty()) {
		return;
	}
	std::lock_guard lock(m_mutex);
	m_images.push_back(std::move(binds));
}

void SparseBinding::Forget(vk::Buffer buffer) {
	std::lock_guard lock(m_mutex);
	m_buffers.erase(static_cast<VkBuffer>(buffer));
}

void SparseBinding::Forget(vk::Image image) {
	std::lock_guard lock(m_mutex);
	std::erase_if(m_images,
	              [image](const SparseImageBinds& binds) { return binds.image == image; });
}

SparseWait SparseBinding::Flush() {
	std::lock_guard lock(m_mutex);
	if (m_buffers.empty() && m_images.empty() && m_queued_release.empty()) {
		return {m_value != 0 ? m_timeline : vk::Semaphore {}, m_value};
	}
	std::vector<std::vector<vk::SparseMemoryBind>> buffer_binds;
	std::vector<vk::SparseBufferMemoryBindInfo>    buffer_infos;
	buffer_binds.reserve(m_buffers.size());
	buffer_infos.reserve(m_buffers.size());
	for (const auto& [buffer, pages]: m_buffers) {
		// Neighbouring pages of one memory run go as one bind.
		auto& binds = buffer_binds.emplace_back();
		for (const auto& [offset, page]: pages) {
			if (!binds.empty()) {
				auto&      last     = binds.back();
				const bool adjacent = last.resourceOffset + last.size == offset;
				const bool continuous =
				    page.memory == last.memory &&
				    (page.memory == nullptr || last.memoryOffset + last.size == page.offset);
				if (adjacent && continuous) {
					last.size += m_page_size;
					continue;
				}
			}
			binds.push_back({offset, m_page_size, page.memory, page.offset});
		}
		buffer_infos.push_back({buffer, static_cast<uint32_t>(binds.size()), binds.data()});
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
	info.bufferBindCount      = static_cast<uint32_t>(buffer_infos.size());
	info.pBufferBinds         = buffer_infos.data();
	info.imageOpaqueBindCount = static_cast<uint32_t>(opaque_infos.size());
	info.pImageOpaqueBinds    = opaque_infos.data();
	info.imageBindCount       = static_cast<uint32_t>(image_infos.size());
	info.pImageBinds          = image_infos.data();
	info.signalSemaphoreCount = 1;
	info.pSignalSemaphores    = &m_timeline;
	const auto result         = m_graphics.queue.bindSparse(1, &info, nullptr);
	if (result != vk::Result::eSuccess) {
		EXIT("vkQueueBindSparse failed: %s, buffers=%zu images=%zu\n",
		     vk::to_string(result).c_str(), buffer_infos.size(), m_images.size());
	}
	m_value = signal_value;
	m_buffers.clear();
	m_images.clear();
	if (!m_queued_release.empty()) {
		m_releases.push_back({m_value, std::move(m_queued_release)});
		m_queued_release.clear();
	}
	return {m_timeline, m_value};
}

void SparseBinding::Collect() {
	std::lock_guard lock(m_mutex);
	if (m_releases.empty()) {
		return;
	}
	uint64_t completed = 0;
	EXIT_NOT_IMPLEMENTED(m_graphics.device.getSemaphoreCounterValue(m_timeline, &completed) !=
	                     vk::Result::eSuccess);
	CollectLocked(completed);
}

void SparseBinding::CollectLocked(uint64_t completed) {
	while (!m_releases.empty() && m_releases.front().value <= completed) {
		FreePages(m_releases.front().pages);
		m_releases.pop_front();
	}
}

void SparseBinding::WaitIdle() {
	uint64_t value = 0;
	{
		std::lock_guard lock(m_mutex);
		value = m_value;
	}
	if (value != 0) {
		vk::SemaphoreWaitInfo wait {};
		wait.semaphoreCount = 1;
		wait.pSemaphores    = &m_timeline;
		wait.pValues        = &value;
		EXIT_NOT_IMPLEMENTED(m_graphics.device.waitSemaphores(&wait, UINT64_MAX) !=
		                     vk::Result::eSuccess);
	}
	Collect();
}

} // namespace Libs::Graphics
