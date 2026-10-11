#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERREADCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERREADCACHE_H_

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace Libs::Graphics {

// Serves one materialization's guest reads out of whole lines already proven clean.
class ShaderGuestReadCache {
public:
	static constexpr uint64_t LineBytes = 256;
	static constexpr uint32_t LineBits  = 6;
	static constexpr size_t   LineCount = size_t {1} << LineBits;

	void Reset() {
		if (++m_epoch == 0) {
			for (auto& line: m_lines) {
				line.epoch = 0;
			}
			m_epoch = 1;
		}
	}

	template <typename CleanLine, typename Fallback>
	bool Read(uint64_t address, std::span<uint32_t> values, CleanLine&& clean_line,
	          Fallback&& fallback) {
		const auto bytes = static_cast<uint64_t>(values.size_bytes());
		auto*      line  = Find(address, bytes, clean_line);
		if (line == nullptr || !line->clean) {
			return Uncached(address, values, fallback);
		}
		std::memcpy(values.data(), line->data.data() + (address - line->base),
		            static_cast<size_t>(bytes));
		return true;
	}

	template <typename CleanLine>
	bool CleanCovered(uint64_t address, uint64_t bytes, CleanLine&& clean_line) {
		const auto* line = Find(address, bytes, clean_line);
		return line != nullptr && line->clean;
	}

private:
	struct Line;

	template <typename CleanLine>
	Line* Find(uint64_t address, uint64_t bytes, CleanLine& clean_line) {
		const auto base = address & ~(LineBytes - 1u);
		if (bytes == 0u || address - base > LineBytes - bytes || bytes > LineBytes) {
			return nullptr;
		}
		// Hashed: tables sit at power-of-two alignments that a modulo would map to one slot.
		auto& line = m_lines[static_cast<size_t>(((base / LineBytes) * 0x9e3779b97f4a7c15ull) >>
		                                         (64u - LineBits))];
		if (line.epoch != m_epoch || line.base != base) {
			line.epoch = m_epoch;
			line.base  = base;
			line.clean = clean_line(base, line.data.data(), LineBytes);
		}
		return &line;
	}

	template <typename Fallback>
	bool Uncached(uint64_t address, std::span<uint32_t> values, Fallback& fallback) {
		bool       drained = false;
		const bool result  = fallback(address, values, drained);
		if (drained) {
			Reset();
		}
		return result;
	}

	struct Line {
		uint64_t                       base  = 0;
		uint32_t                       epoch = 0;
		bool                           clean = false;
		std::array<uint8_t, LineBytes> data {};
	};

	std::array<Line, LineCount> m_lines {};
	uint32_t                    m_epoch = 1;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERREADCACHE_H_ */
