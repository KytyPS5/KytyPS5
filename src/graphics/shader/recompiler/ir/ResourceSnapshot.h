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

// Replicate: destination[i] = source[i % pattern_elements] for i < bounded_elements, both 32-bit
// formatted buffers (a verified shader, see RecognizeReplicateFill()).
enum class UniformFillKind { None, Buffer, Image, Replicate };

struct UniformFill {
	UniformFillKind          kind         = UniformFillKind::None;
	uint32_t                 resource     = 0;
	std::array<uint32_t, 3> group_stride {};
	uint32_t                 words        = 0;
	uint32_t                 value        = 0;
	uint32_t                 bounded_elements = 0;
	uint32_t                 source           = 0;
	uint32_t                 pattern_elements = 0;

	bool operator==(const UniformFill&) const = default;
};

// The distinct T#s of a bindless image table; slot 0 is null.
struct ImageTableSnapshot {
	uint64_t                     base         = 0;
	uint64_t                     size         = 0;
	uint32_t                     offset       = 0;
	uint32_t                     entry_mask   = 0;
	std::vector<uint32_t>        raw;     // The table words the slots were built from.
	std::vector<DescriptorValue> slots;
	std::vector<uint32_t>        mapping; // Entry count, then the slot of each entry.
	// Names the content of `slots`: snapshots that reuse the slots of an earlier one (the table's
	// words did not change) keep its id; 0 for none.
	uint64_t                     slots_id = 0;
};

// A flattened SRT dword run whose guest bytes the GPU writes (see ResourceSnapshot).
struct GpuFlatLoad {
	uint64_t address = 0;
	uint32_t slot    = 0;
	uint32_t count   = 0;
};

struct ResourceSnapshot {
	std::vector<DescriptorValue> buffers;
	std::vector<DescriptorValue> images;
	std::vector<DescriptorValue> samplers;
	std::vector<uint32_t>        flattened_srt;
	std::vector<ImageTableSnapshot> image_tables;
	std::vector<uint32_t>        user_data;
	std::vector<std::pair<uint64_t, uint64_t>> specialization_reads;
	UniformFill                 uniform_fill;
	// Flattened SRT dwords that the GPU writes before the shader runs and that the execution thread
	// did not read back: the bindings copy them into the uploaded dwords on the GPU.
	mutable std::vector<GpuFlatLoad> gpu_flat_loads;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCESNAPSHOT_H_
