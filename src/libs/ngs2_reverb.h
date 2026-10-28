#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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
	// The module id a custom rack uses for this reverb.
	static constexpr uint32_t ModuleId    = 0x1b;
	static constexpr uint32_t MaxChannels = 8;
	static constexpr uint32_t EarlyTaps   = 4;
	static constexpr uint32_t Combs       = 16;
	static constexpr uint32_t Allpasses   = 4;

	// Takes new settings, which apply from the next grain.
	void Configure(const Ngs2ReverbI3dl2Param& param, uint32_t rate);

	// Whether the reverb must run even on silent input. That is while its tail is still
	// sounding, or while new settings are pending, since the library starts their crossfade on
	// the next grain whether or not there is sound.
	[[nodiscard]] bool HasTail() const {
		return m_active || (m_configured && (m_reset || m_changed));
	}

	// The length in samples of one channel's all-pass delay line, so tests can check how the
	// lines are sized for a rate. The lines are built on the first Process after Configure.
	[[nodiscard]] uint32_t AllpassLength(uint32_t channel, uint32_t stage) const {
		return static_cast<uint32_t>(m_channels.at(channel).allpasses.at(stage).buffer.size());
	}

	// Applies the reverb in place to `samples`, which holds `channels` blocks of `grain`
	// samples, one block per channel. Once configured, it treats NaN and inf input samples as
	// silence and writes 0 in their place.
	void Process(float* samples, uint32_t channels, uint32_t grain);

	// Empties the reverb so a new start does not replay the tail of the sound before it. The
	// settings are kept, and the next sound starts at the current wet and dry levels.
	void ClearTail();

	// Whether Process has ever replaced a NaN or inf input sample with silence, so the caller can
	// warn about it once. It stays set, since a game that sends garbage once is worth one warning.
	[[nodiscard]] bool SawNonFiniteInput() const { return m_non_finite_input; }

	// Whether the output ever overflowed to NaN or inf, which finite but huge input can cause.
	// Process silences those samples, and drops the tail when the reverb itself overflowed. It
	// stays set like SawNonFiniteInput.
	[[nodiscard]] bool SawNonFiniteOutput() const { return m_non_finite_output; }

private:
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

	// The steps of Process. They and the stateless helpers are defined in ngs2_reverb.cpp.
	[[nodiscard]] bool  StartGrain(float* samples, size_t total, uint32_t grain);
	void                WriteHistory(uint32_t grain);
	void                RunCombs(uint32_t grain);
	[[nodiscard]] bool  MixChannel(float* samples, uint32_t channel, uint32_t grain, bool reset);
	void                FinishGrain(float* samples, size_t total, bool overflowed);
	void                EndTail(uint32_t grain);
	void                DropTail();
	[[nodiscard]] bool  IsQuiet() const;
	[[nodiscard]] bool  RoomFilterQuiet(float limit) const;
	[[nodiscard]] float FadeLevel(uint32_t grain) const;
	void                RunTaps(const Taps& taps, const float* input, uint32_t grain, float* early,
	                            float* late) const;
	void                RenderTaps(uint32_t grain, float rate);
	[[nodiscard]] Taps  TapsFor(uint32_t size, float rate) const;
	void                SetupComb(Comb& comb, uint32_t length, float rate, bool clear) const;
	static void         SetDamping(Comb& comb, float ratio);
	void                Reset();
	void                ClearHistory();
	void                ApplyParams(float rate);
	void                UpdateRoomFilter(float rate);
	static void         RunComb(Comb& comb, const float* input, float* output, uint32_t grain);
	static void         RunAllpass(Allpass& allpass, float gain, float* samples, uint32_t grain);
	[[nodiscard]] float CombWeight(uint32_t selected) const;
	void                RenderWet(Channel& state, uint32_t channel, float* out, uint32_t grain);

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
	// Whether the next grain that reaches the mix starts its wet and dry levels at the current
	// settings, as after a reset. ClearTail sets it, since levels from before the clear are stale
	// and a channel added by a new setup has no last level even when no settings are pending.
	bool m_restart_levels = false;
	// Whether a NaN or inf input sample was ever replaced with silence.
	bool m_non_finite_input = false;
	// Whether an output sample ever overflowed to NaN or inf.
	bool m_non_finite_output = false;
	// Whether the reverb still holds sound, so it must keep running on silent input.
	bool m_active = false;
	// Silent input samples since the last sound. The tail can only end once the history ring
	// holds nothing but silence, and the ceiling counts from the last sound.
	uint64_t m_silent_samples = 0;
	// Samples left in the fade that ends a tail at the ceiling, and the fade's full length. No
	// fade runs while m_fade_remaining is 0.
	uint32_t m_fade_remaining = 0;
	uint32_t m_fade_length    = 0;
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
