#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERCONTEXT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERCONTEXT_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/gpuResourceManager.h"
#include "graphics/host_gpu/renderer/cache/samplerCache.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/pipeline/descriptorCache.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "kernel/eventQueue.h"

#include <memory>
#include <vector>

namespace Libs::VideoOut {
class VideoOutDriver;
}

namespace Libs::Graphics {

constexpr int AGC_USER_INTERRUPT_EVENT = 0x1800;
class Gpu;

class RenderContext {
public:
	explicit RenderContext(GraphicContext& graphics);
	~RenderContext();
	KYTY_CLASS_NO_COPY(RenderContext);

	[[nodiscard]] GraphicContext&           GetGraphics() const noexcept { return m_graphics; }
	void                                    InitializeGpu(VideoOut::VideoOutDriver* video_out);
	void                                    ShutdownGpu();
	[[nodiscard]] Gpu&                      GetGpu() const;
	[[nodiscard]] VideoOut::VideoOutDriver& GetVideoOut() const;

	Common::Mutex&      GetMutex() { return m_mutex; }
	CommandScheduler&   GetCommandScheduler() { return m_command_scheduler; }
	PipelineCache&      GetPipelineCache() { return m_pipeline_cache; }
	DescriptorCache&    GetDescriptorCache() { return m_descriptor_cache; }
	SamplerCache&       GetSamplerCache() { return m_sampler_cache; }
	GpuResourceManager& GetGpuResources() { return m_gpu_resources; }
	BufferCache&        GetBufferCache() { return m_gpu_resources.GetBufferCache(); }
	TextureCache&       GetTextureCache() { return m_gpu_resources.GetTextureCache(); }
	RenderExecutor&     GetRenderExecutor() { return m_render_executor; }

	void AddEopEq(LibKernel::EventQueue::KernelEqueue eq, int id);
	void DeleteEopEq(LibKernel::EventQueue::KernelEqueue eq, int id);
	void TriggerEopEvent(uint32_t context_id);

private:
	struct EopEqRegistration {
		LibKernel::EventQueue::KernelEqueue    eq = LibKernel::EventQueue::KERNEL_EQUEUE_INVALID;
		LibKernel::EventQueue::KernelEqueueRef queue;
		int                                    id = 0;
	};

	GraphicContext&           m_graphics;
	Common::Mutex             m_mutex;
	RenderExecutor            m_render_executor;
	CommandScheduler          m_command_scheduler;
	DescriptorCache           m_descriptor_cache;
	PipelineCache             m_pipeline_cache;
	SamplerCache              m_sampler_cache;
	GpuResourceManager        m_gpu_resources;
	std::unique_ptr<Gpu>      m_gpu;
	VideoOut::VideoOutDriver* m_video_out = nullptr;

	Common::Mutex                  m_eop_mutex;
	std::vector<EopEqRegistration> m_eop_eqs;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERCONTEXT_H_
