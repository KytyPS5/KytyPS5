#include "graphics/host_gpu/renderer/renderContext.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/presentation/window.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <algorithm>

namespace Libs::Graphics {

void RenderContext::SetGraphicCtx(GraphicContext* ctx) {
	EXIT_IF(ctx == nullptr);

	Common::LockGuard lock(m_graphic_ctx_mutex);

	m_graphic_ctx = ctx;
}

GraphicContext* RenderContext::GetGraphicCtx() {
	{
		Common::LockGuard lock(m_graphic_ctx_mutex);
		if (m_graphic_ctx != nullptr) {
			return m_graphic_ctx;
		}
	}

	WindowWaitForGraphicInitialized();

	auto* ctx = WindowGetGraphicContext();
	LOGF("Graphic context initialized\n ctx = %p\n", static_cast<void*>(ctx));
	EXIT_IF(ctx == nullptr);

	GraphicContext* ret = nullptr;
	{
		Common::LockGuard lock(m_graphic_ctx_mutex);
		if (m_graphic_ctx == nullptr) {
			m_graphic_ctx = ctx;
		}
		ret = m_graphic_ctx;
	}

	return ret;
}

void RenderContext::AddEopEq(LibKernel::EventQueue::KernelEqueue eq, int id) {
	Common::LockGuard lock(m_eop_mutex);

	auto it = std::find_if(m_eop_eqs.begin(), m_eop_eqs.end(), [eq, id](const auto& entry) {
		return entry.eq == eq && entry.id == id;
	});
	if (it != m_eop_eqs.end()) {
		it->count++;
		return;
	}

	m_eop_eqs.push_back({eq, id, 1});
}

void RenderContext::DeleteEopEq(LibKernel::EventQueue::KernelEqueue eq, int id) {
	Common::LockGuard lock(m_eop_mutex);

	auto it = std::find_if(m_eop_eqs.begin(), m_eop_eqs.end(), [eq, id](const auto& entry) {
		return entry.eq == eq && entry.id == id;
	});
	if (it == m_eop_eqs.end()) {
		return;
	}

	if (--it->count == 0) {
		m_eop_eqs.erase(it);
	}
}

void RenderContext::TriggerEopEvent(uint32_t context_id) {
	Common::LockGuard lock(m_eop_mutex);

	for (auto& eop_entry: m_eop_eqs) {
		if (eop_entry.eq != nullptr) {
			const auto id     = static_cast<uintptr_t>(eop_entry.id);
			auto       result = LibKernel::EventQueue::KernelTriggerEvent(
			    eop_entry.eq, id, LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS,
			    reinterpret_cast<void*>(static_cast<uintptr_t>(context_id)));
			EXIT_NOT_IMPLEMENTED(result != OK && result != LibKernel::KERNEL_ERROR_ENOENT);
		}
	}

	auto tsc    = LibKernel::KernelReadTsc();
	auto result = LibKernel::EventQueue::KernelTriggerUserEventForAll(AGC_USER_INTERRUPT_EVENT,
	                                                                  reinterpret_cast<void*>(tsc));
	EXIT_NOT_IMPLEMENTED(result != OK && result != LibKernel::KERNEL_ERROR_ENOENT);
}

} // namespace Libs::Graphics
