#pragma once

#include "common/assert.h"
#include "libs/errno.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace Libs::Audio::Ajm {

struct AjmSidebandFormat {
	uint32_t channel_num;
	uint32_t channel_mask;
	uint32_t sampling_frequency;
	uint32_t sample_encoding;
	uint32_t bitrate;
	uint32_t reserved;
};

struct AjmSidebandGaplessDecode {
	uint32_t total_samples;
	uint16_t skip_samples;
	uint16_t skipped_samples;
};

struct AjmGaplessState {
	AjmSidebandGaplessDecode init {};
	AjmSidebandGaplessDecode current {};

	[[nodiscard]] bool HasSampleLimit() const {
		return init.total_samples != 0 &&
		       init.total_samples != std::numeric_limits<uint32_t>::max();
	}

	[[nodiscard]] bool IsEnd() const { return HasSampleLimit() && current.total_samples == 0; }

	void Reset() {
		current                 = init;
		current.skipped_samples = 0;
	}

	void Set(const AjmSidebandGaplessDecode& params, bool reset) {
		init.total_samples = params.total_samples;
		init.skip_samples  = params.skip_samples;
		if (reset || (current.total_samples == 0 && current.skip_samples == 0 &&
		              current.skipped_samples == 0)) {
			Reset();
		}
	}
};

enum class AjmSampleEncoding : uint32_t {
	S16   = 0,
	S32   = 1,
	Float = 2,
};

struct AjmDecodeResult {
	int32_t           result                = OK;
	int32_t           internal_result       = OK;
	size_t            input_consumed        = 0;
	size_t            output_written        = 0;
	uint64_t          total_decoded_samples = 0;
	uint32_t          frames                = 0;
	uint32_t          frames_per_packet     = 0;
	AjmSidebandFormat format {};
};

constexpr int32_t AJM_RESULT_NOT_INITIALIZED    = 0x00000001;
constexpr int32_t AJM_RESULT_INVALID_DATA       = 0x00000002;
constexpr int32_t AJM_RESULT_INVALID_PARAMETER  = 0x00000004;
constexpr int32_t AJM_RESULT_PARTIAL_INPUT      = 0x00000008;
constexpr int32_t AJM_RESULT_NOT_ENOUGH_ROOM    = 0x00000010;
constexpr int32_t AJM_RESULT_CODEC_ERROR        = 0x40000000;
constexpr int32_t AJM_RESULT_FATAL              = static_cast<int32_t>(0x80000000u);

constexpr uint64_t AJM_INSTANCE_FLAG_CODEC_OFFSET     = 32u;

inline size_t AjmBytesPerSample(AjmSampleEncoding encoding) {
	switch (encoding) {
		case AjmSampleEncoding::S16: return sizeof(int16_t);
		case AjmSampleEncoding::S32: return sizeof(int32_t);
		case AjmSampleEncoding::Float: return sizeof(float);
		default: EXIT("unsupported AJM PCM sample encoding %u\n", static_cast<uint32_t>(encoding));
	}
	return sizeof(int16_t);
}

inline uint32_t AjmChannelMask(uint32_t channels) {
	switch (channels) {
		case 1: return 0x4;
		case 2: return 0x3;
		case 3: return 0x7;
		case 4: return 0x33;
		case 5: return 0x607;
		case 6: return 0x60f;
		case 7: return 0x70f;
		case 8: return 0x63f;
		default: return 0;
	}
}

inline AjmSidebandFormat AjmMakeFormat(uint32_t channels, uint32_t sample_rate,
                                       AjmSampleEncoding encoding) {
	AjmSidebandFormat format {};
	format.channel_num        = channels;
	format.channel_mask       = AjmChannelMask(channels);
	format.sampling_frequency = sample_rate;
	format.sample_encoding    = static_cast<uint32_t>(encoding);
	format.bitrate            = 0;
	return format;
}

class AjmDecoder {
public:
	AjmDecoder(uint32_t channels, uint32_t sample_rate, AjmSampleEncoding encoding)
	    : m_channels(channels), m_sample_rate(sample_rate), m_sample_encoding(encoding) {}
	virtual ~AjmDecoder() = default;

	KYTY_CLASS_NO_COPY(AjmDecoder);

	virtual AjmDecodeResult Initialize(const void* codec_parameters, size_t codec_parameters_size) {
		(void)codec_parameters;
		(void)codec_parameters_size;
		return MakeResult();
	}

	virtual void Reset() { m_total_decoded_samples = 0; }

	virtual AjmDecodeResult Decode(const void* input, size_t input_size, void* output,
	                               size_t output_size, bool multiple_frames,
	                               AjmGaplessState* gapless) = 0;

	virtual void WriteCodecInfo(void* output, size_t output_size,
	                            const AjmDecodeResult& result) const {
		(void)output;
		(void)output_size;
		(void)result;
	}

	[[nodiscard]] virtual size_t CodecInfoSize() const { return 0; }

	[[nodiscard]] virtual AjmSidebandFormat GetFormat() const {
		return AjmMakeFormat(m_channels, m_sample_rate, m_sample_encoding);
	}

	[[nodiscard]] AjmDecodeResult MakeResult() const {
		AjmDecodeResult result {};
		result.total_decoded_samples = m_total_decoded_samples;
		result.format                = GetFormat();
		return result;
	}

protected:
	void SetFormat(uint32_t channels, uint32_t sample_rate, AjmSampleEncoding encoding) {
		m_channels        = channels;
		m_sample_rate     = sample_rate;
		m_sample_encoding = encoding;
	}

	uint32_t          m_channels              = 2;
	uint32_t          m_sample_rate           = 48000;
	AjmSampleEncoding m_sample_encoding       = AjmSampleEncoding::S16;
	uint64_t          m_total_decoded_samples = 0;
};

} // namespace Libs::Audio::Ajm
