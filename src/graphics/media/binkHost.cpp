#include "graphics/media/binkHost.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace Libs::Graphics::BinkHost {

namespace {

// Only the entry points a decode needs. Bound by name so a build of ffmpeg that carries
// binkvideo2 can be dropped in without relinking the emulator.
struct Api {
	int (*open_input)(AVFormatContext**, const char*, const AVInputFormat*,
	                  AVDictionary**)                                   = nullptr;
	int (*find_stream_info)(AVFormatContext*, AVDictionary**)           = nullptr;
	void (*close_input)(AVFormatContext**)                              = nullptr;
	int (*read_frame)(AVFormatContext*, AVPacket*)                      = nullptr;
	const AVCodec* (*find_decoder)(AVCodecID)                           = nullptr;
	AVCodecContext* (*alloc_context)(const AVCodec*)                    = nullptr;
	int (*params_to_context)(AVCodecContext*, const AVCodecParameters*) = nullptr;
	int (*codec_open)(AVCodecContext*, const AVCodec*, AVDictionary**)  = nullptr;
	int (*send_packet)(AVCodecContext*, const AVPacket*)                = nullptr;
	int (*receive_frame)(AVCodecContext*, AVFrame*)                     = nullptr;
	void (*free_context)(AVCodecContext**)                              = nullptr;
	AVPacket* (*packet_alloc)()                                         = nullptr;
	void (*packet_free)(AVPacket**)                                     = nullptr;
	void (*packet_unref)(AVPacket*)                                     = nullptr;
	AVFrame* (*frame_alloc)()                                           = nullptr;
	void (*frame_free)(AVFrame**)                                       = nullptr;
	SwsContext* (*get_sws)(int, int, AVPixelFormat, int, int, AVPixelFormat, int, SwsFilter*,
	                       SwsFilter*, const double*)                   = nullptr;
	int (*sws_scale_fn)(SwsContext*, const uint8_t* const*, const int*, int, int, uint8_t* const*,
	                    const int*)                                     = nullptr;
	void (*free_sws)(SwsContext*)                                       = nullptr;
	bool ready                                                          = false;
};

std::mutex       g_lock;
Api              g_api;
bool             g_load_attempted = false;
std::string      g_path;
AVFormatContext* g_format  = nullptr;
AVCodecContext*  g_codec   = nullptr;
AVFrame*         g_frame   = nullptr;
AVPacket*        g_packet  = nullptr;
SwsContext*      g_sws     = nullptr;
int              g_sws_w   = 0;
int              g_sws_h   = 0;
AVPixelFormat    g_sws_fmt = AV_PIX_FMT_NONE;
int              g_stream  = -1;

#if defined(_WIN32)
template <typename T>
bool Bind(T& slot, HMODULE module, const char* name) {
	auto* symbol = GetProcAddress(module, name);
	slot         = reinterpret_cast<T>(symbol);
	if (symbol == nullptr) {
		std::fprintf(stderr, "[bink] missing ffmpeg symbol %s\n", name);
		return false;
	}
	return true;
}
#endif

bool LoadApi() {
	if (g_load_attempted) {
		return g_api.ready;
	}
	g_load_attempted = true;

#if defined(_WIN32)
	const char* directory = std::getenv("KYTY_BINK_FFMPEG_DIR");
	if (directory == nullptr) {
		std::fprintf(stderr, "[bink] KYTY_BINK_FFMPEG_DIR is not set; host playback disabled\n");
		return false;
	}
	SetDllDirectoryA(directory);
	char    path[1024];
	HMODULE avutil   = nullptr;
	HMODULE avcodec  = nullptr;
	HMODULE avformat = nullptr;
	HMODULE swscale  = nullptr;
	// Load in dependency order; avcodec pulls avutil and swresample.
	const char* names[] = {"avutil-59.dll", "swresample-5.dll", "avcodec-61.dll", "avformat-61.dll",
	                       "swscale-8.dll"};
	HMODULE*    slots[] = {&avutil, nullptr, &avcodec, &avformat, &swscale};
	for (int i = 0; i < 5; i++) {
		std::snprintf(path, sizeof(path), "%s\\%s", directory, names[i]);
		HMODULE module = LoadLibraryA(path);
		if (module == nullptr) {
			std::fprintf(stderr, "[bink] could not load %s\n", path);
			return false;
		}
		if (slots[i] != nullptr) {
			*slots[i] = module;
		}
	}

	bool ok = true;
	ok &= Bind(g_api.open_input, avformat, "avformat_open_input");
	ok &= Bind(g_api.find_stream_info, avformat, "avformat_find_stream_info");
	ok &= Bind(g_api.close_input, avformat, "avformat_close_input");
	ok &= Bind(g_api.read_frame, avformat, "av_read_frame");
	ok &= Bind(g_api.find_decoder, avcodec, "avcodec_find_decoder");
	ok &= Bind(g_api.alloc_context, avcodec, "avcodec_alloc_context3");
	ok &= Bind(g_api.params_to_context, avcodec, "avcodec_parameters_to_context");
	ok &= Bind(g_api.codec_open, avcodec, "avcodec_open2");
	ok &= Bind(g_api.send_packet, avcodec, "avcodec_send_packet");
	ok &= Bind(g_api.receive_frame, avcodec, "avcodec_receive_frame");
	ok &= Bind(g_api.free_context, avcodec, "avcodec_free_context");
	ok &= Bind(g_api.packet_alloc, avcodec, "av_packet_alloc");
	ok &= Bind(g_api.packet_free, avcodec, "av_packet_free");
	ok &= Bind(g_api.packet_unref, avcodec, "av_packet_unref");
	// av_frame_alloc/av_frame_free live in avutil, not avcodec.
	ok &= Bind(g_api.frame_alloc, avutil, "av_frame_alloc");
	ok &= Bind(g_api.frame_free, avutil, "av_frame_free");
	ok &= Bind(g_api.get_sws, swscale, "sws_getContext");
	ok &= Bind(g_api.sws_scale_fn, swscale, "sws_scale");
	ok &= Bind(g_api.free_sws, swscale, "sws_freeContext");
	g_api.ready = ok;
	if (ok) {
		std::fprintf(stderr, "[bink] ffmpeg bound from %s\n", directory);
		std::fflush(stderr);
	}
	return ok;
#else
	return false;
#endif
}

void CloseLocked() {
	if (g_sws != nullptr && g_api.free_sws != nullptr) {
		g_api.free_sws(g_sws);
	}
	g_sws = nullptr;
	if (g_frame != nullptr) {
		g_api.frame_free(&g_frame);
	}
	if (g_packet != nullptr) {
		g_api.packet_free(&g_packet);
	}
	if (g_codec != nullptr) {
		g_api.free_context(&g_codec);
	}
	if (g_format != nullptr) {
		g_api.close_input(&g_format);
	}
	g_stream = -1;
	// Clear the path too: NotifyFileOpen skips reopening a file it believes is already open, so a
	// stale path here would refuse the second play of a movie the title runs more than once.
	g_path.clear();
	g_sws_w   = 0;
	g_sws_h   = 0;
	g_sws_fmt = AV_PIX_FMT_NONE;
}

bool EndsWithBk2(const std::string& path) {
	if (path.size() < 4) {
		return false;
	}
	std::string tail = path.substr(path.size() - 4);
	std::transform(tail.begin(), tail.end(), tail.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return tail == ".bk2";
}

bool OpenLocked(const std::string& path) {
	CloseLocked();
	if (g_api.open_input(&g_format, path.c_str(), nullptr, nullptr) < 0) {
		std::fprintf(stderr, "[bink] could not open %s\n", path.c_str());
		g_format = nullptr;
		return false;
	}
	g_api.find_stream_info(g_format, nullptr);
	for (unsigned i = 0; i < g_format->nb_streams; i++) {
		if (g_format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			g_stream = static_cast<int>(i);
			break;
		}
	}
	if (g_stream < 0) {
		CloseLocked();
		return false;
	}
	auto*       params  = g_format->streams[g_stream]->codecpar;
	const auto* decoder = g_api.find_decoder(params->codec_id);
	if (decoder == nullptr) {
		std::fprintf(stderr, "[bink] no decoder for codec_id=%d (need binkvideo2)\n",
		             static_cast<int>(params->codec_id));
		CloseLocked();
		return false;
	}
	g_codec = g_api.alloc_context(decoder);
	g_api.params_to_context(g_codec, params);
	if (g_api.codec_open(g_codec, decoder, nullptr) < 0) {
		CloseLocked();
		return false;
	}
	g_frame  = g_api.frame_alloc();
	g_packet = g_api.packet_alloc();
	std::fprintf(stderr, "[bink] playing %s with %s %dx%d\n", path.c_str(), decoder->name,
	             params->width, params->height);
	std::fflush(stderr);
	return true;
}

} // namespace

void NotifyFileOpen(const std::string& host_path) {
	if (std::getenv("KYTY_BINK_HOST") == nullptr || !EndsWithBk2(host_path)) {
		return;
	}
	std::lock_guard lock(g_lock);
	if (!LoadApi()) {
		return;
	}
	if (g_path == host_path && g_format != nullptr) {
		return;
	}
	// Record the path only after the open succeeds: OpenLocked starts by closing, and closing
	// clears the path, so assigning first would leave it empty and defeat the check above.
	if (OpenLocked(host_path)) {
		g_path = host_path;
	}
}

bool Active() {
	std::lock_guard lock(g_lock);
	return g_format != nullptr && g_codec != nullptr;
}

bool NextFrame(uint32_t width, uint32_t height, int av_pixel_format, uint32_t bytes_per_pixel,
               std::vector<uint8_t>& rgba) {
	std::lock_guard lock(g_lock);
	if (g_format == nullptr || g_codec == nullptr || width == 0 || height == 0) {
		return false;
	}

	bool decoded = false;
	// Drain anything already buffered before pulling more packets.
	if (g_api.receive_frame(g_codec, g_frame) == 0) {
		decoded = true;
	}
	for (int guard = 0; guard < 4096 && !decoded; guard++) {
		if (g_api.read_frame(g_format, g_packet) < 0) {
			std::fprintf(stderr, "[bink] end of %s\n", g_path.c_str());
			std::fflush(stderr);
			// Release the movie the moment it finishes instead of parking it as "ended". Holding
			// the contexts open kept the decoder and its last frame alive for the rest of the run,
			// and the retained path blocked reopening the same file.
			CloseLocked();
			return false;
		}
		if (g_packet->stream_index != g_stream) {
			g_api.packet_unref(g_packet);
			continue;
		}
		const int sent = g_api.send_packet(g_codec, g_packet);
		g_api.packet_unref(g_packet);
		if (sent < 0) {
			continue;
		}
		if (g_api.receive_frame(g_codec, g_frame) == 0) {
			decoded = true;
		}
	}
	if (!decoded) {
		return false;
	}

	const auto target_w = static_cast<int>(width);
	const auto target_h = static_cast<int>(height);
	const auto want     = static_cast<AVPixelFormat>(av_pixel_format);
	if (g_sws == nullptr || g_sws_w != target_w || g_sws_h != target_h || g_sws_fmt != want) {
		if (g_sws != nullptr) {
			g_api.free_sws(g_sws);
		}
		g_sws     = g_api.get_sws(g_frame->width, g_frame->height,
		                          static_cast<AVPixelFormat>(g_frame->format), target_w, target_h, want,
		                          SWS_BILINEAR, nullptr, nullptr, nullptr);
		g_sws_w   = target_w;
		g_sws_h   = target_h;
		g_sws_fmt = want;
	}
	if (g_sws == nullptr) {
		return false;
	}

	rgba.resize(static_cast<size_t>(target_w) * target_h * bytes_per_pixel);
	uint8_t* destination[4]      = {rgba.data(), nullptr, nullptr, nullptr};
	int      destination_line[4] = {target_w * static_cast<int>(bytes_per_pixel), 0, 0, 0};
	g_api.sws_scale_fn(g_sws, g_frame->data, g_frame->linesize, 0, g_frame->height, destination,
	                   destination_line);
	return true;
}

} // namespace Libs::Graphics::BinkHost
