#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_DECOMPILER_OPCODETABLE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_DECOMPILER_OPCODETABLE_H_

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

#include <cstddef>

namespace Libs::Graphics::ShaderRecompiler::Decoder::Detail {

struct OpcodeMap {
	uint32_t encoding = 0;
	Opcode   decoded  = Opcode::Unknown;
};

template <typename Entry, size_t N>
constexpr const Entry* FindOpcode(const Entry (&table)[N], uint32_t encoding) {
	for (const auto& entry: table) {
		if (entry.encoding == encoding) {
			return &entry;
		}
	}
	return nullptr;
}

template <typename Entry, size_t N>
constexpr Opcode LookupOpcode(const Entry (&table)[N], uint32_t encoding) {
	const auto* entry = FindOpcode(table, encoding);
	return entry != nullptr ? entry->decoded : Opcode::Unsupported;
}

template <typename Entry, size_t N>
constexpr bool HasUniqueEncodings(const Entry (&table)[N]) {
	for (size_t i = 0; i < N; i++) {
		for (size_t j = i + 1; j < N; j++) {
			if (table[i].encoding == table[j].encoding) {
				return false;
			}
		}
	}
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder::Detail

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_DECOMPILER_OPCODETABLE_H_
