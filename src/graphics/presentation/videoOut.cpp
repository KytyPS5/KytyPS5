#include "graphics/presentation/videoOut.h"

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/timer.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/textureCache.h"
#include "graphics/presentation/displayBuffer.h"
#include "graphics/presentation/window.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <algorithm>
#include <limits>
#include <list>
#include <vector>

namespace Libs::Graphics {
struct GraphicContext;
} // namespace Libs::Graphics

namespace Libs::VideoOut {

LIB_NAME("VideoOut", "VideoOut");

namespace EventQueue = LibKernel::EventQueue;

constexpr int      VIDEO_OUT_EVENT_FLIP                                 = 0;
constexpr int      VIDEO_OUT_EVENT_VBLANK                               = 1;
constexpr int      VIDEO_OUT_EVENT_PRE_VBLANK_START                     = 2;
constexpr int      VIDEO_OUT_EVENT_SET_MODE                             = 8;
constexpr int      VIDEO_OUT_TRUE                                       = 1;
constexpr int      VIDEO_OUT_FALSE                                      = 0;
constexpr int      VIDEO_OUT_FLIP_MODE_VSYNC                            = 1;
constexpr int      VIDEO_OUT_FLIP_MODE_VSYNC_MULTI                      = 4;
constexpr int      VIDEO_OUT_BUFFER_INDEX_BLACK                         = -2;
constexpr int      VIDEO_OUT_BUFFER_INDEX_BLANK                         = -1;
constexpr int      VIDEO_OUT_BUFFER_NUM_MAX                             = 16;
constexpr size_t   VIDEO_OUT_FLIP_QUEUE_CAPACITY                        = 16;
constexpr int      VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX                   = 4;
constexpr uint64_t VIDEO_OUT_OUTPUT_MODE_DEFAULT                        = 0x0000000000000001ULL;
constexpr uint64_t VIDEO_OUT_OUTPUT_MODE_119_88HZ                       = 0x000000000000000FULL;
constexpr uint64_t VIDEO_OUT_REFRESH_RATE_59_94HZ                       = 3;
constexpr uint64_t VIDEO_OUT_REFRESH_RATE_119_88HZ                      = 13;
constexpr int      VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_UNCOMPRESSED     = 0;
constexpr int      VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_COMPRESSED       = 1;
constexpr uint64_t VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_STRICT_COLORIMETRY = 8;

enum class VideoOutEventKind : uintptr_t {
	Flip           = VIDEO_OUT_EVENT_FLIP,
	Vblank         = VIDEO_OUT_EVENT_VBLANK,
	PreVblankStart = VIDEO_OUT_EVENT_PRE_VBLANK_START,
	OutputMode     = VIDEO_OUT_EVENT_SET_MODE,
};

enum class FlipRequestSource { Cpu, GpuEop };

struct VideoOutBufferAttribute2 {
	uint32_t reserved0;
	uint32_t tiling_mode;
	uint32_t aspect_ratio;
	uint32_t width;
	uint32_t height;
	uint32_t pitch_in_pixel;
	uint64_t option;
	uint64_t pixel_format;
	uint64_t dcc_cb_register_clear_color;
	uint32_t dcc_control;
	uint32_t pad0;
	uint64_t reserved1[3];
};

// PS5 layout
struct VideoOutFlipStatus {
	uint64_t count                    = 0;
	uint64_t processTime              = 0;
	uint64_t reserved0                = 0;
	int64_t  flipArg                  = 0;
	uint64_t reserved1                = 0;
	uint64_t processTimeCounter       = 0;
	int32_t  gcQueueNum               = 0;
	int32_t  flipPendingNum           = 0;
	int32_t  currentBuffer            = 0;
	uint32_t reserved2                = 0;
	uint64_t submitProcessTimeCounter = 0;
	uint64_t reserved3[7]             = {};
};

// PS5 layout
struct VideoOutVblankStatus {
	uint64_t count              = 0;
	uint64_t processTime        = 0;
	uint64_t reserved           = 0;
	uint64_t processTimeCounter = 0;
	uint8_t  flags              = 0;
	uint8_t  phase              = 0;
	uint8_t  pad1[6]            = {};
};

struct VideoOutOutputStatus {
	uint32_t resolution   = 0;
	uint32_t dynamicRange = 0;
	uint64_t refreshRate  = 0;
	uint64_t flags        = 0;
	uint64_t reserved[3]  = {};
};

struct VideoOutOutputOptions {
	uint32_t internalData[16] = {};
};

struct VideoOutColorSettings {
	float    gamma       = 1.0f;
	uint32_t reserved[3] = {};
};

struct VideoOutBuffers {
	const void* data;
	const void* metadata;
	const void* reserved[2];
};

struct VideoOutBufferSet {
	int start_index = 0;
	int num         = 0;
	int set_id      = 0;
};

struct VideoOutBufferInfo {
	const void*                    buffer        = nullptr;
	Graphics::VideoOutVulkanImage* buffer_vulkan = nullptr;
	uint64_t                       buffer_size   = 0;
	uint64_t                       buffer_pitch  = 0;
	int                            set_id        = 0;
};

struct VideoOutConfig {
	Common::Mutex                         mutex;
	uint32_t                              width             = 0;
	uint32_t                              height            = 0;
	bool                                  opened            = false;
	bool                                  closing           = false;
	bool                                  unregistering[16] = {};
	int                                   flip_rate         = 0;
	std::vector<EventQueue::KernelEqueue> flip_eqs;
	std::vector<EventQueue::KernelEqueue> pre_vblank_eqs;
	std::vector<EventQueue::KernelEqueue> vblank_eqs;
	std::vector<EventQueue::KernelEqueue> output_mode_eqs;
	uint64_t                              output_mode = VIDEO_OUT_OUTPUT_MODE_DEFAULT;
	float                                 gamma       = 1.0f;
	VideoOutFlipStatus                    flip_status;
	VideoOutVblankStatus                  pre_vblank_status;
	VideoOutVblankStatus                  vblank_status;
	VideoOutBufferInfo                    buffers[16];
	std::vector<VideoOutBufferSet>        buffers_sets;
};

class FlipQueue {
public:
	FlipQueue() { EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread()); }
	~FlipQueue() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(FlipQueue);

	bool     Reserve(VideoOutConfig* cfg, int index, int64_t flip_arg, FlipRequestSource source,
	                 uint64_t* request_id);
	void     Prepare(uint64_t request_id, Graphics::CommandBuffer* buffer);
	uint64_t PrepareNextCpu(Graphics::CommandBuffer* buffer);
	void     Complete(uint64_t request_id);
	void     WaitForSubmitSlot();
	bool     Flip(uint32_t micros);
	bool     HasPending(VideoOutConfig* cfg, int start_index, int count);
	void     GetFlipStatus(VideoOutConfig* cfg, VideoOutFlipStatus* out);
	void     Wait(VideoOutConfig* cfg, int index);

private:
	enum class RequestState { Reserved, Recording, Ready, Presenting };

	struct Request {
		uint64_t                 id;
		VideoOutConfig*          cfg;
		int                      index;
		int64_t                  flip_arg;
		uint64_t                 submit_ptc;
		FlipRequestSource        source;
		RequestState             state;
		Graphics::PreparedFrame* frame;
	};

	Common::Mutex      m_mutex;
	Common::CondVar    m_submit_cond_var;
	Common::CondVar    m_submit_slot_cond_var;
	Common::CondVar    m_done_cond_var;
	std::list<Request> m_requests;
	std::list<Request> m_cpu_requests;
	bool               m_processing      = false;
	uint64_t           m_next_request_id = 1;
};

class VideoOutContext {
public:
	static constexpr int VIDEO_OUT_NUM_MAX = 2;

	VideoOutContext() { EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread()); }
	~VideoOutContext() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(VideoOutContext);

	int             Open();
	void            Close(int handle);
	VideoOutConfig* Get(int handle);
	bool            IsOpened(int handle);

	Presentation::DisplayBufferImage FindImage(const void* buffer, bool render_target);

	void Init(uint32_t width, uint32_t height);

	Graphics::GraphicContext* GetGraphicCtx() {
		Common::LockGuard lock(m_mutex);

		if (m_graphic_ctx == nullptr) {
			m_graphic_ctx = Graphics::WindowGetGraphicContext();
		}

		return m_graphic_ctx;
	}

	FlipQueue& GetFlipQueue() { return m_flip_queue; }

	void VblankBegin();
	void VblankEnd();

private:
	Common::Mutex             m_mutex;
	VideoOutConfig            m_video_out_ctx[VIDEO_OUT_NUM_MAX];
	Graphics::GraphicContext* m_graphic_ctx = nullptr;
	FlipQueue                 m_flip_queue;
};

static VideoOutContext* g_video_out_context = nullptr;

using VideoOutEventQueues = std::vector<EventQueue::KernelEqueue>;

static uintptr_t VideoOutEventId(VideoOutEventKind kind) {
	return static_cast<uintptr_t>(kind);
}

static VideoOutEventKind GetVideoOutEventKind(uintptr_t event_id) {
	switch (event_id) {
		case VIDEO_OUT_EVENT_FLIP: return VideoOutEventKind::Flip;
		case VIDEO_OUT_EVENT_VBLANK: return VideoOutEventKind::Vblank;
		case VIDEO_OUT_EVENT_PRE_VBLANK_START: return VideoOutEventKind::PreVblankStart;
		case VIDEO_OUT_EVENT_SET_MODE: return VideoOutEventKind::OutputMode;
		default: EXIT("unsupported video-out event id=%" PRIuPTR "\n", event_id);
	}
	return VideoOutEventKind::Flip;
}

static VideoOutEventQueues& VideoOutEventQueuesFor(VideoOutConfig&   video_out,
                                                   VideoOutEventKind kind) {
	switch (kind) {
		case VideoOutEventKind::Flip: return video_out.flip_eqs;
		case VideoOutEventKind::Vblank: return video_out.vblank_eqs;
		case VideoOutEventKind::PreVblankStart: return video_out.pre_vblank_eqs;
		case VideoOutEventKind::OutputMode: return video_out.output_mode_eqs;
	}
	EXIT("unsupported video-out event kind\n");
	return video_out.flip_eqs;
}

static intptr_t MakeVideoOutEventData(intptr_t current_data, void* trigger_data) {
	const uint64_t old_data = static_cast<uint64_t>(current_data);
	uint64_t       counter  = (old_data >> 12u) & 0xfu;
	if (counter != 0xfu) {
		counter++;
	}

	const uint64_t time    = LibKernel::KernelReadTsc() & 0xfffu;
	const uint64_t payload = static_cast<uint64_t>(reinterpret_cast<intptr_t>(trigger_data));

	return static_cast<intptr_t>(time | (counter << 12u) |
	                             ((payload & 0x0000ffffffffffffULL) << 16u));
}

static void ResetVideoOutEvent(EventQueue::KernelEqueueEvent* event) {
	EXIT_IF(event == nullptr);
	event->triggered    = false;
	event->event.fflags = 0;
	event->event.data   = 0;
}

static void TriggerVideoOutEvent(EventQueue::KernelEqueueEvent* event, void* trigger_data) {
	EXIT_IF(event == nullptr);

	auto triggered_event = event->event;
	triggered_event.fflags =
	    triggered_event.fflags < 0xfu ? triggered_event.fflags + 1u : triggered_event.fflags;
	triggered_event.data = MakeVideoOutEventData(triggered_event.data, trigger_data);
	if (event->triggered) {
		event->pending_events.push_back(triggered_event);
		return;
	}
	event->event     = triggered_event;
	event->triggered = true;
}

static void RemoveVideoOutEventQueue(EventQueue::KernelEqueue       eq,
                                     EventQueue::KernelEqueueEvent* event) {
	EXIT_IF(event == nullptr);
	EXIT_IF(event->filter.data == nullptr);
	EXIT_NOT_IMPLEMENTED(event->event.filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT);

	auto* video_out = static_cast<VideoOutConfig*>(event->filter.data);
	auto& queues    = VideoOutEventQueuesFor(*video_out, GetVideoOutEventKind(event->event.ident));
	Common::LockGuard lock(video_out->mutex);
	EXIT_IF(queues.empty());
	const auto entry = std::find(queues.begin(), queues.end(), eq);
	EXIT_NOT_IMPLEMENTED(entry == queues.end());
	*entry = nullptr;
}

static void TriggerVideoOutEventsLocked(const VideoOutEventQueues& queues, VideoOutEventKind kind,
                                        void* trigger_data) {
	for (auto eq: queues) {
		if (eq == nullptr) {
			continue;
		}
		const auto result = EventQueue::KernelTriggerEvent(
		    eq, VideoOutEventId(kind), EventQueue::KERNEL_EVFILT_VIDEO_OUT, trigger_data);
		EXIT_NOT_IMPLEMENTED(result != OK);
	}
}

static void DeleteVideoOutEventsLocked(VideoOutEventQueues& queues, VideoOutEventKind kind) {
	for (auto eq: queues) {
		if (eq == nullptr) {
			continue;
		}
		const auto result = EventQueue::KernelDeleteEvent(eq, VideoOutEventId(kind),
		                                                  EventQueue::KERNEL_EVFILT_VIDEO_OUT);
		EXIT_NOT_IMPLEMENTED(result != OK);
	}
	queues.clear();
}

static int RegisterVideoOutEvent(int handle, EventQueue::KernelEqueue eq, VideoOutEventKind kind,
                                 void* udata) {
	EXIT_IF(g_video_out_context == nullptr);

	auto* video_out = g_video_out_context->Get(handle);
	if (video_out == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	Common::LockGuard lock(video_out->mutex);
	if (kind == VideoOutEventKind::OutputMode) {
		LOGF("\t eq     = 0x%016" PRIx64 "\n"
		     "\t handle = %d\n"
		     "\t udata  = 0x%016" PRIx64 "\n",
		     reinterpret_cast<uint64_t>(eq), handle, reinterpret_cast<uint64_t>(udata));
	}
	if (eq == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}
	auto&       queues              = VideoOutEventQueuesFor(*video_out, kind);
	const bool  add_queue           = std::find(queues.begin(), queues.end(), eq) == queues.end();
	const bool  initially_triggered = kind == VideoOutEventKind::OutputMode;
	void* const initial_trigger_data =
	    initially_triggered ? reinterpret_cast<void*>(video_out->output_mode) : nullptr;

	EventQueue::KernelEqueueEvent event {};
	event.triggered    = initially_triggered;
	event.event.ident  = VideoOutEventId(kind);
	event.event.filter = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
	event.event.udata  = udata;
	event.event.fflags = initially_triggered ? 1u : 0u;
	event.event.data   = initially_triggered ? MakeVideoOutEventData(0, initial_trigger_data) : 0;
	event.filter.delete_event_func = RemoveVideoOutEventQueue;
	event.filter.reset_func        = ResetVideoOutEvent;
	event.filter.trigger_func      = TriggerVideoOutEvent;
	event.filter.data              = video_out;

	const int result = EventQueue::KernelAddEvent(eq, event);
	if (result == OK && add_queue) {
		queues.push_back(eq);
	}
	return result;
}

static int DeleteVideoOutEvent(int handle, EventQueue::KernelEqueue eq, VideoOutEventKind kind) {
	EXIT_IF(g_video_out_context == nullptr);

	if (g_video_out_context->Get(handle) == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	if (eq == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}
	return EventQueue::KernelDeleteEvent(eq, VideoOutEventId(kind),
	                                     EventQueue::KERNEL_EVFILT_VIDEO_OUT);
}

static void WaitForNextVblank() {
	static uint64_t next_vblank_ticks = 0;

	const uint64_t frequency = Common::Timer::QueryPerformanceFrequency();
	const uint64_t period    = frequency / Config::GetVblankFrequency();
	uint64_t       now       = Common::Timer::QueryPerformanceCounter();

	if (next_vblank_ticks == 0) {
		next_vblank_ticks = now;
	}

	if (now < next_vblank_ticks) {
		const uint64_t wait_ticks  = next_vblank_ticks - now;
		const uint64_t wait_micros = (wait_ticks * 1000000u) / frequency;

		if (wait_micros > 0) {
			Common::Thread::SleepMicro(
			    static_cast<uint32_t>(std::min<uint64_t>(wait_micros, UINT32_MAX)));
		}

		now = Common::Timer::QueryPerformanceCounter();
	}

	do {
		next_vblank_ticks += period;
	} while (next_vblank_ticks <= now);
}

static bool IsFlipDue(VideoOutConfig* cfg) {
	EXIT_IF(cfg == nullptr);

	Common::LockGuard lock(cfg->mutex);

	const int interval = cfg->flip_rate + 1;

	return interval <= 1 || (cfg->vblank_status.count % static_cast<uint64_t>(interval)) == 0;
}

static bool IsValidBufferIndex(int index) {
	return index >= VIDEO_OUT_BUFFER_INDEX_BLACK && index < VIDEO_OUT_BUFFER_NUM_MAX;
}

static bool IsSpecialBufferIndex(int index) {
	return index == VIDEO_OUT_BUFFER_INDEX_BLANK || index == VIDEO_OUT_BUFFER_INDEX_BLACK;
}

static bool IsValidFlipMode(int mode) {
	return mode >= VIDEO_OUT_FLIP_MODE_VSYNC && mode <= VIDEO_OUT_FLIP_MODE_VSYNC_MULTI;
}

static int ReserveFlipRequest(int handle, int index, int flip_mode, int64_t flip_arg,
                              FlipRequestSource source, uint64_t* request_id) {
	EXIT_IF(g_video_out_context == nullptr || request_id == nullptr);

	auto* video_out = g_video_out_context->Get(handle);
	if (video_out == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	if (!IsValidFlipMode(flip_mode)) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}
	if (!IsValidBufferIndex(index)) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}

	Common::LockGuard lock(video_out->mutex);
	if (video_out->closing ||
	    (!IsSpecialBufferIndex(index) &&
	     (video_out->unregistering[index] || video_out->buffers[index].buffer_vulkan == nullptr))) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}
	if (!g_video_out_context->GetFlipQueue().Reserve(video_out, index, flip_arg, source,
	                                                 request_id)) {
		return VIDEO_OUT_ERROR_FLIP_QUEUE_FULL;
	}
	return OK;
}

static Graphics::VideoOutInfo MakeVideoOutInfo(const VideoOutBufferAttribute2& attribute,
                                               uint64_t address, uint64_t metadata_address,
                                               Graphics::VideoOutCompression compression) {
	if (attribute.reserved0 != 0 || attribute.aspect_ratio != 0 || attribute.width == 0 ||
	    attribute.height == 0 || attribute.width > 16384 || attribute.height > 16384 ||
	    attribute.pitch_in_pixel != 0 ||
	    (attribute.option != 0 &&
	     attribute.option != VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_STRICT_COLORIMETRY) ||
	    attribute.tiling_mode != 0 || attribute.pad0 != 0 || attribute.reserved1[0] != 0 ||
	    attribute.reserved1[1] != 0 || attribute.reserved1[2] != 0 || address == 0 ||
	    compression == Graphics::VideoOutCompression::Unsupported) {
		EXIT("unsupported or invalid video-out surface attributes\n");
	}
	Graphics::VideoOutPixelFormatInfo pixel_format {};
	if (!Graphics::DecodeVideoOutPixelFormat(attribute.pixel_format, &pixel_format)) {
		EXIT("unsupported video-out pixel format: 0x%016" PRIx64 "\n", attribute.pixel_format);
	}
	const auto tile_mode =
	    Graphics::Prospero::GpuEnumValue(Graphics::Prospero::TileMode::kRenderTarget);
	const auto pitch =
	    Graphics::TileGetTexturePitch(pixel_format.guest_format, attribute.width, 1, tile_mode);
	Graphics::TileSizeAlign total {};
	Graphics::TileGetTextureTotalSize(pixel_format.guest_format, attribute.width, attribute.height,
	                                  1, pitch, 1, tile_mode, false, &total);
	if (total.size == 0 || total.align != 65536 || (address & (total.align - 1u)) != 0) {
		EXIT("invalid video-out surface footprint or alignment\n");
	}
	Graphics::VideoOutInfo info {};
	info.address           = address;
	info.size              = total.size;
	info.metadata_address  = metadata_address;
	info.format            = pixel_format.format;
	info.guest_format      = pixel_format.guest_format;
	info.width             = attribute.width;
	info.height            = attribute.height;
	info.pitch             = pitch;
	info.bytes_per_element = pixel_format.bytes_per_element;
	info.tile_mode         = tile_mode;
	info.dcc_control       = attribute.dcc_control;
	info.compression       = compression;
	info.bgra16            = pixel_format.bgra16;
	return info;
}

void VideoOutInit(uint32_t width, uint32_t height) {
	EXIT_IF(g_video_out_context != nullptr);

	g_video_out_context = new VideoOutContext;

	g_video_out_context->Init(width, height);
}

void VideoOutContext::Init(uint32_t width, uint32_t height) {
	for (auto& ctx: m_video_out_ctx) {
		ctx.width  = width;
		ctx.height = height;
	}
}

int VideoOutContext::Open() {
	Common::LockGuard lock(m_mutex);

	int handle = -1;

	for (int i = 1; i < VIDEO_OUT_NUM_MAX; i++) {
		if (!m_video_out_ctx[i].opened) {
			handle = i;
			break;
		}
	}

	if (handle < 0) {
		return -1;
	}
	auto&             config = m_video_out_ctx[handle];
	Common::LockGuard config_lock(config.mutex);

	EXIT_IF(!config.flip_eqs.empty());
	EXIT_IF(!config.pre_vblank_eqs.empty());
	EXIT_IF(!config.vblank_eqs.empty());
	EXIT_IF(!config.output_mode_eqs.empty());
	EXIT_IF(config.flip_rate != 0);
	EXIT_IF(!config.buffers_sets.empty());
	for (const auto& buffer: config.buffers) {
		EXIT_IF(buffer.buffer != nullptr || buffer.buffer_vulkan != nullptr ||
		        buffer.buffer_size != 0 || buffer.buffer_pitch != 0);
	}
	for (bool unregistering: config.unregistering) {
		EXIT_IF(unregistering);
	}

	config.closing                   = false;
	config.opened                    = true;
	config.output_mode               = VIDEO_OUT_OUTPUT_MODE_DEFAULT;
	config.flip_status               = VideoOutFlipStatus();
	config.flip_status.flipArg       = -1;
	config.flip_status.currentBuffer = -1;
	config.flip_status.count         = 0;
	config.pre_vblank_status         = VideoOutVblankStatus();
	config.vblank_status             = VideoOutVblankStatus();

	return handle;
}

void VideoOutContext::Close(int handle) {
	Common::LockGuard lock(m_mutex);

	EXIT_NOT_IMPLEMENTED(handle >= VIDEO_OUT_NUM_MAX);
	EXIT_NOT_IMPLEMENTED(!m_video_out_ctx[handle].opened);

	auto& config  = m_video_out_ctx[handle];
	config.opened = false;

	config.mutex.Lock();
	if (config.closing) {
		EXIT("video-out handle is already closing\n");
	}
	config.closing = true;
	if (m_flip_queue.HasPending(&config, VIDEO_OUT_BUFFER_INDEX_BLACK,
	                            VIDEO_OUT_BUFFER_NUM_MAX - VIDEO_OUT_BUFFER_INDEX_BLACK)) {
		EXIT("cannot close video-out handle with pending flips\n");
	}
	DeleteVideoOutEventsLocked(config.flip_eqs, VideoOutEventKind::Flip);
	DeleteVideoOutEventsLocked(config.pre_vblank_eqs, VideoOutEventKind::PreVblankStart);
	DeleteVideoOutEventsLocked(config.vblank_eqs, VideoOutEventKind::Vblank);
	DeleteVideoOutEventsLocked(config.output_mode_eqs, VideoOutEventKind::OutputMode);

	config.flip_rate = 0;

	std::vector<Graphics::VideoOutVulkanImage*> images;
	for (const auto& buffer: config.buffers) {
		if ((buffer.buffer == nullptr) != (buffer.buffer_vulkan == nullptr) ||
		    (buffer.buffer_vulkan != nullptr &&
		     (buffer.buffer_size == 0 || buffer.buffer_pitch == 0))) {
			EXIT("inconsistent registered video-out buffer state\n");
		}
		if (buffer.buffer_vulkan != nullptr) {
			images.push_back(buffer.buffer_vulkan);
		}
	}
	if (!images.empty()) {
		if (Graphics::g_render_ctx == nullptr) {
			EXIT("cannot unregister video-out surfaces without a render context\n");
		}
		Graphics::g_render_ctx->GetTextureCache()->UnregisterVideoOutSurfaces(images);
	}
	for (auto& buffer: config.buffers) {
		buffer = VideoOutBufferInfo {};
	}

	config.buffers_sets.clear();
	for (bool unregistering: config.unregistering) {
		if (unregistering) {
			EXIT("video-out close raced with buffer unregistration\n");
		}
	}
	config.mutex.Unlock();
}

VideoOutConfig* VideoOutContext::Get(int handle) {
	Common::LockGuard lock(m_mutex);
	if (handle <= 0 || handle >= VIDEO_OUT_NUM_MAX || !m_video_out_ctx[handle].opened) {
		return nullptr;
	}

	return m_video_out_ctx + handle;
}

bool VideoOutContext::IsOpened(int handle) {
	Common::LockGuard lock(m_mutex);

	return handle > 0 && handle < VIDEO_OUT_NUM_MAX && m_video_out_ctx[handle].opened;
}

void VideoOutContext::VblankBegin() {
	Common::LockGuard lock(m_mutex);

	for (int i = 1; i < VIDEO_OUT_NUM_MAX; i++) {
		auto& ctx = m_video_out_ctx[i];
		if (ctx.opened) {
			ctx.mutex.Lock();
			ctx.pre_vblank_status.count++;
			ctx.pre_vblank_status.processTime        = LibKernel::KernelGetProcessTime();
			ctx.pre_vblank_status.reserved           = LibKernel::KernelReadTsc();
			ctx.pre_vblank_status.processTimeCounter = LibKernel::KernelGetProcessTimeCounter();

			TriggerVideoOutEventsLocked(ctx.pre_vblank_eqs, VideoOutEventKind::PreVblankStart,
			                            reinterpret_cast<void*>(ctx.pre_vblank_status.count));
			ctx.mutex.Unlock();
		}
	}
}

void VideoOutContext::VblankEnd() {
	Common::LockGuard lock(m_mutex);

	for (int i = 1; i < VIDEO_OUT_NUM_MAX; i++) {
		auto& ctx = m_video_out_ctx[i];
		if (ctx.opened) {
			ctx.mutex.Lock();
			ctx.vblank_status.count++;
			ctx.vblank_status.processTime        = LibKernel::KernelGetProcessTime();
			ctx.vblank_status.reserved           = LibKernel::KernelReadTsc();
			ctx.vblank_status.processTimeCounter = LibKernel::KernelGetProcessTimeCounter();

			TriggerVideoOutEventsLocked(ctx.vblank_eqs, VideoOutEventKind::Vblank,
			                            reinterpret_cast<void*>(ctx.vblank_status.count));
			ctx.mutex.Unlock();
		}
	}
}

Presentation::DisplayBufferImage VideoOutContext::FindImage(const void* buffer,
                                                            bool        render_target) {
	Presentation::DisplayBufferImage ret;
	Common::LockGuard                lock(m_mutex);
	for (auto& ctx: m_video_out_ctx) {
		if (!ctx.opened) {
			continue;
		}
		Common::LockGuard config_lock(ctx.mutex);
		if (ctx.closing) {
			EXIT("display-buffer lookup raced with video-out close\n");
		}
		for (const auto& set: ctx.buffers_sets) {
			for (int j = set.start_index; j < set.start_index + set.num; j++) {
				if (ctx.buffers[j].buffer == buffer) {
					if (ctx.unregistering[j] || ctx.buffers[j].buffer_vulkan == nullptr) {
						EXIT("display-buffer lookup found an unavailable video-out surface\n");
					}
					ret.image = ctx.buffers[j].buffer_vulkan;
					ret.size  = ctx.buffers[j].buffer_size;
					ret.pitch = ctx.buffers[j].buffer_pitch;
					ret.index = j - set.start_index;
					Graphics::g_render_ctx->GetTextureCache()->RefreshVideoOut(ret.image,
					                                                           render_target);
					return ret;
				}
			}
		}
	}
	return ret;
}

bool FlipQueue::Reserve(VideoOutConfig* cfg, int index, int64_t flip_arg, FlipRequestSource source,
                        uint64_t* request_id) {
	EXIT_IF(cfg == nullptr || request_id == nullptr);
	Common::LockGuard lock(m_mutex);

	if (m_requests.size() + m_cpu_requests.size() >= VIDEO_OUT_FLIP_QUEUE_CAPACITY) {
		return false;
	}
	auto& pending = source == FlipRequestSource::GpuEop ? m_requests : m_cpu_requests;

	Request r {};
	r.id         = m_next_request_id++;
	r.cfg        = cfg;
	r.index      = index;
	r.flip_arg   = flip_arg;
	r.submit_ptc = LibKernel::KernelGetProcessTimeCounter();
	r.source     = source;
	r.state      = RequestState::Reserved;

	pending.push_back(r);
	*request_id = r.id;

	cfg->flip_status.flipPendingNum = static_cast<int>(m_requests.size() + m_cpu_requests.size());
	cfg->flip_status.submitProcessTimeCounter = r.submit_ptc;
	if (source == FlipRequestSource::GpuEop) {
		cfg->flip_status.gcQueueNum++;
	}

	return true;
}

void FlipQueue::Prepare(uint64_t request_id, Graphics::CommandBuffer* buffer) {
	EXIT_IF(buffer == nullptr);

	VideoOutConfig* cfg   = nullptr;
	int             index = 0;
	{
		Common::LockGuard lock(m_mutex);
		auto request = std::find_if(m_requests.begin(), m_requests.end(),
		                            [request_id](const auto& r) { return r.id == request_id; });
		if (request == m_requests.end()) {
			auto pending = std::find_if(m_cpu_requests.begin(), m_cpu_requests.end(),
			                            [request_id](const auto& r) { return r.id == request_id; });
			if (pending == m_cpu_requests.end()) {
				EXIT("cannot prepare video-out request id=%" PRIu64 "\n", request_id);
			}
			request = m_requests.insert(m_requests.end(), *pending);
			m_cpu_requests.erase(pending);
		}
		if (request->state != RequestState::Reserved) {
			EXIT("cannot prepare video-out request id=%" PRIu64 "\n", request_id);
		}
		request->state = RequestState::Recording;
		cfg            = request->cfg;
		index          = request->index;
	}

	const bool                     special = IsSpecialBufferIndex(index);
	Graphics::VideoOutVulkanImage* source  = nullptr;
	uint32_t                       width   = 0;
	uint32_t                       height  = 0;
	{
		Common::LockGuard lock(cfg->mutex);
		if (cfg->closing) {
			EXIT("cannot prepare flip for a closing video-out, id=%" PRIu64 "\n", request_id);
		}
		if (special) {
			width  = cfg->width;
			height = cfg->height;
		} else {
			if (cfg->unregistering[index]) {
				EXIT("cannot prepare flip from an unavailable surface, id=%" PRIu64 " index=%d\n",
				     request_id, index);
			}
			source = cfg->buffers[index].buffer_vulkan;
			if (source == nullptr) {
				EXIT("cannot prepare flip without a native surface, id=%" PRIu64 " index=%d\n",
				     request_id, index);
			}
		}
	}
	auto* frame = special ? Graphics::WindowPrepareBlankFrame(buffer, width, height,
	                                                          index == VIDEO_OUT_BUFFER_INDEX_BLACK)
	                      : Graphics::WindowPrepareFrame(buffer, source);

	Common::LockGuard lock(m_mutex);
	auto request = std::find_if(m_requests.begin(), m_requests.end(),
	                            [request_id](const auto& r) { return r.id == request_id; });
	if (request == m_requests.end() || request->state != RequestState::Recording ||
	    request->frame != nullptr) {
		EXIT("video-out request changed while recording, id=%" PRIu64 "\n", request_id);
	}
	request->frame = frame;
}

uint64_t FlipQueue::PrepareNextCpu(Graphics::CommandBuffer* buffer) {
	uint64_t request_id = 0;
	{
		Common::LockGuard lock(m_mutex);
		if (m_cpu_requests.empty()) {
			EXIT("CPU flip preparation has no accepted request\n");
		}
		request_id = m_cpu_requests.front().id;
	}
	Prepare(request_id, buffer);
	return request_id;
}

void FlipQueue::Complete(uint64_t request_id) {
	Common::LockGuard lock(m_mutex);
	auto request = std::find_if(m_requests.begin(), m_requests.end(),
	                            [request_id](const auto& r) { return r.id == request_id; });
	if (request == m_requests.end() || request->state != RequestState::Recording ||
	    request->frame == nullptr) {
		EXIT("completed GPU flip has no prepared recording, id=%" PRIu64 "\n", request_id);
	}
	request->state = RequestState::Ready;
	m_submit_cond_var.Signal();
}

void FlipQueue::WaitForSubmitSlot() {
	Common::LockGuard lock(m_mutex);
	while (m_requests.size() + m_cpu_requests.size() >= VIDEO_OUT_FLIP_QUEUE_CAPACITY) {
		if (m_requests.empty()) {
			EXIT("video-out queue is saturated by CPU flips queued behind the current EOP\n");
		}
		m_submit_slot_cond_var.Wait(&m_mutex);
	}
}

void FlipQueue::Wait(VideoOutConfig* cfg, int index) {
	Common::LockGuard lock(m_mutex);

	auto has_request = [this, cfg, index] {
		auto matches = [cfg, index](const auto& r) { return r.cfg == cfg && r.index == index; };
		return std::any_of(m_requests.begin(), m_requests.end(), matches) ||
		       std::any_of(m_cpu_requests.begin(), m_cpu_requests.end(), matches);
	};
	while (has_request()) {
		m_done_cond_var.Wait(&m_mutex);
	}
}

bool FlipQueue::HasPending(VideoOutConfig* cfg, int start_index, int count) {
	if (cfg == nullptr || count <= 0 || start_index > INT_MAX - count) {
		EXIT("invalid video-out pending-flip query range\n");
	}
	Common::LockGuard lock(m_mutex);
	auto              matches = [&](const auto& request) {
		return request.cfg == cfg && request.index >= start_index &&
		       request.index < start_index + count;
	};
	return std::any_of(m_requests.begin(), m_requests.end(), matches) ||
	       std::any_of(m_cpu_requests.begin(), m_cpu_requests.end(), matches);
}

bool FlipQueue::Flip(uint32_t micros) {
	KYTY_PROFILER_BLOCK("FlipQueue::Flip");

	m_mutex.Lock();
	if (m_requests.empty()) {
		m_submit_cond_var.WaitFor(&m_mutex, micros);

		if (m_requests.empty()) {
			m_mutex.Unlock();
			return false;
		}
	}
	if (m_processing) {
		EXIT("video-out flip queue processing is already active\n");
	}
	if (m_requests.front().state != RequestState::Ready) {
		m_mutex.Unlock();
		return false;
	}
	m_processing = true;
	auto r       = m_requests.front();
	m_mutex.Unlock();
	if (!IsFlipDue(r.cfg)) {
		Common::LockGuard lock(m_mutex);
		m_processing = false;
		return false;
	}

	m_mutex.Lock();
	if (m_requests.empty() || m_requests.front().id != r.id ||
	    m_requests.front().state != RequestState::Ready || !m_processing) {
		EXIT("video-out request changed before presentation, id=%" PRIu64 "\n", r.id);
	}
	m_requests.front().state = RequestState::Presenting;
	m_mutex.Unlock();

	Graphics::WindowPresentFrame(r.frame);

	m_mutex.Lock();
	if (m_requests.empty() || m_requests.front().id != r.id ||
	    m_requests.front().state != RequestState::Presenting) {
		EXIT("video-out flip queue changed while processing its front request\n");
	}
	m_requests.pop_front();

	r.cfg->flip_status.count++;
	r.cfg->flip_status.processTime              = LibKernel::KernelGetProcessTime();
	r.cfg->flip_status.processTimeCounter       = LibKernel::KernelGetProcessTimeCounter();
	r.cfg->flip_status.submitProcessTimeCounter = r.submit_ptc;
	r.cfg->flip_status.flipArg                  = r.flip_arg;
	r.cfg->flip_status.currentBuffer            = r.index;
	r.cfg->flip_status.flipPendingNum = static_cast<int>(m_requests.size() + m_cpu_requests.size());
	if (r.source == FlipRequestSource::GpuEop && r.cfg->flip_status.gcQueueNum > 0) {
		r.cfg->flip_status.gcQueueNum--;
	}

	m_processing = false;
	m_done_cond_var.SignalAll();
	m_submit_slot_cond_var.Signal();
	m_mutex.Unlock();

	r.cfg->mutex.Lock();
	TriggerVideoOutEventsLocked(r.cfg->flip_eqs, VideoOutEventKind::Flip,
	                            reinterpret_cast<void*>(r.flip_arg));
	r.cfg->mutex.Unlock();

	if (Config::GraphicsDebugDumpEnabled() &&
	    Config::GetPrintfDirection() != Config::OutputDirection::Silent) {
		LOGF("Flip done: %d\n", r.index);
	}

	return true;
}

void FlipQueue::GetFlipStatus(VideoOutConfig* cfg, VideoOutFlipStatus* out) {
	EXIT_IF(cfg == nullptr);
	EXIT_IF(out == nullptr);

	Common::LockGuard lock(m_mutex);

	*out = cfg->flip_status;
}

bool VideoOutFlipWindow(uint32_t micros) {
	EXIT_IF(g_video_out_context == nullptr);

	WaitForNextVblank();

	return g_video_out_context->GetFlipQueue().Flip(micros);
}

void VideoOutBeginVblank() {
	EXIT_IF(g_video_out_context == nullptr);

	g_video_out_context->VblankBegin();
}

void VideoOutEndVblank() {
	EXIT_IF(g_video_out_context == nullptr);

	g_video_out_context->VblankEnd();
}

KYTY_SYSV_ABI int VideoOutOpen(int user_id, int bus_type, int index, const void* param) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	EXIT_NOT_IMPLEMENTED(user_id != 255 && user_id != 0);
	EXIT_NOT_IMPLEMENTED(bus_type != 0);
	EXIT_NOT_IMPLEMENTED(index != 0);

	LOGF("\t param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));

	int handle = g_video_out_context->Open();

	if (handle < 0) {
		return VIDEO_OUT_ERROR_RESOURCE_BUSY;
	}

	return handle;
}

KYTY_SYSV_ABI int VideoOutClose(int handle) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	if (!g_video_out_context->IsOpened(handle)) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	g_video_out_context->Close(handle);

	return OK;
}

KYTY_SYSV_ABI void VideoOutSetBufferAttribute2(VideoOutBufferAttribute2* attribute,
                                               uint64_t pixel_format, uint32_t tiling_mode,
                                               uint32_t width, uint32_t height, uint64_t option,
                                               uint32_t dcc_control,
                                               uint64_t dcc_cb_register_clear_color) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attribute == nullptr);

	LOGF("\t pixel_format                = %016" PRIx64 "\n"
	     "\t tiling_mode                 = %" PRIu32 "\n"
	     "\t width                       = %" PRIu32 "\n"
	     "\t height                      = %" PRIu32 "\n"
	     "\t option                      = %016" PRIx64 "\n"
	     "\t dcc_control                 = %08" PRIx32 "\n"
	     "\t dcc_cb_register_clear_color = %016" PRIx64 "\n",
	     pixel_format, tiling_mode, width, height, option, dcc_control,
	     dcc_cb_register_clear_color);

	memset(attribute, 0, sizeof(VideoOutBufferAttribute2));

	attribute->tiling_mode                 = tiling_mode;
	attribute->aspect_ratio                = 0;
	attribute->width                       = width;
	attribute->height                      = height;
	attribute->pitch_in_pixel              = 0;
	attribute->option                      = option;
	attribute->pixel_format                = pixel_format;
	attribute->dcc_cb_register_clear_color = dcc_cb_register_clear_color;
	attribute->dcc_control                 = dcc_control;
}

KYTY_SYSV_ABI int VideoOutSetFlipRate(int handle, int rate) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	LOGF("\trate = %d\n", rate);

	auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	if (rate < 0 || rate > 2) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	ctx->flip_rate = rate;

	return OK;
}

KYTY_SYSV_ABI int VideoOutDeleteFlipEvent(EventQueue::KernelEqueue eq, int handle) {
	PRINT_NAME();
	return DeleteVideoOutEvent(handle, eq, VideoOutEventKind::Flip);
}

KYTY_SYSV_ABI int VideoOutAddFlipEvent(EventQueue::KernelEqueue eq, int handle, void* udata) {
	PRINT_NAME();
	return RegisterVideoOutEvent(handle, eq, VideoOutEventKind::Flip, udata);
}

KYTY_SYSV_ABI int VideoOutDeleteVblankEvent(EventQueue::KernelEqueue eq, int handle) {
	PRINT_NAME();
	return DeleteVideoOutEvent(handle, eq, VideoOutEventKind::Vblank);
}

KYTY_SYSV_ABI int VideoOutDeletePreVblankStartEvent(EventQueue::KernelEqueue eq, int handle) {
	PRINT_NAME();
	return DeleteVideoOutEvent(handle, eq, VideoOutEventKind::PreVblankStart);
}

KYTY_SYSV_ABI int VideoOutAddVblankEvent(LibKernel::EventQueue::KernelEqueue eq, int handle,
                                         void* udata) {
	PRINT_NAME();
	return RegisterVideoOutEvent(handle, eq, VideoOutEventKind::Vblank, udata);
}

KYTY_SYSV_ABI int VideoOutAddPreVblankStartEvent(LibKernel::EventQueue::KernelEqueue eq, int handle,
                                                 void* udata) {
	PRINT_NAME();
	return RegisterVideoOutEvent(handle, eq, VideoOutEventKind::PreVblankStart, udata);
}

KYTY_SYSV_ABI int VideoOutAddOutputModeEvent(LibKernel::EventQueue::KernelEqueue eq, int handle,
                                             void* udata) {
	PRINT_NAME();
	return RegisterVideoOutEvent(handle, eq, VideoOutEventKind::OutputMode, udata);
}

static int RegisterBuffersInternal(VideoOutConfig* ctx, int set_id, int start_index,
                                   const void* const* addresses, int buffer_num,
                                   const std::vector<Graphics::VideoOutInfo>& infos) {
	if (ctx == nullptr || addresses == nullptr || buffer_num <= 0 ||
	    infos.size() != static_cast<size_t>(buffer_num)) {
		EXIT("invalid internal video-out buffer registration arguments\n");
	}
	if (set_id < 0 || set_id >= VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX) {
		EXIT("internal video-out buffer set identifier is out of range\n");
	}
	Graphics::WindowWaitForGraphicInitialized();
	Graphics::GraphicsRenderCreateContext();
	auto*             graphic_ctx = g_video_out_context->GetGraphicCtx();
	Common::LockGuard lock(ctx->mutex);
	if (ctx->closing) {
		EXIT("cannot register buffers on a closing video-out handle\n");
	}
	if (std::any_of(ctx->buffers_sets.begin(), ctx->buffers_sets.end(),
	                [set_id](const auto& set) { return set.set_id == set_id; })) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}
	for (int i = 0; i < buffer_num; i++) {
		if (ctx->unregistering[start_index + i]) {
			EXIT("video-out buffer registration raced with unregistration\n");
		}
		if (ctx->buffers[start_index + i].buffer != nullptr) {
			return VIDEO_OUT_ERROR_SLOT_OCCUPIED;
		}
	}
	auto images =
	    Graphics::g_render_ctx->GetTextureCache()->RegisterVideoOutSurfaces(graphic_ctx, infos);
	if (images.size() != infos.size()) {
		EXIT("video-out texture cache returned an incomplete surface set\n");
	}
	ctx->buffers_sets.push_back({start_index, buffer_num, set_id});
	for (int i = 0; i < buffer_num; i++) {
		auto& dst         = ctx->buffers[i + start_index];
		dst.set_id        = set_id;
		dst.buffer        = addresses[i];
		dst.buffer_size   = infos[i].size;
		dst.buffer_pitch  = infos[i].pitch;
		dst.buffer_vulkan = images[i];
		LOGF("\tbuffers[%d] = %016" PRIx64 " metadata = %016" PRIx64 " dcc = %08" PRIx32 "\n",
		     i + start_index, reinterpret_cast<uint64_t>(addresses[i]), infos[i].metadata_address,
		     infos[i].dcc_control);
	}

	return OK;
}

KYTY_SYSV_ABI int VideoOutRegisterBuffers2(int handle, int set_index, int buffer_index_start,
                                           const VideoOutBuffers* buffers, int buffer_num,
                                           const VideoOutBufferAttribute2* attribute, int category,
                                           void* option) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	if (buffers == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (attribute == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_OPTION;
	}

	if (set_index < 0 || set_index >= VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX ||
	    buffer_index_start < 0 || buffer_index_start >= VIDEO_OUT_BUFFER_NUM_MAX ||
	    buffer_num < 1 || buffer_num > VIDEO_OUT_BUFFER_NUM_MAX ||
	    buffer_index_start + buffer_num > VIDEO_OUT_BUFFER_NUM_MAX) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	LOGF("\t start_index    = %d\n"
	     "\t buffer_num     = %d\n"
	     "\t set_index      = %d\n"
	     "\t pixel_format   = 0x%016" PRIx64 "\n"
	     "\t tiling_mode    = %" PRIu32 "\n"
	     "\t aspect_ratio   = %" PRIu32 "\n"
	     "\t width          = %" PRIu32 "\n"
	     "\t height         = %" PRIu32 "\n"
	     "\t pitch_in_pixel = %" PRIu32 "\n"
	     "\t option         = %" PRIu64 "\n"
	     "\t category       = %d\n",
	     buffer_index_start, buffer_num, set_index, attribute->pixel_format, attribute->tiling_mode,
	     attribute->aspect_ratio, attribute->width, attribute->height, attribute->pitch_in_pixel,
	     attribute->option, category);

	if (option != nullptr) {
		EXIT("video-out buffer registration options are unsupported\n");
	}
	if (category != VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_UNCOMPRESSED &&
	    category != VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_COMPRESSED) {
		return VIDEO_OUT_ERROR_INVALID_CATEGORY;
	}
	const bool compressed = category == VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_COMPRESSED;
	std::vector<const void*>            addresses(static_cast<size_t>(buffer_num));
	std::vector<Graphics::VideoOutInfo> infos;
	infos.reserve(static_cast<size_t>(buffer_num));

	for (int i = 0; i < buffer_num; i++) {
		LOGF("\t buffers[%d]: data=%p metadata=%p\n", i, buffers[i].data, buffers[i].metadata);
		if (buffers[i].reserved[0] != nullptr || buffers[i].reserved[1] != nullptr) {
			LOGF("\t buffers[%d]: ignoring reserved fields {%p, %p}\n", i, buffers[i].reserved[0],
			     buffers[i].reserved[1]);
		}
		const auto data_address     = reinterpret_cast<uint64_t>(buffers[i].data);
		const auto metadata_address = reinterpret_cast<uint64_t>(buffers[i].metadata);
		const auto compression      = Graphics::ClassifyVideoOutCompression(
		    compressed, metadata_address, attribute->dcc_control,
		    attribute->dcc_cb_register_clear_color);
		if (compression == Graphics::VideoOutCompression::Unsupported) {
			EXIT("unsupported video-out compression, category=%d data=0x%016" PRIx64
			     " metadata=0x%016" PRIx64 " dcc_control=0x%08" PRIx32 " clear_color=0x%016" PRIx64
			     "\n",
			     category, data_address, metadata_address, attribute->dcc_control,
			     attribute->dcc_cb_register_clear_color);
		}
		addresses[static_cast<size_t>(i)] = buffers[i].data;
		infos.push_back(MakeVideoOutInfo(*attribute, data_address, metadata_address, compression));
	}

	return RegisterBuffersInternal(ctx, set_index, buffer_index_start, addresses.data(), buffer_num,
	                               infos);
}

KYTY_SYSV_ABI int VideoOutSubmitChangeBufferAttribute2(int handle, int set_index,
                                                       const VideoOutBufferAttribute2* attribute,
                                                       void*                           option) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	if (attribute == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_OPTION;
	}
	if (set_index < 0 || set_index >= VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}

	if (option != nullptr) {
		return VIDEO_OUT_ERROR_INVALID_OPTION;
	}
	(void)ctx;
	(void)set_index;
	EXIT("video-out buffer attribute query is unsupported\n");
}

KYTY_SYSV_ABI int VideoOutUnregisterBuffers(int handle, int set_index) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	if (set_index < 0 || set_index >= VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}

	Common::LockGuard lock(ctx->mutex);
	if (ctx->closing) {
		EXIT("cannot unregister buffers from a closing video-out handle\n");
	}
	auto set_it = std::find_if(ctx->buffers_sets.begin(), ctx->buffers_sets.end(),
	                           [set_index](const auto& set) { return set.set_id == set_index; });
	if (set_it == ctx->buffers_sets.end()) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}
	std::vector<Graphics::VideoOutVulkanImage*> images;
	images.reserve(static_cast<size_t>(set_it->num));
	for (int i = set_it->start_index; i < set_it->start_index + set_it->num; i++) {
		if (ctx->unregistering[i]) {
			EXIT("video-out buffer is already being unregistered\n");
		}
		ctx->unregistering[i] = true;
		if (ctx->buffers[i].set_id != set_index || ctx->buffers[i].buffer == nullptr ||
		    ctx->buffers[i].buffer_vulkan == nullptr) {
			EXIT("video-out buffer set contains inconsistent surface state\n");
		}
		images.push_back(ctx->buffers[i].buffer_vulkan);
	}
	if (g_video_out_context->GetFlipQueue().HasPending(ctx, set_it->start_index, set_it->num)) {
		EXIT("cannot unregister video-out buffers with pending flips\n");
	}
	Graphics::g_render_ctx->GetTextureCache()->UnregisterVideoOutSurfaces(images);
	for (int i = set_it->start_index; i < set_it->start_index + set_it->num; i++) {
		ctx->buffers[i]       = VideoOutBufferInfo {};
		ctx->unregistering[i] = false;
	}
	ctx->buffers_sets.erase(set_it);

	return OK;
}

} // namespace Libs::VideoOut

namespace Libs::Presentation {

DisplayBufferImage DisplayBufferFind(uint64_t addr, bool render_target) {
	EXIT_IF(VideoOut::g_video_out_context == nullptr);

	return VideoOut::g_video_out_context->FindImage(reinterpret_cast<void*>(addr), render_target);
}

} // namespace Libs::Presentation

namespace Libs::VideoOut {

KYTY_SYSV_ABI int VideoOutSubmitFlip(int handle, int index, int flip_mode, int64_t flip_arg) {
	PRINT_NAME();

	uint64_t  request_id = 0;
	const int result =
	    ReserveFlipRequest(handle, index, flip_mode, flip_arg, FlipRequestSource::Cpu, &request_id);
	if (result == VIDEO_OUT_ERROR_INVALID_VALUE) {
		LOGF("\t unsupported flip_mode = %d\n", flip_mode);
	}
	if (result != OK) {
		return result;
	}
	Graphics::GraphicsRunSubmitFlipPreparation();

	return OK;
}

} // namespace Libs::VideoOut

namespace Libs::Presentation {

int DisplayBufferSubmitFlipFromGpu(Graphics::CommandBuffer* buffer, int handle, int index,
                                   int flip_mode, int64_t flip_arg, uint64_t* request_id) {
	EXIT_IF(VideoOut::g_video_out_context == nullptr || Graphics::g_render_ctx == nullptr ||
	        buffer == nullptr || request_id == nullptr);

	const int result = VideoOut::ReserveFlipRequest(
	    handle, index, flip_mode, flip_arg, VideoOut::FlipRequestSource::GpuEop, request_id);
	if (result != OK) {
		return result;
	}
	VideoOut::g_video_out_context->GetFlipQueue().Prepare(*request_id, buffer);

	return OK;
}

uint64_t DisplayBufferPrepareNextFlipOnGpu(Graphics::CommandBuffer* buffer) {
	EXIT_IF(VideoOut::g_video_out_context == nullptr || Graphics::g_render_ctx == nullptr);
	return VideoOut::g_video_out_context->GetFlipQueue().PrepareNextCpu(buffer);
}

void DisplayBufferCompleteFlipFromGpu(uint64_t request_id) {
	EXIT_IF(VideoOut::g_video_out_context == nullptr || Graphics::g_render_ctx == nullptr);
	VideoOut::g_video_out_context->GetFlipQueue().Complete(request_id);
}

void DisplayBufferWaitForFlipQueueSlot() {
	EXIT_IF(VideoOut::g_video_out_context == nullptr || Graphics::g_render_ctx == nullptr);
	VideoOut::g_video_out_context->GetFlipQueue().WaitForSubmitSlot();
}

} // namespace Libs::Presentation

namespace Libs::VideoOut {

void VideoOutWaitFlipDone(int handle, int index) {
	EXIT_IF(g_video_out_context == nullptr);

	auto* ctx = g_video_out_context->Get(handle);
	EXIT_IF(ctx == nullptr);

	EXIT_NOT_IMPLEMENTED(!IsValidBufferIndex(index));
	g_video_out_context->GetFlipQueue().Wait(ctx, index);
}

KYTY_SYSV_ABI int VideoOutGetFlipStatus(int handle, VideoOutFlipStatus* status) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	if (status == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	g_video_out_context->GetFlipQueue().GetFlipStatus(ctx, status);

	LOGF("\t count = %" PRIu64 "\n"
	     "\t processTime = %" PRIu64 "\n"
	     "\t processTimeCounter = %" PRIu64 "\n"
	     "\t submitProcessTimeCounter = %" PRIu64 "\n"
	     "\t flipArg = %" PRId64 "\n"
	     "\t gcQueueNum = %d\n"
	     "\t flipPendingNum = %d\n"
	     "\t currentBuffer = %d\n",
	     status->count, status->processTime, status->processTimeCounter,
	     status->submitProcessTimeCounter, status->flipArg, status->gcQueueNum,
	     status->flipPendingNum, status->currentBuffer);

	return OK;
}

KYTY_SYSV_ABI int VideoOutIsFlipPending(int handle) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	VideoOutFlipStatus status {};
	g_video_out_context->GetFlipQueue().GetFlipStatus(ctx, &status);

	LOGF("\t flipPendingNum = %d\n", status.flipPendingNum);

	return status.flipPendingNum;
}

KYTY_SYSV_ABI int VideoOutGetVblankStatus(int handle, VideoOutVblankStatus* status) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	if (status == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	ctx->mutex.Lock();
	*status = ctx->vblank_status;
	ctx->mutex.Unlock();

	LOGF("\t count = %" PRIu64 "\n"
	     "\t processTime = %" PRIu64 "\n"
	     "\t processTimeCounter = %" PRIu64 "\n",
	     status->count, status->processTime, status->processTimeCounter);

	return OK;
}

KYTY_SYSV_ABI int VideoOutGetEventId(const EventQueue::KernelEvent* ev) {
	PRINT_NAME();

	if (ev == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (ev->filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT) {
		return VIDEO_OUT_ERROR_INVALID_EVENT;
	}

	switch (ev->ident) {
		case VIDEO_OUT_EVENT_FLIP:
		case VIDEO_OUT_EVENT_VBLANK:
		case VIDEO_OUT_EVENT_PRE_VBLANK_START:
		case VIDEO_OUT_EVENT_SET_MODE: return static_cast<int>(ev->ident);
		default: return VIDEO_OUT_ERROR_INVALID_EVENT;
	}
}

KYTY_SYSV_ABI int VideoOutGetEventData(const EventQueue::KernelEvent* ev, int64_t* data) {
	PRINT_NAME();

	if (ev == nullptr || data == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (ev->filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT) {
		return VIDEO_OUT_ERROR_INVALID_EVENT;
	}

	uint64_t event_data = static_cast<uint64_t>(ev->data) >> 16u;
	if (ev->ident == VIDEO_OUT_EVENT_FLIP &&
	    (static_cast<uint64_t>(ev->data) & 0x8000000000000000ULL) != 0) {
		event_data |= 0xffff000000000000ULL;
	}

	*data = static_cast<int64_t>(event_data);

	return OK;
}

KYTY_SYSV_ABI int VideoOutGetEventCount(const EventQueue::KernelEvent* ev) {
	PRINT_NAME();

	if (ev == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (ev->filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT) {
		return VIDEO_OUT_ERROR_INVALID_EVENT;
	}

	return static_cast<int>((static_cast<uint64_t>(ev->data) >> 12u) & 0xfu);
}

KYTY_SYSV_ABI int VideoOutWaitVblank(int handle) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	[[maybe_unused]] auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	WaitForNextVblank();
	g_video_out_context->VblankEnd();

	return OK;
}

KYTY_SYSV_ABI int VideoOutGetOutputStatus(int handle, VideoOutOutputStatus* status) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	if (status == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	ctx->mutex.Lock();
	status->resolution   = (ctx->width >= 3840 || ctx->height >= 2160 ? 2u : 1u);
	status->dynamicRange = 1;
	status->refreshRate =
	    (ctx->output_mode == VIDEO_OUT_OUTPUT_MODE_119_88HZ || Config::GetVblankFrequency() >= 119
	         ? VIDEO_OUT_REFRESH_RATE_119_88HZ
	         : VIDEO_OUT_REFRESH_RATE_59_94HZ);
	status->flags       = 0;
	status->reserved[0] = 0;
	status->reserved[1] = 0;
	status->reserved[2] = 0;
	ctx->mutex.Unlock();

	return OK;
}

static int ValidateOutputConfig(int handle, uint64_t mode, const VideoOutOutputOptions* options,
                                void* reserved_ptr, uint64_t reserved) {
	EXIT_IF(g_video_out_context == nullptr);

	if (!g_video_out_context->IsOpened(handle)) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	if (reserved_ptr != nullptr || reserved != 0) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	if (options != nullptr) {
		for (auto v: options->internalData) {
			if (v != 0) {
				return VIDEO_OUT_ERROR_INVALID_OPTION;
			}
		}
	}

	if (mode != VIDEO_OUT_OUTPUT_MODE_DEFAULT && mode != VIDEO_OUT_OUTPUT_MODE_119_88HZ) {
		return VIDEO_OUT_ERROR_UNSUPPORTED_OUTPUT_MODE;
	}

	return OK;
}

KYTY_SYSV_ABI int VideoOutInitializeOutputOptions(VideoOutOutputOptions* options) {
	PRINT_NAME();

	if (options == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	memset(options, 0, sizeof(VideoOutOutputOptions));

	return OK;
}

KYTY_SYSV_ABI int VideoOutIsOutputSupported(int handle, uint64_t mode,
                                            const VideoOutOutputOptions* options,
                                            void* reserved_ptr, uint64_t reserved) {
	PRINT_NAME();

	LOGF("\t mode = 0x%016" PRIx64 "\n", mode);

	int result = ValidateOutputConfig(handle, mode, options, reserved_ptr, reserved);
	if (result != OK) {
		return result;
	}

	if (mode == VIDEO_OUT_OUTPUT_MODE_119_88HZ) {
		return (Config::GetVblankFrequency() >= 119 ? VIDEO_OUT_TRUE : VIDEO_OUT_FALSE);
	}

	return VIDEO_OUT_TRUE;
}

KYTY_SYSV_ABI int VideoOutConfigureOutput(int handle, uint64_t mode,
                                          const VideoOutOutputOptions* options, void* reserved_ptr,
                                          uint64_t reserved) {
	PRINT_NAME();

	LOGF("\t mode = 0x%016" PRIx64 "\n", mode);

	int result = VideoOutIsOutputSupported(handle, mode, options, reserved_ptr, reserved);
	if (result < 0) {
		return result;
	}
	if (result == VIDEO_OUT_FALSE) {
		return VIDEO_OUT_ERROR_UNAVAILABLE_OUTPUT_MODE;
	}

	auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	ctx->mutex.Lock();
	ctx->output_mode = mode;
	TriggerVideoOutEventsLocked(ctx->output_mode_eqs, VideoOutEventKind::OutputMode,
	                            reinterpret_cast<void*>(ctx->output_mode));
	ctx->mutex.Unlock();

	return OK;
}

KYTY_SYSV_ABI int VideoOutSetWindowModeMargins(int handle, int top, int bottom) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	[[maybe_unused]] auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	LOGF("\t top    = %d\n"
	     "\t bottom = %d\n",
	     top, bottom);

	return OK;
}

KYTY_SYSV_ABI int VideoOutLatencyControlWaitBeforeInput(int handle) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	[[maybe_unused]] auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	return OK;
}

KYTY_SYSV_ABI int VideoOutLatencyMeasureSetStartPoint(int handle, uint32_t point) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	[[maybe_unused]] auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	LOGF("\t point = %" PRIu32 "\n", point);

	return OK;
}

KYTY_SYSV_ABI int VideoOutColorSettingsSetGamma(VideoOutColorSettings* settings, float gamma) {
	PRINT_NAME();

	if (settings == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (gamma < 0.1f || gamma > 2.0f) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	settings->gamma = gamma;
	return OK;
}

KYTY_SYSV_ABI int VideoOutAdjustColor(int handle, const VideoOutColorSettings* settings) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	if (settings == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (!g_video_out_context->IsOpened(handle)) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	ctx->mutex.Lock();
	ctx->gamma = settings->gamma;
	ctx->mutex.Unlock();

	return OK;
}

} // namespace Libs::VideoOut
