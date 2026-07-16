#pragma once

#include "common/logging/log.h"
#include "libs/ajm/ffmpeg_decoder_common.h"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace Libs::Audio::Ajm {

struct AjmDecM4aacInitializeParameters {
	uint32_t config_number;
	uint32_t sampling_freq_index;
};

struct AjmSidebandDecM4aacCodecInfo {
	uint32_t heaac;
	uint32_t reserved;
};

static_assert(sizeof(AjmDecM4aacInitializeParameters) == 8);
static_assert(sizeof(AjmSidebandDecM4aacCodecInfo) == 8);

static constexpr uint64_t AJM_INSTANCE_FLAG_DEC_M4AAC_ENABLE_SBR_DECODE =
    1ull << (AJM_INSTANCE_FLAG_CODEC_OFFSET + 0u);
static constexpr uint64_t AJM_INSTANCE_FLAG_DEC_M4AAC_ENABLE_NONDELAY_OUTPUT =
    1ull << (AJM_INSTANCE_FLAG_CODEC_OFFSET + 1u);
static constexpr uint32_t AJM_DEC_M4AAC_CONFIG_NUMBER_ADTS      = 1;
static constexpr uint32_t AJM_DEC_M4AAC_CONFIG_NUMBER_RAW       = 2;
static constexpr uint32_t AJM_DEC_M4AAC_CONFIG_NUMBER_SAF       = 3;
static constexpr uint32_t AJM_DEC_M4AAC_MAX_CHANNELS            = 8;
static constexpr uint32_t AJM_DEC_M4AAC_MAX_SAMPLING_FREQ_INDEX = 11;

class AjmAacDecoder final: public AjmDecoder {
public:
	AjmAacDecoder(uint32_t channels, uint32_t sample_rate, AjmSampleEncoding encoding,
	              uint64_t flags)
	    : AjmDecoder(channels, sample_rate, encoding), m_flags(flags) {
		m_codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
		if (m_codec == nullptr) {
			LOGF("AJM AAC: avcodec_find_decoder failed\n");
			return;
		}
	}

	~AjmAacDecoder() override { Release(); }

	AjmDecodeResult Initialize(const void* codec_parameters,
	                           size_t      codec_parameters_size) override {
		auto result = MakeResult();

		if (codec_parameters == nullptr ||
		    codec_parameters_size < sizeof(AjmDecM4aacInitializeParameters)) {
			result.result = AJM_RESULT_INVALID_PARAMETER;
			return result;
		}

		const auto* params = static_cast<const AjmDecM4aacInitializeParameters*>(codec_parameters);
		if (params->config_number != AJM_DEC_M4AAC_CONFIG_NUMBER_ADTS &&
		    params->config_number != AJM_DEC_M4AAC_CONFIG_NUMBER_RAW &&
		    params->config_number != AJM_DEC_M4AAC_CONFIG_NUMBER_SAF) {
			result.result = AJM_RESULT_INVALID_PARAMETER;
			return result;
		}
		if (params->config_number == AJM_DEC_M4AAC_CONFIG_NUMBER_RAW &&
		    params->sampling_freq_index > AJM_DEC_M4AAC_MAX_SAMPLING_FREQ_INDEX) {
			result.result = AJM_RESULT_INVALID_PARAMETER;
			return result;
		}

		m_config_number       = params->config_number;
		m_sampling_freq_index = params->sampling_freq_index;
		m_is_initialized      = true;
		Reset();
		if (m_codec_context == nullptr) {
			m_is_initialized = false;
			result.result    = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
			return result;
		}

		LOGF("AJM AAC initialized: config=%" PRIu32 ", freq_index=%" PRIu32 "\n", m_config_number,
		     m_sampling_freq_index);
		return result;
	}

	void Reset() override {
		Release();
		m_skip_frames =
		    ((m_flags & AJM_INSTANCE_FLAG_DEC_M4AAC_ENABLE_NONDELAY_OUTPUT) != 0 ? 0u : 2u);
		m_total_decoded_samples = 0;
		m_bitrate               = 0;
		if (m_is_initialized) {
			OpenDecoder();
		}
	}

	AjmDecodeResult Decode(const void* input, size_t input_size, void* output, size_t output_size,
	                       bool multiple_frames, AjmGaplessState* gapless) override {
		(void)multiple_frames;
		auto result = MakeResult();

		if (!m_is_initialized) {
			result.result = AJM_RESULT_NOT_INITIALIZED;
			return result;
		}
		if (m_codec_context == nullptr) {
			result.result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
			return result;
		}
		if (input == nullptr || input_size == 0) {
			result.result = AJM_RESULT_PARTIAL_INPUT;
			return result;
		}
		if (input_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
			result.result = AJM_RESULT_INVALID_PARAMETER;
			return result;
		}

		size_t output_offset = 0;
		if (!DecodePacket(static_cast<const uint8_t*>(input), static_cast<int>(input_size), output,
		                  output_size, &output_offset, gapless, &result)) {
			result.input_consumed        = input_size;
			result.output_written        = output_offset;
			result.total_decoded_samples = m_total_decoded_samples;
			result.format                = GetFormat();
			return result;
		}

		if (result.frames == 0 && result.result == OK) {
			result.result = AJM_RESULT_PARTIAL_INPUT;
		}

		result.input_consumed        = input_size;
		result.output_written        = output_offset;
		result.total_decoded_samples = m_total_decoded_samples;
		result.format                = GetFormat();
		return result;
	}

	void WriteCodecInfo(void* output, size_t output_size,
	                    const AjmDecodeResult& result) const override {
		(void)result;
		if (output == nullptr || output_size < sizeof(AjmSidebandDecM4aacCodecInfo)) {
			return;
		}

		auto* info     = static_cast<AjmSidebandDecM4aacCodecInfo*>(output);
		info->heaac    = ((m_flags & AJM_INSTANCE_FLAG_DEC_M4AAC_ENABLE_SBR_DECODE) != 0 ? 1u : 0u);
		info->reserved = 0;
	}

	[[nodiscard]] size_t CodecInfoSize() const override {
		return sizeof(AjmSidebandDecM4aacCodecInfo);
	}

	[[nodiscard]] AjmSidebandFormat GetFormat() const override {
		auto format    = AjmMakeFormat(m_channels, m_sample_rate, m_sample_encoding);
		format.bitrate = m_bitrate;
		return format;
	}

private:
	static uint32_t SamplingRateFromIndex(uint32_t index) {
		static constexpr uint32_t SAMPLE_RATES[] = {
		    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000,
		};
		return (index < (sizeof(SAMPLE_RATES) / sizeof(SAMPLE_RATES[0])) ? SAMPLE_RATES[index]
		                                                                 : 48000u);
	}

	void Release() {
		if (m_codec_context != nullptr) {
			avcodec_free_context(&m_codec_context);
		}
	}

	void OpenDecoder() {
		if (m_codec == nullptr) {
			return;
		}

		m_codec_context = avcodec_alloc_context3(m_codec);
		if (m_codec_context == nullptr) {
			return;
		}

		if (m_config_number == AJM_DEC_M4AAC_CONFIG_NUMBER_RAW) {
			const auto sample_index =
			    std::min(m_sampling_freq_index, AJM_DEC_M4AAC_MAX_SAMPLING_FREQ_INDEX);
			const auto channels = std::clamp<uint32_t>(m_channels, 1u, AJM_DEC_M4AAC_MAX_CHANNELS);
			constexpr uint32_t audio_object_type = 2;

			auto* extradata = static_cast<uint8_t*>(av_mallocz(2 + AV_INPUT_BUFFER_PADDING_SIZE));
			if (extradata == nullptr) {
				Release();
				return;
			}

			extradata[0] = static_cast<uint8_t>((audio_object_type << 3u) | (sample_index >> 1u));
			extradata[1] = static_cast<uint8_t>(((sample_index & 0x1u) << 7u) |
			                                    (std::min(channels, 7u) << 3u));
			m_codec_context->extradata      = extradata;
			m_codec_context->extradata_size = 2;
			SetFormat(channels, SamplingRateFromIndex(sample_index), m_sample_encoding);
		}

		if (avcodec_open2(m_codec_context, m_codec, nullptr) < 0) {
			LOGF("AJM AAC: decoder initialization failed\n");
			Release();
		}
	}

	bool WriteFrame(const AVFrame* frame, void* output, size_t output_size, size_t* output_offset,
	                AjmGaplessState* gapless, AjmDecodeResult* result) {
		if (frame == nullptr || output_offset == nullptr || result == nullptr) {
			return false;
		}

		const auto channels = static_cast<uint32_t>(std::max(frame->ch_layout.nb_channels, 1));
		const auto bpf      = channels * AjmBytesPerSample(m_sample_encoding);
		if (bpf == 0) {
			result->result = AJM_RESULT_INVALID_PARAMETER;
			return false;
		}

		SetFormat(channels, static_cast<uint32_t>(std::max(frame->sample_rate, 1)),
		          m_sample_encoding);
		if (m_codec_context != nullptr && m_codec_context->bit_rate > 0) {
			m_bitrate = static_cast<uint32_t>(m_codec_context->bit_rate);
		}

		result->frames++;
		if (m_skip_frames > 0) {
			m_skip_frames--;
			result->format = GetFormat();
			return true;
		}

		uint32_t skip_samples = 0;
		if (gapless != nullptr && gapless->current.skip_samples > 0) {
			skip_samples = std::min<uint32_t>(frame->nb_samples, gapless->current.skip_samples);
			gapless->current.skip_samples =
			    static_cast<uint16_t>(gapless->current.skip_samples - skip_samples);
		}

		uint32_t samples_to_write = static_cast<uint32_t>(frame->nb_samples) - skip_samples;
		if (gapless != nullptr && gapless->HasSampleLimit()) {
			samples_to_write = std::min(samples_to_write, gapless->current.total_samples);
		}

		const auto requested_bytes = static_cast<size_t>(samples_to_write) * bpf;
		auto       writable_bytes  = requested_bytes;
		if (output == nullptr || *output_offset >= output_size) {
			writable_bytes = 0;
		} else {
			writable_bytes = std::min(writable_bytes, output_size - *output_offset);
			writable_bytes -= writable_bytes % bpf;
		}

		if (writable_bytes != 0) {
			auto*       dst = static_cast<uint8_t*>(output) + *output_offset;
			const auto* src = frame->data[0] + static_cast<size_t>(skip_samples) * bpf;
			std::memcpy(dst, src, writable_bytes);
			*output_offset += writable_bytes;
		}

		const auto samples_written = static_cast<uint32_t>(writable_bytes / bpf);
		if (gapless != nullptr && gapless->HasSampleLimit()) {
			gapless->current.total_samples -=
			    std::min(gapless->current.total_samples, samples_written);
		}
		if (gapless != nullptr) {
			const auto skipped_total =
			    std::min<uint32_t>(std::numeric_limits<uint16_t>::max(),
			                       static_cast<uint32_t>(gapless->current.skipped_samples) +
			                           skip_samples + (samples_to_write - samples_written));
			gapless->current.skipped_samples = static_cast<uint16_t>(skipped_total);
		}

		m_total_decoded_samples += samples_written;
		result->total_decoded_samples = m_total_decoded_samples;
		result->format                = GetFormat();

		if (writable_bytes != requested_bytes) {
			result->result |= AJM_RESULT_NOT_ENOUGH_ROOM;
			return false;
		}
		return true;
	}

	bool DecodePacket(const uint8_t* packet_data, int packet_size, void* output, size_t output_size,
	                  size_t* output_offset, AjmGaplessState* gapless, AjmDecodeResult* result) {
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

		for (;;) {
			AVFrame* frame = av_frame_alloc();
			if (frame == nullptr) {
				result->result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
				return false;
			}

			rc = avcodec_receive_frame(m_codec_context, frame);
			if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
				av_frame_free(&frame);
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

			const bool ok =
			    WriteFrame(converted, output, output_size, output_offset, gapless, result);
			av_frame_free(&converted);
			if (!ok) {
				return false;
			}
		}
	}

	const AVCodec*  m_codec               = nullptr;
	AVCodecContext* m_codec_context       = nullptr;
	uint64_t        m_flags               = 0;
	uint32_t        m_config_number       = AJM_DEC_M4AAC_CONFIG_NUMBER_ADTS;
	uint32_t        m_sampling_freq_index = 3;
	uint32_t        m_skip_frames         = 2;
	uint32_t        m_bitrate             = 0;
	bool            m_is_initialized      = false;
};

} // namespace Libs::Audio::Ajm
