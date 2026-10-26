#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

namespace Libs::Audio::Ngs2 {

// I3DL2 reverb settings (Ngs2ReverbI3DL2Param), used by the custom reverb module (0x1b). Levels
// are in millibels, which are hundredths of a dB, so -1000 is -10 dB and -10000 is -100 dB.
// Configure clamps each value to the range given in its comment, as the library does.
struct Ngs2ReverbI3dl2Param {
	// Linear gain of the reverb in the module's output, 0 to 32. 0 removes the reverb.
	float wet;
	// Linear gain of the original signal in the module's output, 0 to 32. 1 passes it through
	// unchanged and 0 leaves only the reverb.
	float dry;
	// Master level of the whole reverb, early reflections and late reverb together, from -10000
	// to 0. 0 is full level, and values above 0 count as 0.
	int32_t room;
	// How much the reverb's high frequencies are cut, as the level at hf_reference, from -10000
	// to 0. 0 leaves the highs alone, and lower values make the room sound duller, like one
	// with soft furnishings.
	int32_t room_hf;
	// Which of the eight early reflection patterns to use (0 to 7). A pattern sets the timing
	// and polarity of the first four echoes. 8 to 15 use the same patterns but let them run
	// past the start of the late reverb. Other values select pattern 0.
	uint32_t reflection_pattern;
	// Seconds for the late reverb to fade by 60 dB, 0.1 to 20. Bigger rooms with harder walls
	// ring for longer.
	float decay_time;
	// How long the high frequencies ring compared with the lows, 0.01 to 2. 1 makes every
	// frequency fade at the same rate, and lower values make the highs die away sooner, as
	// they do in most real rooms.
	float decay_hf_ratio;
	// Level of the early reflections relative to room, from -10000 to 1000 (+10 dB).
	int32_t reflections;
	// Seconds from the dry sound to the first reflection, 0 to 0.3. A longer delay suggests a
	// bigger space.
	float reflections_delay;
	// Level of the late reverb relative to room, from -10000 to 2000 (+20 dB).
	int32_t reverb;
	// Seconds from the first reflection to the start of the late reverb, 0 to 0.1.
	float reverb_delay;
	// How smoothly the late reverb's echoes blend, 0 to 100 percent. Low values leave
	// separate echoes audible as a flutter, and 100 blends them into a smooth wash.
	float diffusion;
	// How closely packed the late reverb's resonances are, 0 to 100 percent. Lower values
	// shorten the comb filters, which makes the tail sound more hollow and metallic.
	float density;
	// The frequency in Hz where room_hf is measured, 20 to 20000.
	float hf_reference;
	// Unused.
	uint32_t reserved[8];
};
static_assert(sizeof(Ngs2ReverbI3dl2Param) == 88);

// The NGS2 I3DL2 reverb (custom module 0x1b), ported from AnyPS5 (GPL-2.0) commit
// e15f1644e384ab055f721347a4d31cdff02e9a47 (core/libs/prx/libSceNgs2.native/src/Reverb.cpp).
// That code follows the PS4 libSceNgs2 (firmware 11.00) and was checked against its output.
//
// A reverb makes a sound seem to be in a room. Right after a sound, a listener hears a few
// distinct echoes off nearby walls, called the early reflections. These quickly blur into a
// dense wash that fades away, called the late reverb. I3DL2 is a standard set of settings that
// describes such a room, and Ngs2ReverbI3dl2Param holds them.
//
// NGS2 renders audio in grains, fixed blocks of samples per channel (num_grain_samples,
// usually 256, about 5 ms at 48 kHz). Process handles one grain per call, and each grain goes
// through these steps.
//  1. Every channel except the LFE is mixed down into one mono signal.
//  2. The recent history of that mono signal is read at four delays to make the early
//     reflections, and at one more delay to feed the late reverb.
//  3. The late feed drives sixteen comb filters. A comb is a delay line that feeds its output
//     back into itself a little quieter each time, so one sound becomes a train of fading
//     echoes. Sixteen different lengths blend into a dense tail.
//  4. Each output channel sums its own selection of combs and smears them further with four
//     all-pass filters. It then adds the early reflections, cuts the highs with the roomHF
//     filter, and mixes the result with the dry input.
// When the settings change, the output crossfades from the old settings to the new ones over
// one grain.
//
// The arithmetic is kept in float and in the library's order, so the output matches the
// library to within rounding. A few odd-looking expressions are kept exactly for that reason.
class Ngs2Reverb {
public:
	// Takes new settings, which apply from the next grain.
	void Configure(const Ngs2ReverbI3dl2Param& param, uint32_t rate) {
		// The library ignores the whole update when any float setting is NaN.
		const float values[] = {param.wet,
		                        param.dry,
		                        param.decay_time,
		                        param.decay_hf_ratio,
		                        param.reflections_delay,
		                        param.reverb_delay,
		                        param.diffusion,
		                        param.density,
		                        param.hf_reference};
		if (std::ranges::any_of(values, [](float value) { return std::isnan(value); })) {
			return;
		}
		// These are the library's limits, which mostly follow the I3DL2 ranges. The millibel
		// levels are clamped later, where they are used.
		m_params                    = param;
		m_params.wet                = std::clamp(param.wet, 0.0f, 32.0f);
		m_params.dry                = std::clamp(param.dry, 0.0f, 32.0f);
		m_params.reflection_pattern = param.reflection_pattern < 16 ? param.reflection_pattern : 0;
		m_params.decay_time         = std::clamp(param.decay_time, 0.1f, 20.0f);
		m_params.decay_hf_ratio     = std::clamp(param.decay_hf_ratio, 0.01f, 2.0f);
		m_params.reflections_delay  = std::clamp(param.reflections_delay, 0.0f, 0.3f);
		m_params.reverb_delay       = std::clamp(param.reverb_delay, 0.0f, 0.1f);
		m_params.diffusion          = std::clamp(param.diffusion, 0.0f, 100.0f);
		m_params.density            = std::clamp(param.density, 0.0f, 100.0f);
		m_params.hf_reference       = std::clamp(param.hf_reference, 20.0f, 20000.0f);
		// The delay lines are sized for the sample rate, so a new rate rebuilds them.
		if (m_rate != rate) {
			m_rate  = rate;
			m_reset = true;
		}
		m_changed    = true;
		m_configured = true;
	}

	// Whether the reverb must run even on silent input. That is while its tail is still
	// sounding, or while new settings are pending, since the library starts their crossfade on
	// the next grain whether or not there is sound.
	[[nodiscard]] bool HasTail() const {
		return m_tail_remaining != 0 || (m_configured && (m_reset || m_changed));
	}

	// Applies the reverb in place to `samples`, which holds `channels` blocks of `grain`
	// samples, one block per channel.
	void Process(float* samples, uint32_t channels, uint32_t grain) {
		if (!m_configured || channels == 0 || channels > MaxChannels) {
			return;
		}
		const size_t total = size_t(channels) * grain;
		const bool   silent_input =
		    std::all_of(samples, samples + total, [](float sample) { return sample == 0.0f; });
		if (!silent_input) {
			// Any sound restarts the countdown to the end of the tail. The combs fade 60 dB
			// per decay time, so the tail is 120 dB down after two decay times (2000 ms per
			// second of decay). The extra 400 ms covers the longest reflections delay (0.3 s)
			// plus the longest reverb delay (0.1 s).
			m_tail_remaining = Samples(2000.0f * m_params.decay_time + 400.0f, float(m_rate));
		} else if (m_tail_remaining == 0 && !m_reset && !m_changed) {
			// Silent input and no tail left means silent output. Pending settings skip this,
			// so their crossfade starts on the same grain as the library's would.
			std::fill(samples, samples + total, 0.0f);
			return;
		}
		const float rate  = static_cast<float>(m_rate);
		const bool  reset = m_reset;
		if (reset) {
			Reset();
			m_changed = true;
		}
		// Keep a copy of the dry input, since the output is written over `samples`.
		m_input.assign(samples, samples + total);
		m_mono.resize(grain);
		m_early.resize(grain);
		m_late.resize(grain);
		MixDown(m_input.data(), channels, grain, m_mono.data());
		RenderTaps(grain, rate);
		// Add this grain to the history, so later grains can reach back into it.
		for (uint32_t i = 0; i < grain; i++) {
			m_ring[m_ring_cursor] = m_mono[i];
			m_ring_cursor         = m_ring_cursor + 1 == m_ring.size() ? 0 : m_ring_cursor + 1;
		}
		m_combs_out.resize(size_t(Combs) * grain);
		for (uint32_t i = 0; i < Combs; i++) {
			RunComb(m_combs[i], m_late.data(), m_combs_out.data() + size_t(i) * grain, grain);
		}
		UpdateRoomFilter(rate);
		m_tail.resize(grain);
		for (uint32_t channel = 0; channel < channels; channel++) {
			auto&        state = m_channels[channel];
			float*       out   = samples + size_t(channel) * grain;
			const float* in    = m_input.data() + size_t(channel) * grain;
			// The LFE channel gets no reverb, only its own dry signal.
			if (channel == LfeChannel) {
				std::copy(in, in + grain, out);
			} else {
				RenderWet(state, channel, out, grain);
			}
			// Scale the reverb by wet and add the dry input scaled by dry. Each level moves
			// from where the last grain left it to the new setting over this grain. Right
			// after a reset there is no last grain, so they start at the new settings.
			const float wet = channel == LfeChannel ? 0.0f : m_params.wet;
			Ramp(out, reset ? wet : state.wet, wet, grain, out, false);
			Ramp(out, reset ? m_params.dry : state.dry, m_params.dry, grain, in, true);
			state.wet = wet;
			state.dry = m_params.dry;
		}
		m_reset   = false;
		m_changed = false;
		if (m_tail_remaining != 0) {
			m_tail_remaining = m_tail_remaining > grain ? m_tail_remaining - grain : 0;
			if (m_tail_remaining == 0) {
				// The tail is now 120 dB down. Drop the tiny leftovers so the next sound
				// starts from true silence.
				ClearHistory();
			}
		}
	}

private:
	static constexpr uint32_t MaxChannels = 8;
	// The history holds 300 ms at 48 kHz, enough for the longest reflections delay.
	static constexpr uint32_t RingSamples = 14400;
	static constexpr uint32_t EarlyTaps   = 4;
	static constexpr uint32_t Combs       = 16;
	static constexpr uint32_t Allpasses   = 4;
	static constexpr uint32_t LfeChannel  = 3;

	// The reflection patterns. Each has four tap times in ms after the reflections delay, and
	// a gain for each tap. A negative gain flips the echo's polarity.
	static constexpr float TapTimes[8][EarlyTaps] = {{0, 11, 23, 35}, {0, 14.75f, 26.55f, 38.03f},
	                                                 {0, 6, 17, 29},  {0, 8, 20, 53},
	                                                 {0, 11, 17, 30}, {0, 16, 30, 55},
	                                                 {0, 20, 29, 40}, {0, 37, 47, 62}};
	static constexpr float TapGains[8][EarlyTaps] = {
	    {0.7f, 0.4f, 0.134f, 0.1f}, {0.7f, 0.4f, 0.3f, 0.1f},   {0.7f, 0.45f, -0.6f, 0.2f},
	    {0.7f, 0.55f, -0.6f, 0.2f}, {0.7f, -0.45f, 0.6f, 0.3f}, {0.7f, 0.7f, 0.7f, 0.4f},
	    {0.7f, 0.7f, -0.4f, 0.3f},  {0.7f, 0.7f, -0.4f, 0.4f}};
	// The comb delays in ms at full density. They are spread out and share no common factor,
	// so their echoes rarely line up.
	static constexpr float CombTimes[Combs] = {22.01f, 23.25f, 24.75f, 26.6f,  28.25f, 29.63f,
	                                           31.06f, 32.44f, 33.69f, 34.95f, 36.29f, 37.65f,
	                                           38.99f, 40.35f, 41.67f, 43.05f};
	// The all-pass delays in ms.
	static constexpr float AllpassTimes[Allpasses] = {11.58f, 9.19f, 7.1f, 4.69f};
	// Per-channel adjustments to the all-pass delays, in samples. Slightly different lengths
	// keep the channels from sounding identical.
	static constexpr int32_t AllpassOffsets[8][Allpasses] = {
	    {0, -32, -6, -11}, {-10, -5, 0, -34},   {-8, -3, -33, -5},  {-7, -9, -5, -8},
	    {-33, -8, -8, 0},  {-11, -21, -10, -7}, {-9, -10, -27, -3}, {-6, -11, -9, -24}};
	// The combs each output channel sums, with bit i standing for comb i. Every channel takes
	// a different eight of the sixteen, so the channels' tails differ and sound wide.
	static constexpr uint16_t ChannelCombs[8] = {0xaaaa, 0x5555, 0xcccc, 0x3333,
	                                             0x0ff0, 0x9ab2, 0x3c3c, 0x399c};

	// Where to read the history for the early reflections and the late feed.
	struct Taps {
		// How many samples back each tap reads. The last one is the late feed.
		std::array<uint32_t, EarlyTaps + 1> delays {};
		// The gain of each tap.
		std::array<float, EarlyTaps + 1> gains {};
	};

	struct Comb {
		std::vector<float> buffer;               // The delay line.
		uint32_t           cursor        = 0;    // Where the delay line is read and written.
		float              feedback      = 0.0f; // Gain applied on each trip around the loop.
		float              lowpass_in    = 0.0f; // Low-pass weight of the new sample.
		float              lowpass_back  = 0.0f; // Low-pass weight of its previous output.
		float              direct        = 0.0f; // How much unfiltered signal is fed back.
		float              filtered      = 0.0f; // How much low-passed signal is fed back.
		float              emphasis      = 0.0f; // Pre-emphasis weight of the new input.
		float              emphasis_back = 0.0f; // Pre-emphasis weight of the previous input.
		float              previous      = 0.0f; // The previous input, for the pre-emphasis.
		float              lowpass       = 0.0f; // The low-pass's previous output.
	};

	struct Allpass {
		std::vector<float> buffer;     // The delay line.
		uint32_t           cursor = 0; // Where the delay line is read and written.
	};

	struct Channel {
		std::array<Allpass, Allpasses> allpasses;
		float                          y1  = 0.0f; // The roomHF filter's last output.
		float                          y2  = 0.0f; // The roomHF filter's output before that.
		float                          wet = 0.0f; // The wet level at the end of the last grain.
		float                          dry = 0.0f; // The dry level at the end of the last grain.
	};

	// Converts millibels to a linear gain, 10^(mB / 2000). Below -10000 mB (-100 dB) the result
	// is silence, but -10000 mB itself still gives 0.00001.
	static float Millibels(float millibels) {
		if (millibels < -10000.0f) {
			return 0.0f;
		}
		return static_cast<float>(std::pow(10.0, static_cast<double>(millibels * 0.0005f)));
	}

	// Converts a time in ms to a whole number of samples, rounding to the nearest.
	static uint32_t Samples(float milliseconds, float rate) {
		return static_cast<uint32_t>(rate * milliseconds * 0.001f + 0.5f);
	}

	// Converts a time in seconds to a whole number of samples. The library goes through ms on
	// the way, and the extra rounding of `* 1000 * 0.001` is kept so the delays match.
	static uint32_t SecondsToSamples(float seconds, float rate) {
		return static_cast<uint32_t>(seconds * rate * 1000.0f * 0.001f + 0.5f);
	}

	// Writes `source` times a gain that moves in a straight line from `from` toward `to` over
	// the grain into `samples`, or adds it when `accumulate` is set. This fades levels and
	// crossfades settings without clicks.
	static void Ramp(float* samples, float from, float to, uint32_t grain, const float* source,
	                 bool accumulate) {
		const float step = (to - from) / static_cast<float>(grain);
		float       gain = from;
		for (uint32_t i = 0; i < grain; i++) {
			const float value = source[i] * (from == to ? to : gain);
			samples[i]        = accumulate ? samples[i] + value : value;
			gain += step;
		}
	}

	// Mixes every channel except the LFE into `mono`. Each channel is scaled by a fixed gain
	// that keeps the sum from getting louder with more channels. The gains are 0.707 (-3 dB)
	// for stereo, 0.316 (-10 dB) for 5.1 and 0.267 (-11.5 dB) for anything else.
	static void MixDown(const float* input, uint32_t channels, uint32_t grain, float* mono) {
		if (channels == 1) {
			std::copy(input, input + grain, mono);
			return;
		}
		const float gain = channels == 2 ? 0.707f : channels == 6 ? 0.316f : 0.267f;
		for (uint32_t i = 0; i < grain; i++) {
			mono[i] = input[i] * gain;
		}
		for (uint32_t c = 1; c < channels; c++) {
			if (c == LfeChannel) {
				continue;
			}
			const float* channel = input + size_t(c) * grain;
			for (uint32_t i = 0; i < grain; i++) {
				mono[i] += channel[i] * gain;
			}
		}
	}

	// Fills `early` with the four reflections and `late` with the late feed for this grain.
	// Each tap reads the mono signal `delay` samples back. The first `delay` samples of the
	// grain come from the history of earlier grains and the rest from this grain's `input`.
	void RunTaps(const Taps& taps, const float* input, uint32_t grain, float* early,
	             float* late) const {
		const auto size = static_cast<uint32_t>(m_ring.size());
		std::fill(early, early + grain, 0.0f);
		std::fill(late, late + grain, 0.0f);
		for (uint32_t tap = 0; tap <= EarlyTaps; tap++) {
			float*         out   = tap < EarlyTaps ? early : late;
			const float    gain  = taps.gains[tap];
			const uint32_t delay = taps.delays[tap];
			uint32_t       read  = (m_ring_cursor + size - delay) % size;
			for (uint32_t i = 0; i < grain; i++) {
				if (i < delay) {
					out[i] += m_ring[read] * gain;
					read = read + 1 == size ? 0 : read + 1;
				} else {
					out[i] += input[i - delay] * gain;
				}
			}
		}
	}

	// Fills m_early and m_late for this grain. When the settings have changed, both the old
	// and the new taps run and the result crossfades from the old to the new over the grain.
	void RenderTaps(uint32_t grain, float rate) {
		if (!m_changed) {
			RunTaps(m_taps, m_mono.data(), grain, m_early.data(), m_late.data());
			return;
		}
		m_old_early.resize(grain);
		m_old_late.resize(grain);
		RunTaps(m_taps, m_mono.data(), grain, m_old_early.data(), m_old_late.data());
		ApplyParams(rate);
		RunTaps(m_taps, m_mono.data(), grain, m_early.data(), m_late.data());
		Ramp(m_early.data(), 0.0f, 1.0f, grain, m_early.data(), false);
		Ramp(m_early.data(), 1.0f, 0.0f, grain, m_old_early.data(), true);
		Ramp(m_late.data(), 0.0f, 1.0f, grain, m_late.data(), false);
		Ramp(m_late.data(), 1.0f, 0.0f, grain, m_old_late.data(), true);
	}

	// Works out the tap delays and gains for the current settings. No delay reaches further
	// back than `size` samples, the length of the history.
	[[nodiscard]] Taps TapsFor(uint32_t size, float rate) const {
		const auto&    params = m_params;
		Taps           taps;
		const uint32_t pattern = params.reflection_pattern < 8 ? params.reflection_pattern
		                                                       : params.reflection_pattern - 8;
		// The first reflection comes after the reflections delay, and the late feed after the
		// reflections delay plus the reverb delay.
		const auto early = std::min(SecondsToSamples(params.reflections_delay, rate), size);
		const auto late =
		    std::min(SecondsToSamples(params.reflections_delay + params.reverb_delay, rate), size);
		// When the pattern's last reflection would land past `limit`, every tap time is scaled
		// down by the same factor so it fits. Patterns 0 to 7 must fit before the late feed,
		// and 8 to 15 only before the end of the history.
		const auto     last  = Samples(TapTimes[pattern][EarlyTaps - 1], rate);
		const uint32_t limit = params.reflection_pattern > 7 ? size : late;
		const float    scale = early + last > limit
		                           ? static_cast<float>(limit - early) / static_cast<float>(last)
		                           : 1.0f;
		taps.delays[0]       = early;
		taps.gains[0]        = TapGains[pattern][0];
		for (uint32_t tap = 1; tap < EarlyTaps; tap++) {
			taps.delays[tap] = Samples(scale * TapTimes[pattern][tap], rate) + early;
			// Squeezing can land two taps on the same sample. The later one is then dropped.
			taps.gains[tap] =
			    taps.delays[tap] == taps.delays[tap - 1] ? 0.0f : TapGains[pattern][tap];
		}
		taps.delays[EarlyTaps] =
		    std::min(early + SecondsToSamples(params.reverb_delay, rate), size);
		taps.gains[EarlyTaps] = 1.0f;
		return taps;
	}

	// Sets up a comb of `length` samples for the current settings. `clear` empties its delay
	// line, which a change of density needs.
	void SetupComb(Comb& comb, uint32_t length, float rate, bool clear) const {
		const auto& params = m_params;
		if (clear || comb.buffer.size() != length) {
			comb.buffer.assign(length, 0.0f);
			if (comb.cursor >= length) {
				comb.cursor = 0;
			}
		}
		// An echo goes around the loop rate * decay_time / length times in one decay time. A
		// feedback of 10^(-3 * length / rate / decay_time) makes those trips multiply out to
		// 10^-3, which is the 60 dB fade that decay_time promises.
		comb.feedback =
		    std::pow(10.0f, static_cast<float>(length) / rate / params.decay_time * -3.0f);
		const float ratio = params.decay_hf_ratio;
		if (ratio > 1.0f) {
			// The library feeds back the delayed signal in full, plus a share of a fixed
			// low-pass of it. The share is 1 - feedback^(ratio - 1), written with pow and log10.
			// The extra low-passed signal makes the low frequencies ring longer than decay_time
			// while the highs keep to it.
			const float high  = std::pow(10.0f, (ratio - 1.0f) * std::log10(comb.feedback));
			comb.lowpass_in   = 0.050620973f;
			comb.lowpass_back = 0.949379f;
			comb.direct       = 1.0f;
			comb.filtered     = 1.0f - high;
		} else {
			// Only a low-pass of the delayed signal is fed back, so the highs lose a little more
			// on every trip and fade sooner than the lows. The library reshapes the ratio before
			// using it. Below 0.8 it pulls the ratio in toward 0.8, keeping about 6/7 of the
			// distance, and then it squares the result. The low-pass coefficient comes from
			// ln(feedback), written as log10(feedback) * ln(10), and is kept below 0.99978 so the
			// filter stays stable.
			float shaped = ratio * ratio;
			if (0.8f > ratio) {
				shaped = (0.8f - ratio) * -0.857142925f + 0.8f;
				shaped *= shaped;
			}
			const float back =
			    std::min(0.99977994f, std::log10(comb.feedback) * std::numbers::ln10_v<float> *
			                              0.25f * (1.0f - 1.0f / shaped));
			comb.lowpass_in   = 1.0f - back;
			comb.lowpass_back = back;
			comb.direct       = 0.0f;
			comb.filtered     = 1.0f;
		}
		// The pre-emphasis filter is y = (x - tilt * previous x) / (1 - tilt). It leaves the
		// lows unchanged and scales the highest frequencies by 1 / sqrt(ratio). With a ratio
		// below 1 it lifts the highs going into the comb, which offsets part of the damping.
		const float root   = std::sqrt(ratio);
		const float tilt   = (1.0f - root) / (root + 1.0f);
		comb.emphasis      = tilt / (1.0f - tilt) + 1.0f;
		comb.emphasis_back = -tilt / (1.0f - tilt);
	}

	// Empties all of the history and filters for a fresh start. ApplyParams then sizes them.
	void Reset() {
		m_ring.assign(RingSamples, 0.0f);
		m_ring_cursor = 0;
		m_taps        = {};
		m_early_gain  = 1.0f;
		m_late_gain   = 1.0f;
		m_density     = std::numeric_limits<float>::quiet_NaN();
		m_combs       = {};
		m_channels    = {};
	}

	// Zeroes every delay line and filter state but keeps their sizes and settings.
	void ClearHistory() {
		std::ranges::fill(m_ring, 0.0f);
		for (auto& comb: m_combs) {
			std::ranges::fill(comb.buffer, 0.0f);
			comb.previous = comb.lowpass = 0.0f;
		}
		for (auto& channel: m_channels) {
			for (auto& allpass: channel.allpasses) {
				std::ranges::fill(allpass.buffer, 0.0f);
			}
			channel.y1 = channel.y2 = 0.0f;
		}
	}

	// Recomputes the taps, levels, combs and all-passes from the current settings.
	void ApplyParams(float rate) {
		const auto& params = m_params;
		m_taps             = TapsFor(static_cast<uint32_t>(m_ring.size()), rate);
		// room scales everything and is capped at 0 dB. The late level also drops by 24 dB,
		// which is the library's allowance for summing many combs.
		const float room = std::min(1.0f, Millibels(static_cast<float>(params.room)));
		m_early_gain =
		    Millibels(static_cast<float>(std::clamp<int32_t>(params.reflections, -10000, 1000))) *
		    room;
		m_late_gain =
		    Millibels(static_cast<float>(std::clamp<int32_t>(params.reverb, -10000, 2000) - 2400)) *
		    room;
		// The comb lengths depend on the density, so a new density empties the combs. m_density
		// starts as NaN, which never compares equal, so the first call counts as a change.
		const bool density_changed = !(m_density == params.density);
		m_density                  = params.density;
		// The comb times are divided by a factor from 1 at density 100 to 4 at density 0.
		const float density_scale = params.density * -3.0f / 100.0f + 4.0f;
		for (uint32_t i = 0; i < Combs; i++) {
			auto length =
			    static_cast<uint32_t>(rate * CombTimes[i] / density_scale * 0.001f + 0.5f);
			// Rounds an even length up to the next odd one.
			length += ~length & 1u;
			SetupComb(m_combs[i], length, rate, density_changed);
		}
		for (uint32_t channel = 0; channel < MaxChannels; channel++) {
			for (uint32_t stage = 0; stage < Allpasses; stage++) {
				auto&      allpass = m_channels[channel].allpasses[stage];
				const auto length =
				    static_cast<uint32_t>(static_cast<int32_t>(Samples(AllpassTimes[stage], rate)) +
				                          AllpassOffsets[channel][stage]);
				if (allpass.buffer.size() != length) {
					allpass.buffer.assign(length, 0.0f);
				}
				if (allpass.cursor >= length) {
					allpass.cursor = 0;
				}
			}
		}
	}

	// Sets up the roomHF filter, which is two identical one-pole low-passes in a row with pole
	// `p`. Their combined gain at angular frequency w is (1 - p)^2 / (1 - 2p cos(w) + p^2).
	// Setting that equal to roomHF at hf_reference and solving the quadratic for p gives the
	// expression below. A roomHF of 0 dB needs no filter.
	void UpdateRoomFilter(float rate) {
		const auto& params = m_params;
		const float room_hf =
		    Millibels(static_cast<float>(std::clamp<int32_t>(params.room_hf, -10000, 0)));
		m_room_filter = 1.0f > room_hf;
		if (!m_room_filter) {
			return;
		}
		const float cosine = std::cos(params.hf_reference * TwoPi / rate);
		const float root   = std::sqrt((room_hf + room_hf) * (1.0f - cosine) -
		                               room_hf * room_hf * (1.0f - cosine * cosine));
		const float pole   = ((1.0f - room_hf * cosine) - root) / (1.0f - room_hf);
		// Multiplying out the two filters gives y = b0 x + a1 y1 + a2 y2.
		m_room_b0 = (1.0f - pole) * (1.0f - pole);
		m_room_a1 = pole + pole;
		m_room_a2 = pole * -pole;
	}

	// Runs `input` through the comb and writes what the comb feeds back to `output`. Each
	// sample, the delayed signal is damped and scaled by the feedback, then stored back in the
	// delay line along with the pre-emphasized input.
	static void RunComb(Comb& comb, const float* input, float* output, uint32_t grain) {
		const auto length = static_cast<uint32_t>(comb.buffer.size());
		for (uint32_t i = 0; i < grain; i++) {
			const float emphasized = input[i] * comb.emphasis + comb.previous * comb.emphasis_back;
			comb.previous          = input[i];
			const float delayed    = comb.buffer[comb.cursor];
			comb.lowpass           = delayed * comb.lowpass_in + comb.lowpass * comb.lowpass_back;
			const float back =
			    (delayed * comb.direct + comb.lowpass * comb.filtered) * comb.feedback;
			comb.buffer[comb.cursor] = emphasized + back;
			output[i]                = back;
			comb.cursor              = comb.cursor + 1 == length ? 0 : comb.cursor + 1;
		}
	}

	// Runs `samples` in place through an all-pass filter. It passes every frequency at the
	// same level but delays each by a different amount, which smears sharp echoes into a
	// smoother wash. A higher `gain` smears more.
	static void RunAllpass(Allpass& allpass, float gain, float* samples, uint32_t grain) {
		const auto length = static_cast<uint32_t>(allpass.buffer.size());
		for (uint32_t i = 0; i < grain; i++) {
			const float delayed            = allpass.buffer[allpass.cursor];
			const float stored             = delayed * gain + samples[i];
			allpass.buffer[allpass.cursor] = stored;
			samples[i]                     = delayed - gain * stored;
			allpass.cursor                 = allpass.cursor + 1 == length ? 0 : allpass.cursor + 1;
		}
	}

	// How much comb number `selected` (counting only the combs a channel uses) adds to that
	// channel. At full density every comb counts fully. Below that, every other comb is
	// turned down and some are flipped, starting at -1.5 for the first at density 0.
	[[nodiscard]] float CombWeight(uint32_t selected) const {
		if ((selected & 1u) != 0) {
			return 1.0f;
		}
		return 1.0f / (static_cast<float>(selected) * 10.0f + 40.0f) * (m_params.density - 100.0f) +
		       1.0f;
	}

	// Writes the reverb for one output channel to `out`. The channel's combs are summed and
	// smeared by its all-passes, the early reflections are added, and the roomHF filter is
	// applied.
	void RenderWet(Channel& state, uint32_t channel, float* out, uint32_t grain) {
		std::fill(m_tail.begin(), m_tail.end(), 0.0f);
		uint32_t selected = 0;
		for (uint32_t i = 0; i < Combs; i++) {
			if ((ChannelCombs[channel] >> i & 1u) == 0) {
				continue;
			}
			const float  weight = CombWeight(selected);
			const float* comb   = m_combs_out.data() + size_t(i) * grain;
			for (uint32_t n = 0; n < grain; n++) {
				m_tail[n] += comb[n] * weight;
			}
			selected++;
		}
		// Diffusion 0 to 100 gives an all-pass gain of 0 to 0.5.
		for (auto& allpass: state.allpasses) {
			RunAllpass(allpass, m_params.diffusion * 0.005f, m_tail.data(), grain);
		}
		for (uint32_t n = 0; n < grain; n++) {
			out[n] = m_early[n] * m_early_gain + m_tail[n] * m_late_gain;
		}
		// The roomHF filter keeps its history even while it is off, as the library's does.
		for (uint32_t n = 0; n < grain; n++) {
			const float y = m_room_filter
			                    ? m_room_b0 * out[n] + m_room_a1 * state.y1 + m_room_a2 * state.y2
			                    : out[n];
			state.y2      = state.y1;
			state.y1      = y;
			out[n]        = y;
		}
	}

	static constexpr float TwoPi = 2.0f * std::numbers::pi_v<float>;

	// The current settings, already clamped.
	Ngs2ReverbI3dl2Param m_params {};
	// Whether the game has sent settings. Until it does, Process leaves the audio alone.
	bool m_configured = false;
	// Whether the history and filters must be rebuilt before the next grain. That happens on
	// first use and when the sample rate changes.
	bool m_reset = true;
	// Whether the settings changed since the last grain, which starts a crossfade.
	bool     m_changed = true;
	uint32_t m_rate    = 0;
	// Samples left until the tail is 120 dB down. 0 means the reverb is quiet.
	uint32_t m_tail_remaining = 0;
	// The last RingSamples samples of mono input, for the taps to read.
	std::vector<float> m_ring;
	// Where the next mono sample goes in m_ring.
	uint32_t m_ring_cursor = 0;
	Taps     m_taps;
	// The linear levels of the early reflections and the late reverb.
	float m_early_gain = 0.0f;
	float m_late_gain  = 0.0f;
	// The density the combs were sized for. NaN means they have not been sized yet.
	float                            m_density = std::numeric_limits<float>::quiet_NaN();
	std::array<Comb, Combs>          m_combs;
	std::array<Channel, MaxChannels> m_channels;
	// Whether the roomHF filter is in use, and its coefficients.
	bool  m_room_filter = false;
	float m_room_b0     = 0.0f;
	float m_room_a1     = 0.0f;
	float m_room_a2     = 0.0f;
	// Per-grain working buffers, kept between grains to avoid allocating.
	std::vector<float> m_input;     // The dry input of every channel.
	std::vector<float> m_mono;      // The mixed-down input.
	std::vector<float> m_early;     // The early reflections.
	std::vector<float> m_late;      // The feed into the combs.
	std::vector<float> m_old_early; // The early reflections with the previous settings.
	std::vector<float> m_old_late;  // The late feed with the previous settings.
	std::vector<float> m_combs_out; // The output of every comb, one block per comb.
	std::vector<float> m_tail;      // The late reverb of the channel being rendered.
};

} // namespace Libs::Audio::Ngs2
