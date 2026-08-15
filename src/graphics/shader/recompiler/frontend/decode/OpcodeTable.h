#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_DECOMPILER_OPCODETABLE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_DECOMPILER_OPCODETABLE_H_

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::Decoder::Detail {

struct OpcodeMap {
	uint32_t encoding = 0;
	Opcode   decoded  = Opcode::Unknown;
};

template <typename Entry, size_t EncodingCount, size_t EntryCount>
struct OpcodeTable {
	std::array<Entry, EntryCount>       entries = {};
	std::array<uint16_t, EncodingCount> indices = {};
};

template <size_t EncodingCount, typename Entry, size_t EntryCount>
consteval auto MakeOpcodeTable(const Entry (&entries)[EntryCount]) {
	static_assert(EntryCount < UINT16_MAX);
	OpcodeTable<Entry, EncodingCount, EntryCount> table;
	for (size_t i = 0; i < EntryCount; i++) {
		const auto encoding = entries[i].encoding;
		if (encoding >= EncodingCount || table.indices[encoding] != 0) {
			throw "invalid opcode table";
		}
		table.entries[i]        = entries[i];
		table.indices[encoding] = static_cast<uint16_t>(i + 1);
	}
	return table;
}

template <typename Entry, size_t EncodingCount, size_t EntryCount>
constexpr const Entry* FindOpcode(const OpcodeTable<Entry, EncodingCount, EntryCount>& table,
                                  uint32_t                                             encoding) {
	if (table.indices[encoding] == 0) {
		return nullptr;
	}
	return &table.entries[table.indices[encoding] - 1];
}

template <typename Entry, size_t EncodingCount, size_t EntryCount>
constexpr Opcode LookupOpcode(const OpcodeTable<Entry, EncodingCount, EntryCount>& table,
                              uint32_t                                             encoding) {
	const auto* entry = FindOpcode(table, encoding);
	return entry != nullptr ? entry->decoded : Opcode::Unsupported;
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder::Detail

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_DECOMPILER_OPCODETABLE_H_
