#include "graphics/host_gpu/renderer/scratchRingBuffer.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/vma.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>

namespace Libs::Graphics {

void ScratchRingBuffer::EnsureCapacity(uint64_t bytes) {
	if (bytes == 0) {
		return;
	}

	Common::LockGuard lock(m_mutex);
	if (m_capacity >= bytes) {
		return;
	}

	const uint64_t next =
	    m_capacity == 0 ? bytes : std::max(bytes, m_capacity + (m_capacity / 2u));

	static std::atomic_uint log_count {0};
	if (log_count.fetch_add(1, std::memory_order_relaxed) < 8) {
		LOGF("temporary: host scratch ring grow %" PRIu64 " -> %" PRIu64
		     " bytes (RDNA2 TMPRING; FLAT_SCRATCH aperture not emulated)\n",
		     m_capacity, next);
	}

	auto grown           = std::make_unique<VulkanBuffer>();
	grown->usage         = vk::BufferUsageFlagBits::eStorageBuffer;
	grown->memory.property = vk::MemoryPropertyFlagBits::eDeviceLocal;
	m_graphics.CreateBuffer(next, *grown);

	if (m_buffer != nullptr) {
		m_retired.push_back(std::move(m_buffer));
	}
	m_buffer   = std::move(grown);
	m_capacity = next;
}

uint64_t ScratchRingBuffer::Capacity() const {
	Common::LockGuard lock(m_mutex);
	return m_capacity;
}

VulkanBuffer& ScratchRingBuffer::GetBuffer() {
	Common::LockGuard lock(m_mutex);
	EXIT_IF(m_buffer == nullptr || m_capacity == 0);
	return *m_buffer;
}

} // namespace Libs::Graphics
