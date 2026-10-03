#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCESNAPSHOT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCESNAPSHOT_H_

#include <array>
#include <cstdint>
#include <ranges>
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
	// Elements each invocation writes; group_stride[0] == local_size_x * stores.
	uint32_t                 bounded_elements = 0;

	static constexpr uint32_t MaxStores = 64;

	bool operator==(const UniformFill&) const = default;
};

// The read slot of a resident image heap's table. The shader samples through the heap's slot
// buffer, and the T# fetches it replaces are scalar loads that never see the draw's own writes, so
// the table is a materialization dependency, not a read the draw must keep apart from its writes.
inline constexpr uint32_t HeapReadSlot = UINT32_MAX - 1u;

struct ResourceSnapshot {
	std::vector<DescriptorValue> buffers;
	std::vector<DescriptorValue> images;
	std::vector<DescriptorValue> samplers;
	std::vector<uint32_t>        flattened_srt;
	std::vector<uint32_t>        user_data;
	std::vector<std::pair<uint64_t, uint64_t>> specialization_reads;
	UniformFill                 uniform_fill;

	void AddHeapRead(uint64_t base, uint64_t size) {
		specialization_reads.emplace_back(base, size);
		specialization_read_slots.push_back(HeapReadSlot);
	}
	[[nodiscard]] bool HeapRead(size_t index) const {
		return index < specialization_read_slots.size() &&
		       specialization_read_slots[index] == HeapReadSlot;
	}
	// The specialization reads a draw's writes must not overlap: all but heap tables.
	[[nodiscard]] auto ScalarReads() const {
		return std::views::iota(size_t {0}, specialization_reads.size()) |
		       std::views::filter([this](size_t index) { return !HeapRead(index); }) |
		       std::views::transform([this](size_t index) { return specialization_reads[index]; });
	}
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCESNAPSHOT_H_
