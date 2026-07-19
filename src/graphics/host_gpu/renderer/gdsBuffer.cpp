#include "graphics/host_gpu/renderer/gdsBuffer.h"

#include "common/assert.h"
#include "graphics/host_gpu/transfer.h"
#include "graphics/host_gpu/vma.h"

namespace Libs::Graphics {

void GdsBuffer::Init(GraphicContext* ctx) {
	if (m_buffer == nullptr) {
		m_buffer = std::make_unique<VulkanBuffer>();

		m_buffer->usage           = vk::BufferUsageFlagBits::eStorageBuffer;
		m_buffer->memory.property = vk::MemoryPropertyFlagBits::eHostVisible |
		                            vk::MemoryPropertyFlagBits::eHostCoherent |
		                            vk::MemoryPropertyFlagBits::eHostCached;
		VulkanCreateBuffer(ctx, DW_SIZE * 4, m_buffer.get());
	}
}

void GdsBuffer::Clear(GraphicContext* ctx, uint64_t dw_offset, uint32_t dw_num,
                      uint32_t clear_value) {
	EXIT_IF(ctx == nullptr);

	Common::LockGuard lock(m_mutex);

	Init(ctx);

	EXIT_NOT_IMPLEMENTED(dw_offset >= DW_SIZE);
	EXIT_NOT_IMPLEMENTED(dw_offset + dw_num > DW_SIZE);

	void* data = nullptr;
	VulkanMapMemory(ctx, &m_buffer->memory, &data);

	for (uint32_t i = 0; i < dw_num; i++) {
		static_cast<uint32_t*>(data)[dw_offset + i] = clear_value;
	}

	VulkanUnmapMemory(ctx, &m_buffer->memory);
}

void GdsBuffer::Read(GraphicContext* ctx, uint32_t* dst, uint32_t dw_offset, uint32_t dw_size) {
	EXIT_IF(dst == nullptr);

	Common::LockGuard lock(m_mutex);

	Init(ctx);

	EXIT_NOT_IMPLEMENTED(dw_offset >= DW_SIZE);
	EXIT_NOT_IMPLEMENTED(dw_offset + dw_size > DW_SIZE);

	void* data = nullptr;
	VulkanMapMemory(ctx, &m_buffer->memory, &data);

	for (uint32_t i = 0; i < dw_size; i++) {
		dst[i] = static_cast<uint32_t*>(data)[dw_offset + i];
	}

	VulkanUnmapMemory(ctx, &m_buffer->memory);
}

VulkanBuffer* GdsBuffer::GetBuffer(GraphicContext* ctx) {
	Common::LockGuard lock(m_mutex);

	Init(ctx);

	return m_buffer.get();
}

} // namespace Libs::Graphics
