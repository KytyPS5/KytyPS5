#pragma once

#include "graphics/shader/recompiler/ir/Block.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

struct ValueBlockInfo {
	uint32_t        id       = 0;
	uint32_t        start_pc = 0;
	uint32_t        end_pc   = 0;
	CFG::Terminator terminator;
	Value           condition;
	Value           indirect_target;
};

struct DescriptorSource {
	struct IndirectImage {
		uint32_t material_source = 0;
		uint32_t heap_source     = 0;
		uint32_t selector_stride = 0;
		uint32_t selector_offset = 0;

		bool operator==(const IndirectImage& other) const = default;
	};

	std::array<Value, 8>         dwords {};
	uint32_t                     dword_count = 0;
	std::optional<IndirectImage> indirect_image;

	bool operator==(const DescriptorSource& other) const = default;
};

struct SrtRead {
	Value    value;
	uint32_t flat_offset = 0;
	uint32_t use_pc      = 0;

	bool operator==(const SrtRead& other) const = default;
};

struct ValueProgram {
	ValueProgram() = default;
	~ValueProgram();

	ValueProgram(const ValueProgram&)            = delete;
	ValueProgram& operator=(const ValueProgram&) = delete;
	ValueProgram(ValueProgram&&)                 = default;
	ValueProgram& operator=(ValueProgram&&)      = default;

	std::vector<std::unique_ptr<Block>> block_storage;
	BlockList                           blocks;
	std::vector<ValueBlockInfo>         block_info;
	// Decoded MIMG/VMEM metadata carries details such as RDNA2 NSA address registers and
	// storage-image swizzles. Typed memory instructions carry a dense index into these shader-local
	// tables until those fields are consumed by emission.
	std::vector<MemoryInfo>       memory_info;
	std::vector<ExportInfo>       export_info;
	std::vector<DescriptorSource> descriptor_sources;
	std::vector<SrtRead>          srt_reads;
	std::vector<Value>            dynamic_reads;
	bool                          dispatcher_fallback = false;
};

bool        ValidateValueProgram(const ValueProgram& program, bool require_ssa, std::string* error);
bool        EquivalentValue(const ValueProgram& program, Value left, Value right);
Value       ResolveInvariantPhi(const ValueProgram& program, Value value);
std::string ValueProgramToString(const ValueProgram& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR
