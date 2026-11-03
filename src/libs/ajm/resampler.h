#pragma once

#include "libs/ajm/decoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace Libs::Audio::Ajm {

constexpr uint64_t AJM_INSTANCE_FLAG_GAPLESS_LOOP           = 1ull << 10u;
constexpr uint64_t AJM_INSTANCE_FLAG_RESAMPLE               = 1ull << 12u;
constexpr uint64_t AJM_INSTANCE_FLAG_SEAMLESS_LOOP          = 1ull << 13u;
constexpr uint32_t AJM_RESAMPLE_FLAG_RESET                  = 1u;
constexpr uint32_t AJM_RESAMPLE_FLAG_END                    = 2u;
constexpr uint32_t AJM_RESAMPLE_FLAG_IGNORE_RATIO_PARAMETER = 8u;

class AjmResampler {
public:
	int32_t SetParameters(float ratio, float change, uint32_t flags) {
		if ((flags & AJM_RESAMPLE_FLAG_IGNORE_RATIO_PARAMETER) == 0 &&
		    (!std::isfinite(ratio) || ratio < 0 || ratio > 4 || !std::isfinite(change))) {
			return AJM_RESULT_INVALID_PARAMETER;
		}
		if ((flags & AJM_RESAMPLE_FLAG_RESET) != 0) {
			m_pcm.clear();
			m_position = 0;
			m_end      = false;
		}
		if ((flags & AJM_RESAMPLE_FLAG_IGNORE_RATIO_PARAMETER) == 0) {
			m_ratio  = ratio;
			m_change = std::abs(change) < 0.000001f ? 0 : change;
		}
		m_end |= (flags & AJM_RESAMPLE_FLAG_END) != 0;
		return OK;
	}

	[[nodiscard]] AjmSidebandResampleInfo GetInfo() const {
		AjmSidebandResampleInfo info;
		info.ratio       = static_cast<float>(m_ratio);
		info.num_samples = m_end && m_position >= Samples()
		                       ? -1
		                       : static_cast<int32_t>(std::ceil(Samples() - m_position));
		return info;
	}

	AjmDecodeResult Decode(AjmDecoder& decoder, uint64_t flags, const void* input,
	                       size_t input_size, void* output, size_t output_size,
	                       bool multiple_frames, AjmGaplessState* gapless) {
		auto result = decoder.MakeResult();
		if (result.format.channel_num == 0) {
			return decoder.Decode(input, input_size, output, output_size, multiple_frames, gapless);
		}
		bool stop_decoding = false;
		for (;;) {
			const auto format       = decoder.GetFormat();
			const auto encoding     = static_cast<AjmSampleEncoding>(format.sample_encoding);
			const auto sample_bytes = format.channel_num * AjmBytesPerSample(encoding);
			if (!m_pcm.empty() && sample_bytes != m_sample_bytes) {
				result.result = AJM_RESULT_INVALID_PARAMETER;
				break;
			}
			if (output == nullptr || output_size - result.output_written < sample_bytes) {
				if (m_position < Samples() || result.input_consumed < input_size) {
					result.result |= AJM_RESULT_NOT_ENOUGH_ROOM;
				}
				break;
			}

			const auto index     = static_cast<size_t>(m_position);
			const auto fraction  = m_position - static_cast<double>(index);
			const bool need_next = fraction != 0 && index + 1 >= Samples();
			if (index >= Samples() || need_next) {
				Compact();
				if (!stop_decoding && input != nullptr && result.input_consumed < input_size &&
				    (gapless == nullptr || !gapless->IsEnd())) {
					// A decode job can produce up to six 20 ms Opus frames in one packet.
					const auto channels = std::max<uint32_t>(format.channel_num, flags & 0x7fu);
					m_frame.resize(5760 * channels * AjmBytesPerSample(encoding));
					auto decoded =
					    decoder.Decode(static_cast<const uint8_t*>(input) + result.input_consumed,
						               input_size - result.input_consumed, m_frame.data(),
						               m_frame.size(), false, gapless);
					result.input_consumed += decoded.input_consumed;
					result.frames += decoded.frames;
					result.frames_per_packet     = decoded.frames_per_packet;
					result.total_decoded_samples = decoded.total_decoded_samples;
					result.format                = decoded.format;
					result.result |= decoded.result;
					result.internal_result = decoded.internal_result;
					m_sample_bytes = decoded.format.channel_num * AjmBytesPerSample(encoding);
					m_pcm.insert(m_pcm.end(), m_frame.begin(),
					             m_frame.begin() + decoded.output_written);
					stop_decoding =
					    !multiple_frames || decoded.result != OK || decoded.input_consumed == 0;
					if (gapless != nullptr && gapless->IsEnd() &&
					    (flags & AJM_INSTANCE_FLAG_GAPLESS_LOOP) != 0) {
						gapless->Reset();
						stop_decoding |= (flags & AJM_INSTANCE_FLAG_SEAMLESS_LOOP) == 0;
					}
					continue;
				}
				if (!m_end || m_position >= Samples()) {
					if (!m_end && !stop_decoding && (gapless == nullptr || !gapless->IsEnd())) {
						result.result |= AJM_RESULT_PARTIAL_INPUT;
					}
					break;
				}
			}

			const auto  current = static_cast<size_t>(m_position);
			const auto  next    = std::min(current + 1, Samples() - 1);
			auto*       dst     = static_cast<uint8_t*>(output) + result.output_written;
			const auto* a       = m_pcm.data() + current * sample_bytes;
			const auto* b       = m_pcm.data() + next * sample_bytes;
			const auto  weight  = m_position - static_cast<double>(current);
			if (weight == 0) {
				std::memcpy(dst, a, sample_bytes);
			} else {
				switch (encoding) {
					case AjmSampleEncoding::S16:
						Interpolate<int16_t>(dst, a, b, format.channel_num, weight);
						break;
					case AjmSampleEncoding::S32:
						Interpolate<int32_t>(dst, a, b, format.channel_num, weight);
						break;
					case AjmSampleEncoding::Float:
						Interpolate<float>(dst, a, b, format.channel_num, weight);
						break;
				}
			}
			result.output_written += sample_bytes;
			m_position += m_ratio;
			m_ratio = std::clamp(m_ratio + m_change, 0.0, 4.0);
			if (m_ratio == 0 || m_ratio == 4) {
				m_change = 0;
			}
		}
		Compact();
		result.resample_info = GetInfo();
		return result;
	}

private:
	template <typename T>
	static void Interpolate(uint8_t* dst, const uint8_t* a, const uint8_t* b, uint32_t channels,
	                        double weight) {
		for (uint32_t channel = 0; channel < channels; ++channel) {
			T first, second;
			std::memcpy(&first, a + channel * sizeof(T), sizeof(T));
			std::memcpy(&second, b + channel * sizeof(T), sizeof(T));
			const auto value =
			    static_cast<T>(first + (static_cast<double>(second) - first) * weight);
			std::memcpy(dst + channel * sizeof(T), &value, sizeof(T));
		}
	}

	[[nodiscard]] size_t Samples() const {
		return m_sample_bytes == 0 ? 0 : m_pcm.size() / m_sample_bytes;
	}

	void Compact() {
		const auto consumed = std::min(static_cast<size_t>(m_position), Samples());
		const auto bytes    = consumed * m_sample_bytes;
		if (bytes != 0) {
			std::memmove(m_pcm.data(), m_pcm.data() + bytes, m_pcm.size() - bytes);
			m_pcm.resize(m_pcm.size() - bytes);
			m_position -= static_cast<double>(consumed);
		}
	}

	std::vector<uint8_t> m_pcm;
	std::vector<uint8_t> m_frame;
	size_t               m_sample_bytes = 0;
	double               m_position     = 0;
	double               m_ratio        = 1;
	double               m_change       = 0;
	bool                 m_end          = false;
};

} // namespace Libs::Audio::Ajm
