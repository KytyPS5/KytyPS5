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
	// Elements a guarded buffer fill writes at most; unguarded fills cover the dispatch.
	uint32_t                 count        = UINT32_MAX;

	bool operator==(const UniformFill&) const = default;
};

// One proven buffer-to-buffer copy: lanes whose global index is below `count` copy one element of
// `words` DWORDs from the source V# to the destination V#, at the same index.
struct UniformCopy {
	std::array<uint32_t, 4> source {};
	std::array<uint32_t, 4> destination {};
	uint32_t                count     = 0;
	uint32_t                words     = 1;
	bool                    formatted = false;

	bool operator==(const UniformCopy&) const = default;
};

struct ResourceSnapshot {
	std::vector<DescriptorValue> buffers;
	std::vector<DescriptorValue> images;
	std::vector<DescriptorValue> samplers;
	std::vector<uint32_t>        flattened_srt;
	std::vector<uint32_t>        user_data;
	std::vector<std::pair<uint64_t, uint64_t>> specialization_reads;
	// Evaluated ResourcePlan::address_bases; they do not affect specialization.
	std::vector<uint64_t>        address_bases;
	UniformFill                 uniform_fill;
	// Every copy a proven copy kernel performs in this dispatch, valid when `copies_proven` is set;
	// the memory read to evaluate them; and the lanes per workgroup the kernel's index assumes.
	bool                         copies_proven      = false;
	std::vector<UniformCopy>     uniform_copies;
	std::vector<std::pair<uint64_t, uint64_t>> uniform_copy_reads;
	uint32_t                     copy_group_stride  = 0;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCESNAPSHOT_H_
