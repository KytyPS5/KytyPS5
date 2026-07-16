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
constexpr int      VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX                   = 4;
constexpr uint64_t VIDEO_OUT_OUTPUT_MODE_DEFAULT                        = 0x0000000000000001ULL;
constexpr uint64_t VIDEO_OUT_OUTPUT_MODE_119_88HZ                       = 0x000000000000000FULL;
constexpr uint64_t VIDEO_OUT_REFRESH_RATE_59_94HZ                       = 3;
constexpr uint64_t VIDEO_OUT_REFRESH_RATE_119_88HZ                      = 13;
constexpr int      VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_UNCOMPRESSED     = 0;
constexpr int      VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_COMPRESSED       = 1;
constexpr uint64_t VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_STRICT_COLORIMETRY = 8;

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
	virtual ~FlipQueue() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(FlipQueue);

	bool Submit(VideoOutConfig* cfg, int index, int64_t flip_arg, bool gpu_eop);
	bool Flip(uint32_t micros);
	bool HasPending(VideoOutConfig* cfg, int start_index, int count);
	void GetFlipStatus(VideoOutConfig* cfg, VideoOutFlipStatus* out);
	void AllowEopPresent(VideoOutConfig* cfg, int index);
	void Wait(VideoOutConfig* cfg, int index);

private:
	struct Request {
		VideoOutConfig* cfg;
		int             index;
		int64_t         flip_arg;
		uint64_t        submit_ptc;
		bool            gpu_eop;
		bool            prepared;
	};

	Common::Mutex      m_mutex;
	Common::CondVar    m_submit_cond_var;
	Common::CondVar    m_done_cond_var;
	std::list<Request> m_requests;
	bool               m_processing = false;
};

class VideoOutContext {
public:
	static constexpr int VIDEO_OUT_NUM_MAX = 2;

	VideoOutContext() { EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread()); }
	virtual ~VideoOutContext() { KYTY_NOT_IMPLEMENTED; }

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
	const auto tile_mode = Graphics::Prospero::GpuEnumValue(Graphics::Prospero::TileMode::kRenderTarget);
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
	info.bytes_per_element = 4;
	info.tile_mode         = tile_mode;
	info.dcc_control       = attribute.dcc_control;
	info.compression       = compression;
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

	m_video_out_ctx[handle].opened = false;

	m_video_out_ctx[handle].mutex.Lock();
	if (m_video_out_ctx[handle].closing) {
		EXIT("video-out handle is already closing\n");
	}
	m_video_out_ctx[handle].closing = true;
	if (m_flip_queue.HasPending(&m_video_out_ctx[handle], VIDEO_OUT_BUFFER_INDEX_BLACK,
	                            VIDEO_OUT_BUFFER_NUM_MAX - VIDEO_OUT_BUFFER_INDEX_BLACK)) {
		EXIT("cannot close video-out handle with pending flips\n");
	}
	for (auto& flip_eq: m_video_out_ctx[handle].flip_eqs) {
		if (flip_eq != nullptr) {
			auto result = EventQueue::KernelDeleteEvent(flip_eq, VIDEO_OUT_EVENT_FLIP,
			                                            EventQueue::KERNEL_EVFILT_VIDEO_OUT);
			EXIT_NOT_IMPLEMENTED(result != OK);
		}
	}
	m_video_out_ctx[handle].flip_eqs.clear();
	for (auto& vblank_eq: m_video_out_ctx[handle].pre_vblank_eqs) {
		if (vblank_eq != nullptr) {
			auto result = EventQueue::KernelDeleteEvent(vblank_eq, VIDEO_OUT_EVENT_PRE_VBLANK_START,
			                                            EventQueue::KERNEL_EVFILT_VIDEO_OUT);
			EXIT_NOT_IMPLEMENTED(result != OK);
		}
	}
	m_video_out_ctx[handle].pre_vblank_eqs.clear();
	for (auto& vblank_eq: m_video_out_ctx[handle].vblank_eqs) {
		if (vblank_eq != nullptr) {
			auto result = EventQueue::KernelDeleteEvent(vblank_eq, VIDEO_OUT_EVENT_VBLANK,
			                                            EventQueue::KERNEL_EVFILT_VIDEO_OUT);
			EXIT_NOT_IMPLEMENTED(result != OK);
		}
	}
	m_video_out_ctx[handle].vblank_eqs.clear();
	for (auto& output_mode_eq: m_video_out_ctx[handle].output_mode_eqs) {
		if (output_mode_eq != nullptr) {
			auto result = EventQueue::KernelDeleteEvent(output_mode_eq, VIDEO_OUT_EVENT_SET_MODE,
			                                            EventQueue::KERNEL_EVFILT_VIDEO_OUT);
			EXIT_NOT_IMPLEMENTED(result != OK);
		}
	}
	m_video_out_ctx[handle].output_mode_eqs.clear();

	m_video_out_ctx[handle].flip_rate = 0;

	std::vector<Graphics::VideoOutVulkanImage*> images;
	for (const auto& buffer: m_video_out_ctx[handle].buffers) {
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
	for (auto& buffer: m_video_out_ctx[handle].buffers) {
		buffer.buffer        = nullptr;
		buffer.buffer_vulkan = nullptr;
		buffer.buffer_size   = 0;
		buffer.buffer_pitch  = 0;
		buffer.set_id        = 0;
	}

	m_video_out_ctx[handle].buffers_sets.clear();
	for (bool unregistering: m_video_out_ctx[handle].unregistering) {
		if (unregistering) {
			EXIT("video-out close raced with buffer unregistration\n");
		}
	}
	m_video_out_ctx[handle].mutex.Unlock();
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

			for (auto& vblank_eq: ctx.pre_vblank_eqs) {
				if (vblank_eq != nullptr) {
					auto result = EventQueue::KernelTriggerEvent(
					    vblank_eq, VIDEO_OUT_EVENT_PRE_VBLANK_START,
					    EventQueue::KERNEL_EVFILT_VIDEO_OUT,
					    reinterpret_cast<void*>(ctx.pre_vblank_status.count));
					EXIT_NOT_IMPLEMENTED(result != OK);
				}
			}
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

			for (auto& vblank_eq: ctx.vblank_eqs) {
				if (vblank_eq != nullptr) {
					auto result = EventQueue::KernelTriggerEvent(
					    vblank_eq, VIDEO_OUT_EVENT_VBLANK, EventQueue::KERNEL_EVFILT_VIDEO_OUT,
					    reinterpret_cast<void*>(ctx.vblank_status.count));
					EXIT_NOT_IMPLEMENTED(result != OK);
				}
			}
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

bool FlipQueue::Submit(VideoOutConfig* cfg, int index, int64_t flip_arg, bool gpu_eop) {
	Common::LockGuard lock(m_mutex);

	if (m_requests.size() >= 16) {
		return false;
	}

	Request r {};
	r.cfg        = cfg;
	r.index      = index;
	r.flip_arg   = flip_arg;
	r.submit_ptc = LibKernel::KernelGetProcessTimeCounter();
	r.gpu_eop    = gpu_eop;
	r.prepared   = !gpu_eop;

	m_requests.push_back(r);

	cfg->flip_status.flipPendingNum           = static_cast<int>(m_requests.size());
	cfg->flip_status.submitProcessTimeCounter = r.submit_ptc;
	if (gpu_eop) {
		cfg->flip_status.gcQueueNum++;
	}

	m_submit_cond_var.Signal();

	return true;
}

void FlipQueue::AllowEopPresent(VideoOutConfig* cfg, int index) {
	Common::LockGuard lock(m_mutex);
	auto request = std::find_if(m_requests.begin(), m_requests.end(), [cfg, index](const auto& r) {
		return r.cfg == cfg && r.index == index && r.gpu_eop && !r.prepared;
	});
	if (request == m_requests.end()) {
		EXIT("completed GPU flip has no queued request\n");
	}
	request->prepared = true;
	m_submit_cond_var.Signal();
}

void FlipQueue::Wait(VideoOutConfig* cfg, int index) {
	Common::LockGuard lock(m_mutex);

	while (std::find_if(m_requests.begin(), m_requests.end(), [cfg, index](const auto& r) {
		       return r.cfg == cfg && r.index == index;
	       }) != m_requests.end()) {
		m_done_cond_var.Wait(&m_mutex);
	}
}

bool FlipQueue::HasPending(VideoOutConfig* cfg, int start_index, int count) {
	if (cfg == nullptr || count <= 0 || start_index > INT_MAX - count) {
		EXIT("invalid video-out pending-flip query range\n");
	}
	Common::LockGuard lock(m_mutex);
	return std::any_of(m_requests.begin(), m_requests.end(), [&](const auto& request) {
		return request.cfg == cfg && request.index >= start_index &&
		       request.index < start_index + count;
	});
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
	if (m_requests.front().gpu_eop && !m_requests.front().prepared) {
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

	Graphics::VideoOutVulkanImage* buffer = nullptr;
	r.cfg->mutex.Lock();
	if (!IsSpecialBufferIndex(r.index)) {
		buffer = r.cfg->buffers[r.index].buffer_vulkan;
		if (buffer == nullptr) {
			EXIT("queued video-out flip has no native surface\n");
		}
	}
	r.cfg->mutex.Unlock();
	if (buffer != nullptr) {
		Graphics::g_render_ctx->GetTextureCache()->RefreshVideoOut(buffer);
		Graphics::WindowDrawBuffer(buffer);
	}

	m_mutex.Lock();
	if (m_requests.empty() || m_requests.front().cfg != r.cfg ||
	    m_requests.front().index != r.index || m_requests.front().submit_ptc != r.submit_ptc) {
		EXIT("video-out flip queue changed while processing its front request\n");
	}
	m_requests.pop_front();

	r.cfg->flip_status.count++;
	r.cfg->flip_status.processTime              = LibKernel::KernelGetProcessTime();
	r.cfg->flip_status.processTimeCounter       = LibKernel::KernelGetProcessTimeCounter();
	r.cfg->flip_status.submitProcessTimeCounter = r.submit_ptc;
	r.cfg->flip_status.flipArg                  = r.flip_arg;
	r.cfg->flip_status.currentBuffer            = r.index;
	r.cfg->flip_status.flipPendingNum           = static_cast<int>(m_requests.size());
	if (r.gpu_eop && r.cfg->flip_status.gcQueueNum > 0) {
		r.cfg->flip_status.gcQueueNum--;
	}

	m_processing = false;
	m_done_cond_var.Signal();
	m_mutex.Unlock();

	r.cfg->mutex.Lock();
	for (auto& flip_eq: r.cfg->flip_eqs) {
		if (flip_eq != nullptr) {
			auto result = EventQueue::KernelTriggerEvent(flip_eq, VIDEO_OUT_EVENT_FLIP,
			                                             EventQueue::KERNEL_EVFILT_VIDEO_OUT,
			                                             reinterpret_cast<void*>(r.flip_arg));
			EXIT_NOT_IMPLEMENTED(result != OK);
		}
	}
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

static void FlipEventResetFunc(LibKernel::EventQueue::KernelEqueueEvent* event) {
	EXIT_IF(event == nullptr);
	event->triggered    = false;
	event->event.fflags = 0;
	event->event.data   = 0;
}

static void FlipEventDeleteFunc(EventQueue::KernelEqueue                  eq,
                                LibKernel::EventQueue::KernelEqueueEvent* event) {
	EXIT_IF(event == nullptr);
	EXIT_IF(event->filter.data == nullptr);

	EXIT_NOT_IMPLEMENTED(event->event.ident != VIDEO_OUT_EVENT_FLIP);
	EXIT_NOT_IMPLEMENTED(event->event.filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT);

	if (event->filter.data != nullptr) {
		auto* video_out = static_cast<VideoOutConfig*>(event->filter.data);
		video_out->mutex.Lock();
		EXIT_IF(video_out->flip_eqs.empty());
		auto it = std::find(video_out->flip_eqs.begin(), video_out->flip_eqs.end(), eq);
		EXIT_NOT_IMPLEMENTED(it == video_out->flip_eqs.end());
		*it = nullptr;
		video_out->mutex.Unlock();
	}
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

static void FlipEventTriggerFunc(LibKernel::EventQueue::KernelEqueueEvent* event,
                                 void*                                     trigger_data) {
	EXIT_IF(event == nullptr);

	auto triggered_event = event->event;
	triggered_event.fflags =
	    ((triggered_event.fflags < 0xfu) ? triggered_event.fflags + 1u : triggered_event.fflags);
	triggered_event.data = MakeVideoOutEventData(triggered_event.data, trigger_data);
	if (event->triggered) {
		event->pending_events.push_back(triggered_event);
	} else {
		event->event     = triggered_event;
		event->triggered = true;
	}
}

static void VblankEventResetFunc(LibKernel::EventQueue::KernelEqueueEvent* event) {
	EXIT_IF(event == nullptr);
	event->triggered    = false;
	event->event.fflags = 0;
	event->event.data   = 0;
}

static void VblankEventDeleteFunc(EventQueue::KernelEqueue                  eq,
                                  LibKernel::EventQueue::KernelEqueueEvent* event) {
	EXIT_IF(event == nullptr);
	EXIT_IF(event->filter.data == nullptr);

	EXIT_NOT_IMPLEMENTED(event->event.ident != VIDEO_OUT_EVENT_VBLANK);
	EXIT_NOT_IMPLEMENTED(event->event.filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT);

	if (event->filter.data != nullptr) {
		auto* video_out = static_cast<VideoOutConfig*>(event->filter.data);
		video_out->mutex.Lock();
		EXIT_IF(video_out->vblank_eqs.empty());
		auto it = std::find(video_out->vblank_eqs.begin(), video_out->vblank_eqs.end(), eq);
		EXIT_NOT_IMPLEMENTED(it == video_out->vblank_eqs.end());
		*it = nullptr;
		video_out->mutex.Unlock();
	}
}

static void PreVblankEventDeleteFunc(EventQueue::KernelEqueue                  eq,
                                     LibKernel::EventQueue::KernelEqueueEvent* event) {
	EXIT_IF(event == nullptr);
	EXIT_IF(event->filter.data == nullptr);

	EXIT_NOT_IMPLEMENTED(event->event.ident != VIDEO_OUT_EVENT_PRE_VBLANK_START);
	EXIT_NOT_IMPLEMENTED(event->event.filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT);

	if (event->filter.data != nullptr) {
		auto* video_out = static_cast<VideoOutConfig*>(event->filter.data);
		video_out->mutex.Lock();
		EXIT_IF(video_out->pre_vblank_eqs.empty());
		auto it = std::find(video_out->pre_vblank_eqs.begin(), video_out->pre_vblank_eqs.end(), eq);
		EXIT_NOT_IMPLEMENTED(it == video_out->pre_vblank_eqs.end());
		*it = nullptr;
		video_out->mutex.Unlock();
	}
}

static void VblankEventTriggerFunc(LibKernel::EventQueue::KernelEqueueEvent* event,
                                   void*                                     trigger_data) {
	EXIT_IF(event == nullptr);

	auto triggered_event = event->event;
	triggered_event.fflags =
	    ((triggered_event.fflags < 0xfu) ? triggered_event.fflags + 1u : triggered_event.fflags);
	triggered_event.data = MakeVideoOutEventData(triggered_event.data, trigger_data);
	if (event->triggered) {
		event->pending_events.push_back(triggered_event);
	} else {
		event->event     = triggered_event;
		event->triggered = true;
	}
}

static void OutputModeEventResetFunc(LibKernel::EventQueue::KernelEqueueEvent* event) {
	EXIT_IF(event == nullptr);
	event->triggered    = false;
	event->event.fflags = 0;
	event->event.data   = 0;
}

static void OutputModeEventDeleteFunc(EventQueue::KernelEqueue                  eq,
                                      LibKernel::EventQueue::KernelEqueueEvent* event) {
	EXIT_IF(event == nullptr);
	EXIT_IF(event->filter.data == nullptr);

	EXIT_NOT_IMPLEMENTED(event->event.ident != VIDEO_OUT_EVENT_SET_MODE);
	EXIT_NOT_IMPLEMENTED(event->event.filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT);

	if (event->filter.data != nullptr) {
		auto* video_out = static_cast<VideoOutConfig*>(event->filter.data);
		video_out->mutex.Lock();
		EXIT_IF(video_out->output_mode_eqs.empty());
		auto it =
		    std::find(video_out->output_mode_eqs.begin(), video_out->output_mode_eqs.end(), eq);
		EXIT_NOT_IMPLEMENTED(it == video_out->output_mode_eqs.end());
		*it = nullptr;
		video_out->mutex.Unlock();
	}
}

static void OutputModeEventTriggerFunc(LibKernel::EventQueue::KernelEqueueEvent* event,
                                       void*                                     trigger_data) {
	EXIT_IF(event == nullptr);

	auto triggered_event = event->event;
	triggered_event.fflags =
	    ((triggered_event.fflags < 0xfu) ? triggered_event.fflags + 1u : triggered_event.fflags);
	triggered_event.data = MakeVideoOutEventData(triggered_event.data, trigger_data);
	if (event->triggered) {
		event->pending_events.push_back(triggered_event);
	} else {
		event->event     = triggered_event;
		event->triggered = true;
	}
}

KYTY_SYSV_ABI int VideoOutDeleteFlipEvent(EventQueue::KernelEqueue eq, int handle) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	[[maybe_unused]] auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	if (eq == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}

	return EventQueue::KernelDeleteEvent(eq, VIDEO_OUT_EVENT_FLIP,
	                                     EventQueue::KERNEL_EVFILT_VIDEO_OUT);
}

KYTY_SYSV_ABI int VideoOutAddFlipEvent(EventQueue::KernelEqueue eq, int handle, void* udata) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	ctx->mutex.Lock();

	if (eq == nullptr) {
		ctx->mutex.Unlock();
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}

	bool add_eq =
	    (std::find(ctx->flip_eqs.begin(), ctx->flip_eqs.end(), eq) == ctx->flip_eqs.end());

	EventQueue::KernelEqueueEvent event;
	event.triggered                = false;
	event.event.ident              = VIDEO_OUT_EVENT_FLIP;
	event.event.filter             = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
	event.event.udata              = udata;
	event.event.fflags             = 0;
	event.event.data               = 0;
	event.filter.delete_event_func = FlipEventDeleteFunc;
	event.filter.reset_func        = FlipEventResetFunc;
	event.filter.trigger_func      = FlipEventTriggerFunc;
	event.filter.data              = ctx;

	int result = EventQueue::KernelAddEvent(eq, event);

	if (add_eq) {
		ctx->flip_eqs.push_back(eq);
	}

	ctx->mutex.Unlock();

	return result;
}

KYTY_SYSV_ABI int VideoOutDeleteVblankEvent(EventQueue::KernelEqueue eq, int handle) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	[[maybe_unused]] auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	if (eq == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}

	return EventQueue::KernelDeleteEvent(eq, VIDEO_OUT_EVENT_VBLANK,
	                                     EventQueue::KERNEL_EVFILT_VIDEO_OUT);
}

KYTY_SYSV_ABI int VideoOutDeletePreVblankStartEvent(EventQueue::KernelEqueue eq, int handle) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	[[maybe_unused]] auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	if (eq == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}

	return EventQueue::KernelDeleteEvent(eq, VIDEO_OUT_EVENT_PRE_VBLANK_START,
	                                     EventQueue::KERNEL_EVFILT_VIDEO_OUT);
}

KYTY_SYSV_ABI int VideoOutAddVblankEvent(LibKernel::EventQueue::KernelEqueue eq, int handle,
                                         void* udata) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	ctx->mutex.Lock();

	if (eq == nullptr) {
		ctx->mutex.Unlock();
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}

	bool add_eq =
	    (std::find(ctx->vblank_eqs.begin(), ctx->vblank_eqs.end(), eq) == ctx->vblank_eqs.end());

	EventQueue::KernelEqueueEvent event;
	event.triggered                = false;
	event.event.ident              = VIDEO_OUT_EVENT_VBLANK;
	event.event.filter             = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
	event.event.udata              = udata;
	event.event.fflags             = 0;
	event.event.data               = 0;
	event.filter.delete_event_func = VblankEventDeleteFunc;
	event.filter.reset_func        = VblankEventResetFunc;
	event.filter.trigger_func      = VblankEventTriggerFunc;
	event.filter.data              = ctx;

	int result = EventQueue::KernelAddEvent(eq, event);

	if (add_eq) {
		ctx->vblank_eqs.push_back(eq);
	}

	ctx->mutex.Unlock();

	return result;
}

KYTY_SYSV_ABI int VideoOutAddPreVblankStartEvent(LibKernel::EventQueue::KernelEqueue eq, int handle,
                                                 void* udata) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	ctx->mutex.Lock();

	if (eq == nullptr) {
		ctx->mutex.Unlock();
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}

	bool add_eq = (std::find(ctx->pre_vblank_eqs.begin(), ctx->pre_vblank_eqs.end(), eq) ==
	               ctx->pre_vblank_eqs.end());

	EventQueue::KernelEqueueEvent event;
	event.triggered                = false;
	event.event.ident              = VIDEO_OUT_EVENT_PRE_VBLANK_START;
	event.event.filter             = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
	event.event.udata              = udata;
	event.event.fflags             = 0;
	event.event.data               = 0;
	event.filter.delete_event_func = PreVblankEventDeleteFunc;
	event.filter.reset_func        = VblankEventResetFunc;
	event.filter.trigger_func      = VblankEventTriggerFunc;
	event.filter.data              = ctx;

	int result = EventQueue::KernelAddEvent(eq, event);

	if (add_eq) {
		ctx->pre_vblank_eqs.push_back(eq);
	}

	ctx->mutex.Unlock();

	return result;
}

KYTY_SYSV_ABI int VideoOutAddOutputModeEvent(LibKernel::EventQueue::KernelEqueue eq, int handle,
                                             void* udata) {
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	ctx->mutex.Lock();

	LOGF("\t eq     = 0x%016" PRIx64 "\n"
	     "\t handle = %d\n"
	     "\t udata  = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(eq), handle, reinterpret_cast<uint64_t>(udata));

	if (eq == nullptr) {
		ctx->mutex.Unlock();
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}

	bool add_eq = (std::find(ctx->output_mode_eqs.begin(), ctx->output_mode_eqs.end(), eq) ==
	               ctx->output_mode_eqs.end());

	EventQueue::KernelEqueueEvent event;
	event.triggered    = true;
	event.event.ident  = VIDEO_OUT_EVENT_SET_MODE;
	event.event.filter = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
	event.event.udata  = udata;
	event.event.fflags = 1;
	event.event.data   = MakeVideoOutEventData(0, reinterpret_cast<void*>(ctx->output_mode));
	event.filter.delete_event_func = OutputModeEventDeleteFunc;
	event.filter.reset_func        = OutputModeEventResetFunc;
	event.filter.trigger_func      = OutputModeEventTriggerFunc;
	event.filter.data              = ctx;

	int result = EventQueue::KernelAddEvent(eq, event);

	if (add_eq) {
		ctx->output_mode_eqs.push_back(eq);
	}

	ctx->mutex.Unlock();

	return result;
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
		if (buffers[i].reserved[0] != nullptr || buffers[i].reserved[1] != nullptr) {
			EXIT("video-out buffer reserved fields are unsupported\n");
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

	EXIT_IF(g_video_out_context == nullptr);

	auto* ctx = g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	if (flip_mode < VIDEO_OUT_FLIP_MODE_VSYNC || flip_mode > VIDEO_OUT_FLIP_MODE_VSYNC_MULTI) {
		LOGF("\t unsupported flip_mode = %d\n", flip_mode);
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	if (!IsValidBufferIndex(index)) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}
	Common::LockGuard lock(ctx->mutex);
	if (ctx->closing || (!IsSpecialBufferIndex(index) && ctx->unregistering[index])) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}
	if (!IsSpecialBufferIndex(index) && ctx->buffers[index].buffer_vulkan == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}

	if (!g_video_out_context->GetFlipQueue().Submit(ctx, index, flip_arg, false)) {
		return VIDEO_OUT_ERROR_FLIP_QUEUE_FULL;
	}

	return OK;
}

} // namespace Libs::VideoOut

namespace Libs::Presentation {

int DisplayBufferSubmitFlipFromGpu(int handle, int index, int flip_mode, int64_t flip_arg) {
	EXIT_IF(VideoOut::g_video_out_context == nullptr || Graphics::g_render_ctx == nullptr);

	auto* ctx = VideoOut::g_video_out_context->Get(handle);
	if (ctx == nullptr) {
		return VideoOut::VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	if (flip_mode < VideoOut::VIDEO_OUT_FLIP_MODE_VSYNC ||
	    flip_mode > VideoOut::VIDEO_OUT_FLIP_MODE_VSYNC_MULTI) {
		return VideoOut::VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	if (!VideoOut::IsValidBufferIndex(index)) {
		return VideoOut::VIDEO_OUT_ERROR_INVALID_INDEX;
	}
	Common::LockGuard lock(ctx->mutex);
	if (ctx->closing || (!VideoOut::IsSpecialBufferIndex(index) && ctx->unregistering[index])) {
		return VideoOut::VIDEO_OUT_ERROR_INVALID_INDEX;
	}
	if (!VideoOut::IsSpecialBufferIndex(index) && ctx->buffers[index].buffer_vulkan == nullptr) {
		return VideoOut::VIDEO_OUT_ERROR_INVALID_INDEX;
	}
	if (!VideoOut::g_video_out_context->GetFlipQueue().Submit(ctx, index, flip_arg, true)) {
		return VideoOut::VIDEO_OUT_ERROR_FLIP_QUEUE_FULL;
	}

	return OK;
}

void DisplayBufferCompleteFlipFromGpu(int handle, int index) {
	EXIT_IF(VideoOut::g_video_out_context == nullptr || Graphics::g_render_ctx == nullptr);

	auto* ctx = VideoOut::g_video_out_context->Get(handle);
	if (ctx == nullptr || !VideoOut::IsValidBufferIndex(index)) {
		EXIT("GPU flip completed with invalid handle or index, handle=%d index=%d\n", handle,
		     index);
	}
	Common::LockGuard lock(ctx->mutex);
	if (ctx->closing || (!VideoOut::IsSpecialBufferIndex(index) && ctx->unregistering[index])) {
		EXIT("GPU flip completed for an unavailable surface, handle=%d index=%d\n", handle, index);
	}
	if (!VideoOut::IsSpecialBufferIndex(index)) {
		auto* image = ctx->buffers[index].buffer_vulkan;
		if (image == nullptr) {
			EXIT("GPU flip completed without a native surface, handle=%d index=%d\n", handle,
			     index);
		}
		Graphics::g_render_ctx->GetTextureCache()->RefreshVideoOut(image);
	}
	VideoOut::g_video_out_context->GetFlipQueue().AllowEopPresent(ctx, index);
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
	for (auto& output_mode_eq: ctx->output_mode_eqs) {
		if (output_mode_eq != nullptr) {
			auto trigger_result = EventQueue::KernelTriggerEvent(
			    output_mode_eq, VIDEO_OUT_EVENT_SET_MODE, EventQueue::KERNEL_EVFILT_VIDEO_OUT,
			    reinterpret_cast<void*>(ctx->output_mode));
			EXIT_NOT_IMPLEMENTED(trigger_result != OK);
		}
	}
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
