#pragma once

#include <cstdint>
#include <span>

namespace Libs::Graphics {

// Only adjacent, non-predicated, plain 32-bit CPU labels may share a submission.
class ReleaseMemBatch {
public:
	static constexpr uint32_t MaxPackets = 8;

	static bool Eligible(std::span<const uint32_t> packet) noexcept {
		if (packet.size() < 8 || packet[0] != 0xc0061060u || packet[1] != 0x528u ||
		    packet[2] != (1u << 29u)) {
			return false;
		}
		const uint64_t address = uint64_t {packet[3]} | (uint64_t {packet[4]} << 32u);
		return address != 0 && (address & 3u) == 0;
	}

	bool Defer(std::span<const uint32_t> current, std::span<const uint32_t> next) noexcept {
		if (m_pending + 1 >= MaxPackets || !Eligible(current) || !Eligible(next)) {
			return false;
		}
		++m_pending;
		return true;
	}

	[[nodiscard]] bool Pending() const noexcept { return m_pending != 0; }
	void Reset() noexcept { m_pending = 0; }

private:
	uint32_t m_pending = 0;
};

} // namespace Libs::Graphics
