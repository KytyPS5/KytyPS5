#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SCRATCHRINGBUFFER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SCRATCHRINGBUFFER_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"

#include <memory>
#include <vector>

namespace Libs::Graphics {

// Host-owned SSBO approximating the RDNA2 private scratch ring (TMPRING).
// Shared by graphics and compute; grown to the max ring size observed.
class ScratchRingBuffer {
public:
	explicit ScratchRingBuffer(GraphicContext& graphics): m_graphics(graphics) {
		EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	}
	~ScratchRingBuffer() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(ScratchRingBuffer);

	void EnsureCapacity(uint64_t bytes);
	[[nodiscard]] uint64_t      Capacity() const;
	[[nodiscard]] VulkanBuffer& GetBuffer();

private:
	GraphicContext&                            m_graphics;
	mutable Common::Mutex                      m_mutex;
	std::unique_ptr<VulkanBuffer>              m_buffer;
	uint64_t                                   m_capacity = 0;
	std::vector<std::unique_ptr<VulkanBuffer>> m_retired;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SCRATCHRINGBUFFER_H_
