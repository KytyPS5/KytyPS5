#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/objects/label.h"
#include "graphics/host_gpu/renderer/bufferCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderState.h"
#include "graphics/presentation/displayBuffer.h"
#include "kernel/eventQueue.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <cstring>

namespace Libs::Graphics {

constexpr int GRAPHICS_EVENT_QUEUED_GRAPHICS_INTERRUPT = 0x00;
constexpr int GRAPHICS_EVENT_EOP                       = 0x40;

static void SubmitLabel32(CommandBuffer* buffer, uint32_t* dst, uint32_t value,
                          LabelCallback callback_1 = nullptr, LabelCallback callback_2 = nullptr,
                          const uint64_t* args = nullptr) {
	auto* label =
	    LabelCreate32(g_render_ctx->GetGraphicCtx(), dst, value, callback_1, callback_2, args);
	LabelSet(buffer, label);
	LabelDelete(label);
}

static void SubmitLabel64(CommandBuffer* buffer, uint64_t* dst, uint64_t value,
                          LabelCallback callback_1 = nullptr, LabelCallback callback_2 = nullptr,
                          const uint64_t* args = nullptr) {
	auto* label =
	    LabelCreate64(g_render_ctx->GetGraphicCtx(), dst, value, callback_1, callback_2, args);
	LabelSet(buffer, label);
	LabelDelete(label);
}

template <typename T>
static void PublishImmediateFence(T* dst, T value) {
	static_assert(sizeof(T) == sizeof(uint32_t) || sizeof(T) == sizeof(uint64_t));
	EXIT_IF(dst == nullptr);
	// EOP/EOS fence payloads are constant and published while parsing the packet. Kyty performs the
	// direct mapped write under CommandProcessorGuestAccessScope: a protected access is first resolved by
	// BufferCache inline, then this instruction retries so the EOP payload wins over prior GPU bytes.
	std::memcpy(dst, &value, sizeof(value));
}

static void SubmitDisplayBufferFlip(const uint64_t* args) {
	if (g_render_ctx == nullptr || args == nullptr) {
		EXIT("GPU flip submission has invalid state, render_ctx=%p args=%p\n",
		     static_cast<const void*>(g_render_ctx), static_cast<const void*>(args));
	}
	const auto handle    = static_cast<int>(args[0]);
	const auto index     = static_cast<int>(args[1]);
	const auto flip_mode = static_cast<int>(args[2]);
	const auto flip_arg  = static_cast<int64_t>(args[3]);
	const auto result =
	    Presentation::DisplayBufferSubmitFlipFromGpu(handle, index, flip_mode, flip_arg);
	if (result != 0) {
		EXIT("GPU flip submission failed, result=%d handle=%d index=%d mode=%d arg=%" PRId64 "\n",
		     result, handle, index, flip_mode, flip_arg);
	}
}

static bool CompleteDisplayBufferFlip(const uint64_t* args) {
	if (g_render_ctx == nullptr || args == nullptr) {
		EXIT("GPU flip completion has invalid state, render_ctx=%p args=%p\n",
		     static_cast<const void*>(g_render_ctx), static_cast<const void*>(args));
	}
	Presentation::DisplayBufferCompleteFlipFromGpu(static_cast<int>(args[0]),
	                                               static_cast<int>(args[1]));
	return true;
}

void GraphicsRenderTriggerAgcUserInterrupt() {
	auto tsc    = LibKernel::KernelReadTsc();
	auto result = LibKernel::EventQueue::KernelTriggerUserEventForAll(AGC_USER_INTERRUPT_EVENT,
	                                                                  reinterpret_cast<void*>(tsc));
	EXIT_NOT_IMPLEMENTED(result != OK && result != LibKernel::KERNEL_ERROR_ENOENT);
}

void GraphicsRenderTriggerEopEvent(uint32_t context_id) {
	EXIT_IF(g_render_ctx == nullptr);
	g_render_ctx->TriggerEopEvent(context_id);
}

void GraphicsRenderWriteAtEndOfPipe32(uint64_t submit_id, CommandBuffer* buffer,
                                      uint32_t* dst_gpu_addr, uint32_t value) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWrite), submit_id, 4, value,
	                     0, 0, reinterpret_cast<uint64_t>(dst_gpu_addr));

	PublishImmediateFence(dst_gpu_addr, value);
}

void GraphicsRenderWriteAtEndOfPipeGds32(uint64_t submit_id, CommandBuffer* buffer,
                                         uint32_t* dst_gpu_addr, uint32_t dw_offset,
                                         uint32_t dw_num) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWrite), submit_id,
	                     dw_offset, dw_num, 0, 0, reinterpret_cast<uint64_t>(dst_gpu_addr));

	uint64_t args[LABEL_ARGS_MAX] = {static_cast<uint64_t>(dw_offset),
	                                 static_cast<uint64_t>(dw_num),
	                                 reinterpret_cast<uint64_t>(dst_gpu_addr), 0};

	SubmitLabel32(
	    buffer, dst_gpu_addr, 0,
	    [](const uint64_t* args) {
		    auto  dw_offset    = static_cast<uint32_t>(args[0]);
		    auto  dw_num       = static_cast<uint32_t>(args[1]);
		    auto* dst_gpu_addr = reinterpret_cast<uint32_t*>(args[2]);
		    if (dst_gpu_addr == nullptr || dw_num == 0) {
			    EXIT("invalid asynchronous EOP GDS write\n");
		    }
		    const auto size = static_cast<uint64_t>(dw_num) * sizeof(uint32_t);
		    auto transaction = g_render_ctx->GetGpuResources()->LockAsynchronousGuestWrite(
		        reinterpret_cast<uint64_t>(dst_gpu_addr), size);
		    g_render_ctx->GetGdsBuffer()->Read(g_render_ctx->GetGraphicCtx(), dst_gpu_addr,
		                                       dw_offset, dw_num);
		    return false;
	    },
	    nullptr, args);
}

void GraphicsRenderWriteAtEndOfPipe64(uint64_t submit_id, CommandBuffer* buffer,
                                      uint64_t* dst_gpu_addr, uint64_t value) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWrite), submit_id, 8,
	                     static_cast<uint32_t>(value), static_cast<uint32_t>(value >> 32u), 0,
	                     reinterpret_cast<uint64_t>(dst_gpu_addr));

	PublishImmediateFence(dst_gpu_addr, value);
}

void GraphicsRenderWriteAtEndOfPipeClockCounter(uint64_t submit_id, CommandBuffer* buffer,
                                                uint64_t* dst_gpu_addr) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWrite), submit_id, 8, 0, 0,
	                     0, reinterpret_cast<uint64_t>(dst_gpu_addr));

	const auto value = LibKernel::KernelReadTsc();
	PublishImmediateFence(dst_gpu_addr, value);
	LOGF_COLOR(Log::Color::BrightGreen,
	           "EndOfPipe Signal!!! [0x%016" PRIx64 "] <- Clock: 0x%016" PRIx64 "\n",
	           reinterpret_cast<uint64_t>(dst_gpu_addr), value);
}

void GraphicsRenderWriteAtEndOfPipeClockCounterWithWriteBack(uint64_t       submit_id,
                                                             CommandBuffer* buffer,
                                                             uint64_t*      dst_gpu_addr) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWriteBack), submit_id, 8, 0,
	                     0, 0, reinterpret_cast<uint64_t>(dst_gpu_addr));

	const auto value = LibKernel::KernelReadTsc();
	PublishImmediateFence(dst_gpu_addr, value);
	LOGF_COLOR(Log::Color::BrightGreen,
	           "EndOfPipe Signal!!! [0x%016" PRIx64 "] <- Clock: 0x%016" PRIx64 "\n",
	           reinterpret_cast<uint64_t>(dst_gpu_addr), value);
}

void GraphicsRenderWriteAtEndOfPipeWithWriteBack64(uint64_t submit_id, CommandBuffer* buffer,
                                                   uint64_t* dst_gpu_addr, uint64_t value) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWriteBack), submit_id, 8,
	                     static_cast<uint32_t>(value), static_cast<uint32_t>(value >> 32u), 0,
	                     reinterpret_cast<uint64_t>(dst_gpu_addr));

	PublishImmediateFence(dst_gpu_addr, value);
}

void GraphicsRenderWriteAtEndOfPipeWithWriteBack32(uint64_t submit_id, CommandBuffer* buffer,
                                                   uint32_t* dst_gpu_addr, uint32_t value) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWriteBack), submit_id, 4,
	                     value, 0, 0, reinterpret_cast<uint64_t>(dst_gpu_addr));

	PublishImmediateFence(dst_gpu_addr, value);
}

void GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBack64(uint64_t       submit_id,
                                                            CommandBuffer* buffer,
                                                            uint64_t* dst_gpu_addr, uint64_t value,
                                                            uint32_t context_id) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWriteBack), submit_id, 8,
	                     context_id, static_cast<uint32_t>(value),
	                     static_cast<uint32_t>(value >> 32u),
	                     reinterpret_cast<uint64_t>(dst_gpu_addr));
	PublishImmediateFence(dst_gpu_addr, value);

	uint64_t args[LABEL_ARGS_MAX] = {context_id != 0 ? context_id : value};

	SubmitLabel64(
	    buffer, nullptr, 0, nullptr,
	    [](const uint64_t* args) {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent(static_cast<uint32_t>(args[0]));
		    return true;
	    },
	    args);
}

void GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBack32(uint64_t       submit_id,
                                                            CommandBuffer* buffer,
                                                            uint32_t* dst_gpu_addr, uint32_t value,
                                                            uint32_t context_id) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWriteBack), submit_id, 4,
	                     context_id, value, 0, reinterpret_cast<uint64_t>(dst_gpu_addr));
	PublishImmediateFence(dst_gpu_addr, value);

	uint64_t args[LABEL_ARGS_MAX] = {context_id != 0 ? context_id : value};

	SubmitLabel32(
	    buffer, nullptr, 0, nullptr,
	    [](const uint64_t* args) {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent(static_cast<uint32_t>(args[0]));
		    return true;
	    },
	    args);
}

void GraphicsRenderWriteAtEndOfPipeWithInterrupt64(uint64_t submit_id, CommandBuffer* buffer,
                                                   uint64_t* dst_gpu_addr, uint64_t value,
                                                   uint32_t context_id) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopInterrupt), submit_id, 8,
	                     context_id, static_cast<uint32_t>(value),
	                     static_cast<uint32_t>(value >> 32u),
	                     reinterpret_cast<uint64_t>(dst_gpu_addr));
	PublishImmediateFence(dst_gpu_addr, value);

	uint64_t args[LABEL_ARGS_MAX] = {context_id != 0 ? context_id : value};

	SubmitLabel64(
	    buffer, nullptr, 0, nullptr,
	    [](const uint64_t* args) {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent(static_cast<uint32_t>(args[0]));
		    return true;
	    },
	    args);
}

void GraphicsRenderWriteAtEndOfPipeWithInterrupt32(uint64_t submit_id, CommandBuffer* buffer,
                                                   uint32_t* dst_gpu_addr, uint32_t value,
                                                   uint32_t context_id) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopInterrupt), submit_id, 4,
	                     context_id, value, 0, reinterpret_cast<uint64_t>(dst_gpu_addr));
	PublishImmediateFence(dst_gpu_addr, value);

	uint64_t args[LABEL_ARGS_MAX] = {context_id != 0 ? context_id : value};

	SubmitLabel32(
	    buffer, nullptr, 0, nullptr,
	    [](const uint64_t* args) {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent(static_cast<uint32_t>(args[0]));
		    return true;
	    },
	    args);
}

void GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBackFlip32(
    uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t value, int handle,
    int index, int flip_mode, int64_t flip_arg) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWriteBackFlip), submit_id,
	                     static_cast<uint32_t>(handle), static_cast<uint32_t>(index),
	                     static_cast<uint32_t>(flip_mode), value, static_cast<uint64_t>(flip_arg));
	PublishImmediateFence(dst_gpu_addr, value);

	uint64_t args[LABEL_ARGS_MAX] = {static_cast<uint64_t>(handle), static_cast<uint64_t>(index),
	                                 static_cast<uint64_t>(flip_mode),
	                                 static_cast<uint64_t>(flip_arg)};

	SubmitDisplayBufferFlip(args);
	SubmitLabel32(
	    buffer, nullptr, 0, CompleteDisplayBufferFlip,
	    [](const uint64_t* /*args*/) {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent(0);
		    return true;
	    },
	    args);
}

void GraphicsRenderWriteAtEndOfPipeWithFlip32(uint64_t submit_id, CommandBuffer* buffer,
                                              uint32_t* dst_gpu_addr, uint32_t value, int handle,
                                              int index, int flip_mode, int64_t flip_arg) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopFlip), submit_id,
	                     static_cast<uint32_t>(handle), static_cast<uint32_t>(index),
	                     static_cast<uint32_t>(flip_mode), value, static_cast<uint64_t>(flip_arg));
	PublishImmediateFence(dst_gpu_addr, value);

	uint64_t args[LABEL_ARGS_MAX] = {static_cast<uint64_t>(handle), static_cast<uint64_t>(index),
	                                 static_cast<uint64_t>(flip_mode),
	                                 static_cast<uint64_t>(flip_arg)};

	SubmitDisplayBufferFlip(args);
	SubmitLabel32(buffer, nullptr, 0, CompleteDisplayBufferFlip, nullptr, args);
}

void GraphicsRenderWriteAtEndOfPipeOnlyFlip(uint64_t submit_id, CommandBuffer* buffer, int handle,
                                            int index, int flip_mode, int64_t flip_arg) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopOnlyFlip), submit_id,
	                     static_cast<uint32_t>(handle), static_cast<uint32_t>(index),
	                     static_cast<uint32_t>(flip_mode), 0, static_cast<uint64_t>(flip_arg));

	uint64_t args[LABEL_ARGS_MAX] = {static_cast<uint64_t>(handle), static_cast<uint64_t>(index),
	                                 static_cast<uint64_t>(flip_mode),
	                                 static_cast<uint64_t>(flip_arg)};

	SubmitDisplayBufferFlip(args);
	SubmitLabel32(buffer, nullptr, 0, CompleteDisplayBufferFlip, nullptr, args);
}

void GraphicsRenderTriggerEopEventAtEndOfPipe(CommandBuffer* buffer, uint32_t context_id) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	uint64_t args[LABEL_ARGS_MAX] = {static_cast<uint64_t>(context_id)};

	SubmitLabel32(
	    buffer, nullptr, 0, nullptr,
	    [](const uint64_t* args) {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent(static_cast<uint32_t>(args[0]));
		    return true;
	    },
	    args);
}

static void EopEventResetFunc(LibKernel::EventQueue::KernelEqueueEvent* event) {
	EXIT_IF(event == nullptr);
	event->triggered    = false;
	event->event.fflags = 0;
	event->event.data   = 0;
}

static void EopEventDeleteFunc(LibKernel::EventQueue::KernelEqueue       eq,
                               LibKernel::EventQueue::KernelEqueueEvent* event) {
	EXIT_IF(event == nullptr);
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_NOT_IMPLEMENTED(event->event.filter != LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS);
	if (event->event.ident == GRAPHICS_EVENT_QUEUED_GRAPHICS_INTERRUPT ||
	    event->event.ident == GRAPHICS_EVENT_EOP) {
		g_render_ctx->DeleteEopEq(eq, static_cast<int>(event->event.ident));
	}
}

static void EopEventTriggerFunc(LibKernel::EventQueue::KernelEqueueEvent* event,
                                void*                                     trigger_data) {
	EXIT_IF(event == nullptr);

	auto triggered_event = event->event;
	triggered_event.fflags++;
	triggered_event.data = reinterpret_cast<intptr_t>(trigger_data);
	if (event->triggered) {
		event->pending_events.push_back(triggered_event);
	} else {
		event->event     = triggered_event;
		event->triggered = true;
	}
}

int GraphicsRenderAddEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id, void* udata) {
	EXIT_IF(g_render_ctx == nullptr);

	LibKernel::EventQueue::KernelEqueueEvent event;
	event.triggered                = false;
	event.event.ident              = static_cast<uintptr_t>(id);
	event.event.filter             = LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS;
	event.event.udata              = udata;
	event.event.fflags             = 0;
	event.event.data               = id;
	event.filter.delete_event_func = EopEventDeleteFunc;
	event.filter.reset_func        = EopEventResetFunc;
	event.filter.trigger_func      = EopEventTriggerFunc;
	event.filter.data              = nullptr;

	int result = LibKernel::EventQueue::KernelAddEvent(eq, event);

	if (id == GRAPHICS_EVENT_QUEUED_GRAPHICS_INTERRUPT || id == GRAPHICS_EVENT_EOP) {
		g_render_ctx->AddEopEq(eq, id);
	}

	return result;
}

int GraphicsRenderDeleteEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id) {
	EXIT_IF(g_render_ctx == nullptr);

	int result = LibKernel::EventQueue::KernelDeleteEvent(
	    eq, static_cast<uintptr_t>(id), LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS);

	return result;
}

void GraphicsRenderClearGds(uint64_t dw_offset, uint32_t dw_num, uint32_t clear_value) {
	EXIT_IF(g_render_ctx == nullptr);

	g_render_ctx->GetGdsBuffer()->Clear(g_render_ctx->GetGraphicCtx(), dw_offset, dw_num,
	                                    clear_value);
}

void GraphicsRenderReadGds(uint32_t* dst, uint32_t dw_offset, uint32_t dw_size) {
	EXIT_IF(g_render_ctx == nullptr);

	g_render_ctx->GetGdsBuffer()->Read(g_render_ctx->GetGraphicCtx(), dst, dw_offset, dw_size);
}

void GraphicsRenderDeleteBuffers() {
	EXIT_IF(g_render_ctx == nullptr);
	g_render_ctx->GetBufferCache()->DeleteAll(g_render_ctx->GetGraphicCtx());
}

} // namespace Libs::Graphics
