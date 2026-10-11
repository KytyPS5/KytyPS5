// Approximate conversion of DualSense-style haptic audio into classic two-motor rumble for
// non-Sony gamepads. Pure math (no SDL, no locks) so it can be unit tested.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Libs::Controller::HapticsRumble {

// Which AudioOut port carries the haptic signal. The VIBRATION port (type 10) is dedicated to
// haptics (stereo, real signal measured in Astro Bot). PADSPK (type 4) is a mono pad-speaker
// stream that may carry speaker audio, so it is not used: feeding speech/music into the motors
// would buzz constantly.
constexpr int HAPTICS_RUMBLE_PORT_TYPE = 10;

// Band split: the low band drives the large (low-frequency) motor, the remainder the small one.
constexpr float SPLIT_FREQUENCY_HZ = 160.0f;
// Band peaks below this are silence (the game's noise floor).
constexpr float NOISE_GATE = 0.004f;
// Input amplitude (per band peak) that saturates the motor. Observed content peaks at 0.05-0.1
// overall, so a 0.3 reference with a square-root curve gives ~0.32 motor at 0.03, ~0.5 at 0.08.
constexpr float FULL_SCALE_AMPLITUDE = 0.3f;
constexpr float CURVE_EXPONENT       = 0.5f;
// Smoothing time constants: fast attack, slower decay so short taps do not buzz or chatter.
constexpr float ATTACK_TAU_MS = 6.0f;
constexpr float DECAY_TAU_MS  = 70.0f;

// Smoothed level below which a silent input snaps to exactly zero.
constexpr float OUTPUT_FLOOR = 0.02f;

struct Levels {
	float low  = 0.0f;
	float high = 0.0f;
};

// Compressive gain: amplitude -> 0..1 motor level, with the noise gate applied.
inline float Curve(float peak) {
	if (!(peak > NOISE_GATE)) {
		return 0.0f;
	}
	return std::min(1.0f, std::pow(peak / FULL_SCALE_AMPLITUDE, CURVE_EXPONENT));
}

class Envelope {
public:
	void Reset() { *this = Envelope {}; }

	// Process one buffer of interleaved float samples. Returns smoothed motor levels in 0..1.
	Levels Process(const float* data, uint32_t frames, uint32_t channels, uint32_t freq) {
		if (data == nullptr || frames == 0 || channels == 0 || freq == 0) {
			return m_out;
		}
		const float a = 1.0f - std::exp(-6.2831853f * SPLIT_FREQUENCY_HZ / static_cast<float>(freq));
		const uint32_t mix = std::min(channels, 2u);
		float peak_low = 0.0f, peak_high = 0.0f;
		for (uint32_t i = 0; i < frames; i++) {
			float x = 0.0f;
			for (uint32_t c = 0; c < mix; c++) {
				x += data[static_cast<size_t>(i) * channels + c];
			}
			x /= static_cast<float>(mix);
			m_lp += a * (x - m_lp);
			peak_low  = std::max(peak_low, std::fabs(m_lp));
			peak_high = std::max(peak_high, std::fabs(x - m_lp));
		}
		const float dt_ms = 1000.0f * static_cast<float>(frames) / static_cast<float>(freq);
		m_out.low         = Smooth(m_out.low, Curve(peak_low), dt_ms);
		m_out.high        = Smooth(m_out.high, Curve(peak_high), dt_ms);
		return m_out;
	}

	[[nodiscard]] Levels Current() const { return m_out; }

private:
	static float Smooth(float current, float target, float dt_ms) {
		const float tau = target > current ? ATTACK_TAU_MS : DECAY_TAU_MS;
		const float v   = target + (current - target) * std::exp(-dt_ms / tau);
		return (v < OUTPUT_FLOOR && target == 0.0f) ? 0.0f : v;
	}

	float  m_lp = 0.0f;
	Levels m_out;
};

inline uint16_t ToMotor16(float level, float scale) {
	return static_cast<uint16_t>(std::lround(std::clamp(level * scale, 0.0f, 1.0f) * 65535.0f));
}

} // namespace Libs::Controller::HapticsRumble
