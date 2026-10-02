#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCESNAPSHOT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCESNAPSHOT_H_

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

struct DescriptorValue {
	std::array<uint32_t, 8> dwords      = {};
	uint32_t                dword_count = 0;

	bool operator==(const DescriptorValue& other) const {
		return dword_count == other.dword_count && dwords == other.dwords;
	}
};

enum class UniformFillKind { None, Buffer, Image };

struct UniformFill {
	UniformFillKind          kind         = UniformFillKind::None;
	uint32_t                 resource     = 0;
	std::array<uint32_t, 3> group_stride {};
	uint32_t                 words        = 0;
	uint32_t                 value        = 0;

	bool operator==(const UniformFill&) const = default;
};

inline constexpr uint32_t NoSrtSlot = UINT32_MAX;
// A specialization read made by a scalar load the shader also issues itself (not flattened):
// RawReadSlotBit | the load's memory-info index.
inline constexpr uint32_t RawReadSlotBit = 0x80000000u;
[[nodiscard]] constexpr uint32_t RawReadSlot(uint32_t memory_index) {
	return memory_index < RawReadSlotBit ? (RawReadSlotBit | memory_index) : NoSrtSlot;
}

struct ResourceSnapshot {
	std::vector<DescriptorValue> buffers;
	std::vector<DescriptorValue> images;
	std::vector<DescriptorValue> samplers;
	std::vector<uint32_t>        flattened_srt;
	std::vector<uint32_t>        user_data;
	std::vector<std::pair<uint64_t, uint64_t>> specialization_reads;
	// For each specialization read, the flat SRT slot whose scalar load made it, the RawReadSlot()
	// of a scalar load the shader issues itself, or NoSrtSlot.
	std::vector<uint32_t>        specialization_read_slots;
	UniformFill                 uniform_fill;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCESNAPSHOT_H_
