#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_SPARSEBINDING_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_SPARSEBINDING_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;

// The binds of one sparse image: per-level binds below the mip tail, then the opaque binds of the
// mip tail and metadata.
struct SparseImageBinds {
	vk::Image                              image;
	std::vector<vk::SparseImageMemoryBind> levels;
	std::vector<vk::SparseMemoryBind>      opaque;
};

// What a queue submission waits on so that every bind queued before it has executed.
struct SparseWait {
	vk::Semaphore semaphore = nullptr;
	uint64_t      value     = 0;
};

// Queues sparse image binds and submits them as one vkQueueBindSparse batch right before the next
// queue submission, which waits for it. Batches apply in order; an image is bound once, when it
// is created, and no work can use it before the submission that waits for its batch.
class SparseBinding {
public:
	explicit SparseBinding(GraphicContext& graphics);
	~SparseBinding();
	KYTY_CLASS_NO_COPY(SparseBinding);

	void BindImage(SparseImageBinds&& binds);
	// Drops the queued binds of an image about to be destroyed.
	void Forget(vk::Image image);

	// The caller holds GraphicContext::queue_mutex and submits to the queue right after.
	[[nodiscard]] SparseWait Flush();
	void                     WaitIdle();

private:
	GraphicContext&               m_graphics;
	vk::Semaphore                 m_timeline = nullptr;
	uint64_t                      m_value    = 0;
	std::mutex                    m_mutex;
	std::vector<SparseImageBinds> m_images;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_SPARSEBINDING_H_
