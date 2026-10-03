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
	// Elements each invocation writes, one per store. The stores tile the workgroup's range, so
	// group_stride[0] == local_size_x * stores. One store per invocation is the common shape.
	uint32_t                 stores       = 1;

	static constexpr uint32_t MaxStores = 64;

	bool operator==(const UniformFill&) const = default;
};

// A bitmap the draw writes its selected descriptor-heap keys into, for a table too large for the
// host to enumerate. It lives in the flattened SRT because that buffer is already bound, writable
// and sized per draw; the host reads it back once the submit retires.
struct IndirectKeyFeedback {
	uint64_t signature  = 0;
	uint32_t srt_offset = 0;
	uint32_t words      = 0;

	bool operator==(const IndirectKeyFeedback&) const = default;
};

struct BindlessImageTable {
	uint32_t                srt_offset    = 0;
	std::array<uint32_t, 4> heap          = {};
	uint32_t                stride        = 0;
	uint32_t                record_offset = 0;

	bool operator==(const BindlessImageTable&) const = default;
};

struct ResourceSnapshot {
	std::vector<DescriptorValue>               buffers;
	std::vector<DescriptorValue>               images;
	std::vector<DescriptorValue>               samplers;
	std::vector<uint32_t>                      flattened_srt;
	std::vector<uint32_t>                      user_data;
	std::vector<std::pair<uint64_t, uint64_t>> specialization_reads;
	std::vector<uint32_t>                      specialization_read_slots;
	UniformFill                                uniform_fill;
	std::vector<IndirectKeyFeedback>           key_feedback;
	std::vector<BindlessImageTable>            bindless_tables;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCESNAPSHOT_H_
