#pragma once

#include "common/assert.h"
#include "common/logging/log.h"
#include "libs/ajm/decoder.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

extern "C" {
#include "error_codes.h"
#include "libatrac9.h"
}

namespace Libs::Audio::Ajm {

struct AjmAt9Config {
	std::array<uint8_t, ATRAC9_CONFIG_DATA_SIZE> decoder_data {};
	uint32_t stream_count = 1;
	Atrac9CodecInfo info {};
};

using AjmAt9Handle = std::unique_ptr<void, decltype(&Atrac9ReleaseHandle)>;

inline int AjmAt9InitDecoder(void* handle, const uint8_t* config_data, AjmAt9Config& config) {
	config = {};
	std::memcpy(config.decoder_data.data(), config_data, config.decoder_data.size());
	if (config_data[0] == 0x30) {
		// Discrete multichannel config: reserved:1, rate:3, channels-1:6, reserved:1,
		// mono frame bytes-1:11, superframe index:2.
		const uint32_t bits = (uint32_t{config_data[1]} << 16u) |
		                      (uint32_t{config_data[2]} << 8u) | config_data[3];
		const uint32_t rate = (bits >> 20u) & 7u;
		const uint32_t frame_bytes = ((bits >> 2u) & 0x7ffu) + 1u;
		if ((bits & ((1u << 23u) | (1u << 13u))) != 0 ||
		    (rate != 1u && rate != 4u && rate != 7u) || frame_bytes > 96u ||
		    ((bits & 3u) != 0u && (bits & 3u) != 2u)) {
			return static_cast<int>(ERR_BAD_CONFIG_DATA);
		}
		config.stream_count = ((bits >> 14u) & 63u) + 1u;
		const uint32_t mono = 0xfe000000u | (bits & 0xf00000u) |
		                      (((bits >> 2u) & 0x7ffu) << 5u) | ((bits & 3u) << 3u);
		for (uint32_t i = 0; i < config.decoder_data.size(); ++i) {
			config.decoder_data[i] = static_cast<uint8_t>(mono >> ((3u - i) * 8u));
		}
	} else if (config_data[0] == 0xfe && ((config_data[1] >> 1u) & 7u) >= 6u) {
		// LibAtrac9 lacks vibration layouts; their blocks match ordinary mono/dual-mono.
		// Translate only the channel field, preserving the other configuration bits.
		config.decoder_data[1] -= 6u << 1u;
	}
	const int initialized = Atrac9InitDecoder(handle, config.decoder_data.data());
	if (initialized != 0) {
		return initialized;
	}
	const int result = Atrac9GetCodecInfo(handle, &config.info);
	if (result != 0) {
		return result;
	}
	if (config.info.channels <= 0 || config.info.samplingRate <= 0 ||
	    config.info.frameSamples <= 0 || config.info.framesInSuperframe <= 0 ||
	    config.info.superframeSize <= 0) {
		return static_cast<int>(ERR_BAD_CONFIG_DATA);
	}
	config.info.channels *= static_cast<int>(config.stream_count);
	config.info.superframeSize *= static_cast<int>(config.stream_count);
	std::memcpy(config.info.configData, config_data, ATRAC9_CONFIG_DATA_SIZE);
	return 0;
}

inline bool AjmAt9GetCodecInfo(const uint8_t* config_data, Atrac9CodecInfo& info) {
	AjmAt9Handle handle(Atrac9GetHandle(), Atrac9ReleaseHandle);
	AjmAt9Config config;
	if (handle == nullptr || AjmAt9InitDecoder(handle.get(), config_data, config) != 0) {
		return false;
	}
	info = config.info;
	return true;
}

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
	      m_parse_riff_header((flags & AJM_INSTANCE_FLAG_DEC_AT9_PARSE_RIFF_HEADER) != 0) {}

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
		m_streams.clear();
		m_is_initialized          = false;
		m_num_frames              = 0;
		m_total_decoded_samples   = 0;
		m_superframe_bytes_remain = 0;

		if (m_has_config) {
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
		m_is_initialized = false;
		m_streams.clear();
		m_streams.resize(1);
		m_streams[0].handle.reset(Atrac9GetHandle());
		if (m_streams[0].handle == nullptr) {
			result->result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
			return false;
		}

		std::memmove(m_config_data, config_data, ATRAC9_CONFIG_DATA_SIZE);
		m_has_config = true;

		AjmAt9Config config;
		const int init_ret = AjmAt9InitDecoder(m_streams[0].handle.get(), m_config_data, config);
		if (init_ret != 0) {
			result->result          = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
			result->internal_result = init_ret;
			return false;
		}
		m_streams.resize(config.stream_count);
		for (size_t i = 1; i < m_streams.size(); ++i) {
			m_streams[i].handle.reset(Atrac9GetHandle());
			if (m_streams[i].handle == nullptr) {
				result->result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
				return false;
			}
			const int initialized =
			    Atrac9InitDecoder(m_streams[i].handle.get(), config.decoder_data.data());
			if (initialized != 0) {
				result->result          = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
				result->internal_result = initialized;
				return false;
			}
		}
		m_codec_info = config.info;

		m_is_initialized          = true;
		m_num_frames              = 0;
		m_total_decoded_samples   = 0;
		m_superframe_bytes_remain = static_cast<uint32_t>(m_codec_info.superframeSize);
		SetFormat(static_cast<uint32_t>(m_codec_info.channels),
		          static_cast<uint32_t>(m_codec_info.samplingRate), m_sample_encoding);
		m_pcm_buffer.assign(FrameOutputBytes(), 0);
		m_stream_pcm.resize(m_streams.size() > 1
		                        ? m_codec_info.frameSamples * AjmBytesPerSample(m_sample_encoding)
		                        : 0);

		LOGF("AJM ATRAC9 initialized: %d Hz, %d ch, frame_samples=%d, superframe=%d bytes/%d "
		     "frames\n",
		     m_codec_info.samplingRate, m_codec_info.channels, m_codec_info.frameSamples,
		     m_codec_info.superframeSize, m_codec_info.framesInSuperframe);

		return true;
	}

	int DecodeStream(void* handle, const uint8_t* input, uint8_t* output, int* bytes_used) {
		switch (m_sample_encoding) {
			case AjmSampleEncoding::S16:
				return Atrac9Decode(handle, input, reinterpret_cast<int16_t*>(output), bytes_used, 0);
			case AjmSampleEncoding::S32:
				return Atrac9DecodeS32(handle, input, reinterpret_cast<int32_t*>(output), bytes_used, 0);
			case AjmSampleEncoding::Float:
				return Atrac9DecodeF32(handle, input, reinterpret_cast<float*>(output), bytes_used, 0);
			default:
				EXIT("unsupported AJM PCM sample encoding %u\n",
				     static_cast<uint32_t>(m_sample_encoding));
		}
		return -1;
	}

	template <size_t SampleBytes>
	void InterleaveStream(size_t channel) {
		for (int sample = 0; sample < m_codec_info.frameSamples; ++sample) {
			std::memcpy(m_pcm_buffer.data() + (sample * m_streams.size() + channel) * SampleBytes,
			            m_stream_pcm.data() + sample * SampleBytes, SampleBytes);
		}
	}

	int DecodeFrame(const uint8_t* input, int* bytes_used) {
		if (m_streams.size() == 1) {
			return DecodeStream(m_streams[0].handle.get(), input, m_pcm_buffer.data(), bytes_used);
		}
		const auto channels = m_streams.size();
		const auto sample_bytes = AjmBytesPerSample(m_sample_encoding);
		const auto stream_size = static_cast<uint32_t>(m_codec_info.superframeSize / channels);
		const bool last_frame = m_num_frames + 1u ==
		                        static_cast<uint32_t>(m_codec_info.framesInSuperframe);
		*bytes_used = 0;
		for (size_t channel = 0; channel < channels; ++channel) {
			auto& stream = m_streams[channel];
			if (stream.bytes_used >= stream_size) {
				return static_cast<int>(ERR_BAD_CONFIG_DATA);
			}
			int used = 0;
			const int result = DecodeStream(stream.handle.get(), input + *bytes_used,
			                                m_stream_pcm.data(), &used);
			if (result != 0) {
				return result;
			}
			if (used <= 0 || static_cast<uint32_t>(used) > stream_size - stream.bytes_used) {
				return static_cast<int>(ERR_BAD_CONFIG_DATA);
			}
			// DMC interleaves mono frames. On the last frame each channel's padding
			// precedes the next channel, rather than following the aggregate frame.
			*bytes_used += last_frame ? stream_size - stream.bytes_used : used;
			stream.bytes_used = last_frame ? 0 : stream.bytes_used + used;
			if (sample_bytes == 2) InterleaveStream<2>(channel);
			else InterleaveStream<4>(channel);
		}
		return 0;
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
			// The data chunk describes the whole stream; this job may contain only its prefix.
			if (AjmFourCcEquals(chunk, 'd', 'a', 't', 'a')) {
				*data_offset                  = payload;
				result->input_consumed        = payload;
				result->format                = GetFormat();
				result->total_decoded_samples = m_total_decoded_samples;
				return true;
			}
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
	struct Stream {
		AjmAt9Handle handle {nullptr, Atrac9ReleaseHandle};
		uint32_t bytes_used = 0;
	};
	std::vector<Stream>  m_streams;
	uint8_t              m_config_data[ATRAC9_CONFIG_DATA_SIZE] {};
	uint32_t             m_superframe_bytes_remain = 0;
	uint32_t             m_num_frames              = 0;
	Atrac9CodecInfo      m_codec_info {};
	std::vector<uint8_t> m_pcm_buffer;
	std::vector<uint8_t> m_stream_pcm;
};

} // namespace Libs::Audio::Ajm
