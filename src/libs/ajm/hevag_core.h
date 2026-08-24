#pragma once

#include "libs/ajm/hevag_coefficients.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>

namespace Libs::Audio::Ajm {

struct HeVagChannelState {
	std::array<int32_t, 4> history {};
};

inline uint8_t HeVagDecodeFrame(const uint8_t* frame, HeVagChannelState* state, int16_t* output) {
	auto       predictor = static_cast<uint32_t>((frame[0] >> 4u) | (frame[1] & 0xf0u));
	const auto shift     = static_cast<uint32_t>(frame[0] & 0x0fu);
	const auto flag      = static_cast<uint8_t>(frame[1] & 0x0fu);
	if (predictor >= std::size(HevagCoefficients)) {
		predictor = 0;
	}

	for (uint32_t sample = 0; sample < 28; sample++) {
		int32_t value = 0;
		if (flag < 7) {
			const auto packed = frame[2u + sample / 2u];
			int32_t    nibble =
			    static_cast<int32_t>((packed >> ((sample & 1u) != 0 ? 4u : 0u)) & 0x0fu);
			if (nibble >= 8) {
				nibble -= 16;
			}
			const auto  code         = (nibble * 4096) >> std::min(shift, 15u);
			const auto& coefficients = HevagCoefficients[predictor];
			value = code + static_cast<int32_t>(state->history[0] * coefficients[0] +
			                                    state->history[1] * coefficients[1] +
			                                    state->history[2] * coefficients[2] +
			                                    state->history[3] * coefficients[3]);
		}
		output[sample]    = static_cast<int16_t>(std::clamp(value, -32768, 32767));
		state->history[3] = state->history[2];
		state->history[2] = state->history[1];
		state->history[1] = state->history[0];
		state->history[0] = value;
	}
	return flag;
}

} // namespace Libs::Audio::Ajm
