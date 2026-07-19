#include "graphics/host_gpu/renderer/streamBuffer.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/transfer.h"
#include "graphics/host_gpu/vma.h"

#include <atomic>
#include <cstring>

namespace Libs::Graphics {

namespace {
std::atomic<uint64_t> g_stream_buffer_count {0};
} // namespace

HostStreamBuffer::HostStreamBuffer() = default;

HostStreamBuffer::~HostStreamBuffer() {
	Release();
}

void HostStreamBuffer::EnsureBuffer(GraphicContext* ctx) {
	if (m_buffer != nullptr) {
		if (m_ctx != ctx || m_mapped == nullptr) {
			EXIT("host stream buffer changed graphics context\n");
		}
		return;
	}
	KYTY_PROFILER_BLOCK("HostStreamBuffer::create");
	m_ctx           = ctx;
	m_buffer        = std::make_unique<VulkanBuffer>();
	m_buffer->usage = vk::BufferUsageFlagBits::eStorageBuffer |
	                  vk::BufferUsageFlagBits::eVertexBuffer |
	                  vk::BufferUsageFlagBits::eIndexBuffer |
	                  vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;
	m_buffer->memory.property = vk::MemoryPropertyFlagBits::eHostVisible |
	                            vk::MemoryPropertyFlagBits::eHostCoherent |
	                            vk::MemoryPropertyFlagBits::eHostCached;
	VulkanCreateBuffer(ctx, CAPACITY, m_buffer.get());
	VulkanMapMemory(ctx, &m_buffer->memory, &m_mapped);
	const auto count = g_stream_buffer_count.fetch_add(1, std::memory_order_relaxed) + 1;
	LOGF("HostStreamBuffer: created serial=%" PRIu64 " capacity=%" PRIu64 "\n", count, CAPACITY);
}

bool HostStreamBuffer::Copy(GraphicContext* ctx, const void* src, uint64_t size, uint64_t alignment,
                            VulkanBuffer** out_buffer, uint64_t* out_offset, uint64_t* out_range) {
	KYTY_PROFILER_FUNCTION();
	if (ctx == nullptr || src == nullptr || out_buffer == nullptr || out_offset == nullptr ||
	    out_range == nullptr) {
		EXIT("invalid host stream upload\n");
	}
	std::lock_guard lock(m_mutex);
	if (size == 0) {
		return false;
	}
	if (alignment == 0) {
		alignment = 1;
	}
	if ((alignment & (alignment - 1)) != 0 || m_offset > UINT64_MAX - (alignment - 1)) {
		return false;
	}
	const auto offset = (m_offset + alignment - 1) & ~(alignment - 1);
	if (offset > CAPACITY || size > CAPACITY - offset) {
		return false;
	}
	EnsureBuffer(ctx);
	std::memcpy(static_cast<uint8_t*>(m_mapped) + offset, src, static_cast<size_t>(size));
	m_offset    = offset + size;
	*out_buffer = m_buffer.get();
	*out_offset = offset;
	*out_range  = size;
	return true;
}

void HostStreamBuffer::Reset() noexcept {
	std::lock_guard lock(m_mutex);
	m_offset = 0;
}

void HostStreamBuffer::Release() noexcept {
	std::lock_guard lock(m_mutex);
	if (m_buffer != nullptr) {
		if (m_ctx == nullptr || m_mapped == nullptr) {
			EXIT("invalid host stream buffer release state\n");
		}
		VulkanUnmapMemory(m_ctx, &m_buffer->memory);
		VulkanDeleteBuffer(m_ctx, m_buffer.get());
		m_buffer.reset();
	}
	m_ctx    = nullptr;
	m_mapped = nullptr;
	m_offset = 0;
}

} // namespace Libs::Graphics
