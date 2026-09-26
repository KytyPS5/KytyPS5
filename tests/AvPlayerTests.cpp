#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/tile.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/audio.h"
#include "loader/runtimeLinker.h"
#include "loader/timer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

namespace AvPlayer = Libs::Audio::AvPlayer;

// The clip is narrower than its 256-aligned pitch and shorter than its 16-aligned height, so every
// frame carries padding. Its audio ends 50 ms before the last video frame.
constexpr uint32_t ClipWidth         = 336;
constexpr uint32_t ClipHeight        = 120;
constexpr uint32_t ClipAlignedHeight = 128;
constexpr uint32_t ClipPitch         = 512;
constexpr int      ClipFrames        = 10;
constexpr int      ClipFps           = 10;
constexpr int      AudioRate         = 48000;
constexpr int      AudioChunkSamples = 2400;
constexpr int      AudioChunks       = 17;

constexpr int SkipReturnCode = 77;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "AvPlayerTests: failed: %s\n", text);
		std::abort();
	}
}

// Guest-side layouts of the SceAvPlayer structures that AvPlayer reads and fills.
using GuestAllocate    = KYTY_SYSV_ABI void* (*)(void*, uint32_t, uint32_t);
using GuestDeallocate  = KYTY_SYSV_ABI void (*)(void*, void*);
using GuestOpenFile    = KYTY_SYSV_ABI int (*)(void*, const char*);
using GuestCloseFile   = KYTY_SYSV_ABI int (*)(void*);
using GuestReadFile    = KYTY_SYSV_ABI int (*)(void*, uint8_t*, uint64_t, uint32_t);
using GuestFileSize    = KYTY_SYSV_ABI uint64_t (*)(void*);
using GuestEventNotify = KYTY_SYSV_ABI void (*)(void*, int32_t, int32_t, void*);

struct GuestInitData {
	void*            memory_object                 = nullptr;
	GuestAllocate    allocate                      = nullptr;
	GuestDeallocate  deallocate                    = nullptr;
	GuestAllocate    allocate_texture              = nullptr;
	GuestDeallocate  deallocate_texture            = nullptr;
	void*            file_object                   = nullptr;
	GuestOpenFile    open                          = nullptr;
	GuestCloseFile   close                         = nullptr;
	GuestReadFile    read_offset                   = nullptr;
	GuestFileSize    size                          = nullptr;
	void*            event_object                  = nullptr;
	GuestEventNotify event_callback                = nullptr;
	int32_t          debug_level                   = 0;
	uint32_t         base_priority                 = 0;
	int32_t          num_output_video_framebuffers = 0;
	uint8_t          auto_start                    = 0;
	uint8_t          reserved[3]                   = {};
	const char*      default_language              = nullptr;
};

struct GuestVideoDetails {
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
	uint8_t  video_full_range_flag    = 0;
	uint8_t  reserved2[5]             = {};
	double   framerate                = 0.0;
	uint32_t colour_primaries         = 0;
	uint32_t transfer_characteristics = 0;
	uint8_t  reserved3[16]            = {};
};

struct GuestFrameInfoEx {
	void*             data        = nullptr;
	uint8_t           reserved[4] = {};
	uint64_t          time_stamp  = 0;
	GuestVideoDetails video;
};

struct GuestFrameInfo {
	void*    data        = nullptr;
	uint8_t  reserved[4] = {};
	uint64_t time_stamp  = 0;
	uint8_t  details[16] = {};
};

static_assert(sizeof(GuestInitData) == 120);
static_assert(offsetof(GuestInitData, num_output_video_framebuffers) == 104);
static_assert(sizeof(GuestVideoDetails) == 80);
static_assert(offsetof(GuestVideoDetails, pitch) == 36);
static_assert(offsetof(GuestVideoDetails, framerate) == 48);
static_assert(offsetof(GuestFrameInfoEx, video) == 24);
static_assert(sizeof(GuestFrameInfo) == 40);

constexpr std::align_val_t BufferAlignment {0x1000};

KYTY_SYSV_ABI void* Allocate(void* /*object*/, uint32_t alignment, uint32_t size) {
	Check(alignment <= static_cast<uint32_t>(BufferAlignment), "unexpected buffer alignment");
	return ::operator new(size, BufferAlignment, std::nothrow);
}

KYTY_SYSV_ABI void Deallocate(void* /*object*/, void* memory) {
	::operator delete(memory, BufferAlignment);
}

KYTY_SYSV_ABI int OpenClip(void* object, const char* /*path*/) {
	return object != nullptr ? 0 : -1;
}

KYTY_SYSV_ABI int CloseClip(void* /*object*/) {
	return 0;
}

KYTY_SYSV_ABI int ReadClip(void* object, uint8_t* buffer, uint64_t position, uint32_t length) {
	const auto& clip = *static_cast<const std::vector<uint8_t>*>(object);
	if (position >= clip.size()) {
		return 0;
	}
	const auto count = std::min<uint64_t>(length, clip.size() - position);
	std::memcpy(buffer, clip.data() + position, count);
	return static_cast<int>(count);
}

KYTY_SYSV_ABI uint64_t ClipSize(void* object) {
	return static_cast<const std::vector<uint8_t>*>(object)->size();
}

// In-memory output for the AVI muxer, which seeks back to patch its headers.
struct MemoryOutput {
	std::vector<uint8_t> bytes;
	int64_t              position = 0;
};

using WriteBuffer = std::conditional_t<(LIBAVFORMAT_VERSION_MAJOR >= 61), const uint8_t*, uint8_t*>;

int WriteOutput(void* opaque, WriteBuffer buffer, int size) {
	auto*      output = static_cast<MemoryOutput*>(opaque);
	const auto end    = static_cast<size_t>(output->position) + static_cast<size_t>(size);
	if (end > output->bytes.size()) {
		output->bytes.resize(end);
	}
	std::memcpy(output->bytes.data() + output->position, buffer, static_cast<size_t>(size));
	output->position += size;
	return size;
}

int64_t SeekOutput(void* opaque, int64_t offset, int whence) {
	auto* output = static_cast<MemoryOutput*>(opaque);
	if ((whence & AVSEEK_SIZE) != 0) {
		return static_cast<int64_t>(output->bytes.size());
	}
	int64_t position = -1;
	switch (whence) {
		case SEEK_SET: position = offset; break;
		case SEEK_CUR: position = output->position + offset; break;
		case SEEK_END: position = static_cast<int64_t>(output->bytes.size()) + offset; break;
		default: break;
	}
	if (position < 0) {
		return AVERROR(EINVAL);
	}
	output->position = position;
	return position;
}

void ConfigureVideoEncoder(AVCodecContext* context) {
	context->width        = ClipWidth;
	context->height       = ClipHeight;
	context->pix_fmt      = AV_PIX_FMT_YUV420P;
	context->time_base    = {1, ClipFps};
	context->framerate    = {ClipFps, 1};
	context->gop_size     = ClipFrames;
	context->max_b_frames = 0;
	context->bit_rate     = 400000;
}

void ConfigureAudioEncoder(AVCodecContext* context) {
	context->sample_fmt  = AV_SAMPLE_FMT_S16;
	context->sample_rate = AudioRate;
	context->time_base   = {1, AudioRate};
	av_channel_layout_default(&context->ch_layout, 1);
}

AVCodecContext* OpenEncoder(const AVCodec* codec, AVFormatContext* format,
                            void (*configure)(AVCodecContext*)) {
	auto* context = avcodec_alloc_context3(codec);
	Check(context != nullptr, "allocate encoder context");
	configure(context);
	if ((format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
		context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}
	Check(avcodec_open2(context, codec, nullptr) == 0, "open encoder");
	return context;
}

void WritePackets(AVFormatContext* format, AVCodecContext* encoder, AVStream* stream,
                  const AVFrame* frame) {
	Check(avcodec_send_frame(encoder, frame) == 0, "send frame to encoder");
	for (;;) {
		auto*      packet = av_packet_alloc();
		const auto result = avcodec_receive_packet(encoder, packet);
		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
			av_packet_free(&packet);
			return;
		}
		Check(result == 0, "receive encoded packet");
		av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
		packet->stream_index = stream->index;
		Check(av_interleaved_write_frame(format, packet) == 0, "write packet");
		av_packet_free(&packet);
	}
}

// Encodes the test clip as MPEG-4 video with PCM audio in AVI, all of which the bundled FFmpeg
// can write. Returns false when this FFmpeg build cannot.
bool BuildClip(std::vector<uint8_t>* clip) {
	const auto* video_codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
	const auto* audio_codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
	if (video_codec == nullptr || audio_codec == nullptr ||
	    av_guess_format("avi", nullptr, nullptr) == nullptr) {
		return false;
	}

	AVFormatContext* format = nullptr;
	Check(avformat_alloc_output_context2(&format, nullptr, "avi", nullptr) == 0,
	      "allocate AVI muxer");
	MemoryOutput output;
	auto*        io_buffer = static_cast<unsigned char*>(av_malloc(4096));
	format->pb = avio_alloc_context(io_buffer, 4096, 1, &output, nullptr, WriteOutput, SeekOutput);
	Check(format->pb != nullptr, "allocate muxer output");

	auto* video        = OpenEncoder(video_codec, format, ConfigureVideoEncoder);
	auto* audio        = OpenEncoder(audio_codec, format, ConfigureAudioEncoder);
	auto* video_stream = avformat_new_stream(format, nullptr);
	auto* audio_stream = avformat_new_stream(format, nullptr);
	Check(video_stream != nullptr && audio_stream != nullptr, "create streams");
	Check(avcodec_parameters_from_context(video_stream->codecpar, video) >= 0 &&
	          avcodec_parameters_from_context(audio_stream->codecpar, audio) >= 0,
	      "copy stream parameters");
	video_stream->time_base = video->time_base;
	audio_stream->time_base = audio->time_base;
	Check(avformat_write_header(format, nullptr) >= 0, "write AVI header");

	auto* picture   = av_frame_alloc();
	picture->format = AV_PIX_FMT_YUV420P;
	picture->width  = ClipWidth;
	picture->height = ClipHeight;
	Check(av_frame_get_buffer(picture, 0) == 0, "allocate picture");
	auto* samples        = av_frame_alloc();
	samples->format      = AV_SAMPLE_FMT_S16;
	samples->nb_samples  = AudioChunkSamples;
	samples->sample_rate = AudioRate;
	av_channel_layout_default(&samples->ch_layout, 1);
	Check(av_frame_get_buffer(samples, 0) == 0, "allocate samples");

	// Interleave by presentation time: a 100 ms video frame, then the audio up to the next one.
	int audio_chunk = 0;
	for (int frame = 0; frame < ClipFrames; frame++) {
		Check(av_frame_make_writable(picture) == 0, "write picture");
		for (int y = 0; y < static_cast<int>(ClipHeight); y++) {
			for (int x = 0; x < static_cast<int>(ClipWidth); x++) {
				picture->data[0][y * picture->linesize[0] + x] =
				    static_cast<uint8_t>(64 + x / 2 + frame);
			}
		}
		for (int y = 0; y < static_cast<int>(ClipHeight) / 2; y++) {
			for (int x = 0; x < static_cast<int>(ClipWidth) / 2; x++) {
				picture->data[1][y * picture->linesize[1] + x] = static_cast<uint8_t>(96 + x / 2);
				picture->data[2][y * picture->linesize[2] + x] = static_cast<uint8_t>(160 - y);
			}
		}
		picture->pts = frame;
		WritePackets(format, video, video_stream, picture);

		const int audio_end =
		    std::min(AudioChunks, (frame + 1) * AudioRate / ClipFps / AudioChunkSamples);
		for (; audio_chunk < audio_end; audio_chunk++) {
			Check(av_frame_make_writable(samples) == 0, "write samples");
			auto* pcm = reinterpret_cast<int16_t*>(samples->data[0]);
			for (int i = 0; i < AudioChunkSamples; i++) {
				pcm[i] = static_cast<int16_t>((i % 96) * 256 - 12288);
			}
			samples->pts = static_cast<int64_t>(audio_chunk) * AudioChunkSamples;
			WritePackets(format, audio, audio_stream, samples);
		}
	}
	WritePackets(format, video, video_stream, nullptr);
	WritePackets(format, audio, audio_stream, nullptr);
	Check(av_write_trailer(format) == 0, "write AVI trailer");

	av_frame_free(&picture);
	av_frame_free(&samples);
	avcodec_free_context(&video);
	avcodec_free_context(&audio);
	av_freep(&format->pb->buffer);
	avio_context_free(&format->pb);
	avformat_free_context(format);

	*clip = std::move(output.bytes);
	return true;
}

// Right and bottom padding contains limited-range black luma and neutral chroma.
void CheckBlackFramePadding(const GuestFrameInfoEx& frame) {
	const auto& video = frame.video;
	Check(video.width == ClipWidth && video.height == ClipAlignedHeight, "reported frame size");
	Check(video.pitch == ClipPitch, "pitch is the width aligned to 256");
	Check(video.crop_right_offset == ClipPitch - ClipWidth, "right crop includes pitch padding");
	Check(video.crop_bottom_offset == ClipAlignedHeight - ClipHeight, "bottom crop");
	Check(video.video_full_range_flag == 0, "the fixture uses limited range");

	const auto* luma = static_cast<const uint8_t*>(frame.data);
	Check(luma[ClipWidth - 1] != 16, "decoded picture luma differs from black padding");
	for (uint32_t y = 0; y < ClipAlignedHeight; y++) {
		const auto* row = luma + static_cast<size_t>(y) * ClipPitch;
		for (uint32_t x = y < ClipHeight ? ClipWidth : 0; x < ClipPitch; x++) {
			Check(row[x] == 16, "luma padding is limited-range black");
		}
	}
	const auto* chroma = luma + static_cast<size_t>(ClipPitch) * ClipAlignedHeight;
	Check(chroma[ClipWidth - 2] != 128 || chroma[ClipWidth - 1] != 128,
	      "decoded picture chroma differs from neutral padding");
	for (uint32_t y = 0; y < ClipAlignedHeight / 2; y++) {
		const auto* row = chroma + static_cast<size_t>(y) * ClipPitch;
		for (uint32_t x = y < ClipHeight / 2 ? ClipWidth : 0; x < ClipPitch; x++) {
			Check(row[x] == 128, "chroma padding is neutral");
		}
	}
}

struct PlaybackResult {
	int  video_frames = 0;
	int  audio_frames = 0;
	bool finished     = false;
};

// Plays the clip the way a title does: it pulls audio and video until the player reports that
// it is no longer active.
PlaybackResult Play(std::vector<uint8_t>& clip, bool check_padding) {
	GuestInitData init;
	init.allocate                      = Allocate;
	init.deallocate                    = Deallocate;
	init.allocate_texture              = Allocate;
	init.deallocate_texture            = Deallocate;
	init.file_object                   = &clip;
	init.open                          = OpenClip;
	init.close                         = CloseClip;
	init.read_offset                   = ReadClip;
	init.size                          = ClipSize;
	init.num_output_video_framebuffers = 4;

	// Without an event callback the player starts as soon as the source is added.
	auto* player = AvPlayer::AvPlayerInit(reinterpret_cast<AvPlayer::AvPlayerInitData*>(&init));
	Check(player != nullptr, "initialize the player");
	Check(AvPlayer::AvPlayerAddSource(player, "clip.avi") == 0, "add the clip");

	PlaybackResult result;
	const auto     deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < deadline) {
		GuestFrameInfo audio;
		while (AvPlayer::AvPlayerGetAudioData(
		           player, reinterpret_cast<AvPlayer::AvPlayerFrameInfo*>(&audio)) != 0) {
			result.audio_frames++;
		}
		GuestFrameInfoEx video;
		if (AvPlayer::AvPlayerGetVideoDataEx(
		        player, reinterpret_cast<AvPlayer::AvPlayerFrameInfoEx*>(&video)) != 0) {
			if (check_padding && result.video_frames == 0) {
				CheckBlackFramePadding(video);
			}
			result.video_frames++;
		}
		if (AvPlayer::AvPlayerIsActive(player) == 0) {
			result.finished = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	Check(AvPlayer::AvPlayerClose(player) == 0, "close the player");
	return result;
}

void TestPlaysVideoPastTheEndOfAudio(std::vector<uint8_t>& clip) {
	const auto result = Play(clip, false);
	Check(result.audio_frames > 0, "audio frames were delivered");
	Check(result.finished, "the player finishes when the audio track ends before the video");
	Check(result.video_frames == ClipFrames, "every video frame is presented");
}

void TestPadsFramesWithBlack(std::vector<uint8_t>& clip) {
	const auto result = Play(clip, true);
	Check(result.video_frames > 0, "a video frame was delivered");
}

// Titles copy each plane into a linear R8 or R8G8 texture as wide as the pitch, so the texture
// rows must be read with the stride the frame was written with.
void TestPlanesMatchLinearTexturePitch() {
	using Libs::Graphics::TileGetTexturePitch;
	using Libs::Graphics::Prospero::BufferFormat;
	using Libs::Graphics::Prospero::TileMode;
	for (const uint32_t pitch: {ClipPitch, 640u, 1920u}) {
		Check(TileGetTexturePitch(BufferFormat::k8UNorm, pitch, TileMode::kLinear) == pitch,
		      "luma plane rows are unpadded");
		Check(TileGetTexturePitch(BufferFormat::k8_8UNorm, pitch / 2, TileMode::kLinear) ==
		          pitch / 2,
		      "chroma plane rows are unpadded");
	}
}

} // namespace

int main() {
	std::vector<uint8_t> clip;
	if (!BuildClip(&clip)) {
		std::printf("AvPlayerTests: skipped, FFmpeg cannot encode the MPEG-4/PCM AVI test clip\n");
		return SkipReturnCode;
	}

	Common::InitializeThreads();
	Common::Subsystems subsystems;
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	Config::Load(options);
	subsystems.Initialize<Log::Lifecycle>();
	subsystems.Initialize<Loader::Timer::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::PthreadLifecycle>();
	subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
	// Guest threads release their TLS through the runtime linker, which the emulator creates on
	// the main thread before any guest thread starts.
	(void)Common::Singleton<Loader::RuntimeLinker>::Instance();

	TestPlaysVideoPastTheEndOfAudio(clip);
	TestPadsFramesWithBlack(clip);
	TestPlanesMatchLinearTexturePitch();

	subsystems.Destroy();
	std::printf("AvPlayerTests: all cases passed\n");
	return 0;
}
