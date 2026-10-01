#pragma once

#include "libs/ajm/decoder.h"
#include "libs/ajm/ffmpeg_decoder_common.h"

#include <algorithm>
#include <cstring>

namespace Libs::Audio::Ajm {

struct AjmDecOpusInitializeParameters {
	uint32_t channel_num;
	uint32_t sample_rate;
	uint32_t mapping_family;
};

struct AjmSidebandDecOpusCodecInfo {
	uint32_t frames_per_packet;
};

static_assert(sizeof(AjmDecOpusInitializeParameters) == 12);
static_assert(sizeof(AjmSidebandDecOpusCodecInfo) == 4);

class AjmOpusDecoder final: public AjmDecoder {
public:
	AjmOpusDecoder(uint32_t max_channels, AjmSampleEncoding encoding)
	    : AjmDecoder(0, 48000, encoding), m_max_channels(max_channels) {}

	~AjmOpusDecoder() override { avcodec_free_context(&m_context); }

	AjmDecodeResult Initialize(const void* parameters, size_t parameters_size) override {
		avcodec_free_context(&m_context);
		Reset();
		auto result = MakeResult();
		if (parameters == nullptr || parameters_size < sizeof(AjmDecOpusInitializeParameters)) {
			result.result = AJM_RESULT_INVALID_PARAMETER | AJM_RESULT_FATAL;
			return result;
		}
		const auto& params = *static_cast<const AjmDecOpusInitializeParameters*>(parameters);
		if (params.channel_num == 0 || params.channel_num > 2 || params.sample_rate != 48000 ||
		    params.mapping_family != 0) {
			result.result = AJM_RESULT_INVALID_PARAMETER | AJM_RESULT_FATAL;
			return result;
		}
		if (params.channel_num > m_max_channels) {
			result.result = AJM_RESULT_TOO_MANY_CHANNELS | AJM_RESULT_FATAL;
			return result;
		}

		const auto* codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
		m_context         = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
		if (m_context != nullptr) {
			av_channel_layout_default(&m_context->ch_layout, static_cast<int>(params.channel_num));
			m_context->sample_rate = static_cast<int>(params.sample_rate);
			if (avcodec_open2(m_context, codec, nullptr) < 0) {
				avcodec_free_context(&m_context);
			}
		}
		if (m_context == nullptr) {
			result.result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
			return result;
		}
		SetFormat(params.channel_num, params.sample_rate, m_sample_encoding);
		return MakeResult();
	}

	void Reset() override {
		if (m_context != nullptr) {
			avcodec_flush_buffers(m_context);
		}
		m_total_decoded_samples = 0;
		m_frames_per_packet     = 0;
	}

	AjmDecodeResult Decode(const void* input, size_t input_size, void* output, size_t output_size,
	                       bool multiple_frames, AjmGaplessState* gapless) override {
		auto result = MakeResult();
		if (m_context == nullptr) {
			result.result = AJM_RESULT_NOT_INITIALIZED;
			return result;
		}
		if (input == nullptr || input_size == 0) {
			result.result = AJM_RESULT_PARTIAL_INPUT;
			return result;
		}

		while (gapless == nullptr || !gapless->IsEnd()) {
			const auto* data      = static_cast<const uint8_t*>(input) + result.input_consumed;
			const auto  remaining = input_size - result.input_consumed;
			if (remaining < 2) {
				result.result = AJM_RESULT_PARTIAL_INPUT;
				break;
			}
			const auto packet_size = static_cast<uint32_t>(data[0]) | (uint32_t {data[1]} << 8u);
			if (packet_size > remaining - 2) {
				result.result = AJM_RESULT_PARTIAL_INPUT;
				break;
			}
			data += 2;
			if (packet_size == 0 || ((data[0] & 3u) == 3 && packet_size < 2)) {
				result.result          = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
				result.internal_result = 0x1043;
				break;
			}
			const uint32_t config        = data[0] >> 3u;
			const uint32_t frames        = (data[0] & 3u) == 0   ? 1
			                               : (data[0] & 3u) == 3 ? data[1] & 0x3fu
			                                                     : 2;
			const uint32_t frame_samples = config >= 16         ? 120u << (config & 3u)
			                               : config >= 12       ? 480u << (config & 1u)
			                               : (config & 3u) == 3 ? 2880u
			                                                    : 480u << (config & 3u);
			const uint32_t samples       = frame_samples * frames;
			if (frames == 0 || frames > 6 || frame_samples > 960) {
				result.result          = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
				result.internal_result = static_cast<int32_t>(0xffff8800u);
				break;
			}
			const auto skip  = gapless != nullptr
			                       ? std::min(samples, uint32_t {gapless->current.skip_samples})
			                       : 0;
			const auto write = gapless != nullptr && gapless->HasSampleLimit()
			                       ? std::min(samples - skip, gapless->current.total_samples)
			                       : samples - skip;
			const auto sample_bytes = m_channels * AjmBytesPerSample(m_sample_encoding);
			const auto output_bytes = write * sample_bytes;
			if (output_bytes > output_size - result.output_written ||
			    (output_bytes != 0 && output == nullptr)) {
				result.result = AJM_RESULT_NOT_ENOUGH_ROOM;
				break;
			}

			AVFrame* frame = DecodePacket(data, packet_size, &result);
			if (frame == nullptr) {
				break;
			}
			if (frame->nb_samples != static_cast<int>(samples) ||
			    frame->ch_layout.nb_channels != static_cast<int>(m_channels)) {
				av_frame_free(&frame);
				result.result          = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
				result.internal_result = 0x1043;
				break;
			}
			if (output_bytes != 0) {
				std::memcpy(static_cast<uint8_t*>(output) + result.output_written,
				            frame->data[0] + skip * sample_bytes, output_bytes);
			}
			av_frame_free(&frame);
			if (gapless != nullptr) {
				gapless->current.skip_samples -= static_cast<uint16_t>(skip);
				gapless->current.skipped_samples = static_cast<uint16_t>(
				    std::min<uint32_t>(std::numeric_limits<uint16_t>::max(),
				                       gapless->current.skipped_samples + samples - write));
				if (gapless->HasSampleLimit()) {
					gapless->current.total_samples -= write;
				}
			}
			result.input_consumed += packet_size + 2;
			result.output_written += output_bytes;
			result.frames += frames;
			m_frames_per_packet = frames;
			m_total_decoded_samples += write;
			if (!multiple_frames) {
				break;
			}
		}
		result.frames_per_packet     = m_frames_per_packet;
		result.total_decoded_samples = m_total_decoded_samples;
		return result;
	}

	void WriteCodecInfo(void* output, size_t size, const AjmDecodeResult&) const override {
		if (output != nullptr && size >= sizeof(AjmSidebandDecOpusCodecInfo)) {
			static_cast<AjmSidebandDecOpusCodecInfo*>(output)->frames_per_packet =
			    m_frames_per_packet;
		}
	}

	[[nodiscard]] size_t CodecInfoSize() const override {
		return sizeof(AjmSidebandDecOpusCodecInfo);
	}

private:
	AVFrame* DecodePacket(const uint8_t* data, uint32_t size, AjmDecodeResult* result) {
		AVPacket* packet = av_packet_alloc();
		AVFrame*  frame  = av_frame_alloc();
		if (packet == nullptr || frame == nullptr ||
		    av_new_packet(packet, static_cast<int>(size)) < 0) {
			av_packet_free(&packet);
			av_frame_free(&frame);
			result->result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
			return nullptr;
		}
		std::memcpy(packet->data, data, size);
		int rc = avcodec_send_packet(m_context, packet);
		av_packet_free(&packet);
		if (rc >= 0) {
			rc = avcodec_receive_frame(m_context, frame);
		}
		if (rc < 0) {
			av_frame_free(&frame);
			result->result          = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
			result->internal_result = 0x1043;
			return nullptr;
		}
		AVFrame* converted = AjmConvertFfmpegFrame(frame, m_sample_encoding, result);
		av_frame_free(&frame);
		return converted;
	}

	AVCodecContext* m_context = nullptr;
	const uint32_t  m_max_channels;
	uint32_t        m_frames_per_packet = 0;
};

}
