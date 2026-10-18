#ifndef KYTY_MEMORY_PRESSURE_POLICY_H
#define KYTY_MEMORY_PRESSURE_POLICY_H
#include <cstdint>

namespace Libs::Graphics {
class MemoryPressurePolicy {
public:
	enum class Level : uint8_t { None, Collect, Pressure, Critical };
	static constexpr uint64_t Percent(uint64_t budget, uint64_t percent) {
		return (budget / 100) * percent + ((budget % 100) * percent) / 100;
	}
	// A zero budget selects the caller's legacy collection policy and resets hysteresis.
	Level Update(uint64_t usage, uint64_t budget) {
		if (budget == 0) {
			m_level = Level::None;
			return m_level;
		}
		const auto target = usage >= Percent(budget, 90)   ? Level::Critical
		                    : usage >= Percent(budget, 80) ? Level::Pressure
		                    : usage >= Percent(budget, 70) ? Level::Collect
		                                                   : Level::None;
		if (target >= m_level)
			m_level = target;
		else {
			while (m_level > target) {
				const auto release = 55 + 10 * static_cast<unsigned>(m_level);
				if (usage >= Percent(budget, release)) break;
				m_level = static_cast<Level>(static_cast<unsigned>(m_level) - 1);
			}
		}
		return m_level;
	}
	static constexpr uint64_t TextureAge(Level level, bool has_live_budget) {
		if (!has_live_budget)
			return level == Level::Critical ? 160 : level == Level::Pressure ? 80 : 16;
		return level == Level::Critical ? 16 : level == Level::Pressure ? 80 : 160;
	}

private:
	Level m_level = Level::None;
};
} // namespace Libs::Graphics
#endif
