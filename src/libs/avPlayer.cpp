#include "common/common.h"
#include "common/logging/log.h"
#include "common/magicEnum.h"
#include "common/stringUtils.h"
#include "kernel/fileSystem.h"
#include "kernel/pthread.h"
#include "libs/audio.h"
#include "libs/libs.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixfmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace Libs::Audio::AvPlayer {

LIB_NAME("AvPlayer", "AvPlayer");

using AvPlayerAllocate          = KYTY_SYSV_ABI void* (*)(void*, uint32_t, uint32_t);
using AvPlayerDeallocate        = KYTY_SYSV_ABI void (*)(void*, void*);
using AvPlayerAllocateTexture   = KYTY_SYSV_ABI void* (*)(void*, uint32_t, uint32_t);
using AvPlayerDeallocateTexture = KYTY_SYSV_ABI void (*)(void*, void*);
using AvPlayerOpenFile          = KYTY_SYSV_ABI int (*)(void*, const char*);
using AvPlayerCloseFile         = KYTY_SYSV_ABI int (*)(void*);
using AvPlayerReadOffsetFile    = KYTY_SYSV_ABI int (*)(void*, uint8_t*, uint64_t, uint32_t);
using AvPlayerSizeFile          = KYTY_SYSV_ABI uint64_t (*)(void*);
using AvPlayerEventCallback     = KYTY_SYSV_ABI void (*)(void*, int32_t, int32_t, void*);
using Bool                      = uint8_t;

constexpr int32_t  AVPLAYER_EVENT_STATE_STOP       = 0x01;
constexpr int32_t  AVPLAYER_EVENT_STATE_READY      = 0x02;
constexpr int32_t  AVPLAYER_EVENT_STATE_PLAY       = 0x03;
constexpr int32_t  AVPLAYER_EVENT_STATE_PAUSE      = 0x04;
constexpr int32_t  AVPLAYER_EVENT_WARNING_ID       = 0x20;
constexpr int32_t  AVPLAYER_ERROR_INVALID_PARAMS   = -2140536831;
constexpr int32_t  AVPLAYER_ERROR_OPERATION_FAILED = -2140536830;
constexpr int32_t  AVPLAYER_ERROR_NO_MEMORY        = -2140536829;
constexpr int32_t  AVPLAYER_ERROR_NOT_SUPPORTED    = -2140536828;
constexpr int32_t  AVPLAYER_WARNING_LOOPING_BACK   = -2140536671;
constexpr int32_t  AVPLAYER_WARNING_JUMP_COMPLETE  = -2140536669;
constexpr int32_t  AVPLAYER_TRICK_SPEED_NORMAL     = 100;
constexpr int32_t  AVPLAYER_TRICK_SPEED_MIN        = 400;
constexpr int32_t  AVPLAYER_TRICK_SPEED_MAX        = 3200;
constexpr uint32_t AVPLAYER_DEMUX_BUFFER_DEFAULT   = 4 * 1024 * 1024;
constexpr uint32_t AVPLAYER_DEMUX_BUFFER_MIN       = 128 * 1024;
constexpr uint32_t AVPLAYER_DEMUX_BUFFER_MAX       = 32 * 1024 * 1024;
constexpr int      AVPLAYER_AUDIO_BUFFERS          = 8;

enum AvPlayerUriType : uint32_t { AvPlayerUriTypeSource = 0 };
enum AvPlayerSourceType : uint32_t {
	AvPlayerSourceUnknown  = 0,
	AvPlayerSourceFileMp4  = 1,
	AvPlayerSourceFileWebm = 2,
	AvPlayerSourceHls      = 8
};

enum AvPlayerDebuglevels : int {
	AvplayerDbgNone,
	AvplayerDbgInfo,
	AvplayerDbgWarnings,
	AvplayerDbgAll
};
enum AvPlayerStreamType : uint32_t {
	AvPlayerStreamUnknown   = 0,
	AvPlayerStreamVideo     = 1,
	AvPlayerStreamAudio     = 2,
	AvPlayerStreamTimedText = 3
};
enum AvPlayerVideoDecoderType : uint32_t {
	AvPlayerVideoDecoderDefault   = 0,
	AvPlayerVideoDecoderSoftware2 = 1
};
enum AvPlayerAudioDecoderType : uint32_t { AvPlayerAudioDecoderDefault = 0 };
enum AvPlayerAudioChannelOrder : uint32_t {
	AvPlayerAudioChannelOrderDefault = 0,
	AvPlayerAudioChannelOrderExt     = 1,
	AvPlayerAudioChannelOrderLsRs    = 2
};

struct AvPlayerMemAllocator {
	void*                     object_pointer     = nullptr;
	AvPlayerAllocate          allocate           = nullptr;
	AvPlayerDeallocate        deallocate         = nullptr;
	AvPlayerAllocateTexture   allocate_texture   = nullptr;
	AvPlayerDeallocateTexture deallocate_texture = nullptr;
};
struct AvPlayerFileReplacement {
	void*                  object_pointer = nullptr;
	AvPlayerOpenFile       open           = nullptr;
	AvPlayerCloseFile      close          = nullptr;
	AvPlayerReadOffsetFile read_offset    = nullptr;
	AvPlayerSizeFile       size           = nullptr;
};
struct AvPlayerEventReplacement {
	void*                 object_pointer = nullptr;
	AvPlayerEventCallback event_callback = nullptr;
};
struct AvPlayerUri {
	const char* name   = nullptr;
	uint32_t    length = 0;
};
struct AvPlayerSourceDetails {
	AvPlayerUri        uri;
	uint8_t            reserved1[64] = {};
	AvPlayerSourceType source_type   = AvPlayerSourceUnknown;
	uint8_t            reserved2[44] = {};
};
struct AvPlayerInitData {
	AvPlayerMemAllocator     memory_replacement;
	AvPlayerFileReplacement  file_replacement;
	AvPlayerEventReplacement event_replacement;
	AvPlayerDebuglevels      debug_level                   = AvplayerDbgNone;
	uint32_t                 base_priority                 = 0;
	int32_t                  num_output_video_framebuffers = 0;
	Bool                     auto_start                    = 0;
	uint8_t                  reserved[3]                   = {};
	const char*              default_language              = nullptr;
};
struct AvPlayerThreadInfo {
	uint32_t priority     = 0;
	uint32_t stack_size   = 0;
	uint64_t affinity     = 0;
	uint8_t  reserved[32] = {};
};
struct AvPlayerInitDataEx {
	size_t                   this_size = 0;
	AvPlayerMemAllocator     memory_replacement;
	AvPlayerFileReplacement  file_replacement;
	AvPlayerEventReplacement event_replacement;
	const char*              default_language = nullptr;
	AvPlayerDebuglevels      debug_level      = AvplayerDbgNone;
	Bool                     auto_start       = 0;
	uint8_t                  reserved[3]      = {};
	AvPlayerThreadInfo       audio_decoder;
	AvPlayerThreadInfo       video_decoder;
	AvPlayerThreadInfo       demuxer;
	AvPlayerThreadInfo       event;
	AvPlayerThreadInfo       call_queue;
	AvPlayerThreadInfo       http_command_processor;
	AvPlayerThreadInfo       http_segment_manager;
	AvPlayerThreadInfo       http_streamlist;
	AvPlayerThreadInfo       file_streaming;
	int32_t                  num_output_video_framebuffers = 0;
	uint8_t                  reserved2[4]                  = {};
};
struct AvPlayerDecoderInit {
	union {
		AvPlayerVideoDecoderType video_type;
		AvPlayerAudioDecoderType audio_type;
		uint8_t                  reserved[4];
	} decoder_type {};
	uint8_t reserved[4] = {};
	union {
		struct {
			uint64_t cpu_affinity_mask;
			int32_t  cpu_thread_priority;
			uint8_t  decode_input_queue_depth;
			uint8_t  compute_pipe_id;
			uint8_t  compute_queue_id;
			uint8_t  reserved[17];
		} video_sw2;
		struct {
			AvPlayerAudioChannelOrder audio_channel_order;
			uint8_t                   reserved[28];
		} audio;
		uint8_t reserved[32];
	} decoder_params {};
};
struct AvPlayerHttpContext {
	uint32_t http_context_id = 0;
	uint32_t ssl_context_id  = 0;
};
struct AvPlayerPostInitData {
	uint32_t            demux_video_buffer_size = 0;
	uint8_t             reserved[4]             = {};
	AvPlayerDecoderInit video_decoder_init;
	AvPlayerDecoderInit audio_decoder_init;
	AvPlayerHttpContext http_context;
	uint8_t             reserved2[56] = {};
};
struct AvPlayerStartInfoEx {
	size_t   this_size               = 0;
	uint64_t start_time_milliseconds = 0;
};
struct AvPlayerAudioEx {
	uint16_t channel_count    = 0;
	uint8_t  reserved[2]      = {};
	uint32_t sample_rate      = 0;
	uint32_t size             = 0;
	uint8_t  language_code[4] = {};
	uint8_t  reserved1[64]    = {};
};
struct AvPlayerAudio {
	uint16_t channel_count    = 0;
	uint8_t  reserved[2]      = {};
	uint32_t sample_rate      = 0;
	uint32_t size             = 0;
	uint8_t  language_code[4] = {};
};
struct AvPlayerVideoEx {
	uint32_t width                    = 0;
	uint32_t height                   = 0;
	float    aspect_ratio             = 0.0f;
	uint8_t  language_code[4]         = {};
	uint8_t  reserved[4]              = {};
	uint32_t crop_left_offset         = 0;
	uint32_t crop_right_offset        = 0;
	uint32_t crop_top_offset          = 0;
	uint32_t crop_bottom_offset       = 0;
	uint32_t pitch                    = 0;
	uint8_t  luma_bit_depth           = 0;
	uint8_t  chroma_bit_depth         = 0;
	Bool     video_full_range_flag    = 0;
	uint8_t  reserved2[5]             = {};
	double   framerate                = 0.0;
	uint32_t colour_primaries         = 0;
	uint32_t transfer_characteristics = 0;
	uint8_t  reserved3[16]            = {};
};
struct AvPlayerVideo {
	uint32_t width            = 0;
	uint32_t height           = 0;
	float    aspect_ratio     = 0.0f;
	uint8_t  language_code[4] = {};
};
struct AvPlayerTextPosition {
	int16_t top    = 0;
	int16_t left   = 0;
	int16_t bottom = 0;
	int16_t right  = 0;
};
struct AvPlayerTimedTextEx {
	uint8_t              language_code[4] = {};
	uint16_t             text_size        = 0;
	uint16_t             font_size        = 0;
	AvPlayerTextPosition position;
	uint8_t              reserved1[64] = {};
};
struct AvPlayerTimedText {
	uint8_t              language_code[4] = {};
	uint16_t             text_size        = 0;
	uint16_t             font_size        = 0;
	AvPlayerTextPosition position;
};
union AvPlayerStreamDetailsEx {
	AvPlayerAudioEx     audio;
	AvPlayerVideoEx     video;
	AvPlayerTimedTextEx subs;
	uint8_t             reserved1[80];
};
union AvPlayerStreamDetails {
	AvPlayerAudio     audio;
	AvPlayerVideo     video;
	AvPlayerTimedText subs;
	uint8_t           reserved[16];
};
struct AvPlayerFrameInfo {
	void*                 data        = nullptr;
	uint8_t               reserved[4] = {};
	uint64_t              time_stamp  = 0;
	AvPlayerStreamDetails details {};
};
struct AvPlayerFrameInfoEx {
	void*                   data        = nullptr;
	uint8_t                 reserved[4] = {};
	uint64_t                time_stamp  = 0;
	AvPlayerStreamDetailsEx details {};
};
struct AvPlayerStreamInfo {
	AvPlayerStreamType    type        = AvPlayerStreamUnknown;
	uint8_t               reserved[4] = {};
	AvPlayerStreamDetails details {};
	uint64_t              duration = 0;
};
struct AvPlayerStreamInfoEx {
	size_t                  this_size   = 0;
	AvPlayerStreamType      type        = AvPlayerStreamUnknown;
	uint8_t                 reserved[4] = {};
	AvPlayerStreamDetailsEx details {};
	uint64_t                duration = 0;
};

static_assert(offsetof(AvPlayerPostInitData, video_decoder_init) == 8);
static_assert(offsetof(AvPlayerVideoEx, framerate) == 48);
static_assert(sizeof(AvPlayerStreamDetailsEx) == 80);
static_assert(offsetof(AvPlayerStreamInfoEx, duration) == 96);
static_assert(offsetof(AvPlayerFrameInfoEx, details) == 24);

static std::string fferr(int e) {
	char buf[AV_ERROR_MAX_STRING_SIZE] = {};
	av_make_error_string(buf, sizeof(buf), e);
	return buf;
}
static uint64_t to_ms(int64_t v, AVRational tb) {
	return v == AV_NOPTS_VALUE ? 0
	                           : static_cast<uint64_t>(std::max<int64_t>(
	                                 0, av_rescale_q(v, tb, AVRational {1, 1000})));
}
static uint32_t align_up(uint32_t v, uint32_t a) {
	return a == 0 ? v : ((v + a - 1) / a) * a;
}
static bool ieq(std::string_view a, std::string_view b) {
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char l, char r) {
		       return std::tolower(static_cast<unsigned char>(l)) ==
		              std::tolower(static_cast<unsigned char>(r));
	       });
}
static void lang(uint8_t out[4], AVDictionary* md) {
	std::memset(out, 0, 4);
	if (auto* e = av_dict_get(md, "language", nullptr, 0); e != nullptr && e->value != nullptr) {
		std::memcpy(out, e->value, std::min<size_t>(3, std::strlen(e->value)));
	} else {
		out[0] = 'u';
		out[1] = 'n';
		out[2] = 'd';
	}
}

static AvPlayerSourceType source_type_from_path(std::string_view path) {
	if (auto q = path.find_first_of("?#"); q != std::string_view::npos) {
		path = path.substr(0, q);
	}
	auto dot = path.rfind('.');
	if (dot == std::string_view::npos) {
		return AvPlayerSourceUnknown;
	}
	auto ext = path.substr(dot);
	if (auto slash = ext.find('/'); slash != std::string_view::npos) {
		ext = ext.substr(0, slash);
	}
	if (ieq(ext, ".mp4") || ieq(ext, ".m4v") || ieq(ext, ".m4a") || ieq(ext, ".mov") ||
	    ieq(ext, ".m3d")) {
		return AvPlayerSourceFileMp4;
	}
	if (ieq(ext, ".webm")) {
		return AvPlayerSourceFileWebm;
	}
	if (ieq(ext, ".m3u8")) {
		return AvPlayerSourceHls;
	}
	return AvPlayerSourceUnknown;
}

static bool source_type_is_file(AvPlayerSourceType type) {
	return type == AvPlayerSourceUnknown || type == AvPlayerSourceFileMp4 ||
	       type == AvPlayerSourceFileWebm;
}

static bool codec_is_supported_for_source(AvPlayerSourceType source_type, AVMediaType media_type,
                                          AVCodecID codec_id) {
	if (source_type != AvPlayerSourceFileWebm) {
		return true;
	}

	switch (media_type) {
		case AVMEDIA_TYPE_VIDEO:
			return codec_id == AV_CODEC_ID_VP8 || codec_id == AV_CODEC_ID_VP9 ||
			       codec_id == AV_CODEC_ID_AV1;
		case AVMEDIA_TYPE_AUDIO:
			return codec_id == AV_CODEC_ID_OPUS || codec_id == AV_CODEC_ID_VORBIS;
		default: return true;
	}
}

struct PacketDeleter {
	void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;

struct DemuxPacket {
	Packet   packet;
	uint64_t timestamp_offset = 0;
};

class PacketQueue {
public:
	explicit PacketQueue(size_t byte_capacity): capacity(byte_capacity) {}
	bool Push(DemuxPacket value, const std::atomic_bool& stop) {
		const auto size =
		    value.packet == nullptr ? 0u : static_cast<size_t>(std::max(value.packet->size, 0));
		std::unique_lock lock(mutex);
		available.wait(lock, [&] {
			return stop || bytes + size <= capacity || (queue.empty() && bytes == 0);
		});
		if (stop) {
			return false;
		}
		bytes += size;
		queue.push_back(std::move(value));
		available.notify_all();
		return true;
	}
	std::optional<DemuxPacket> WaitPop(const std::atomic_bool& stop,
	                                   const std::atomic_bool& finished) {
		std::unique_lock lock(mutex);
		available.wait(lock, [&] { return stop || finished || !queue.empty(); });
		if (stop || queue.empty()) {
			return std::nullopt;
		}
		auto value = std::move(queue.front());
		queue.pop_front();
		if (value.packet != nullptr) {
			bytes -= static_cast<size_t>(std::max(value.packet->size, 0));
		}
		available.notify_all();
		return value;
	}
	void Clear() {
		std::lock_guard lock(mutex);
		queue.clear();
		bytes = 0;
		available.notify_all();
	}
	void Notify() { available.notify_all(); }

private:
	std::mutex              mutex;
	std::condition_variable available;
	std::deque<DemuxPacket> queue;
	size_t                  bytes    = 0;
	size_t                  capacity = 0;
};

class GuestBuffer {
public:
	GuestBuffer() = default;
	GuestBuffer(AvPlayerMemAllocator mem, uint32_t align, uint32_t size, bool texture)
	    : m_mem(mem), m_size(size), m_texture(texture) {
		m_data =
		    static_cast<uint8_t*>(texture ? mem.allocate_texture(mem.object_pointer, align, size)
		                                  : mem.allocate(mem.object_pointer, align, size));
	}
	~GuestBuffer() { Reset(); }
	GuestBuffer(const GuestBuffer&)            = delete;
	GuestBuffer& operator=(const GuestBuffer&) = delete;
	GuestBuffer(GuestBuffer&& r) noexcept { Move(r); }
	GuestBuffer& operator=(GuestBuffer&& r) noexcept {
		if (this != &r) {
			Reset();
			Move(r);
		}
		return *this;
	}
	uint8_t* Get() const { return m_data; }
	bool     Valid() const { return m_data != nullptr; }
	uint32_t Size() const { return m_size; }

private:
	void Move(GuestBuffer& r) {
		m_mem     = r.m_mem;
		m_data    = r.m_data;
		m_size    = r.m_size;
		m_texture = r.m_texture;
		r.m_data  = nullptr;
		r.m_size  = 0;
	}
	void Reset() {
		if (m_data != nullptr) {
			if (m_texture) {
				m_mem.deallocate_texture(m_mem.object_pointer, m_data);
			} else {
				m_mem.deallocate(m_mem.object_pointer, m_data);
			}
			m_data = nullptr;
		}
	}
	AvPlayerMemAllocator m_mem {};
	uint8_t*             m_data    = nullptr;
	uint32_t             m_size    = 0;
	bool                 m_texture = false;
};

template <typename T>
class WorkQueue {
public:
	void Push(T value) {
		std::lock_guard lock(mutex);
		queue.push_back(std::move(value));
		available.notify_all();
	}
	std::optional<T> TryPop() {
		std::lock_guard lock(mutex);
		if (queue.empty()) {
			return std::nullopt;
		}
		auto value = std::move(queue.front());
		queue.pop_front();
		available.notify_all();
		return value;
	}
	template <typename Predicate>
	std::optional<T> TryPopIf(Predicate&& predicate) {
		std::lock_guard lock(mutex);
		if (queue.empty() || !std::forward<Predicate>(predicate)(queue.front())) {
			return std::nullopt;
		}
		auto value = std::move(queue.front());
		queue.pop_front();
		available.notify_all();
		return value;
	}
	std::optional<T> WaitPop(const std::atomic_bool& stop) {
		std::unique_lock lock(mutex);
		available.wait(lock, [&] { return stop || !queue.empty(); });
		if (stop || queue.empty()) {
			return std::nullopt;
		}
		auto value = std::move(queue.front());
		queue.pop_front();
		available.notify_all();
		return value;
	}
	bool Empty() const {
		std::lock_guard lock(mutex);
		return queue.empty();
	}
	void Clear() {
		std::lock_guard lock(mutex);
		queue.clear();
		available.notify_all();
	}
	void Notify() { available.notify_all(); }

private:
	mutable std::mutex      mutex;
	std::condition_variable available;
	std::deque<T>           queue;
};

struct ReadyFrame {
	std::unique_ptr<GuestBuffer> buffer;
	AvPlayerFrameInfoEx          info {};
};

class FileStreamer {
public:
	explicit FileStreamer(AvPlayerFileReplacement f): file(f) {}
	~FileStreamer() {
		if (ctx != nullptr) {
			avio_context_free(&ctx);
		}
		if (opened && file.close != nullptr) {
			file.close(file.object_pointer);
		}
	}
	bool Init(const std::string& path) {
		if (file.open == nullptr || file.close == nullptr || file.read_offset == nullptr ||
		    file.size == nullptr) {
			return false;
		}
		if (file.open(file.object_pointer, path.c_str()) < 0) {
			return false;
		}
		opened = true;
		size   = file.size(file.object_pointer);
		if (size == 0) {
			return false;
		}
		auto* buf = static_cast<uint8_t*>(av_malloc(4096));
		if (buf == nullptr) {
			return false;
		}
		ctx = avio_alloc_context(buf, 4096, 0, this, Read, nullptr, Seek);
		return ctx != nullptr;
	}
	AVIOContext* Context() const { return ctx; }

private:
	static int Read(void* opaque, uint8_t* buf, int len) {
		auto* s = static_cast<FileStreamer*>(opaque);
		if (s->pos >= s->size) {
			return AVERROR_EOF;
		}
		len = static_cast<int>(std::min<uint64_t>(len, s->size - s->pos));
		auto r =
		    s->file.read_offset(s->file.object_pointer, buf, s->pos, static_cast<uint32_t>(len));
		if (r <= 0) {
			return r == 0 ? AVERROR_EOF : r;
		}
		s->pos += static_cast<uint64_t>(r);
		return r;
	}
	static int64_t Seek(void* opaque, int64_t off, int whence) {
		auto* s = static_cast<FileStreamer*>(opaque);
		if ((whence & AVSEEK_SIZE) != 0) {
			return static_cast<int64_t>(s->size);
		}
		int64_t p = whence == SEEK_SET
		                ? off
		                : (whence == SEEK_CUR
		                       ? static_cast<int64_t>(s->pos) + off
		                       : (whence == SEEK_END ? static_cast<int64_t>(s->size) + off : -1));
		if (p < 0) {
			return -1;
		}
		p      = std::min<int64_t>(p, static_cast<int64_t>(s->size));
		s->pos = static_cast<uint64_t>(p);
		return p;
	}
	AvPlayerFileReplacement file;
	bool                    opened = false;
	uint64_t                pos    = 0;
	uint64_t                size   = 0;
	AVIOContext*            ctx    = nullptr;
};

class Source {
public:
	Source(AvPlayerMemAllocator m, AvPlayerFileReplacement f, int32_t video_buffers, bool vdec2,
	       uint32_t demux_buffer_size)
	    : mem(m), file(f),
	      max_video_buffers(std::clamp(video_buffers <= 0 ? 2 : video_buffers, 2, 16)),
	      use_vdec2(vdec2),
	      video_packets(
	          std::clamp(demux_buffer_size == 0 ? AVPLAYER_DEMUX_BUFFER_DEFAULT : demux_buffer_size,
	                     AVPLAYER_DEMUX_BUFFER_MIN, AVPLAYER_DEMUX_BUFFER_MAX)),
	      audio_packets(
	          std::clamp(demux_buffer_size == 0 ? AVPLAYER_DEMUX_BUFFER_DEFAULT : demux_buffer_size,
	                     AVPLAYER_DEMUX_BUFFER_MIN, AVPLAYER_DEMUX_BUFFER_MAX)) {}
	~Source() { Close(); }
	int Init(const std::string& p, AvPlayerSourceType requested) {
		path        = p;
		source_type = requested == AvPlayerSourceUnknown ? source_type_from_path(path) : requested;
		if (source_type == AvPlayerSourceHls) {
			return AVPLAYER_ERROR_NOT_SUPPORTED;
		}
		if (!source_type_is_file(source_type)) {
			return AVPLAYER_ERROR_INVALID_PARAMS;
		}
		auto* raw = avformat_alloc_context();
		if (raw == nullptr) {
			return AVPLAYER_ERROR_NO_MEMORY;
		}
		raw->interrupt_callback.callback = [](void* opaque) {
			return static_cast<Source*>(opaque)->interrupt_io.load() ? 1 : 0;
		};
		raw->interrupt_callback.opaque = this;
		if (file.open != nullptr) {
			streamer = std::make_unique<FileStreamer>(file);
			if (!streamer->Init(path)) {
				avformat_free_context(raw);
				return AVPLAYER_ERROR_OPERATION_FAILED;
			}
			raw->pb = streamer->Context();
			if (auto rc = avformat_open_input(&raw, nullptr, nullptr, nullptr); rc < 0) {
				LOGF("\t avformat_open_input callback failed: %s\n", fferr(rc).c_str());
				return AVPLAYER_ERROR_OPERATION_FAILED;
			}
		} else {
			auto real     = LibKernel::FileSystem::GetRealFilename(std::string(path.c_str()));
			auto real_str = Common::PathToString(real);
			if (auto rc = avformat_open_input(&raw, real_str.c_str(), nullptr, nullptr); rc < 0) {
				LOGF("\t avformat_open_input failed: %s path=%s\n", fferr(rc).c_str(),
				     real_str.c_str());
				return AVPLAYER_ERROR_OPERATION_FAILED;
			}
		}
		fmt = raw;
		if (auto rc = avformat_find_stream_info(fmt, nullptr); rc < 0) {
			LOGF("\t avformat_find_stream_info failed: %s\n", fferr(rc).c_str());
			return AVPLAYER_ERROR_OPERATION_FAILED;
		}
		return 0;
	}
	int StreamCount() const { return fmt == nullptr ? 0 : static_cast<int>(fmt->nb_streams); }
	int Enable(uint32_t id) {
		std::scoped_lock lifecycle_lock(lifecycle_mutex);
		std::lock_guard  state_lock(mutex);
		if (!stopped) {
			return AVPLAYER_ERROR_OPERATION_FAILED;
		}
		if (fmt == nullptr || id >= fmt->nb_streams) {
			return AVPLAYER_ERROR_OPERATION_FAILED;
		}
		if (!StreamSupported(static_cast<int>(id))) {
			return AVPLAYER_ERROR_NOT_SUPPORTED;
		}
		switch (fmt->streams[id]->codecpar->codec_type) {
			case AVMEDIA_TYPE_VIDEO: video_id = static_cast<int>(id); return 0;
			case AVMEDIA_TYPE_AUDIO: audio_id = static_cast<int>(id); return 0;
			default: return AVPLAYER_ERROR_NOT_SUPPORTED;
		}
	}
	int Disable(uint32_t id) {
		std::scoped_lock lifecycle_lock(lifecycle_mutex);
		std::lock_guard  state_lock(mutex);
		if (!stopped) {
			return AVPLAYER_ERROR_OPERATION_FAILED;
		}
		if (video_id == static_cast<int>(id)) {
			video_id.reset();
			return 0;
		}
		if (audio_id == static_cast<int>(id)) {
			audio_id.reset();
			return 0;
		}
		return AVPLAYER_ERROR_OPERATION_FAILED;
	}
	int Change(uint32_t old_id, uint32_t new_id) {
		std::scoped_lock lifecycle_lock(lifecycle_mutex);
		if (fmt == nullptr || new_id >= fmt->nb_streams) {
			return AVPLAYER_ERROR_OPERATION_FAILED;
		}
		if (!StreamSupported(static_cast<int>(new_id))) {
			return AVPLAYER_ERROR_NOT_SUPPORTED;
		}
		uint64_t time       = 0;
		bool     restart    = false;
		bool     was_paused = false;
		{
			std::lock_guard state_lock(mutex);
			if (audio_id != static_cast<int>(old_id) ||
			    fmt->streams[new_id]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
				return AVPLAYER_ERROR_OPERATION_FAILED;
			}
			time       = CurrentTimeNoLock();
			restart    = !stopped;
			was_paused = paused;
			stopped    = true;
		}
		if (restart) {
			StopWorkers();
		}
		{
			std::lock_guard state_lock(mutex);
			audio_id = static_cast<int>(new_id);
		}
		if (!restart) {
			return 0;
		}
		return StartImpl(time, was_paused);
	}
	int Start(uint64_t ms = 0) {
		std::scoped_lock lifecycle_lock(lifecycle_mutex);
		return StartImpl(ms);
	}
	int StartImpl(uint64_t ms, bool paused_after_start = false, bool show_seek_frame = false) {
		{
			std::lock_guard lock(mutex);
			stopped = true;
		}
		StopWorkers();
		interrupt_io = false;

		int result = 0;
		{
			std::lock_guard lock(mutex);
			ResetNoLock(true);
			if (!video_id && !audio_id) {
				AutoEnable();
			}
			if (!video_id && !audio_id) {
				return AVPLAYER_ERROR_OPERATION_FAILED;
			}
			if (!OpenCodecs()) {
				ResetNoLock(true);
				return AVPLAYER_ERROR_OPERATION_FAILED;
			}
			if (!AllocateBuffers()) {
				ResetNoLock(true);
				return AVPLAYER_ERROR_NO_MEMORY;
			}
			SeekNoLock(ms);
			start_time_ms = ms;
			clock_start   = std::chrono::steady_clock::now();
			paused_extra  = {};
			paused        = paused_after_start;
			pause_time    = clock_start;
			seek_video_frame_pending =
			    paused_after_start && show_seek_frame && video_id.has_value();
			stopped = false;
		}

		if (!StartWorkers()) {
			std::lock_guard lock(mutex);
			stopped = true;
			ResetNoLock(true);
			result = AVPLAYER_ERROR_OPERATION_FAILED;
		}
		if (result == 0 && video_id) {
			::printf("AvPlayer video started playing\n");
		}
		return result;
	}
	int Stop() {
		std::scoped_lock lifecycle_lock(lifecycle_mutex);
		bool             was_playing_video = false;
		{
			std::lock_guard lock(mutex);
			was_playing_video = !stopped && video_id.has_value();
			stopped           = true;
		}
		StopWorkers();
		{
			std::lock_guard lock(mutex);
			ResetNoLock();
		}
		if (was_playing_video) {
			::printf("AvPlayer video stopped\n");
		}
		return 0;
	}
	void Pause() {
		std::scoped_lock lifecycle_lock(lifecycle_mutex);
		std::lock_guard  lock(mutex);
		paused     = true;
		pause_time = std::chrono::steady_clock::now();
	}
	void Resume() {
		std::scoped_lock lifecycle_lock(lifecycle_mutex);
		std::lock_guard  lock(mutex);
		if (paused) {
			paused_extra += std::chrono::steady_clock::now() - pause_time;
		}
		paused                   = false;
		seek_video_frame_pending = false;
	}
	void SetLoop(bool v) { loop = v; }
	void SetSync(uint32_t v) {
		std::lock_guard lock(mutex);
		sync_mode = v;
	}
	void SetTrick(int32_t v) {
		std::lock_guard lock(mutex);
		trick_speed = v;
	}
	bool Active() const {
		std::lock_guard lock(mutex);
		return !stopped && !pipeline_failed &&
		       (!demux_eof || !video_done || !audio_done || !video_frames.Empty() ||
		        !audio_frames.Empty());
	}
	uint64_t CurrentTime() const {
		std::lock_guard lock(mutex);
		return CurrentTimeNoLock();
	}
	int Jump(uint64_t ms) {
		std::scoped_lock lifecycle_lock(lifecycle_mutex);
		bool             was_paused = false;
		{
			std::lock_guard lock(mutex);
			was_paused = paused;
		}
		return StartImpl(ms, was_paused, true);
	}
	uint64_t CurrentTimeNoLock() const {
		if (stopped) {
			return 0;
		}
		using namespace std::chrono;
		auto now = paused ? pause_time : steady_clock::now();
		return start_time_ms +
		       static_cast<uint64_t>(
		           duration_cast<milliseconds>(now - clock_start - paused_extra).count());
	}
	int Info(uint32_t id, AvPlayerStreamInfo* out) const {
		if (out == nullptr) {
			return AVPLAYER_ERROR_INVALID_PARAMS;
		}
		std::memset(out, 0, sizeof(*out));
		if (fmt == nullptr || id >= fmt->nb_streams) {
			return AVPLAYER_ERROR_OPERATION_FAILED;
		}
		FillCommon(id, &out->type, &out->duration, &out->details, nullptr);
		return 0;
	}
	int InfoEx(uint32_t id, AvPlayerStreamInfoEx* out) const {
		if (out == nullptr) {
			return AVPLAYER_ERROR_INVALID_PARAMS;
		}
		std::memset(out, 0, sizeof(*out));
		out->this_size = sizeof(*out);
		if (fmt == nullptr || id >= fmt->nb_streams) {
			return AVPLAYER_ERROR_OPERATION_FAILED;
		}
		FillCommon(id, &out->type, &out->duration, nullptr, &out->details);
		return 0;
	}
	bool Video(AvPlayerFrameInfoEx* out) {
		if (out == nullptr) {
			return false;
		}
		std::lock_guard lock(mutex);
		if (stopped || !video_id) {
			return false;
		}
		const bool deliver_seek_frame = paused && seek_video_frame_pending;
		if (paused && !deliver_seek_frame) {
			return false;
		}
		auto frame = video_frames.TryPopIf([&](const ReadyFrame& candidate) {
			if (deliver_seek_frame || sync_mode != 0) {
				return true;
			}
			if (audio_id) {
				return candidate.info.time_stamp <= last_audio_ts;
			}
			auto now = CurrentTimeNoLock();
			return now == 0 || candidate.info.time_stamp <= now;
		});
		if (!frame) {
			return false;
		}
		if (current_video) {
			retired_video.push_back(std::move(current_video->buffer));
			if (retired_video.size() >= static_cast<size_t>(max_video_buffers - 1)) {
				video_buffers.Push(std::move(retired_video.front()));
				retired_video.pop_front();
			}
		}
		current_video = std::move(*frame);
		*out          = current_video->info;
		if (deliver_seek_frame) {
			seek_video_frame_pending = false;
		}
		return true;
	}
	bool Audio(AvPlayerFrameInfo* out) {
		if (out == nullptr) {
			return false;
		}
		std::lock_guard lock(mutex);
		if (paused || stopped || !audio_id || trick_speed != AVPLAYER_TRICK_SPEED_NORMAL) {
			return false;
		}
		auto frame = audio_frames.TryPop();
		if (!frame) {
			return false;
		}
		if (current_audio) {
			audio_buffers.Push(std::move(current_audio->buffer));
		}
		current_audio = std::move(*frame);
		std::memset(out, 0, sizeof(*out));
		out->data                        = current_audio->info.data;
		out->time_stamp                  = current_audio->info.time_stamp;
		out->details.audio.channel_count = current_audio->info.details.audio.channel_count;
		out->details.audio.sample_rate   = current_audio->info.details.audio.sample_rate;
		out->details.audio.size          = current_audio->info.details.audio.size;
		std::memcpy(out->details.audio.language_code,
		            current_audio->info.details.audio.language_code, 4);
		last_audio_ts = out->time_stamp;
		return true;
	}
	std::optional<int32_t> TakeWarning() { return warnings.TryPop(); }

private:
	void Close() {
		Stop();
		if (fmt != nullptr) {
			avformat_close_input(&fmt);
		}
		streamer.reset();
	}
	void ResetNoLock(bool retain_output_buffers = false) {
		std::optional<ReadyFrame>                delivered_video;
		std::optional<ReadyFrame>                delivered_audio;
		std::deque<std::unique_ptr<GuestBuffer>> in_flight_video;
		if (retain_output_buffers) {
			delivered_video = std::move(current_video);
			delivered_audio = std::move(current_audio);
			in_flight_video = std::move(retired_video);
		}
		video_packets.Clear();
		audio_packets.Clear();
		video_frames.Clear();
		audio_frames.Clear();
		video_buffers.Clear();
		audio_buffers.Clear();
		retired_video.clear();
		current_video.reset();
		current_audio.reset();
		if (sws != nullptr) {
			sws_freeContext(sws);
			sws = nullptr;
		}
		sws_src_width  = 0;
		sws_src_height = 0;
		sws_src_format = AV_PIX_FMT_NONE;
		if (swr != nullptr) {
			swr_free(&swr);
		}
		if (video_ctx != nullptr) {
			avcodec_free_context(&video_ctx);
		}
		if (audio_ctx != nullptr) {
			avcodec_free_context(&audio_ctx);
		}
		demux_eof                = true;
		video_done               = true;
		audio_done               = true;
		seek_video_frame_pending = false;
		last_audio_ts            = 0;
		current_video            = std::move(delivered_video);
		current_audio            = std::move(delivered_audio);
		retired_video            = std::move(in_flight_video);
	}
	void AutoEnable() {
		for (uint32_t i = 0; i < fmt->nb_streams; i++) {
			if (!StreamSupported(static_cast<int>(i))) {
				continue;
			}
			auto t = fmt->streams[i]->codecpar->codec_type;
			if (t == AVMEDIA_TYPE_VIDEO && !video_id) {
				video_id = static_cast<int>(i);
			}
			if (t == AVMEDIA_TYPE_AUDIO && !audio_id) {
				audio_id = static_cast<int>(i);
			}
		}
	}
	bool StreamSupported(int id) const {
		if (fmt == nullptr || id < 0 || id >= static_cast<int>(fmt->nb_streams)) {
			return false;
		}
		auto* s = fmt->streams[id];
		auto  t = s->codecpar->codec_type;
		if (t != AVMEDIA_TYPE_VIDEO && t != AVMEDIA_TYPE_AUDIO) {
			return true;
		}
		if (!codec_is_supported_for_source(source_type, t, s->codecpar->codec_id)) {
			LOGF("\t unsupported codec for source type: stream=%d codec=%d source=%u\n", id,
			     static_cast<int>(s->codecpar->codec_id), static_cast<uint32_t>(source_type));
			return false;
		}
		if (avcodec_find_decoder(s->codecpar->codec_id) == nullptr) {
			LOGF("\t decoder not found: stream=%d codec=%d\n", id,
			     static_cast<int>(s->codecpar->codec_id));
			return false;
		}
		return true;
	}
	bool OpenCodec(int id, AVCodecContext** out) {
		auto* s   = fmt->streams[id];
		auto* dec = avcodec_find_decoder(s->codecpar->codec_id);
		if (dec == nullptr) {
			LOGF("\t avcodec_find_decoder failed: stream=%d codec=%d\n", id,
			     static_cast<int>(s->codecpar->codec_id));
			return false;
		}
		auto* c = avcodec_alloc_context3(dec);
		if (c == nullptr) {
			return false;
		}
		if (avcodec_parameters_to_context(c, s->codecpar) < 0 ||
		    avcodec_open2(c, dec, nullptr) < 0) {
			avcodec_free_context(&c);
			return false;
		}
		*out = c;
		return true;
	}
	bool OpenCodecs() {
		if (video_id && !OpenCodec(video_id.value(), &video_ctx)) {
			return false;
		}
		if (audio_id && !OpenCodec(audio_id.value(), &audio_ctx)) {
			return false;
		}
		return true;
	}
	bool AllocateBuffers() {
		if (video_id) {
			auto size     = VideoBufferSize(fmt->streams[video_id.value()]);
			auto retained = retired_video.size() + (current_video ? 1u : 0u);
			for (size_t i = retained; i < static_cast<size_t>(max_video_buffers); i++) {
				auto buffer = std::make_unique<GuestBuffer>(mem, 0x100, size, true);
				if (!buffer->Valid()) {
					return false;
				}
				video_buffers.Push(std::move(buffer));
			}
		}
		if (audio_id) {
			auto channels = std::max(1, audio_ctx->ch_layout.nb_channels);
			auto samples  = std::max(1024, audio_ctx->frame_size);
			auto size     = static_cast<uint32_t>(channels * samples * sizeof(int16_t));
			auto retained = current_audio ? 1 : 0;
			for (int i = retained; i < AVPLAYER_AUDIO_BUFFERS; i++) {
				auto buffer = std::make_unique<GuestBuffer>(mem, 0x100, size, false);
				if (!buffer->Valid()) {
					return false;
				}
				audio_buffers.Push(std::move(buffer));
			}
		}
		return true;
	}
	void SeekNoLock(uint64_t ms) {
		int     sid = video_id.value_or(audio_id.value_or(-1));
		int64_t ts  = sid >= 0 ? av_rescale_q(static_cast<int64_t>(ms), AVRational {1, 1000},
		                                      fmt->streams[sid]->time_base)
		                       : static_cast<int64_t>(ms) * 1000;
		avformat_seek_file(fmt, sid, INT64_MIN, ts, INT64_MAX, 0);
		if (video_ctx != nullptr) {
			avcodec_flush_buffers(video_ctx);
		}
		if (audio_ctx != nullptr) {
			avcodec_flush_buffers(audio_ctx);
		}
		video_packets.Clear();
		audio_packets.Clear();
	}
	static KYTY_SYSV_ABI void* DemuxEntry(void* arg) {
		static_cast<Source*>(arg)->Demux();
		return nullptr;
	}
	static KYTY_SYSV_ABI void* VideoDecoderEntry(void* arg) {
		static_cast<Source*>(arg)->VideoDecoder();
		return nullptr;
	}
	static KYTY_SYSV_ABI void* AudioDecoderEntry(void* arg) {
		static_cast<Source*>(arg)->AudioDecoder();
		return nullptr;
	}
	bool CreateWorker(LibKernel::Pthread& thread, LibKernel::pthread_entry_func_t entry,
	                  const char* name) {
		LibKernel::Pthread created = nullptr;
		if (LibKernel::PthreadCreate(&created, nullptr, entry, this, name) != 0) {
			return false;
		}
		thread = created;
		return true;
	}
	bool StartWorkers() {
		worker_stop     = false;
		interrupt_io    = false;
		pipeline_failed = false;
		demux_eof       = false;
		video_done      = !video_id;
		audio_done      = !audio_id;
		if ((video_id && !CreateWorker(video_thread, VideoDecoderEntry, "AvPlayerVideoDecoder")) ||
		    (audio_id && !CreateWorker(audio_thread, AudioDecoderEntry, "AvPlayerAudioDecoder")) ||
		    !CreateWorker(demux_thread, DemuxEntry, "AvPlayerDemuxer")) {
			StopWorkers();
			return false;
		}
		return true;
	}
	void NotifyWorkers() {
		video_packets.Notify();
		audio_packets.Notify();
		video_buffers.Notify();
		audio_buffers.Notify();
	}
	void StopWorkers() {
		worker_stop  = true;
		interrupt_io = true;
		NotifyWorkers();
		if (demux_thread != nullptr) {
			LibKernel::PthreadJoin(demux_thread, nullptr);
			demux_thread = nullptr;
		}
		if (video_thread != nullptr) {
			LibKernel::PthreadJoin(video_thread, nullptr);
			video_thread = nullptr;
		}
		if (audio_thread != nullptr) {
			LibKernel::PthreadJoin(audio_thread, nullptr);
			audio_thread = nullptr;
		}
	}
	void FailPipeline() {
		pipeline_failed = true;
		worker_stop     = true;
		interrupt_io    = true;
		NotifyWorkers();
	}
	void Demux() {
		uint64_t video_offset = 0;
		uint64_t audio_offset = 0;
		uint64_t loop_period  = fmt->duration > 0 ? static_cast<uint64_t>(fmt->duration / 1000) : 0;
		if (loop_period == 0) {
			if (video_id) {
				loop_period = Duration(fmt->streams[video_id.value()]);
			}
			if (audio_id) {
				loop_period = std::max(loop_period, Duration(fmt->streams[audio_id.value()]));
			}
		}
		while (!worker_stop) {
			Packet packet(av_packet_alloc());
			if (packet == nullptr) {
				FailPipeline();
				break;
			}
			auto result = av_read_frame(fmt, packet.get());
			if (result < 0) {
				if (result == AVERROR_EOF && loop) {
					video_offset += loop_period;
					audio_offset += loop_period;
					if ((video_id && !video_packets.Push({nullptr, video_offset}, worker_stop)) ||
					    (audio_id && !audio_packets.Push({nullptr, audio_offset}, worker_stop))) {
						break;
					}
					int stream = video_id.value_or(audio_id.value_or(-1));
					if (avformat_seek_file(fmt, stream, INT64_MIN, 0, INT64_MAX, 0) < 0) {
						break;
					}
					warnings.Push(AVPLAYER_WARNING_LOOPING_BACK);
					continue;
				}
				if (result != AVERROR_EOF && !worker_stop) {
					LOGF("\t av_read_frame failed: %s\n", fferr(result).c_str());
					FailPipeline();
				}
				break;
			}
			if (video_id && packet->stream_index == video_id.value()) {
				if (!video_packets.Push({std::move(packet), video_offset}, worker_stop)) {
					break;
				}
			} else if (audio_id && packet->stream_index == audio_id.value()) {
				if (!audio_packets.Push({std::move(packet), audio_offset}, worker_stop)) {
					break;
				}
			}
		}
		demux_eof = true;
		video_packets.Notify();
		audio_packets.Notify();
	}
	void VideoDecoder() {
		Decoder(video_packets, video_ctx, video_buffers, video_frames, video_done,
		        &Source::PrepareVideo, "video");
	}
	void AudioDecoder() {
		Decoder(audio_packets, audio_ctx, audio_buffers, audio_frames, audio_done,
		        &Source::PrepareAudio, "audio");
	}
	using PrepareFrame = bool (Source::*)(AVFrame*, std::unique_ptr<GuestBuffer>&,
	                                      AvPlayerFrameInfoEx*);
	void Decoder(PacketQueue& packets, AVCodecContext* codec,
	             WorkQueue<std::unique_ptr<GuestBuffer>>& buffers, WorkQueue<ReadyFrame>& frames,
	             std::atomic_bool& done, PrepareFrame prepare, const char* kind) {
		uint64_t timestamp_offset = 0;
		while (!worker_stop) {
			auto item = packets.WaitPop(worker_stop, demux_eof);
			if (!item) {
				if (demux_eof &&
				    !DecodePacket(codec, nullptr, timestamp_offset, buffers, frames, prepare,
				                  kind) &&
				    !worker_stop) {
					FailPipeline();
				}
				break;
			}
			if (item->packet == nullptr) {
				if (!DecodePacket(codec, nullptr, timestamp_offset, buffers, frames, prepare,
				                  kind)) {
					if (!worker_stop) {
						FailPipeline();
					}
					break;
				}
				avcodec_flush_buffers(codec);
				timestamp_offset = item->timestamp_offset;
				continue;
			}
			timestamp_offset = item->timestamp_offset;
			if (!DecodePacket(codec, item->packet.get(), timestamp_offset, buffers, frames, prepare,
			                  kind)) {
				if (!worker_stop) {
					FailPipeline();
				}
				break;
			}
		}
		done = true;
	}
	bool DecodePacket(AVCodecContext* codec, AVPacket* packet, uint64_t timestamp_offset,
	                  WorkQueue<std::unique_ptr<GuestBuffer>>& buffers,
	                  WorkQueue<ReadyFrame>& frames, PrepareFrame prepare, const char* kind) {
		auto result = avcodec_send_packet(codec, packet);
		if (result == AVERROR(EAGAIN)) {
			if (!ReceiveFrames(codec, timestamp_offset, buffers, frames, prepare, kind)) {
				return false;
			}
			result = avcodec_send_packet(codec, packet);
		}
		if (result == AVERROR_EOF) {
			return true;
		}
		if (result < 0) {
			LOGF("\t avcodec_send_packet %s failed: %s\n", kind, fferr(result).c_str());
			return false;
		}
		return ReceiveFrames(codec, timestamp_offset, buffers, frames, prepare, kind);
	}
	bool ReceiveFrames(AVCodecContext* codec, uint64_t timestamp_offset,
	                   WorkQueue<std::unique_ptr<GuestBuffer>>& buffers,
	                   WorkQueue<ReadyFrame>& frames, PrepareFrame prepare, const char* kind) {
		while (!worker_stop) {
			AVFrame* frame = av_frame_alloc();
			if (frame == nullptr) {
				return false;
			}
			auto result = avcodec_receive_frame(codec, frame);
			if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
				av_frame_free(&frame);
				return true;
			}
			if (result < 0) {
				LOGF("\t avcodec_receive_frame %s failed: %s\n", kind, fferr(result).c_str());
				av_frame_free(&frame);
				return false;
			}
			auto buffer = buffers.WaitPop(worker_stop);
			if (!buffer) {
				av_frame_free(&frame);
				return false;
			}
			ReadyFrame ready {.buffer = std::move(*buffer)};
			if (!(this->*prepare)(frame, ready.buffer, &ready.info)) {
				buffers.Push(std::move(ready.buffer));
				av_frame_free(&frame);
				return false;
			}
			ready.info.time_stamp += timestamp_offset;
			frames.Push(std::move(ready));
			av_frame_free(&frame);
		}
		return false;
	}
	uint32_t Width(AVStream* s) const {
		return use_vdec2 ? static_cast<uint32_t>(s->codecpar->width)
		                 : align_up(static_cast<uint32_t>(s->codecpar->width), 16);
	}
	uint32_t Height(AVStream* s) const {
		return use_vdec2 ? static_cast<uint32_t>(s->codecpar->height)
		                 : align_up(static_cast<uint32_t>(s->codecpar->height), 16);
	}
	float Aspect(AVStream* s) const {
		if (s->codecpar->height == 0) {
			return 0.0f;
		}
		double r = static_cast<double>(s->codecpar->width) / s->codecpar->height;
		if (s->sample_aspect_ratio.num > 0 && s->sample_aspect_ratio.den > 0) {
			r *= av_q2d(s->sample_aspect_ratio);
		}
		return static_cast<float>(r);
	}
	uint64_t Duration(AVStream* s) const {
		return s->duration != AV_NOPTS_VALUE
		           ? to_ms(s->duration, s->time_base)
		           : (fmt->duration > 0 ? static_cast<uint64_t>(fmt->duration / 1000) : 0);
	}
	uint32_t VideoPitch(AVStream* s) const { return align_up(Width(s), 256u); }
	uint32_t VideoBufferSize(AVStream* s) const { return VideoPitch(s) * Height(s) * 3 / 2; }
	void     FillVideo(AVStream* s, AvPlayerVideo* v) const {
		std::memset(v, 0, sizeof(*v));
		v->width        = Width(s);
		v->height       = Height(s);
		v->aspect_ratio = Aspect(s);
		lang(v->language_code, s->metadata);
	}
	void FillVideoEx(AVStream* s, AvPlayerVideoEx* v) const {
		std::memset(v, 0, sizeof(*v));
		v->width                 = Width(s);
		v->height                = Height(s);
		v->aspect_ratio          = Aspect(s);
		v->pitch                 = VideoPitch(s);
		v->luma_bit_depth        = 8;
		v->chroma_bit_depth      = 8;
		v->video_full_range_flag = s->codecpar->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
		auto fr                  = s->avg_frame_rate.num != 0 ? s->avg_frame_rate : s->r_frame_rate;
		v->framerate             = av_q2d(fr);
		v->colour_primaries      = static_cast<uint32_t>(s->codecpar->color_primaries);
		v->transfer_characteristics = static_cast<uint32_t>(s->codecpar->color_trc);
		lang(v->language_code, s->metadata);
	}
	void FillAudio(AVStream* s, AvPlayerAudio* a, uint32_t size) const {
		std::memset(a, 0, sizeof(*a));
		a->channel_count = static_cast<uint16_t>(s->codecpar->ch_layout.nb_channels);
		a->sample_rate   = static_cast<uint32_t>(s->codecpar->sample_rate);
		a->size          = size;
		lang(a->language_code, s->metadata);
	}
	void FillAudioEx(AVStream* s, AvPlayerAudioEx* a, uint32_t size) const {
		std::memset(a, 0, sizeof(*a));
		a->channel_count = static_cast<uint16_t>(s->codecpar->ch_layout.nb_channels);
		a->sample_rate   = static_cast<uint32_t>(s->codecpar->sample_rate);
		a->size          = size;
		lang(a->language_code, s->metadata);
	}
	void FillCommon(uint32_t id, AvPlayerStreamType* type, uint64_t* dur, AvPlayerStreamDetails* d,
	                AvPlayerStreamDetailsEx* ex) const {
		auto* s = fmt->streams[id];
		*dur    = Duration(s);
		switch (s->codecpar->codec_type) {
			case AVMEDIA_TYPE_VIDEO:
				*type = AvPlayerStreamVideo;
				if (d) {
					FillVideo(s, &d->video);
				}
				if (ex) {
					FillVideoEx(s, &ex->video);
				}
				break;
			case AVMEDIA_TYPE_AUDIO:
				*type = AvPlayerStreamAudio;
				if (d) {
					FillAudio(s, &d->audio, 0);
				}
				if (ex) {
					FillAudioEx(s, &ex->audio, 0);
				}
				break;
			case AVMEDIA_TYPE_SUBTITLE: *type = AvPlayerStreamTimedText; break;
			default: *type = AvPlayerStreamUnknown; break;
		}
	}
	bool PrepareVideo(AVFrame* src, std::unique_ptr<GuestBuffer>& buffer,
	                  AvPlayerFrameInfoEx* info) {
		auto*    s     = fmt->streams[video_id.value()];
		uint32_t h     = Height(s);
		uint32_t pitch = VideoPitch(s);
		uint64_t size  = static_cast<uint64_t>(pitch) * h * 3 / 2;
		if (buffer == nullptr || src->width <= 0 || src->height <= 0 ||
		    static_cast<uint32_t>(src->width) > pitch || static_cast<uint32_t>(src->height) > h ||
		    size > buffer->Size()) {
			return false;
		}
		AVFrame* nv12 = src;
		AVFrame* tmp  = nullptr;
		if (src->format != AV_PIX_FMT_NV12) {
			tmp = av_frame_alloc();
			if (tmp == nullptr) {
				return false;
			}
			tmp->format = AV_PIX_FMT_NV12;
			tmp->width  = src->width;
			tmp->height = src->height;
			if (av_frame_get_buffer(tmp, 0) < 0) {
				av_frame_free(&tmp);
				return false;
			}
			if (sws == nullptr || sws_src_width != src->width || sws_src_height != src->height ||
			    sws_src_format != src->format) {
				if (sws != nullptr) {
					sws_freeContext(sws);
				}
				sws = sws_getContext(
				    src->width, src->height, static_cast<AVPixelFormat>(src->format), src->width,
				    src->height, AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
				sws_src_width  = src->width;
				sws_src_height = src->height;
				sws_src_format = src->format;
			}
			if (sws == nullptr) {
				av_frame_free(&tmp);
				return false;
			}
			if (sws_scale(sws, src->data, src->linesize, 0, src->height, tmp->data,
			              tmp->linesize) != src->height) {
				av_frame_free(&tmp);
				return false;
			}
			nv12 = tmp;
		}
		if (nv12->data[0] == nullptr || nv12->data[1] == nullptr ||
		    nv12->linesize[0] < src->width || nv12->linesize[1] < src->width) {
			if (tmp != nullptr) {
				av_frame_free(&tmp);
			}
			return false;
		}
		auto* dst = buffer->Get();
		std::memset(dst, 0, static_cast<size_t>(size));
		for (int y = 0; y < src->height; y++) {
			std::memcpy(dst + y * pitch, nv12->data[0] + y * nv12->linesize[0], src->width);
		}
		auto* c = dst + pitch * h;
		for (int y = 0; y < src->height / 2; y++) {
			std::memcpy(c + y * pitch, nv12->data[1] + y * nv12->linesize[1], src->width);
		}
		std::memset(info, 0, sizeof(*info));
		info->data       = dst;
		info->time_stamp = to_ms(
		    src->best_effort_timestamp != AV_NOPTS_VALUE ? src->best_effort_timestamp : src->pts,
		    s->time_base);
		FillVideoEx(s, &info->details.video);
		info->details.video.crop_left_offset = static_cast<uint32_t>(src->crop_left);
		info->details.video.crop_right_offset =
		    static_cast<uint32_t>(src->crop_right + (pitch - src->width));
		info->details.video.crop_top_offset = static_cast<uint32_t>(src->crop_top);
		info->details.video.crop_bottom_offset =
		    static_cast<uint32_t>(src->crop_bottom + (h - src->height));
		info->details.video.pitch = pitch;
		if (tmp) {
			av_frame_free(&tmp);
		}
		return true;
	}
	bool PrepareAudio(AVFrame* src, std::unique_ptr<GuestBuffer>& buffer,
	                  AvPlayerFrameInfoEx* info) {
		auto timestamp = to_ms(
		    src->best_effort_timestamp != AV_NOPTS_VALUE ? src->best_effort_timestamp : src->pts,
		    fmt->streams[audio_id.value()]->time_base);
		AVFrame* pcm = src;
		AVFrame* tmp = nullptr;
		if (src->format != AV_SAMPLE_FMT_S16) {
			tmp = av_frame_alloc();
			if (tmp == nullptr) {
				return false;
			}
			tmp->format      = AV_SAMPLE_FMT_S16;
			tmp->sample_rate = src->sample_rate;
			tmp->nb_samples  = src->nb_samples;
			av_channel_layout_copy(&tmp->ch_layout, &src->ch_layout);
			if (av_frame_get_buffer(tmp, 0) < 0) {
				av_frame_free(&tmp);
				return false;
			}
			if (swr == nullptr) {
				SwrContext*     ctx = nullptr;
				AVChannelLayout out;
				av_channel_layout_copy(&out, &src->ch_layout);
				swr_alloc_set_opts2(&ctx, &out, AV_SAMPLE_FMT_S16, src->sample_rate,
				                    &src->ch_layout, static_cast<AVSampleFormat>(src->format),
				                    src->sample_rate, 0, nullptr);
				swr = ctx;
				swr_init(swr);
				av_channel_layout_uninit(&out);
			}
			if (swr == nullptr || swr_convert_frame(swr, tmp, src) < 0) {
				av_frame_free(&tmp);
				return false;
			}
			pcm = tmp;
		}
		auto channels = std::max(1, pcm->ch_layout.nb_channels);
		auto size     = static_cast<uint32_t>(channels * pcm->nb_samples * sizeof(int16_t));
		if (size > buffer->Size()) {
			auto replacement = std::make_unique<GuestBuffer>(mem, 0x100, size, false);
			if (!replacement->Valid()) {
				if (tmp) {
					av_frame_free(&tmp);
				}
				return false;
			}
			buffer = std::move(replacement);
		}
		std::memcpy(buffer->Get(), pcm->data[0], size);
		auto* s = fmt->streams[audio_id.value()];
		std::memset(info, 0, sizeof(*info));
		info->data       = buffer->Get();
		info->time_stamp = timestamp;
		FillAudioEx(s, &info->details.audio, size);
		info->details.audio.channel_count = static_cast<uint16_t>(channels);
		info->details.audio.sample_rate   = static_cast<uint32_t>(pcm->sample_rate);
		if (tmp) {
			av_frame_free(&tmp);
		}
		return true;
	}
	AvPlayerMemAllocator                     mem;
	AvPlayerFileReplacement                  file;
	int                                      max_video_buffers = 2;
	bool                                     use_vdec2         = false;
	AvPlayerSourceType                       source_type       = AvPlayerSourceUnknown;
	std::string                              path;
	std::unique_ptr<FileStreamer>            streamer;
	AVFormatContext*                         fmt            = nullptr;
	AVCodecContext*                          video_ctx      = nullptr;
	AVCodecContext*                          audio_ctx      = nullptr;
	SwsContext*                              sws            = nullptr;
	SwrContext*                              swr            = nullptr;
	int                                      sws_src_width  = 0;
	int                                      sws_src_height = 0;
	int                                      sws_src_format = AV_PIX_FMT_NONE;
	std::optional<int>                       video_id;
	std::optional<int>                       audio_id;
	PacketQueue                              video_packets;
	PacketQueue                              audio_packets;
	WorkQueue<std::unique_ptr<GuestBuffer>>  video_buffers;
	WorkQueue<std::unique_ptr<GuestBuffer>>  audio_buffers;
	WorkQueue<ReadyFrame>                    video_frames;
	WorkQueue<ReadyFrame>                    audio_frames;
	std::optional<ReadyFrame>                current_video;
	std::optional<ReadyFrame>                current_audio;
	std::deque<std::unique_ptr<GuestBuffer>> retired_video;
	WorkQueue<int32_t>                       warnings;
	LibKernel::Pthread                       demux_thread = nullptr;
	LibKernel::Pthread                       video_thread = nullptr;
	LibKernel::Pthread                       audio_thread = nullptr;
	std::atomic_bool                         worker_stop {true};
	std::atomic_bool                         interrupt_io {false};
	std::atomic_bool                         pipeline_failed {false};
	std::atomic_bool                         demux_eof {true};
	std::atomic_bool                         video_done {true};
	std::atomic_bool                         audio_done {true};
	std::mutex                               lifecycle_mutex;
	mutable std::mutex                       mutex;
	bool                                     stopped                  = true;
	bool                                     paused                   = false;
	bool                                     seek_video_frame_pending = false;
	std::atomic_bool                         loop {false};
	int32_t                                  trick_speed   = AVPLAYER_TRICK_SPEED_NORMAL;
	uint32_t                                 sync_mode     = 0;
	uint64_t                                 start_time_ms = 0;
	uint64_t                                 last_audio_ts = 0;
	std::chrono::steady_clock::time_point    clock_start {};
	std::chrono::steady_clock::time_point    pause_time {};
	std::chrono::steady_clock::duration      paused_extra {};
};

struct AvPlayerInternal {
	AvPlayerMemAllocator     mem;
	AvPlayerFileReplacement  file;
	AvPlayerEventReplacement event;
	AvPlayerPostInitData     post_init {};
	bool                     auto_start        = false;
	int32_t                  video_buffers     = 2;
	uint32_t                 start_bandwidth   = 0;
	uint32_t                 minimum_bandwidth = 0;
	uint32_t                 maximum_bandwidth = 0;
	std::unique_ptr<Source>  source;
};

static bool valid_allocators(const AvPlayerMemAllocator& m) {
	return m.allocate && m.deallocate && m.allocate_texture && m.deallocate_texture;
}
static void emit_event(AvPlayerInternal* h, int32_t id, void* data = nullptr) {
	if (h != nullptr && h->event.event_callback != nullptr) {
		LOGF("\t event_id = %d\n", id);
		h->event.event_callback(h->event.object_pointer, id, 0, data);
	}
}
static void pump_warnings(AvPlayerInternal* h) {
	if (h == nullptr || h->source == nullptr) {
		return;
	}
	while (auto w = h->source->TakeWarning()) {
		int32_t warning = *w;
		emit_event(h, AVPLAYER_EVENT_WARNING_ID, &warning);
	}
}
static AvPlayerInternal* create_player(const AvPlayerMemAllocator&     mem,
                                       const AvPlayerFileReplacement&  file,
                                       const AvPlayerEventReplacement& event, bool auto_start,
                                       int32_t video_buffers) {
	if (!valid_allocators(mem)) {
		return nullptr;
	}
	auto* h          = new AvPlayerInternal;
	h->mem           = mem;
	h->file          = file;
	h->event         = event;
	h->auto_start    = auto_start;
	h->video_buffers = std::clamp(video_buffers <= 0 ? 2 : video_buffers, 2, 16);
	h->post_init.demux_video_buffer_size = 4 * 1024 * 1024;
	return h;
}
static int add_source(AvPlayerInternal* h, const std::string& filename, AvPlayerSourceType type) {
	if (h == nullptr) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	if (h->source != nullptr) {
		return AVPLAYER_ERROR_OPERATION_FAILED;
	}
	bool vdec2 =
	    h->post_init.video_decoder_init.decoder_type.video_type == AvPlayerVideoDecoderSoftware2;
	auto s = std::make_unique<Source>(h->mem, h->file, h->video_buffers, vdec2,
	                                  h->post_init.demux_video_buffer_size);
	if (auto rc = s->Init(filename, type); rc < 0) {
		return rc;
	}
	h->source = std::move(s);
	emit_event(h, AVPLAYER_EVENT_STATE_READY);
	if (h->auto_start) {
		auto rc = h->source->Start();
		if (rc == 0) {
			emit_event(h, AVPLAYER_EVENT_STATE_PLAY);
		}
		return rc;
	}
	return 0;
}
static void log_init_common(const AvPlayerMemAllocator& mem, const AvPlayerFileReplacement& file,
                            const AvPlayerEventReplacement& event, AvPlayerDebuglevels debug_level,
                            int32_t buffers, uint32_t priority, Bool auto_start,
                            const char* language) {
	LOGF("\t memory_replacement.object_pointer     = %016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(mem.object_pointer));
	LOGF("\t memory_replacement.allocate           = %016" PRIx64
	     "\n\t memory_replacement.deallocate         = %016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(mem.allocate), reinterpret_cast<uint64_t>(mem.deallocate));
	LOGF("\t memory_replacement.allocate_texture   = %016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(mem.allocate_texture));
	LOGF("\t memory_replacement.deallocate_texture = %016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(mem.deallocate_texture));
	LOGF("\t file_replacement.object_pointer       = %016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(file.object_pointer));
	LOGF("\t file_replacement.open                 = %016" PRIx64
	     "\n\t file_replacement.close                = %016" PRIx64
	     "\n\t file_replacement.read_offset          = %016" PRIx64
	     "\n\t file_replacement.size                 = %016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(file.open), reinterpret_cast<uint64_t>(file.close),
	     reinterpret_cast<uint64_t>(file.read_offset), reinterpret_cast<uint64_t>(file.size));
	LOGF("\t event_replacement.object_pointer      = %016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(event.object_pointer));
	LOGF("\t event_replacement.event_callback      = %016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(event.event_callback));
	LOGF("\t debug_level                           = %s\n\t num_output_video_framebuffers         "
	     "= %d\n\t base_priority                   "
	     "      = %u\n\t auto_start                            = %u\n\t default_language           "
	     "           = %s\n",
	     Common::EnumName(debug_level).c_str(), buffers, priority, auto_start,
	     language == nullptr ? "(null)" : language);
}

AvPlayerInternal* KYTY_SYSV_ABI AvPlayerInit(AvPlayerInitData* init) {
	PRINT_NAME();
	if (init == nullptr) {
		return nullptr;
	}
	log_init_common(init->memory_replacement, init->file_replacement, init->event_replacement,
	                init->debug_level, init->num_output_video_framebuffers, init->base_priority,
	                init->auto_start, init->default_language);
	return create_player(init->memory_replacement, init->file_replacement, init->event_replacement,
	                     init->auto_start != 0, init->num_output_video_framebuffers);
}
int KYTY_SYSV_ABI AvPlayerInitEx(const void* init_ex, AvPlayerInternal** handle) {
	PRINT_NAME();
	if (init_ex == nullptr || handle == nullptr) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	const auto* init = static_cast<const AvPlayerInitDataEx*>(init_ex);
	LOGF("\t this_size = %zu\n", init->this_size);
	log_init_common(init->memory_replacement, init->file_replacement, init->event_replacement,
	                init->debug_level, init->num_output_video_framebuffers, 0, init->auto_start,
	                init->default_language);
	*handle =
	    create_player(init->memory_replacement, init->file_replacement, init->event_replacement,
	                  init->auto_start != 0, init->num_output_video_framebuffers);
	return *handle == nullptr ? AVPLAYER_ERROR_INVALID_PARAMS : 0;
}
int KYTY_SYSV_ABI AvPlayerPostInit(AvPlayerInternal* h, const void* post_init) {
	PRINT_NAME();
	if (h == nullptr) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	if (post_init != nullptr) {
		h->post_init = *static_cast<const AvPlayerPostInitData*>(post_init);
		LOGF("\t demux_video_buffer_size = %u\n", h->post_init.demux_video_buffer_size);
	}
	return 0;
}
int KYTY_SYSV_ABI AvPlayerAddSource(AvPlayerInternal* h, const char* filename) {
	PRINT_NAME();
	if (h == nullptr || filename == nullptr) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	LOGF("\t filename = %s\n", filename);
	return add_source(h, filename, AvPlayerSourceUnknown);
}
int KYTY_SYSV_ABI AvPlayerAddSourceEx(AvPlayerInternal* h, uint32_t uri_type,
                                      const void* source_details) {
	PRINT_NAME();
	if (h == nullptr || uri_type != AvPlayerUriTypeSource || source_details == nullptr) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	const auto* d = static_cast<const AvPlayerSourceDetails*>(source_details);
	if (d->uri.name == nullptr || d->uri.length == 0) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	if (d->source_type != AvPlayerSourceUnknown && d->source_type != AvPlayerSourceFileMp4 &&
	    d->source_type != AvPlayerSourceFileWebm && d->source_type != AvPlayerSourceHls) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	std::string filename(d->uri.name, d->uri.length);
	LOGF("\t uri_type    = %u\n\t source_type = %u\n\t filename    = %s\n", uri_type,
	     static_cast<uint32_t>(d->source_type), filename.c_str());
	return add_source(h, filename, d->source_type);
}
int KYTY_SYSV_ABI AvPlayerStreamCount(AvPlayerInternal* h) {
	PRINT_NAME();
	return h == nullptr ? AVPLAYER_ERROR_INVALID_PARAMS
	                    : (h->source == nullptr ? 0 : h->source->StreamCount());
}
int KYTY_SYSV_ABI AvPlayerGetStreamInfo(AvPlayerInternal* h, uint32_t stream_id, void* info) {
	PRINT_NAME();
	if (h == nullptr || info == nullptr) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	return h->source == nullptr
	           ? AVPLAYER_ERROR_OPERATION_FAILED
	           : h->source->Info(stream_id, static_cast<AvPlayerStreamInfo*>(info));
}
int KYTY_SYSV_ABI AvPlayerGetStreamInfoEx(AvPlayerInternal* h, uint32_t stream_id, void* info) {
	PRINT_NAME();
	if (h == nullptr || info == nullptr) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	return h->source == nullptr
	           ? AVPLAYER_ERROR_OPERATION_FAILED
	           : h->source->InfoEx(stream_id, static_cast<AvPlayerStreamInfoEx*>(info));
}
int KYTY_SYSV_ABI AvPlayerEnableStream(AvPlayerInternal* h, uint32_t stream_id) {
	PRINT_NAME();
	return h == nullptr || h->source == nullptr ? AVPLAYER_ERROR_INVALID_PARAMS
	                                            : h->source->Enable(stream_id);
}
int KYTY_SYSV_ABI AvPlayerDisableStream(AvPlayerInternal* h, uint32_t stream_id) {
	PRINT_NAME();
	return h == nullptr || h->source == nullptr ? AVPLAYER_ERROR_INVALID_PARAMS
	                                            : h->source->Disable(stream_id);
}
int KYTY_SYSV_ABI AvPlayerChangeStream(AvPlayerInternal* h, uint32_t old_stream_id,
                                       uint32_t new_stream_id) {
	PRINT_NAME();
	return h == nullptr || h->source == nullptr ? AVPLAYER_ERROR_INVALID_PARAMS
	                                            : h->source->Change(old_stream_id, new_stream_id);
}
int KYTY_SYSV_ABI AvPlayerStart(AvPlayerInternal* h) {
	PRINT_NAME();
	if (h == nullptr || h->source == nullptr) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	auto rc = h->source->Start();
	if (rc == 0) {
		emit_event(h, AVPLAYER_EVENT_STATE_PLAY);
	}
	return rc;
}
int KYTY_SYSV_ABI AvPlayerStartEx(AvPlayerInternal* h, const void* start_info_ex) {
	PRINT_NAME();
	if (h == nullptr || h->source == nullptr) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	uint64_t ms =
	    start_info_ex == nullptr
	        ? 0
	        : static_cast<const AvPlayerStartInfoEx*>(start_info_ex)->start_time_milliseconds;
	auto rc = h->source->Start(ms);
	if (rc == 0) {
		emit_event(h, AVPLAYER_EVENT_STATE_PLAY);
	}
	return rc;
}
int KYTY_SYSV_ABI AvPlayerStop(AvPlayerInternal* h) {
	PRINT_NAME();
	if (h == nullptr || h->source == nullptr) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	auto rc = h->source->Stop();
	emit_event(h, AVPLAYER_EVENT_STATE_STOP);
	return rc;
}
int KYTY_SYSV_ABI AvPlayerPause(AvPlayerInternal* h) {
	PRINT_NAME();
	if (h == nullptr || h->source == nullptr) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	h->source->Pause();
	emit_event(h, AVPLAYER_EVENT_STATE_PAUSE);
	return 0;
}
int KYTY_SYSV_ABI AvPlayerResume(AvPlayerInternal* h) {
	PRINT_NAME();
	if (h == nullptr || h->source == nullptr) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	h->source->Resume();
	emit_event(h, AVPLAYER_EVENT_STATE_PLAY);
	return 0;
}
int KYTY_SYSV_ABI AvPlayerSetLooping(AvPlayerInternal* h, Bool loop) {
	PRINT_NAME();
	if (h == nullptr || h->source == nullptr) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	h->source->SetLoop(loop != 0);
	return 0;
}
int KYTY_SYSV_ABI AvPlayerSetTrickSpeed(AvPlayerInternal* h, int32_t trick_speed) {
	PRINT_NAME();
	if (h == nullptr || h->source == nullptr) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	trick_speed = std::clamp(trick_speed, -AVPLAYER_TRICK_SPEED_MAX, AVPLAYER_TRICK_SPEED_MAX);
	if (trick_speed != AVPLAYER_TRICK_SPEED_NORMAL && trick_speed > -AVPLAYER_TRICK_SPEED_MIN &&
	    trick_speed < AVPLAYER_TRICK_SPEED_MIN) {
		return AVPLAYER_ERROR_NOT_SUPPORTED;
	}
	h->source->SetTrick(trick_speed);
	return trick_speed == AVPLAYER_TRICK_SPEED_NORMAL ? 0 : AVPLAYER_ERROR_NOT_SUPPORTED;
}
int KYTY_SYSV_ABI AvPlayerSetAvSyncMode(AvPlayerInternal* h, uint32_t sync_mode) {
	PRINT_NAME();
	if (h == nullptr || h->source == nullptr) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	if (sync_mode > 1) {
		return AVPLAYER_ERROR_NOT_SUPPORTED;
	}
	h->source->SetSync(sync_mode);
	return 0;
}
int KYTY_SYSV_ABI AvPlayerSetAvailableBandwidth(AvPlayerInternal* h, uint32_t start_bandwidth,
                                                uint32_t minimum_bandwidth,
                                                uint32_t maximum_bandwidth) {
	PRINT_NAME();
	if (h == nullptr || (minimum_bandwidth != 0 && maximum_bandwidth != 0 &&
	                     minimum_bandwidth > maximum_bandwidth)) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	h->start_bandwidth   = start_bandwidth;
	h->minimum_bandwidth = minimum_bandwidth;
	h->maximum_bandwidth = maximum_bandwidth;
	return h->source == nullptr ? 0 : AVPLAYER_ERROR_OPERATION_FAILED;
}
Bool KYTY_SYSV_ABI AvPlayerGetVideoData(AvPlayerInternal* h, AvPlayerFrameInfo* video_info) {
	PRINT_NAME();
	if (h == nullptr || h->source == nullptr || video_info == nullptr) {
		return 0;
	}
	AvPlayerFrameInfoEx ex {};
	if (!h->source->Video(&ex)) {
		pump_warnings(h);
		return 0;
	}
	std::memset(video_info, 0, sizeof(*video_info));
	video_info->data                       = ex.data;
	video_info->time_stamp                 = ex.time_stamp;
	video_info->details.video.width        = ex.details.video.width;
	video_info->details.video.height       = ex.details.video.height;
	video_info->details.video.aspect_ratio = ex.details.video.aspect_ratio;
	std::memcpy(video_info->details.video.language_code, ex.details.video.language_code, 4);
	return 1;
}
Bool KYTY_SYSV_ABI AvPlayerGetVideoDataEx(AvPlayerInternal* h, AvPlayerFrameInfoEx* video_info) {
	PRINT_NAME();
	if (h == nullptr || h->source == nullptr || video_info == nullptr) {
		return 0;
	}
	auto ok = h->source->Video(video_info) ? 1 : 0;
	pump_warnings(h);
	return ok;
}
Bool KYTY_SYSV_ABI AvPlayerGetAudioData(AvPlayerInternal* h, AvPlayerFrameInfo* audio_info) {
	PRINT_NAME();
	if (h == nullptr || h->source == nullptr || audio_info == nullptr) {
		return 0;
	}
	auto ok = h->source->Audio(audio_info) ? 1 : 0;
	pump_warnings(h);
	return ok;
}
Bool KYTY_SYSV_ABI AvPlayerIsActive(AvPlayerInternal* h) {
	PRINT_NAME();
	return h != nullptr && h->source != nullptr && h->source->Active() ? 1 : 0;
}
uint64_t KYTY_SYSV_ABI AvPlayerCurrentTime(AvPlayerInternal* h) {
	PRINT_NAME();
	return h == nullptr || h->source == nullptr ? 0 : h->source->CurrentTime();
}
int KYTY_SYSV_ABI AvPlayerJumpToTime(AvPlayerInternal* h, uint64_t time_ms) {
	PRINT_NAME();
	if (h == nullptr || h->source == nullptr) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	auto rc = h->source->Jump(time_ms);
	if (rc == 0) {
		int32_t w = AVPLAYER_WARNING_JUMP_COMPLETE;
		emit_event(h, AVPLAYER_EVENT_WARNING_ID, &w);
	}
	return rc;
}
int KYTY_SYSV_ABI AvPlayerClose(AvPlayerInternal* h) {
	PRINT_NAME();
	if (h == nullptr) {
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	h->source.reset();
	delete h;
	return 0;
}
int KYTY_SYSV_ABI AvPlayerSetLogCallback(void* callback, void* user_data) {
	PRINT_NAME();
	LOGF("\t callback  = 0x%016" PRIx64 "\n\t user_data = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(callback), reinterpret_cast<uint64_t>(user_data));
	return 0;
}

} // namespace Libs::Audio::AvPlayer
