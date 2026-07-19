#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERCONTEXT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERCONTEXT_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/renderer/bufferCache.h"
#include "graphics/host_gpu/renderer/descriptorCache.h"
#include "graphics/host_gpu/renderer/framebufferCache.h"
#include "graphics/host_gpu/renderer/gdsBuffer.h"
#include "graphics/host_gpu/renderer/gpuResourceManager.h"
#include "graphics/host_gpu/renderer/pipelineCache.h"
#include "graphics/host_gpu/renderer/samplerCache.h"
#include "graphics/host_gpu/renderer/textureCache.h"
#include "kernel/eventQueue.h"

#include <vector>

namespace Libs::Graphics {

constexpr int AGC_USER_INTERRUPT_EVENT = 0x1800;

class RenderContext {
public:
	RenderContext() { EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread()); }
	~RenderContext() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(RenderContext);

	void            SetGraphicCtx(GraphicContext* ctx);
	GraphicContext* GetGraphicCtx();

	Common::Mutex&      GetMutex() { return m_mutex; }
	PipelineCache*      GetPipelineCache() { return &m_pipeline_cache; }
	DescriptorCache*    GetDescriptorCache() { return &m_descriptor_cache; }
	FramebufferCache*   GetFramebufferCache() { return &m_framebuffer_cache; }
	SamplerCache*       GetSamplerCache() { return &m_sampler_cache; }
	GdsBuffer*          GetGdsBuffer() { return &m_gds_buffer; }
	GpuResourceManager* GetGpuResources() { return &m_gpu_resources; }
	BufferCache*        GetBufferCache() { return m_gpu_resources.GetBufferCache(); }
	TextureCache*       GetTextureCache() { return m_gpu_resources.GetTextureCache(); }

	void AddEopEq(LibKernel::EventQueue::KernelEqueue eq, int id);
	void DeleteEopEq(LibKernel::EventQueue::KernelEqueue eq, int id);
	void TriggerEopEvent(uint32_t context_id);

private:
	struct EopEqRegistration {
		LibKernel::EventQueue::KernelEqueue eq    = nullptr;
		int                                 id    = 0;
		uint32_t                            count = 0;
	};

	Common::Mutex      m_mutex;
	Common::Mutex      m_graphic_ctx_mutex;
	PipelineCache      m_pipeline_cache;
	DescriptorCache    m_descriptor_cache;
	FramebufferCache   m_framebuffer_cache;
	SamplerCache       m_sampler_cache;
	GraphicContext*    m_graphic_ctx = nullptr;
	GdsBuffer          m_gds_buffer;
	GpuResourceManager m_gpu_resources;

	Common::Mutex                  m_eop_mutex;
	std::vector<EopEqRegistration> m_eop_eqs;
};

extern RenderContext* g_render_ctx;

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERCONTEXT_H_
