#include "graphics/host_gpu/renderer/renderContext.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/presentation/videoOut.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <algorithm>

namespace Libs::Graphics {

RenderContext::RenderContext(GraphicContext& graphics)
    : m_graphics(graphics), m_render_executor(*this), m_command_scheduler(*this, graphics),
      m_descriptor_cache(graphics), m_pipeline_cache(graphics, m_descriptor_cache),
      m_sampler_cache(graphics), m_gpu_resources(graphics, m_command_scheduler) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
}

RenderContext::~RenderContext() {
	ShutdownGpu();
	m_command_scheduler.Shutdown();
}

void RenderContext::InitializeGpu(VideoOut::VideoOutDriver* video_out) {
	EXIT_IF(m_gpu != nullptr);
	m_video_out = video_out;
	m_gpu       = std::make_unique<Gpu>(*this);
	m_gpu_resources.SetGpu(m_gpu.get());
}

void RenderContext::ShutdownGpu() {
	if (m_gpu != nullptr) {
		m_gpu_resources.SetGpu(nullptr);
		m_gpu->Shutdown();
		m_gpu.reset();
	}
	if (m_video_out != nullptr) {
		if (m_command_scheduler.Active()) {
			m_command_scheduler.FinishCurrent();
		}
		m_command_scheduler.DrainPriorityOperations();
		m_video_out = nullptr;
	}
}

Gpu& RenderContext::GetGpu() const {
	EXIT_IF(m_gpu == nullptr);
	return *m_gpu;
}

VideoOut::VideoOutDriver& RenderContext::GetVideoOut() const {
	EXIT_IF(m_video_out == nullptr);
	return *m_video_out;
}

void RenderContext::AddEopEq(LibKernel::EventQueue::KernelEqueue eq, int id) {
	auto queue = LibKernel::EventQueue::KernelPinEqueue(eq);
	if (!queue) {
		return;
	}

	Common::LockGuard lock(m_eop_mutex);

	auto it = std::find_if(m_eop_eqs.begin(), m_eop_eqs.end(), [eq, id](const auto& entry) {
		return entry.eq == eq && entry.id == id;
	});
	if (it != m_eop_eqs.end()) {
		return;
	}

	m_eop_eqs.push_back({eq, std::move(queue), id});
}

void RenderContext::DeleteEopEq(LibKernel::EventQueue::KernelEqueue eq, int id) {
	Common::LockGuard lock(m_eop_mutex);

	auto it = std::find_if(m_eop_eqs.begin(), m_eop_eqs.end(), [eq, id](const auto& entry) {
		return entry.eq == eq && entry.id == id;
	});
	if (it == m_eop_eqs.end()) {
		return;
	}

	m_eop_eqs.erase(it);
}

void RenderContext::TriggerEopEvent(uint32_t context_id) {
	std::vector<EopEqRegistration> registrations;
	{
		Common::LockGuard lock(m_eop_mutex);
		registrations = m_eop_eqs;
	}

	for (const auto& registration: registrations) {
		const auto result = LibKernel::EventQueue::KernelTriggerEvent(
		    registration.eq, static_cast<uintptr_t>(registration.id),
		    LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS,
		    reinterpret_cast<void*>(static_cast<uintptr_t>(context_id)));
		if (result == LibKernel::KERNEL_ERROR_EBADF || result == LibKernel::KERNEL_ERROR_ENOENT) {
			DeleteEopEq(registration.eq, registration.id);
			continue;
		}
		EXIT_NOT_IMPLEMENTED(result != OK);
	}

	auto tsc    = LibKernel::KernelReadTsc();
	auto result = LibKernel::EventQueue::KernelTriggerUserEventForAll(AGC_USER_INTERRUPT_EVENT,
	                                                                  reinterpret_cast<void*>(tsc));
	EXIT_NOT_IMPLEMENTED(result != OK && result != LibKernel::KERNEL_ERROR_ENOENT);
}

} // namespace Libs::Graphics
