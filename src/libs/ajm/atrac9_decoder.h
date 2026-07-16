#pragma once

#include "common/assert.h"
#include "common/logging/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

extern "C" {
#include "libatrac9.h"
}

namespace Libs::Audio::Ajm {

struct AjmDecAt9InitializeParameters {
	uint8_t  config_data[ATRAC9_CONFIG_DATA_SIZE];
	uint32_t reserved;
};

struct AjmSidebandDecAt9CodecInfo {
	uint32_t super_frame_size;
	uint32_t frames_in_super_frame;
	uint32_t next_frame_size;
	uint32_t frame_samples;
};

static constexpr uint64_t AJM_INSTANCE_FLAG_DEC_AT9_PARSE_RIFF_HEADER =
    1ull << (AJM_INSTANCE_FLAG_CODEC_OFFSET + 0u);

static uint16_t AjmReadLe16(const uint8_t* data) {
	return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8u);
}

static uint32_t AjmReadLe32(const uint8_t* data) {
	return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8u) |
	       (static_cast<uint32_t>(data[2]) << 16u) | (static_cast<uint32_t>(data[3]) << 24u);
}

static bool AjmFourCcEquals(const uint8_t* data, char a, char b, char c, char d) {
	return data[0] == static_cast<uint8_t>(a) && data[1] == static_cast<uint8_t>(b) &&
	       data[2] == static_cast<uint8_t>(c) && data[3] == static_cast<uint8_t>(d);
}

class AjmAt9Decoder final: public AjmDecoder {
public:
	AjmAt9Decoder(uint32_t channels, uint32_t sample_rate, AjmSampleEncoding encoding,
	              uint64_t flags)
	    : AjmDecoder(channels, sample_rate, encoding),
	      m_parse_riff_header((flags & AJM_INSTANCE_FLAG_DEC_AT9_PARSE_RIFF_HEADER) != 0),
	      m_handle(Atrac9GetHandle()) {}

	~AjmAt9Decoder() override {
		if (m_handle != nullptr) {
			Atrac9ReleaseHandle(m_handle);
		}
	}

	AjmDecodeResult Initialize(const void* codec_parameters,
	                           size_t      codec_parameters_size) override {
		auto result = MakeResult();

		if (codec_parameters == nullptr || codec_parameters_size < ATRAC9_CONFIG_DATA_SIZE) {
			result.result = AJM_RESULT_INVALID_PARAMETER;
			return result;
		}

		const auto* params = static_cast<const AjmDecAt9InitializeParameters*>(codec_parameters);
		if (!InitializeConfig(params->config_data, &result)) {
			return result;
		}

		return MakeResult();
	}

	void Reset() override {
		if (m_handle != nullptr) {
			Atrac9ReleaseHandle(m_handle);
		}
		m_handle                = Atrac9GetHandle();
		m_num_frames            = 0;
		m_total_decoded_samples = 0;
		m_superframe_bytes_remain =
		    (m_is_initialized ? static_cast<uint32_t>(m_codec_info.superframeSize) : 0);

		if (m_has_config && m_handle != nullptr) {
			auto result = MakeResult();
			(void)InitializeConfig(m_config_data, &result);
		}
	}

	AjmDecodeResult Decode(const void* input, size_t input_size, void* output, size_t output_size,
	                       bool multiple_frames, AjmGaplessState* gapless) override {
		auto result = MakeResult();

		if (input == nullptr || input_size == 0) {
			result.result = AJM_RESULT_PARTIAL_INPUT;
			return result;
		}

		const auto* input_bytes  = static_cast<const uint8_t*>(input);
		auto*       output_bytes = static_cast<uint8_t*>(output);
		size_t      input_offset = 0;

		if (m_parse_riff_header && input_size >= 12 &&
		    AjmFourCcEquals(input_bytes, 'R', 'I', 'F', 'F')) {
			if (!ParseRiffHeader(input_bytes, input_size, &input_offset, gapless, &result)) {
				return result;
			}
			if (input_offset >= input_size) {
				result.input_consumed = input_offset;
				result.result         = AJM_RESULT_PARTIAL_INPUT;
				return result;
			}
		}

		if (!m_is_initialized) {
			result.result = AJM_RESULT_NOT_INITIALIZED;
			return result;
		}

		for (;;) {
			if (input_offset >= input_size) {
				break;
			}
			if (gapless != nullptr && gapless->IsEnd()) {
				break;
			}

			const auto remaining_input = input_size - input_offset;
			if (remaining_input < GetMinimumInputSize()) {
				if (result.frames == 0) {
					result.result = AJM_RESULT_PARTIAL_INPUT;
				}
				break;
			}

			const auto frame_output_bytes = FrameOutputBytes(gapless);
			if (frame_output_bytes != 0 && output == nullptr) {
				result.result |= AJM_RESULT_NOT_ENOUGH_ROOM;
				break;
			}
			if (frame_output_bytes == 0 && !FrameCanProduceNoOutput(gapless)) {
				result.result |= AJM_RESULT_NOT_ENOUGH_ROOM;
				break;
			}
			if (output_size - result.output_written < frame_output_bytes) {
				result.result |= AJM_RESULT_NOT_ENOUGH_ROOM;
				break;
			}

			int       bytes_used = 0;
			const int ret        = DecodeFrame(input_bytes + input_offset, &bytes_used);
			if (ret != 0) {
				result.result          = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
				result.internal_result = ret;
				break;
			}
			if (bytes_used <= 0 || static_cast<size_t>(bytes_used) > remaining_input ||
			    static_cast<uint32_t>(bytes_used) > m_superframe_bytes_remain) {
				result.result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
				break;
			}

			const auto samples_written =
			    CopyDecodedFrame(output_bytes, output_size, &result.output_written, gapless);

			input_offset += static_cast<size_t>(bytes_used);
			result.input_consumed = input_offset;
			result.frames++;
			m_total_decoded_samples += samples_written;

			AdvanceSuperframe(static_cast<uint32_t>(bytes_used), input_size, &input_offset,
			                  &result);
			if (gapless != nullptr && gapless->IsEnd()) {
				DrainSuperframe(input_bytes, input_size, &input_offset, &result);
			}
			if (result.result != OK || !multiple_frames) {
				break;
			}
		}

		result.total_decoded_samples = m_total_decoded_samples;
		result.format                = GetFormat();
		return result;
	}

	void WriteCodecInfo(void* output, size_t output_size,
	                    const AjmDecodeResult& result) const override {
		(void)result;
		if (output == nullptr || output_size < sizeof(AjmSidebandDecAt9CodecInfo)) {
			return;
		}

		auto* info = static_cast<AjmSidebandDecAt9CodecInfo*>(output);
		info->super_frame_size =
		    m_is_initialized ? static_cast<uint32_t>(m_codec_info.superframeSize) : 0;
		info->frames_in_super_frame =
		    m_is_initialized ? static_cast<uint32_t>(m_codec_info.framesInSuperframe) : 0;
		info->next_frame_size = GetMinimumInputSize();
		info->frame_samples =
		    m_is_initialized ? static_cast<uint32_t>(m_codec_info.frameSamples) : 0;
	}

	[[nodiscard]] size_t CodecInfoSize() const override {
		return sizeof(AjmSidebandDecAt9CodecInfo);
	}

	[[nodiscard]] AjmSidebandFormat GetFormat() const override {
		auto format = AjmMakeFormat(
		    m_is_initialized ? static_cast<uint32_t>(m_codec_info.channels) : m_channels,
		    m_is_initialized ? static_cast<uint32_t>(m_codec_info.samplingRate) : m_sample_rate,
		    m_sample_encoding);
		if (m_is_initialized && m_codec_info.framesInSuperframe > 0 &&
		    m_codec_info.frameSamples > 0) {
			format.bitrate =
			    static_cast<uint32_t>((static_cast<uint64_t>(m_codec_info.samplingRate) *
			                           static_cast<uint64_t>(m_codec_info.superframeSize) * 8u) /
			                          (static_cast<uint64_t>(m_codec_info.framesInSuperframe) *
			                           static_cast<uint64_t>(m_codec_info.frameSamples)));
		}
		return format;
	}

private:
	[[nodiscard]] uint32_t FrameOutputSamples(const AjmGaplessState* gapless) const {
		if (!m_is_initialized || m_codec_info.channels <= 0 || m_codec_info.frameSamples <= 0) {
			return 0;
		}
		auto samples = static_cast<uint32_t>(m_codec_info.frameSamples);
		if (gapless != nullptr) {
			const auto skip_samples = std::min<uint32_t>(samples, gapless->current.skip_samples);
			samples -= skip_samples;
			if (gapless->HasSampleLimit()) {
				samples = std::min<uint32_t>(samples, gapless->current.total_samples);
			}
		}
		return samples;
	}

	[[nodiscard]] bool FrameCanProduceNoOutput(const AjmGaplessState* gapless) const {
		return m_is_initialized && m_codec_info.channels > 0 && m_codec_info.frameSamples > 0 &&
		       gapless != nullptr &&
		       (gapless->current.skip_samples != 0 ||
		        (gapless->HasSampleLimit() && gapless->current.total_samples == 0));
	}

	[[nodiscard]] size_t FrameOutputBytes(const AjmGaplessState* gapless = nullptr) const {
		if (!m_is_initialized || m_codec_info.channels <= 0 || m_codec_info.frameSamples <= 0) {
			return 0;
		}
		return static_cast<size_t>(m_codec_info.channels) *
		       static_cast<size_t>(FrameOutputSamples(gapless)) *
		       AjmBytesPerSample(m_sample_encoding);
	}

	[[nodiscard]] uint32_t GetMinimumInputSize() const {
		return (m_is_initialized ? m_superframe_bytes_remain : 0);
	}

	bool InitializeConfig(const uint8_t* config_data, AjmDecodeResult* result) {
		if (config_data == nullptr || result == nullptr) {
			return false;
		}
		if (m_handle == nullptr) {
			m_handle = Atrac9GetHandle();
		}
		if (m_handle == nullptr) {
			result->result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
			return false;
		}

		std::memcpy(m_config_data, config_data, ATRAC9_CONFIG_DATA_SIZE);
		m_has_config = true;

		const int init_ret = Atrac9InitDecoder(m_handle, m_config_data);
		if (init_ret != 0) {
			m_is_initialized        = false;
			result->result          = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
			result->internal_result = init_ret;
			return false;
		}

		const int info_ret = Atrac9GetCodecInfo(m_handle, &m_codec_info);
		if (info_ret != 0 || m_codec_info.channels <= 0 || m_codec_info.frameSamples <= 0 ||
		    m_codec_info.superframeSize <= 0 || m_codec_info.framesInSuperframe <= 0) {
			m_is_initialized        = false;
			result->result          = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
			result->internal_result = info_ret;
			return false;
		}

		m_is_initialized          = true;
		m_num_frames              = 0;
		m_total_decoded_samples   = 0;
		m_superframe_bytes_remain = static_cast<uint32_t>(m_codec_info.superframeSize);
		SetFormat(static_cast<uint32_t>(m_codec_info.channels),
		          static_cast<uint32_t>(m_codec_info.samplingRate), m_sample_encoding);
		m_pcm_buffer.assign(FrameOutputBytes(), 0);

		LOGF("AJM ATRAC9 initialized: %d Hz, %d ch, frame_samples=%d, superframe=%d bytes/%d "
		     "frames\n",
		     m_codec_info.samplingRate, m_codec_info.channels, m_codec_info.frameSamples,
		     m_codec_info.superframeSize, m_codec_info.framesInSuperframe);

		return true;
	}

	int DecodeFrame(const uint8_t* input, int* bytes_used) {
		switch (m_sample_encoding) {
			case AjmSampleEncoding::S16:
				return Atrac9Decode(m_handle, input,
				                    reinterpret_cast<int16_t*>(m_pcm_buffer.data()), bytes_used, 0);
			case AjmSampleEncoding::S32:
				return Atrac9DecodeS32(m_handle, input,
				                       reinterpret_cast<int32_t*>(m_pcm_buffer.data()), bytes_used,
				                       0);
			case AjmSampleEncoding::Float:
				return Atrac9DecodeF32(
				    m_handle, input, reinterpret_cast<float*>(m_pcm_buffer.data()), bytes_used, 0);
			default:
				EXIT("unsupported AJM PCM sample encoding %u\n",
				     static_cast<uint32_t>(m_sample_encoding));
		}
		return -1;
	}

	uint64_t CopyDecodedFrame(uint8_t* output, size_t output_size, size_t* output_written,
	                          AjmGaplessState* gapless) {
		EXIT_IF(output_written == nullptr);
		if (!m_is_initialized || m_codec_info.channels <= 0 || m_codec_info.frameSamples <= 0) {
			return 0;
		}

		const auto channels      = static_cast<uint32_t>(m_codec_info.channels);
		const auto frame_samples = static_cast<uint32_t>(m_codec_info.frameSamples);
		const auto bytes_per_frame =
		    static_cast<size_t>(channels) * AjmBytesPerSample(m_sample_encoding);
		auto skip_samples     = 0u;
		auto samples_to_write = frame_samples;

		if (gapless != nullptr) {
			skip_samples = std::min<uint32_t>(frame_samples, gapless->current.skip_samples);
			gapless->current.skip_samples =
			    static_cast<uint16_t>(gapless->current.skip_samples - skip_samples);

			samples_to_write -= skip_samples;
			if (gapless->HasSampleLimit()) {
				samples_to_write =
				    std::min<uint32_t>(samples_to_write, gapless->current.total_samples);
			}
		}

		const auto copy_bytes = static_cast<size_t>(samples_to_write) * bytes_per_frame;
		if (copy_bytes != 0) {
			EXIT_IF(output == nullptr);
			EXIT_IF(*output_written > output_size || copy_bytes > output_size - *output_written);
			std::memcpy(output + *output_written,
			            m_pcm_buffer.data() + static_cast<size_t>(skip_samples) * bytes_per_frame,
			            copy_bytes);
			*output_written += copy_bytes;
		}

		if (gapless != nullptr) {
			if (gapless->HasSampleLimit()) {
				gapless->current.total_samples =
				    (gapless->current.total_samples > samples_to_write
				         ? gapless->current.total_samples - samples_to_write
				         : 0);
			}

			const auto skipped_samples = frame_samples - samples_to_write;
			const auto skipped_total   = std::min<uint32_t>(
			    std::numeric_limits<uint16_t>::max(),
			    static_cast<uint32_t>(gapless->current.skipped_samples) + skipped_samples);
			gapless->current.skipped_samples = static_cast<uint16_t>(skipped_total);
		}

		return samples_to_write;
	}

	void AdvanceSuperframe(uint32_t bytes_used, size_t input_size, size_t* input_offset,
	                       AjmDecodeResult* result) {
		EXIT_IF(input_offset == nullptr);
		EXIT_IF(result == nullptr);

		m_superframe_bytes_remain -= bytes_used;
		m_num_frames++;

		if (m_num_frames < static_cast<uint32_t>(m_codec_info.framesInSuperframe)) {
			return;
		}

		if (m_superframe_bytes_remain != 0) {
			const auto remaining_input =
			    (*input_offset < input_size ? input_size - *input_offset : 0);
			if (remaining_input < m_superframe_bytes_remain) {
				result->result = AJM_RESULT_PARTIAL_INPUT;
				return;
			}
			*input_offset += m_superframe_bytes_remain;
			result->input_consumed = *input_offset;
		}

		m_superframe_bytes_remain = static_cast<uint32_t>(m_codec_info.superframeSize);
		m_num_frames              = 0;
	}

	void DrainSuperframe(const uint8_t* input, size_t input_size, size_t* input_offset,
	                     AjmDecodeResult* result) {
		EXIT_IF(input_offset == nullptr);
		EXIT_IF(result == nullptr);

		while (m_num_frames != 0) {
			const auto remaining_input =
			    (*input_offset < input_size ? input_size - *input_offset : 0);
			if (remaining_input < GetMinimumInputSize()) {
				result->result = AJM_RESULT_PARTIAL_INPUT;
				return;
			}

			int       bytes_used = 0;
			const int ret        = DecodeFrame(input + *input_offset, &bytes_used);
			if (ret != 0) {
				result->result          = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
				result->internal_result = ret;
				return;
			}
			if (bytes_used <= 0 || static_cast<size_t>(bytes_used) > remaining_input ||
			    static_cast<uint32_t>(bytes_used) > m_superframe_bytes_remain) {
				result->result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
				return;
			}

			*input_offset += static_cast<size_t>(bytes_used);
			result->input_consumed = *input_offset;
			result->frames++;
			AdvanceSuperframe(static_cast<uint32_t>(bytes_used), input_size, input_offset, result);
			if (result->result != OK) {
				return;
			}
		}
	}

	bool ParseRiffHeader(const uint8_t* input, size_t input_size, size_t* data_offset,
	                     AjmGaplessState* gapless, AjmDecodeResult* result) {
		EXIT_IF(data_offset == nullptr);
		EXIT_IF(result == nullptr);

		*data_offset = 0;
		if (input_size < 12 || !AjmFourCcEquals(input, 'R', 'I', 'F', 'F') ||
		    !AjmFourCcEquals(input + 8, 'W', 'A', 'V', 'E')) {
			result->result = AJM_RESULT_INVALID_DATA;
			return false;
		}

		size_t offset = 12;
		while (offset + 8 <= input_size) {
			const auto* chunk      = input + offset;
			const auto  chunk_size = static_cast<size_t>(AjmReadLe32(chunk + 4));
			const auto  payload    = offset + 8;
			if (payload > input_size || chunk_size > input_size - payload) {
				result->result = AJM_RESULT_PARTIAL_INPUT;
				return false;
			}

			if (AjmFourCcEquals(chunk, 'f', 'm', 't', ' ')) {
				if (chunk_size < 48) {
					result->result = AJM_RESULT_INVALID_DATA;
					return false;
				}
				const auto*              fmt          = input + payload;
				static constexpr uint8_t AT9_GUID[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36,
				                                         0x8d, 0x4d, 0x88, 0xfc, 0x61, 0x65,
				                                         0x4f, 0x8c, 0x83, 0x6c};
				if (AjmReadLe16(fmt) != 0xfffe ||
				    std::memcmp(fmt + 24, AT9_GUID, sizeof(AT9_GUID)) != 0) {
					result->result = AJM_RESULT_INVALID_DATA;
					return false;
				}
				if (!InitializeConfig(fmt + 44, result)) {
					return false;
				}
			} else if (AjmFourCcEquals(chunk, 'f', 'a', 'c', 't')) {
				if (chunk_size >= 12 && gapless != nullptr) {
					AjmSidebandGaplessDecode gapless_decode {};
					gapless_decode.total_samples = AjmReadLe32(input + payload);
					gapless_decode.skip_samples  = static_cast<uint16_t>(std::min<uint32_t>(
					    AjmReadLe32(input + payload + 4), std::numeric_limits<uint16_t>::max()));
					gapless->Set(gapless_decode, true);
					LOGF("AJM ATRAC9 gapless: total=%" PRIu32 ", skip=%" PRIu16 "\n",
					     gapless_decode.total_samples, gapless_decode.skip_samples);
				}
			} else if (AjmFourCcEquals(chunk, 'd', 'a', 't', 'a')) {
				*data_offset                  = payload;
				result->input_consumed        = payload;
				result->format                = GetFormat();
				result->total_decoded_samples = m_total_decoded_samples;
				return true;
			}

			const auto padded_size = chunk_size + (chunk_size & 1u);
			if (padded_size > std::numeric_limits<size_t>::max() - 8 ||
			    offset > std::numeric_limits<size_t>::max() - 8 - padded_size) {
				result->result = AJM_RESULT_INVALID_DATA;
				return false;
			}
			offset += 8 + padded_size;
		}

		result->result = AJM_RESULT_PARTIAL_INPUT;
		return false;
	}

	bool                 m_parse_riff_header = false;
	bool                 m_is_initialized    = false;
	bool                 m_has_config        = false;
	void*                m_handle            = nullptr;
	uint8_t              m_config_data[ATRAC9_CONFIG_DATA_SIZE] {};
	uint32_t             m_superframe_bytes_remain = 0;
	uint32_t             m_num_frames              = 0;
	Atrac9CodecInfo      m_codec_info {};
	std::vector<uint8_t> m_pcm_buffer;
};

} // namespace Libs::Audio::Ajm
