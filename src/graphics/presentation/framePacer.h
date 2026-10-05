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
} // namespace Libs::Graphics
#endif
