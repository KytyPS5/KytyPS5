#pragma once

#include "common/assert.h"
#include "common/logging/log.h"
#include "libs/ajm/decoder.h"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

extern "C" {
#include "libatrac9.h"
}

namespace Libs::Audio::Ajm {

inline int AjmAt9InitDecoder(void* handle, const uint8_t* config_data) {
	// Keep the guest configuration unchanged for metadata and decoder resets.
	uint8_t config[ATRAC9_CONFIG_DATA_SIZE];
	std::memcpy(config, config_data, sizeof(config));
	const auto channel_config = (config[1] >> 1u) & 7u;
	if (channel_config >= 6u) {
		// LibAtrac9 lacks vibration layouts; their blocks match ordinary mono/dual-mono.
		// Translate only the channel field, preserving the other configuration bits.
		config[1] -= 6u << 1u;
	}
	return Atrac9InitDecoder(handle, config);
}

inline bool AjmAt9ResolveConfig(const uint8_t* raw, uint8_t* decoder_config,
                               uint32_t* mono_channels) {
	std::memcpy(decoder_config, raw, ATRAC9_CONFIG_DATA_SIZE);
	*mono_channels = 0;
	if (raw[0] != 0x30) {
		return true;
	}
	// Extended ATRAC9 multiplexes independent mono substreams in frame order.
	// Interoperability reference: PS5PCEM baa718235a37d310d91ac001f4d92cdc8099a69c,
	// src/hle/ajm_codec.zig. Keep ordinary FE configurations and haptics separate.
	const uint32_t channels = 2u * ((((raw[1] & 15u) << 1u) | (raw[2] >> 7u)) + 1u);
	if ((raw[2] & 0x7fu) != 0x40u || channels > 36u) {
		return false;
	}
	const uint32_t word = (0xfeu << 24u) | (static_cast<uint32_t>(raw[1] >> 4u) << 20u) |
	                      (static_cast<uint32_t>(raw[3] >> 2u) << 5u) |
	                      (static_cast<uint32_t>(raw[3] & 3u) << 3u);
	for (uint32_t byte = 0; byte < ATRAC9_CONFIG_DATA_SIZE; ++byte) {
		decoder_config[byte] = static_cast<uint8_t>(word >> (24u - byte * 8u));
	}
	*mono_channels = channels;
	return true;
}

inline int AjmAt9InitConfiguration(void* handle, const uint8_t* raw, Atrac9CodecInfo* info,
                                  uint32_t* mono_channels) {
	uint8_t config[ATRAC9_CONFIG_DATA_SIZE];
	if (!AjmAt9ResolveConfig(raw, config, mono_channels)) {
		return -1;
	}
	const int initialized = AjmAt9InitDecoder(handle, config);
	if (initialized != 0) {
		return initialized;
	}
	const int status = Atrac9GetCodecInfo(handle, info);
	if (status == 0 && *mono_channels != 0) {
		if (info->channels != 1 || info->superframeSize <= 0) {
			return -1;
		}
		info->channels = static_cast<int>(*mono_channels);
		info->superframeSize *= static_cast<int>(*mono_channels);
	}
	return status;
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
		m_substream_handles.clear();
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
			const int ret        = DecodeFrame(input_bytes + input_offset, remaining_input, &bytes_used);
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

		m_substream_handles.clear();
		m_substream_used.clear();
		uint32_t mono_channels = 0;
		const int init_ret = AjmAt9InitConfiguration(m_handle, m_config_data, &m_codec_info,
		                                            &mono_channels);
		if (init_ret != 0) {
			m_is_initialized        = false;
			result->result          = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
			result->internal_result = init_ret;
			return false;
		}

		if (m_codec_info.channels <= 0 || m_codec_info.frameSamples <= 0 ||
		    m_codec_info.superframeSize <= 0 || m_codec_info.framesInSuperframe <= 0) {
			m_is_initialized        = false;
			result->result          = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
			result->internal_result = -1;
			return false;
		}
		if (mono_channels != 0) {
			uint8_t mono_config[ATRAC9_CONFIG_DATA_SIZE];
			uint32_t channels = 0;
			(void)AjmAt9ResolveConfig(m_config_data, mono_config, &channels);
			for (uint32_t channel = 1; channel < mono_channels; ++channel) {
				m_substream_handles.emplace_back(Atrac9GetHandle(), Atrac9ReleaseHandle);
				if (!m_substream_handles.back() ||
				    AjmAt9InitDecoder(m_substream_handles.back().get(), mono_config) != 0) {
					m_is_initialized = false;
					result->result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
					return false;
				}
			}
			m_substream_used.assign(mono_channels, 0);
			m_substream_size = static_cast<uint32_t>(m_codec_info.superframeSize) / mono_channels;
			m_mono_pcm_buffer.resize(static_cast<size_t>(m_codec_info.frameSamples) *
			                         AjmBytesPerSample(m_sample_encoding));
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

	int DecodeHandle(void* handle, const uint8_t* input, uint8_t* pcm, int* bytes_used) {
		switch (m_sample_encoding) {
			case AjmSampleEncoding::S16:
				return Atrac9Decode(handle, input, reinterpret_cast<int16_t*>(pcm), bytes_used, 0);
			case AjmSampleEncoding::S32:
				return Atrac9DecodeS32(handle, input, reinterpret_cast<int32_t*>(pcm), bytes_used, 0);
			case AjmSampleEncoding::Float:
				return Atrac9DecodeF32(handle, input, reinterpret_cast<float*>(pcm), bytes_used, 0);
			default:
				EXIT("unsupported AJM PCM sample encoding %u\n",
				     static_cast<uint32_t>(m_sample_encoding));
		}
		return -1;
	}

	int DecodeFrame(const uint8_t* input, size_t input_size, int* bytes_used) {
		if (m_substream_used.empty()) {
			return DecodeHandle(m_handle, input, m_pcm_buffer.data(), bytes_used);
		}
		const size_t channels = m_substream_used.size();
		const size_t sample_bytes = AjmBytesPerSample(m_sample_encoding);
		const bool last = m_num_frames + 1u == static_cast<uint32_t>(m_codec_info.framesInSuperframe);
		size_t offset = 0;
		for (size_t channel = 0; channel < channels; ++channel) {
			if (m_substream_used[channel] >= m_substream_size) {
				return -1;
			}
			const auto remaining = m_substream_size - m_substream_used[channel];
			if (offset > input_size || remaining > input_size - offset) {
				return -1;
			}
			void* handle = channel == 0 ? m_handle : m_substream_handles[channel - 1].get();
			int used = 0;
			const int status = DecodeHandle(handle, input + offset, m_mono_pcm_buffer.data(), &used);
			if (status != 0 || used <= 0 || static_cast<uint32_t>(used) > remaining) {
				return status != 0 ? status : -1;
			}
			offset += last ? remaining : static_cast<uint32_t>(used);
			m_substream_used[channel] += static_cast<uint32_t>(used);
			for (size_t sample = 0; sample < static_cast<size_t>(m_codec_info.frameSamples); ++sample) {
				std::memcpy(m_pcm_buffer.data() + (sample * channels + channel) * sample_bytes,
				            m_mono_pcm_buffer.data() + sample * sample_bytes, sample_bytes);
			}
		}
		*bytes_used = static_cast<int>(offset);
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
		std::fill(m_substream_used.begin(), m_substream_used.end(), 0);
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
			const int ret        = DecodeFrame(input + *input_offset, remaining_input, &bytes_used);
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
	void*                m_handle            = nullptr;
	uint8_t              m_config_data[ATRAC9_CONFIG_DATA_SIZE] {};
	uint32_t             m_superframe_bytes_remain = 0;
	uint32_t             m_num_frames              = 0;
	Atrac9CodecInfo      m_codec_info {};
	std::vector<uint8_t> m_pcm_buffer;
	std::vector<std::unique_ptr<void, decltype(&Atrac9ReleaseHandle)>> m_substream_handles;
	std::vector<uint32_t> m_substream_used;
	uint32_t m_substream_size = 0;
	std::vector<uint8_t> m_mono_pcm_buffer;
};

} // namespace Libs::Audio::Ajm
