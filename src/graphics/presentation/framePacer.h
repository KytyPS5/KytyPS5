#ifndef KYTY_GRAPHICS_PRESENTATION_FRAME_PACER_H_
#define KYTY_GRAPHICS_PRESENTATION_FRAME_PACER_H_

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics {
// Vblank deadlines use an absolute clock; small wake-up errors are compensated.
class FramePacer {
public:
	FramePacer(uint64_t frequency, uint64_t start): m_frequency(frequency), m_deadline(start) {}
	[[nodiscard]] uint64_t Remaining(uint64_t now) const {
		return m_deadline > now ? m_deadline - now : 0;
	}
	[[nodiscard]] bool Late(uint64_t now) const { return now > m_deadline; }
	void Advance(uint64_t now, uint32_t refresh) {
		refresh = std::max(refresh, 1u);
		const uint64_t period = std::max(m_frequency / refresh, uint64_t {1});
		if (m_refresh != 0 && refresh != m_refresh) {
			m_deadline = now;
			m_fraction = 0;
		}
		m_refresh = refresh;
		m_deadline += period;
		m_fraction += m_frequency % refresh;
		if (m_fraction >= refresh) {
			++m_deadline;
			m_fraction -= refresh;
		}
		// A long render/UI stall must not be repaid by firing many vblanks
		// without sleeping. Drop old debt, retaining normal sub-frame correction.
		if (now > m_deadline && now - m_deadline >= period) {
			m_deadline = now;
			m_fraction = 0;
		}
	}

private:
	uint64_t m_frequency, m_deadline, m_fraction = 0;
	uint32_t m_refresh = 0;
};

// Frame Generation interpolates between consecutive real frames. Guest frames
// that land on alternating vblank counts (2,3,2,3 at 60 Hz) make the generated
// cadence uneven, so early frames are held toward the recent average interval.
// ponytail: EMA of due intervals; a stall is skipped, a lasting slowdown just disables holding.
class PresentSmoother {
public:
	// Ticks to wait before presenting a frame that became due at `now`, at most `cap`.
	[[nodiscard]] uint64_t Delay(uint64_t now, uint64_t cap) const {
		if (m_average == 0) return 0;
		const auto target = m_last_present + m_average - m_average / 16;
		return target > now ? std::min(target - now, cap) : 0;
	}
	void Presented(uint64_t due, uint64_t presented) {
		if (m_last_due != 0) {
			const auto interval = due - m_last_due;
			if (m_average == 0) m_average = interval;
			else if (interval <= m_average * 3) m_average = (m_average * 7 + interval) / 8;
		}
		m_last_due     = due;
		m_last_present = presented;
	}
	void Reset() { *this = {}; }

private:
	uint64_t m_last_due = 0, m_last_present = 0, m_average = 0;
};
} // namespace Libs::Graphics
#endif
