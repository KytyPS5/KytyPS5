#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_SPARSEBINDING_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_SPARSEBINDING_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

VK_DEFINE_HANDLE(VmaAllocation)

namespace Libs::Graphics {

struct GraphicContext;

// The binds of one sparse image: per-level binds below the mip tail, then the opaque binds of the
// mip tail and metadata.
struct SparseImageBinds {
	vk::Image                              image;
	std::vector<vk::SparseImageMemoryBind> levels;
	std::vector<vk::SparseMemoryBind>      opaque;
};

// Every usage a sparse buffer is created with; its pages serve any subset of them.
inline constexpr vk::BufferUsageFlags SparseBufferUsage =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

// One page of sparse buffer memory.
struct SparsePage {
	VmaAllocation    allocation = nullptr;
	vk::DeviceMemory memory     = nullptr;
	uint64_t         offset     = 0;
};

// What a queue submission waits on so that every bind queued before it has executed.
struct SparseWait {
	vk::Semaphore semaphore = nullptr;
	uint64_t      value     = 0;
};

// Queues sparse binds and submits them as one vkQueueBindSparse batch right before the next
// queue submission, which waits for it. Batches apply in order. An image is bound once, when it
// is created, and no work can use it before the submission that waits for its batch. A buffer
// takes and gives up pages while it lives; the last bind queued for a page is the one applied.
class SparseBinding {
public:
	explicit SparseBinding(GraphicContext& graphics);
	~SparseBinding();
	KYTY_CLASS_NO_COPY(SparseBinding);

	// The bind granularity of sparse buffers; zero without them.
	[[nodiscard]] uint64_t PageSize() const noexcept { return m_page_size; }
	// All or nothing. False when the device has no memory left for them, or, for pages that are
	// to stay within the device budget, when that has no room.
	[[nodiscard]] bool AllocatePages(std::span<SparsePage> pages, bool within_budget);
	// For pages no executed or queued bind names.
	void FreePages(std::span<const SparsePage> pages);

	// Whole pages; a bind without memory unbinds its range.
	void BindBuffer(vk::Buffer buffer, std::span<const vk::SparseMemoryBind> binds);
	// Unbinds the range and frees `release` once the batch carrying the unbind has executed.
	void UnbindBuffer(vk::Buffer buffer, uint64_t offset, uint64_t size,
	                  std::span<const SparsePage> release);
	void BindImage(SparseImageBinds&& binds);
	// Drops the queued binds of a resource about to be destroyed.
	void Forget(vk::Buffer buffer);
	void Forget(vk::Image image);

	// The caller holds GraphicContext::queue_mutex and submits to the queue right after.
	[[nodiscard]] SparseWait Flush();
	// Frees the pages whose unbind has executed.
	void Collect();
	void WaitIdle();

private:
	struct PageBind {
		vk::DeviceMemory memory = nullptr;
		uint64_t         offset = 0;
	};
	// Pages to free once the batch signalling `value` has executed.
	struct Release {
		uint64_t                value = 0;
		std::vector<SparsePage> pages;
	};

	void ProbeBufferPages();
	void CollectLocked(uint64_t completed);

	GraphicContext& m_graphics;
	uint64_t        m_page_size        = 0;
	uint32_t        m_memory_type_bits = 0;
	vk::Semaphore   m_timeline         = nullptr;
	uint64_t        m_value            = 0;
	std::mutex      m_mutex;
	// Buffer -> resource offset -> the page's final bind in the queued batch.
	std::unordered_map<VkBuffer, std::map<uint64_t, PageBind>> m_buffers;
	std::vector<SparseImageBinds>                              m_images;
	std::vector<SparsePage>                                    m_queued_release;
	std::deque<Release>                                        m_releases;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_SPARSEBINDING_H_
