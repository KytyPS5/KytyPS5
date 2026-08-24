#pragma once

#include "libs/ajm/hevag_core.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace Libs::Audio::Ajm {

struct AjmDecHeVagInitializeParameters {
	uint32_t sample_rate;
	uint32_t channels;
};

struct AjmSidebandDecHeVagHeader {
	uint8_t  id[4];
	uint32_t version;
	uint32_t data_size;
	uint8_t  name[16];
	uint8_t  loop_flags[8];
};

union AjmSidebandDecHeVagStreamInfo {
	AjmSidebandDecHeVagHeader header;
	uint8_t                   headerless_loop_flags[8];
};

struct AjmSidebandDecHeVagCodecInfo {
	uint32_t                      stream_type;
	AjmSidebandDecHeVagStreamInfo stream_info;
};

static_assert(sizeof(AjmDecHeVagInitializeParameters) == 8);
static_assert(sizeof(AjmSidebandDecHeVagCodecInfo) == 40);

class AjmHeVagDecoder final: public AjmDecoder {
public:
	AjmHeVagDecoder(uint32_t channels, uint32_t sample_rate, AjmSampleEncoding encoding,
	                uint64_t flags)
	    : AjmDecoder(std::clamp(channels, 1u, 8u), sample_rate, encoding) {
		(void)flags;
	}

	AjmDecodeResult Initialize(const void* codec_parameters,
	                           size_t      codec_parameters_size) override {
		auto result = MakeResult();
		if (codec_parameters == nullptr ||
		    codec_parameters_size < sizeof(AjmDecHeVagInitializeParameters)) {
			result.result = AJM_RESULT_INVALID_PARAMETER;
			return result;
		}

		const auto* params = static_cast<const AjmDecHeVagInitializeParameters*>(codec_parameters);
		auto        sample_rate = params->sample_rate;
		if (sample_rate > 192000) {
			sample_rate =
			    ((sample_rate & 0x000000ffu) << 24u) | ((sample_rate & 0x0000ff00u) << 8u) |
			    ((sample_rate & 0x00ff0000u) >> 8u) | ((sample_rate & 0xff000000u) >> 24u);
		}
		const auto channels = params->channels == 0 ? m_channels : params->channels;
		if (sample_rate < 2000 || sample_rate > 192000 || channels == 0 || channels > 8) {
			result.result = AJM_RESULT_INVALID_PARAMETER;
			return result;
		}

		SetFormat(channels, sample_rate, m_sample_encoding);
		m_is_initialized = true;
		m_stream_type    = 2;
		m_header         = {};
		Reset();
		return MakeResult();
	}

	void Reset() override {
		m_total_decoded_samples = 0;
		m_loop_flags.fill(0);
		m_channel_state = {};
	}

	AjmDecodeResult Decode(const void* input, size_t input_size, void* output, size_t output_size,
	                       bool multiple_frames, AjmGaplessState* gapless) override {
		auto result = MakeResult();
		if (!m_is_initialized) {
			result.result = AJM_RESULT_NOT_INITIALIZED;
			return result;
		}
		if (input == nullptr || input_size == 0) {
			result.result = AJM_RESULT_PARTIAL_INPUT;
			return result;
		}

		const auto* bytes      = static_cast<const uint8_t*>(input);
		size_t      offset     = ParseHeader(bytes, input_size);
		const auto  frame_size = static_cast<size_t>(16u * m_channels);
		if (input_size - offset < frame_size) {
			result.result         = AJM_RESULT_PARTIAL_INPUT;
			result.input_consumed = offset;
			return result;
		}

		size_t output_offset = 0;
		while (input_size - offset >= frame_size) {
			std::array<std::array<int16_t, 28>, 8> decoded {};
			for (uint32_t channel = 0; channel < m_channels; channel++) {
				m_loop_flags[channel] =
				    HeVagDecodeFrame(bytes + offset + static_cast<size_t>(channel) * 16u,
				                     &m_channel_state[channel], decoded[channel].data());
			}
			uint32_t skip    = 0;
			uint32_t samples = 28;
			if (gapless != nullptr) {
				skip = std::min<uint32_t>(samples, gapless->current.skip_samples);
				samples -= skip;
				if (gapless->HasSampleLimit()) {
					samples = std::min(samples, gapless->current.total_samples);
				}
			}
			const auto bytes_needed =
			    static_cast<size_t>(samples) * m_channels * AjmBytesPerSample(m_sample_encoding);
			if ((bytes_needed != 0 && output == nullptr) || output_offset > output_size ||
			    bytes_needed > output_size - output_offset) {
				result.result = AJM_RESULT_NOT_ENOUGH_ROOM;
				break;
			}
			for (uint32_t sample = 0; sample < samples; sample++) {
				for (uint32_t channel = 0; channel < m_channels; channel++) {
					WriteSample(output, &output_offset, decoded[channel][skip + sample]);
				}
			}
			if (gapless != nullptr) {
				gapless->current.skip_samples =
				    static_cast<uint16_t>(gapless->current.skip_samples - skip);
				gapless->current.skipped_samples = static_cast<uint16_t>(std::min<uint32_t>(
				    std::numeric_limits<uint16_t>::max(),
				    static_cast<uint32_t>(gapless->current.skipped_samples) + skip));
				if (gapless->HasSampleLimit()) {
					gapless->current.total_samples -=
					    std::min(gapless->current.total_samples, samples);
				}
			}
			offset += frame_size;
			m_total_decoded_samples += samples;
			result.frames++;
			if (!multiple_frames) {
				break;
			}
		}

		result.input_consumed        = offset;
		result.output_written        = output_offset;
		result.total_decoded_samples = m_total_decoded_samples;
		result.format                = GetFormat();
		if (result.frames == 0 && result.result == OK) {
			result.result = AJM_RESULT_PARTIAL_INPUT;
		}
		return result;
	}

	void WriteCodecInfo(void* output, size_t output_size,
	                    const AjmDecodeResult& result) const override {
		(void)result;
		if (output == nullptr || output_size < sizeof(AjmSidebandDecHeVagCodecInfo)) {
			return;
		}
		auto* info        = static_cast<AjmSidebandDecHeVagCodecInfo*>(output);
		*info             = {};
		info->stream_type = m_stream_type;
		if (m_stream_type == 1) {
			info->stream_info.header = m_header;
			std::copy(m_loop_flags.begin(), m_loop_flags.end(),
			          info->stream_info.header.loop_flags);
		} else {
			std::copy(m_loop_flags.begin(), m_loop_flags.end(),
			          info->stream_info.headerless_loop_flags);
		}
	}

	[[nodiscard]] size_t CodecInfoSize() const override {
		return sizeof(AjmSidebandDecHeVagCodecInfo);
	}

private:
	static uint32_t ReadBe32(const uint8_t* data) {
		return (static_cast<uint32_t>(data[0]) << 24u) | (static_cast<uint32_t>(data[1]) << 16u) |
		       (static_cast<uint32_t>(data[2]) << 8u) | static_cast<uint32_t>(data[3]);
	}

	size_t ParseHeader(const uint8_t* data, size_t size) {
		if (size < 48 || std::memcmp(data, "VAGp", 4) != 0) {
			return 0;
		}
		m_stream_type = 1;
		std::memcpy(m_header.id, data, 4);
		m_header.version   = ReadBe32(data + 4);
		m_header.data_size = ReadBe32(data + 12);
		std::memcpy(m_header.name, data + 32, sizeof(m_header.name));
		const auto sample_rate = ReadBe32(data + 16);
		if (sample_rate >= 2000 && sample_rate <= 192000 && sample_rate != m_sample_rate) {
			SetFormat(m_channels, sample_rate, m_sample_encoding);
		}
		return 48;
	}

	void WriteSample(void* output, size_t* offset, int16_t sample) const {
		auto* bytes = static_cast<uint8_t*>(output);
		switch (m_sample_encoding) {
			case AjmSampleEncoding::S16:
				std::memcpy(bytes + *offset, &sample, sizeof(sample));
				*offset += sizeof(sample);
				break;
			case AjmSampleEncoding::S32: {
				const auto value = static_cast<int32_t>(sample) * 65536;
				std::memcpy(bytes + *offset, &value, sizeof(value));
				*offset += sizeof(value);
				break;
			}
			case AjmSampleEncoding::Float: {
				const auto value = static_cast<float>(sample) / 32768.0f;
				std::memcpy(bytes + *offset, &value, sizeof(value));
				*offset += sizeof(value);
				break;
			}
			default: break;
		}
	}

	bool                             m_is_initialized = false;
	uint32_t                         m_stream_type    = 2;
	AjmSidebandDecHeVagHeader        m_header {};
	std::array<uint8_t, 8>           m_loop_flags {};
	std::array<HeVagChannelState, 8> m_channel_state {};
};

} // namespace Libs::Audio::Ajm
