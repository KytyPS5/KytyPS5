#include "libs/videoDec2Decoder.h"

#include "common/logging/log.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace Libs::VideoDec2::Decoder {

namespace {

constexpr uint32_t CODEC_TYPE_AVC  = 1;
constexpr uint32_t CODEC_TYPE_HEVC = 974921;
constexpr uint32_t CODEC_TYPE_VP9  = 2382845;

struct PacketMetadata {
	uint64_t pts           = TIMESTAMP_INVALID;
	uint64_t dts           = TIMESTAMP_INVALID;
	uint64_t attached_data = 0;
};

struct StoredPicture {
	const Instance* owner = nullptr;
	PictureInfo     info;
};

std::mutex                               g_picture_mutex;
std::unordered_map<void*, StoredPicture> g_picture_infos;

AVCodecID GetAvCodecId(uint32_t codec_type) {
	switch (codec_type) {
		case CODEC_TYPE_AVC: return AV_CODEC_ID_H264;
		case CODEC_TYPE_HEVC: return AV_CODEC_ID_HEVC;
		case CODEC_TYPE_VP9: return AV_CODEC_ID_VP9;
		default: return AV_CODEC_ID_NONE;
	}
}

const char* AvErrorString(int error) {
	thread_local char text[AV_ERROR_MAX_STRING_SIZE] {};
	if (av_strerror(error, text, sizeof(text)) != 0) {
		std::strcpy(text, "unknown FFmpeg error");
	}
	return text;
}

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
	return (value + alignment - 1u) & ~(alignment - 1u);
}

int64_t ToAvTimestamp(uint64_t timestamp) {
	return timestamp == TIMESTAMP_INVALID ||
	               timestamp > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
	           ? AV_NOPTS_VALUE
	           : static_cast<int64_t>(timestamp);
}

} // namespace

class Instance {
public:
	explicit Instance(const Config& config): m_config(config) {}

	~Instance() {
		ClearPictureMetadata();
		if (m_sws != nullptr) {
			sws_freeContext(m_sws);
		}
		if (m_codec != nullptr) {
			avcodec_free_context(&m_codec);
		}
	}

	Instance(const Instance&)            = delete;
	Instance& operator=(const Instance&) = delete;

	[[nodiscard]] bool Initialize() {
		const AVCodec* decoder = avcodec_find_decoder(GetAvCodecId(m_config.codec_type));
		if (decoder == nullptr) {
			LOGF("Videodec2: FFmpeg decoder is unavailable for codec type %u\n",
			     m_config.codec_type);
			return false;
		}
		m_codec = avcodec_alloc_context3(decoder);
		if (m_codec == nullptr) {
			LOGF("Videodec2: avcodec_alloc_context3 failed\n");
			return false;
		}

		// This carries PTS/DTS/attachedData through codecs that reorder B frames.
		m_codec->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
		const int result = avcodec_open2(m_codec, decoder, nullptr);
		if (result < 0) {
			LOGF("Videodec2: avcodec_open2 failed: %s (%d)\n", AvErrorString(result), result);
			return false;
		}
		return true;
	}

	[[nodiscard]] uint32_t CodecType() const { return m_config.codec_type; }

	[[nodiscard]] Result DecodeInput(const Input& input, const FrameBuffer& frame_buffer,
	                                 Output* output) {
		std::scoped_lock lock(m_mutex);
		*output    = {};
		m_draining = false;

		AVPacket* packet = av_packet_alloc();
		AVFrame*  frame  = av_frame_alloc();
		if (packet == nullptr || frame == nullptr ||
		    input.size > static_cast<size_t>(std::numeric_limits<int>::max())) {
			av_packet_free(&packet);
			av_frame_free(&frame);
			return Result::ApiFail;
		}

		int result = av_new_packet(packet, static_cast<int>(input.size));
		if (result < 0) {
			LOGF("Videodec2: av_new_packet failed: %s (%d)\n", AvErrorString(result), result);
			av_packet_free(&packet);
			av_frame_free(&frame);
			return Result::ApiFail;
		}
		std::memcpy(packet->data, input.data, input.size);
		packet->pts = ToAvTimestamp(input.pts);
		packet->dts = ToAvTimestamp(input.dts);

		packet->opaque_ref = av_buffer_alloc(sizeof(PacketMetadata));
		if (packet->opaque_ref == nullptr) {
			av_packet_free(&packet);
			av_frame_free(&frame);
			return Result::ApiFail;
		}
		const PacketMetadata metadata {input.pts, input.dts, input.attached_data};
		std::memcpy(packet->opaque_ref->data, &metadata, sizeof(metadata));

		bool have_pending_frame = false;
		result                  = avcodec_send_packet(m_codec, packet);
		if (result == AVERROR(EAGAIN)) {
			result = avcodec_receive_frame(m_codec, frame);
			if (result < 0) {
				LOGF("Videodec2: decoder rejected an AU while no output was available: %s (%d)\n",
				     AvErrorString(result), result);
				av_packet_free(&packet);
				av_frame_free(&frame);
				return Result::AccessUnit;
			}
			have_pending_frame = true;
			result             = avcodec_send_packet(m_codec, packet);
		}
		if (result < 0) {
			LOGF("Videodec2: avcodec_send_packet failed: %s (%d)\n", AvErrorString(result), result);
			av_packet_free(&packet);
			av_frame_free(&frame);
			return Result::AccessUnit;
		}

		Result decode_result = Result::Ok;
		if (!have_pending_frame) {
			result = avcodec_receive_frame(m_codec, frame);
			if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
				if (result < 0) {
					LOGF("Videodec2: avcodec_receive_frame failed: %s (%d)\n",
					     AvErrorString(result), result);
					decode_result = Result::AccessUnit;
				} else {
					decode_result = CopyFrame(frame, frame_buffer, output);
				}
			}
		} else {
			decode_result = CopyFrame(frame, frame_buffer, output);
		}

		av_packet_free(&packet);
		av_frame_free(&frame);
		return decode_result;
	}

	[[nodiscard]] Result FlushOutput(const FrameBuffer& frame_buffer, Output* output) {
		std::scoped_lock lock(m_mutex);
		*output = {};

		AVFrame* frame = av_frame_alloc();
		if (frame == nullptr) {
			return Result::ApiFail;
		}

		if (!m_draining) {
			const int send_result = avcodec_send_packet(m_codec, nullptr);
			if (send_result == 0 || send_result == AVERROR_EOF) {
				m_draining = true;
			} else if (send_result != AVERROR(EAGAIN)) {
				LOGF("Videodec2: flushing decoder failed: %s (%d)\n", AvErrorString(send_result),
				     send_result);
				av_frame_free(&frame);
				return Result::ApiFail;
			}
		}

		const int receive_result = avcodec_receive_frame(m_codec, frame);
		if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
			av_frame_free(&frame);
			return Result::Ok;
		}
		if (receive_result < 0) {
			LOGF("Videodec2: receiving a flushed frame failed: %s (%d)\n",
			     AvErrorString(receive_result), receive_result);
			av_frame_free(&frame);
			return Result::ApiFail;
		}

		const auto result = CopyFrame(frame, frame_buffer, output);
		av_frame_free(&frame);
		return result;
	}

	void ResetDecoder() {
		std::scoped_lock lock(m_mutex);
		avcodec_flush_buffers(m_codec);
		m_draining = false;
		ClearPictureMetadata();
	}

private:
	[[nodiscard]] PictureInfo MakePictureInfo(const AVFrame* frame) const {
		PictureInfo result {};
		if (frame->opaque_ref != nullptr && frame->opaque_ref->size >= sizeof(PacketMetadata)) {
			PacketMetadata metadata {};
			std::memcpy(&metadata, frame->opaque_ref->data, sizeof(metadata));
			result.pts           = metadata.pts;
			result.dts           = metadata.dts;
			result.attached_data = metadata.attached_data;
		} else {
			result.pts = frame->pts == AV_NOPTS_VALUE ? TIMESTAMP_INVALID
			                                          : static_cast<uint64_t>(frame->pts);
			result.dts = frame->pkt_dts == AV_NOPTS_VALUE ? TIMESTAMP_INVALID
			                                              : static_cast<uint64_t>(frame->pkt_dts);
		}
		result.codec_type  = m_config.codec_type;
		result.width       = static_cast<uint32_t>(frame->width);
		result.height      = static_cast<uint32_t>(frame->height);
		result.crop_left   = static_cast<uint32_t>(frame->crop_left);
		result.crop_right  = static_cast<uint32_t>(frame->crop_right);
		result.crop_top    = static_cast<uint32_t>(frame->crop_top);
		result.crop_bottom = static_cast<uint32_t>(frame->crop_bottom);
		result.profile     = m_codec->profile > 0 ? static_cast<uint32_t>(m_codec->profile) : 0;
		result.level       = m_codec->level > 0 ? static_cast<uint32_t>(m_codec->level) : 0;
		result.sar_width =
		    frame->sample_aspect_ratio.num > 0
		        ? static_cast<uint16_t>(std::min(frame->sample_aspect_ratio.num, 65535))
		        : 0;
		result.sar_height =
		    frame->sample_aspect_ratio.den > 0
		        ? static_cast<uint16_t>(std::min(frame->sample_aspect_ratio.den, 65535))
		        : 0;
		result.color_range     = static_cast<uint8_t>(frame->color_range);
		result.color_primaries = static_cast<uint8_t>(frame->color_primaries);
		result.color_trc       = static_cast<uint8_t>(frame->color_trc);
		result.color_space     = static_cast<uint8_t>(frame->colorspace);
		result.key_frame       = (frame->flags & AV_FRAME_FLAG_KEY) != 0;
		return result;
	}

	[[nodiscard]] Result CopyFrame(const AVFrame* frame, const FrameBuffer& frame_buffer,
	                               Output* output) {
		if (frame->width <= 0 || frame->height <= 0) {
			return Result::ApiFail;
		}
		if ((m_config.max_width > 0 && frame->width > m_config.max_width) ||
		    (m_config.max_height > 0 && frame->height > m_config.max_height)) {
			return Result::OversizeDecode;
		}

		const auto width       = static_cast<uint32_t>(frame->width);
		const auto height      = static_cast<uint32_t>(frame->height);
		const auto pitch       = AlignUp(width, 256);
		const auto chroma_rows = (static_cast<uint64_t>(height) + 1u) / 2u;
		const auto required =
		    static_cast<uint64_t>(pitch) * height + static_cast<uint64_t>(pitch) * chroma_rows;
		if (required > frame_buffer.size) {
			return Result::FrameBufferSize;
		}

		auto* dst = static_cast<uint8_t*>(frame_buffer.data);
		std::memset(dst, 0, static_cast<size_t>(required));
		if (frame->format == AV_PIX_FMT_NV12) {
			for (uint32_t y = 0; y < height; y++) {
				std::memcpy(dst + static_cast<size_t>(y) * pitch,
				            frame->data[0] + static_cast<ptrdiff_t>(y) * frame->linesize[0], width);
			}
			auto* chroma = dst + static_cast<size_t>(pitch) * height;
			for (uint32_t y = 0; y < chroma_rows; y++) {
				std::memcpy(chroma + static_cast<size_t>(y) * pitch,
				            frame->data[1] + static_cast<ptrdiff_t>(y) * frame->linesize[1], width);
			}
		} else {
			m_sws = sws_getCachedContext(m_sws, frame->width, frame->height,
			                             static_cast<AVPixelFormat>(frame->format), frame->width,
			                             frame->height, AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, nullptr,
			                             nullptr, nullptr);
			if (m_sws == nullptr) {
				return Result::ApiFail;
			}
			uint8_t* output_planes[4]  = {dst, dst + static_cast<size_t>(pitch) * height, nullptr,
			                              nullptr};
			int      output_strides[4] = {static_cast<int>(pitch), static_cast<int>(pitch), 0, 0};
			if (sws_scale(m_sws, frame->data, frame->linesize, 0, frame->height, output_planes,
			              output_strides) != frame->height) {
				return Result::ApiFail;
			}
		}

		output->valid           = true;
		output->error_frame     = (frame->flags & AV_FRAME_FLAG_CORRUPT) != 0;
		output->buffer_accepted = true;
		output->codec_type      = m_config.codec_type;
		output->width           = width;
		output->pitch           = pitch;
		output->height          = height;
		output->buffer          = frame_buffer.data;
		output->buffer_size     = frame_buffer.size;

		{
			std::scoped_lock lock(g_picture_mutex);
			g_picture_infos[frame_buffer.data] = {this, MakePictureInfo(frame)};
			m_picture_buffers.insert(frame_buffer.data);
		}
		return Result::Ok;
	}

	void ClearPictureMetadata() {
		std::scoped_lock lock(g_picture_mutex);
		for (auto* buffer: m_picture_buffers) {
			const auto it = g_picture_infos.find(buffer);
			if (it != g_picture_infos.end() && it->second.owner == this) {
				g_picture_infos.erase(it);
			}
		}
		m_picture_buffers.clear();
	}

	Config                    m_config;
	AVCodecContext*           m_codec    = nullptr;
	SwsContext*               m_sws      = nullptr;
	bool                      m_draining = false;
	std::mutex                m_mutex;
	std::unordered_set<void*> m_picture_buffers;
};

bool IsCodecSupported(uint32_t codec_type) {
	return GetAvCodecId(codec_type) != AV_CODEC_ID_NONE;
}

Instance* Create(const Config& config) {
	if (!IsCodecSupported(config.codec_type)) {
		return nullptr;
	}
	auto* instance = new Instance(config);
	if (!instance->Initialize()) {
		delete instance;
		return nullptr;
	}
	return instance;
}

void Destroy(Instance* instance) {
	delete instance;
}

uint32_t GetCodecType(const Instance* instance) {
	return instance->CodecType();
}

Result Decode(Instance* instance, const Input& input, const FrameBuffer& frame_buffer,
              Output* output) {
	return instance->DecodeInput(input, frame_buffer, output);
}

Result Flush(Instance* instance, const FrameBuffer& frame_buffer, Output* output) {
	return instance->FlushOutput(frame_buffer, output);
}

void Reset(Instance* instance) {
	instance->ResetDecoder();
}

bool GetPictureInfo(void* frame_buffer, PictureInfo* picture_info) {
	std::scoped_lock lock(g_picture_mutex);
	const auto       it = g_picture_infos.find(frame_buffer);
	if (it == g_picture_infos.end()) {
		return false;
	}
	*picture_info = it->second.info;
	return true;
}

} // namespace Libs::VideoDec2::Decoder
