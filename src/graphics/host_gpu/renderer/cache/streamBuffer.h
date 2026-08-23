#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_STREAMBUFFER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_STREAMBUFFER_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class CommandBuffer;
class CommandScheduler;
struct StreamBufferTestAccess;
struct GraphicContext;
struct VulkanBuffer;

enum class MemoryUsage : uint8_t {
	DeviceLocal,
	Upload,
	Download,
	Stream,
};

inline constexpr vk::BufferUsageFlags ReadFlags =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eUniformBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer;

inline constexpr vk::BufferUsageFlags AllFlags =
    ReadFlags | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer;

class Buffer {
public:
	Buffer(GraphicContext& graphics, CommandScheduler& scheduler, MemoryUsage usage,
	       uint64_t cpu_address, vk::BufferUsageFlags flags, uint64_t size);
	~Buffer();
	KYTY_CLASS_NO_COPY(Buffer);

	[[nodiscard]] vk::Buffer         Handle() const noexcept;
	[[nodiscard]] uint64_t           Size() const noexcept { return m_size; }
	[[nodiscard]] std::span<uint8_t> Mapped() const noexcept { return m_mapped; }
	[[nodiscard]] bool               IsCoherent() const noexcept { return m_is_coherent; }
	[[nodiscard]] MemoryUsage        Usage() const noexcept { return m_usage; }
	[[nodiscard]] uint64_t           CpuAddress() const noexcept { return m_cpu_address; }
	[[nodiscard]] uint64_t           Offset(uint64_t address) const noexcept {
		return address - m_cpu_address;
	}
	[[nodiscard]] bool IsInBounds(uint64_t address, uint64_t size) const noexcept;
	void               Write(uint64_t offset, const void* source, uint64_t size);
	void               Flush(uint64_t offset, uint64_t size);
	void CopyFrom(CommandBuffer& command, const Buffer& source, uint64_t source_offset,
	              uint64_t destination_offset, uint64_t size,
	              vk::AccessFlags source_before      = vk::AccessFlagBits::eMemoryWrite,
	              vk::AccessFlags destination_before = vk::AccessFlagBits::eMemoryRead |
	                                                   vk::AccessFlagBits::eMemoryWrite,
	              vk::AccessFlags source_after       = vk::AccessFlagBits::eMemoryRead |
	                                                   vk::AccessFlagBits::eMemoryWrite,
	              vk::AccessFlags destination_after  = vk::AccessFlagBits::eMemoryRead |
	                                                   vk::AccessFlagBits::eMemoryWrite);
	void Fill(uint64_t offset, uint64_t size, uint32_t value);

	// BufferCache state lives directly on the resource.
	bool   is_deleted = false;
	size_t lru_id     = 0;

protected:
	[[nodiscard]] GraphicContext&   Graphics() const noexcept { return *m_graphics; }
	[[nodiscard]] CommandScheduler& Scheduler() const noexcept { return *m_scheduler; }
	[[nodiscard]] VulkanBuffer&     NativeBuffer() noexcept { return *m_buffer; }

private:
	[[nodiscard]] vk::BufferMemoryBarrier Barrier(uint64_t offset, uint64_t size,
	                                              vk::AccessFlags source,
	                                              vk::AccessFlags destination) const;

	GraphicContext*               m_graphics    = nullptr;
	CommandScheduler*             m_scheduler   = nullptr;
	MemoryUsage                   m_usage       = MemoryUsage::DeviceLocal;
	uint64_t                      m_cpu_address = 0;
	uint64_t                      m_size        = 0;
	std::unique_ptr<VulkanBuffer> m_buffer;
	std::span<uint8_t>            m_mapped;
	bool                          m_is_coherent = false;
};

class StreamBuffer final: public Buffer {
public:
	StreamBuffer(GraphicContext& graphics, CommandScheduler& scheduler, MemoryUsage usage,
	             uint64_t size);

	[[nodiscard]] std::pair<uint8_t*, uint64_t> Map(uint64_t size, uint64_t alignment = 0,
	                                                bool allow_wait = true);
	void                                        Commit();
	// Download mappings become visible to the CPU only after their GPU completion tick is free.
	// Call this from the scheduler's deferred completion operation before reading Mapped().
	void                   Invalidate(uint64_t offset, uint64_t size);
	[[nodiscard]] uint64_t Copy(const void* source, uint64_t size, uint64_t alignment = 0);

private:
	friend struct StreamBufferTestAccess;

	struct Watch {
		uint64_t tick        = 0;
		uint64_t upper_bound = 0;
	};

	void                      ReserveWatches(std::vector<Watch>& watches, size_t grow_size);
	[[nodiscard]] static bool NormalizeReservation(bool coherent, uint64_t atom, uint64_t& size,
	                                               uint64_t& alignment);
	[[nodiscard]] bool        WaitPendingOperations(const std::vector<Watch>& watches,
	                                                std::optional<size_t>     invalidation_mark,
	                                                uint64_t requested_upper_bound, bool allow_wait,
	                                                size_t& wait_cursor, uint64_t& wait_bound);

	uint64_t              m_offset      = 0;
	uint64_t              m_mapped_size = 0;
	std::vector<Watch>    m_current_watches;
	size_t                m_current_watch_cursor = 0;
	std::optional<size_t> m_invalidation_mark;
	std::vector<Watch>    m_previous_watches;
	size_t                m_wait_cursor = 0;
	uint64_t              m_wait_bound  = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_STREAMBUFFER_H_
