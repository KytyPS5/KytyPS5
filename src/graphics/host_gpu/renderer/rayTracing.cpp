#include "graphics/host_gpu/renderer/rayTracing.h"

#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/renderContext.h"

namespace Libs::Graphics {


RayTracing::RayTracing(RenderContext& context)
    : m_graphics(context.GetGraphics()), m_scheduler(context.GetCommandScheduler()),
      m_buffer_cache(context.GetBufferCache()), m_context(context),
      m_arena(context.GetGraphics(), context.GetCommandScheduler()) {
	InitializeSlots();
}

RayTracing::~RayTracing() {
	DestroySlots();
}


vk::DeviceAddress RayTracing::Prepare(uint64_t tlas, uint64_t frame) {
	// The mirror reads the guest's BVH as it is now: every call is a device-address reader that comes after the
	// guest's writes, so the CPU writes since the last synchronization go up (see RenderContext::NoteOperation()).
	m_context.NoteOperation(0, true);
	m_context.PrepareBda(false);
	if (!m_enabled) {
		return 0;
	}

	TlasSource source;
	source.layout[3] = PsrHeaderLayout;
	source.header    = tlas;
	Prepare(m_scheduler.Current().Handle(), 0, source, {}, frame, 0);
	return m_context_address;
}

void RayTracing::RunGarbageCollector() {
	if (!m_enabled) {
		return;
	}
	Common::LockGuard lock(m_mutex);
	DestroyRetired();
}

} // namespace Libs::Graphics
