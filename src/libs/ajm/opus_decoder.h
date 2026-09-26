#pragma once

#include "common/logging/log.h"
#include "libs/ajm/decoder.h"
#include "libs/ajm/ffmpeg_decoder_common.h"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

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

static constexpr uint32_t AJM_DEC_OPUS_MAX_CHANNELS_FOR_10CH = 10;
static constexpr uint32_t AJM_DEC_OPUS_MAPPING_FAMILY_RTP    = 0;
static constexpr uint32_t AJM_DEC_OPUS_MAX_CHANNELS_RTP      = 2;
static constexpr uint32_t AJM_DEC_OPUS_SAMPLE_RATE           = 48000;
static constexpr size_t   AJM_DEC_OPUS_OPUS_HEAD_SIZE        = 19;
// Each access unit is a little-endian 16-bit length followed by that many packet bytes.
static constexpr size_t AJM_DEC_OPUS_PACKET_LENGTH_SIZE = 2;

class AjmOpusDecoder final: public AjmDecoder {
public:
	AjmOpusDecoder(uint32_t channels, uint32_t sample_rate, AjmSampleEncoding encoding,
	               uint64_t flags)
	    : AjmDecoder(channels, sample_rate, encoding) {
		(void)flags;
		m_codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
		if (m_codec == nullptr) {
			LOGF("AJM Opus: avcodec_find_decoder failed\n");
		}
	}

	~AjmOpusDecoder() override { Release(); }

	AjmDecodeResult Initialize(const void* codec_parameters,
	                           size_t      codec_parameters_size) override {
		auto result = MakeResult();

		if (codec_parameters == nullptr ||
		    codec_parameters_size < sizeof(AjmDecOpusInitializeParameters)) {
			result.result = AJM_RESULT_INVALID_PARAMETER;
			return result;
		}

		const auto* params = static_cast<const AjmDecOpusInitializeParameters*>(codec_parameters);
		if (params->channel_num == 0 || params->channel_num > AJM_DEC_OPUS_MAX_CHANNELS_FOR_10CH) {
			result.result = AJM_RESULT_INVALID_PARAMETER;
			return result;
		}
		if (params->sample_rate != AJM_DEC_OPUS_SAMPLE_RATE) {
			LOGF("AJM Opus: sample rate %" PRIu32 " Hz is not implemented\n", params->sample_rate);
			result.result = AJM_RESULT_INVALID_PARAMETER;
			return result;
		}
		if (params->mapping_family != AJM_DEC_OPUS_MAPPING_FAMILY_RTP) {
			LOGF("AJM Opus: mapping family %" PRIu32 " is not implemented\n",
			     params->mapping_family);
			result.result = AJM_RESULT_INVALID_PARAMETER;
			return result;
		}
		if (params->channel_num > AJM_DEC_OPUS_MAX_CHANNELS_RTP) {
			LOGF("AJM Opus: mapping family 0 does not define %" PRIu32 " channels\n",
			     params->channel_num);
			result.result = AJM_RESULT_INVALID_PARAMETER;
			return result;
		}

		Release();
		SetFormat(params->channel_num, params->sample_rate, m_sample_encoding);
		m_mapping_family        = params->mapping_family;
		m_total_decoded_samples = 0;
		m_frames_per_packet     = 1;
		m_is_initialized        = false;
		m_pending_offset        = 0;
		m_pending.clear();

		if (!OpenDecoder(params->channel_num, params->sample_rate, params->mapping_family)) {
			result.result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
			return result;
		}

		m_is_initialized = true;

		LOGF("AJM Opus initialized: %" PRIu32 " Hz, %" PRIu32 " ch, mapping=%" PRIu32 "\n",
		     params->sample_rate, params->channel_num, params->mapping_family);

		return MakeResult();
	}

	void Reset() override {
		m_total_decoded_samples = 0;
		m_pending.clear();
		m_pending_offset = 0;
		if (m_codec_context != nullptr) {
			avcodec_flush_buffers(m_codec_context);
		}
	}

	AjmDecodeResult Decode(const void* input, size_t input_size, void* output, size_t output_size,
	                       bool multiple_frames, AjmGaplessState* gapless) override {
		auto result = MakeResult();

		if (!m_is_initialized) {
			result.result = AJM_RESULT_NOT_INITIALIZED;
			return result;
		}
		if (m_codec_context == nullptr) {
			result.result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
			return result;
		}
		if (input == nullptr && input_size != 0) {
			result.result = AJM_RESULT_PARTIAL_INPUT;
			return result;
		}
		if (input_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
			result.result = AJM_RESULT_INVALID_PARAMETER;
			return result;
		}

		if (output != nullptr && output_size != 0) {
			std::memset(output, 0, output_size);
		}

		const auto* data          = static_cast<const uint8_t*>(input);
		size_t      input_offset  = 0;
		size_t      output_offset = 0;
		bool        produced      = DrainPending(output, output_size, &output_offset, &result);

		while (m_pending_offset >= m_pending.size() &&
		       input_offset + AJM_DEC_OPUS_PACKET_LENGTH_SIZE <= input_size &&
		       output_offset < output_size) {
			const size_t length = static_cast<size_t>(data[input_offset]) |
			                      (static_cast<size_t>(data[input_offset + 1]) << 8u);
			if (length == 0 ||
			    input_offset + AJM_DEC_OPUS_PACKET_LENGTH_SIZE + length > input_size) {
				result.result |= AJM_RESULT_INVALID_DATA;
				break;
			}

			if (!DecodePacket(data + input_offset + AJM_DEC_OPUS_PACKET_LENGTH_SIZE,
			                  static_cast<int>(length), gapless, &result)) {
				break;
			}

			input_offset += AJM_DEC_OPUS_PACKET_LENGTH_SIZE + length;
			produced = DrainPending(output, output_size, &output_offset, &result) || produced;
			if (!multiple_frames) {
				break;
			}
		}

		if (m_pending_offset < m_pending.size()) {
			result.result |= AJM_RESULT_NOT_ENOUGH_ROOM;
		}
		if (!produced && result.result == OK) {
			result.result = AJM_RESULT_PARTIAL_INPUT;
		}

		result.input_consumed        = input_offset;
		result.output_written        = output_offset;
		result.frames_per_packet     = m_frames_per_packet;
		result.total_decoded_samples = m_total_decoded_samples;
		result.format                = GetFormat();
		return result;
	}

	void WriteCodecInfo(void* output, size_t output_size,
	                    const AjmDecodeResult& result) const override {
		(void)result;
		if (output == nullptr || output_size < sizeof(AjmSidebandDecOpusCodecInfo)) {
			return;
		}

		auto* info              = static_cast<AjmSidebandDecOpusCodecInfo*>(output);
		info->frames_per_packet = m_frames_per_packet;
	}

	[[nodiscard]] size_t CodecInfoSize() const override {
		return sizeof(AjmSidebandDecOpusCodecInfo);
	}

	[[nodiscard]] AjmSidebandFormat GetFormat() const override {
		return AjmMakeFormat(m_channels, m_sample_rate, m_sample_encoding);
	}

private:
	void Release() {
		if (m_codec_context != nullptr) {
			avcodec_free_context(&m_codec_context);
		}
	}
	// The instance carries no container, so the identification header is rebuilt here.
	static void FillOpusHead(uint8_t* head, uint32_t channels, uint32_t sample_rate,
	                         uint32_t mapping_family) {
		std::memcpy(head, "OpusHead", 8);
		head[8]  = 1;
		head[9]  = static_cast<uint8_t>(channels);
		head[10] = 0;
		head[11] = 0;
		head[12] = static_cast<uint8_t>(sample_rate & 0xffu);
		head[13] = static_cast<uint8_t>((sample_rate >> 8u) & 0xffu);
		head[14] = static_cast<uint8_t>((sample_rate >> 16u) & 0xffu);
		head[15] = static_cast<uint8_t>((sample_rate >> 24u) & 0xffu);
		head[16] = 0;
		head[17] = 0;
		head[18] = static_cast<uint8_t>(mapping_family);
	}

	bool OpenDecoder(uint32_t channels, uint32_t sample_rate, uint32_t mapping_family) {
		if (m_codec == nullptr) {
			return false;
		}

		m_codec_context = avcodec_alloc_context3(m_codec);
		if (m_codec_context == nullptr) {
			return false;
		}

		m_codec_context->sample_rate = static_cast<int>(sample_rate);
		av_channel_layout_default(&m_codec_context->ch_layout, static_cast<int>(channels));

		auto* extradata = static_cast<uint8_t*>(
		    av_mallocz(AJM_DEC_OPUS_OPUS_HEAD_SIZE + AV_INPUT_BUFFER_PADDING_SIZE));
		if (extradata == nullptr) {
			Release();
			return false;
		}
		FillOpusHead(extradata, channels, sample_rate, mapping_family);
		m_codec_context->extradata      = extradata;
		m_codec_context->extradata_size = static_cast<int>(AJM_DEC_OPUS_OPUS_HEAD_SIZE);

		if (avcodec_open2(m_codec_context, m_codec, nullptr) < 0) {
			LOGF("AJM Opus: decoder initialization failed\n");
			Release();
			return false;
		}

		return true;
	}

	bool DrainPending(void* output, size_t output_size, size_t* output_offset,
	                  AjmDecodeResult* result) {
		if (output == nullptr || output_offset == nullptr || result == nullptr) {
			return false;
		}
		const auto available = m_pending.size() - m_pending_offset;
		if (available == 0 || *output_offset >= output_size) {
			return false;
		}
		const auto bpf = m_channels * AjmBytesPerSample(m_sample_encoding);
		if (bpf == 0) {
			return false;
		}

		auto bytes = std::min(available, output_size - *output_offset);
		bytes -= bytes % bpf;
		if (bytes == 0) {
			return false;
		}

		std::memcpy(static_cast<uint8_t*>(output) + *output_offset,
		            m_pending.data() + m_pending_offset, bytes);
		*output_offset += bytes;
		m_pending_offset += bytes;
		if (m_pending_offset >= m_pending.size()) {
			m_pending.clear();
			m_pending_offset = 0;
		}

		const auto samples = static_cast<uint32_t>(bytes / bpf);
		m_total_decoded_samples += samples;
		result->total_decoded_samples = m_total_decoded_samples;
		result->format                = GetFormat();
		return samples != 0;
	}

	bool AppendFrame(const AVFrame* frame, AjmGaplessState* gapless, AjmDecodeResult* result) {
		if (frame == nullptr || result == nullptr) {
			return false;
		}

		const auto channels = static_cast<uint32_t>(std::max(frame->ch_layout.nb_channels, 1));
		if (channels > AJM_DEC_OPUS_MAX_CHANNELS_FOR_10CH) {
			result->result = AJM_RESULT_TOO_MANY_CHANNELS;
			return false;
		}
		const auto bpf = channels * AjmBytesPerSample(m_sample_encoding);
		if (bpf == 0) {
			result->result = AJM_RESULT_INVALID_PARAMETER;
			return false;
		}

		SetFormat(channels, static_cast<uint32_t>(std::max(frame->sample_rate, 1)),
		          m_sample_encoding);
		result->frames++;

		uint32_t skip_samples = 0;
		if (gapless != nullptr && gapless->current.skip_samples > 0) {
			skip_samples = std::min<uint32_t>(frame->nb_samples, gapless->current.skip_samples);
			gapless->current.skip_samples =
			    static_cast<uint16_t>(gapless->current.skip_samples - skip_samples);
		}

		uint32_t samples = static_cast<uint32_t>(frame->nb_samples) - skip_samples;
		if (gapless != nullptr && gapless->HasSampleLimit()) {
			samples = std::min(samples, gapless->current.total_samples);
			gapless->current.total_samples -= samples;
		}
		if (gapless != nullptr) {
			const auto skipped_total = std::min<uint32_t>(
			    std::numeric_limits<uint16_t>::max(),
			    static_cast<uint32_t>(gapless->current.skipped_samples) + skip_samples);
			gapless->current.skipped_samples = static_cast<uint16_t>(skipped_total);
		}

		if (samples == 0) {
			return true;
		}

		const auto* src = frame->data[0] + static_cast<size_t>(skip_samples) * bpf;
		m_pending.insert(m_pending.end(), src, src + static_cast<size_t>(samples) * bpf);
		return true;
	}

	bool DecodePacket(const uint8_t* packet_data, int packet_size, AjmGaplessState* gapless,
	                  AjmDecodeResult* result) {
		if (packet_data == nullptr || packet_size <= 0 || result == nullptr) {
			return true;
		}

		AVPacket* packet = av_packet_alloc();
		if (packet == nullptr || av_new_packet(packet, packet_size) < 0) {
			av_packet_free(&packet);
			result->result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
			return false;
		}
		std::memcpy(packet->data, packet_data, static_cast<size_t>(packet_size));

		int rc = avcodec_send_packet(m_codec_context, packet);
		av_packet_free(&packet);
		if (rc == AVERROR_INVALIDDATA) {
			result->result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
			return false;
		}
		if (rc < 0) {
			result->result = AJM_RESULT_PARTIAL_INPUT;
			return false;
		}

		uint32_t frames_in_packet = 0;
		for (;;) {
			AVFrame* frame = av_frame_alloc();
			if (frame == nullptr) {
				result->result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
				return false;
			}

			rc = avcodec_receive_frame(m_codec_context, frame);
			if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
				av_frame_free(&frame);
				if (frames_in_packet != 0) {
					m_frames_per_packet = frames_in_packet;
				}
				return true;
			}
			if (rc < 0) {
				av_frame_free(&frame);
				result->result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
				return false;
			}

			AVFrame* converted = AjmConvertFfmpegFrame(frame, m_sample_encoding, result);
			av_frame_free(&frame);
			if (converted == nullptr) {
				return false;
			}

			frames_in_packet++;
			const bool ok = AppendFrame(converted, gapless, result);
			av_frame_free(&converted);
			if (!ok) {
				if (frames_in_packet != 0) {
					m_frames_per_packet = frames_in_packet;
				}
				return false;
			}
		}
	}

	const AVCodec*       m_codec             = nullptr;
	AVCodecContext*      m_codec_context     = nullptr;
	uint32_t             m_mapping_family    = AJM_DEC_OPUS_MAPPING_FAMILY_RTP;
	uint32_t             m_frames_per_packet = 1;
	bool                 m_is_initialized    = false;
	std::vector<uint8_t> m_pending;
	size_t               m_pending_offset = 0;
};

} // namespace Libs::Audio::Ajm
