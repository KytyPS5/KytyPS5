#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCESNAPSHOT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCESNAPSHOT_H_

#include <array>
#include <cstdint>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

struct DescriptorValue {
	std::array<uint32_t, 8> dwords      = {};
	uint32_t                dword_count = 0;

	bool operator==(const DescriptorValue& other) const {
		return dword_count == other.dword_count && dwords == other.dwords;
	}
};

struct BufferFill {
	uint32_t buffer       = 0;
	uint32_t group_stride = 0;
	uint32_t element_size = 0;
	uint32_t value        = 0;

	bool operator==(const BufferFill&) const = default;
};

struct ResourceSnapshot {
	std::vector<DescriptorValue> buffers;
	std::vector<DescriptorValue> images;
	std::vector<DescriptorValue> samplers;
	std::vector<uint32_t>        flattened_srt;
	std::vector<uint32_t>        user_data;
	BufferFill                  buffer_fill;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCESNAPSHOT_H_
