#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GDSBUFFER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GDSBUFFER_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"

#include <memory>

namespace Libs::Graphics {

class GdsBuffer {
public:
	GdsBuffer() { EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread()); }
	~GdsBuffer() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(GdsBuffer);

	void Clear(GraphicContext* ctx, uint64_t dw_offset, uint32_t dw_num, uint32_t clear_value);
	void Read(GraphicContext* ctx, uint32_t* dst, uint32_t dw_offset, uint32_t dw_size);

	VulkanBuffer* GetBuffer(GraphicContext* ctx);

private:
	static constexpr uint64_t DW_SIZE = 0x3000;

	void Init(GraphicContext* ctx);

	Common::Mutex                 m_mutex;
	std::unique_ptr<VulkanBuffer> m_buffer;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GDSBUFFER_H_
