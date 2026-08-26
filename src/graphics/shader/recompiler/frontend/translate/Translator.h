#pragma once

#include "graphics/shader/recompiler/frontend/translate/Translate.h"
#include "graphics/shader/recompiler/ir/IREmitter.h"

#include <array>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

class Translator {
public:
	Translator(IR::Program& program, IR::Block* block, uint32_t vector_limit, uint32_t wave_size)
	    : program(program), ir(block), current_vector_limit(vector_limit),
	      current_wave_size(wave_size) {}

	bool TranslateInstruction(const Decoder::Instruction& inst, std::string* error);
	bool TranslateEmbeddedFetch(const Decoder::Instruction& inst, uint32_t attribute,
	                            uint32_t component_count);
	bool AddBranchCondition(const CFG::BasicBlock& source, IR::BlockInfo& info, std::string* error);

private:
	Decoder::Operand SourceAt(const Decoder::Instruction& inst, uint32_t index);
	Decoder::Operand DestinationOperand(const Decoder::Instruction& inst);
	Decoder::Operand OffsetOperand(const Decoder::Operand& operand, uint32_t offset);
	Decoder::Operand ScalarDestinationOperand(const Decoder::Operand& operand, uint32_t offset);
	Decoder::Operand PlainOperand(const Decoder::Operand& operand);
	std::array<IR::U32, 2> BallotMask(IR::U1 value);
	IR::U32                ReadRawU32(const Decoder::Operand& operand);
	IR::U32                ReadScalarCode(uint32_t code);
	IR::U32                ApplyBitSourceModifiers(const Decoder::Operand& operand, IR::U32 value);
	IR::Value              ReadOperand(const Decoder::Operand& operand, IR::Type type);
	IR::U1                 ThreadBit(IR::U32 low);
	void                   WriteRawU32(const Decoder::Operand& operand, IR::U32 value);
	IR::F32                ApplyF32ResultModifiers(const Decoder::Operand& operand, IR::F32 value);
	void                   WriteOperand(const Decoder::Operand& operand, IR::Value value);
	IR::U32                PackHalf2x16(IR::F32 low, IR::F32 high);
	void                   WriteF16(const Decoder::Operand& operand, IR::F32 value);
	void                   WriteU16(const Decoder::Operand& operand, IR::U32 value);
	IR::U32                ReadU32(const Decoder::Operand& operand);
	std::array<IR::U32, 2> ReadU32Pair(const Decoder::Operand& operand);
	IR::U64                ReadU64(const Decoder::Operand& operand);
	IR::F32 ReadF16LaneAsF32(const Decoder::Operand& operand, bool high_lane, bool packed = false);
	IR::F32 ReadF16AsF32(const Decoder::Operand& operand);
	IR::F32 ReadMixF32(const Decoder::Operand& operand);
	IR::U32 ReadU16LaneRaw(const Decoder::Operand& operand, bool high_lane);
	IR::U32 ReadU16LaneAsU32(const Decoder::Operand& operand, bool high_lane, bool sign_extend);
	IR::U32 ReadU16AsU32(const Decoder::Operand& operand, bool sign_extend);
	IR::U32 ReadF16LaneBits(const Decoder::Operand& operand, bool high_lane);
	std::array<IR::U32, 2> ExtractU64(IR::U64 value);
	void    WriteU32Pair(const Decoder::Operand& operand, const std::array<IR::U32, 2>& value);
	IR::U1  ReadCondition(const Decoder::Operand& operand);
	IR::U32 ConditionBit(const Decoder::Operand& operand);
	IR::U1  ReadMask(const Decoder::Operand& operand);
	IR::U1  ReadMaskValid(const Decoder::Operand& operand);
	void    WriteMask(const Decoder::Operand& operand, IR::U1 value);
	void    WriteMask64(const Decoder::Operand& operand, IR::U1 value);
	void    WriteCompareResult(const Decoder::Operand& operand, IR::U1 value);

	IR::MemoryFlags AddMemoryInfo(const IR::MemoryInfo& memory, uint32_t pc);
	IR::ExportFlags AddExportInfo(const Decoder::Instruction& inst);
	struct AddressOperands {
		IR::Value resource;
		IR::Value low;
		IR::Value high;
	};
	struct BufferAddress {
		IR::U32 index;
		IR::U32 offset;
		IR::U32 soffset;
	};
	AddressOperands ReadAddressOperands(const Decoder::Instruction& inst, uint32_t first_source);
	IR::U32         GetResourceDword(uint32_t index, uint32_t dword);
	IR::Value       GetBufferResource(const IR::MemoryInfo& memory);
	IR::Value       GetAddressResource(IR::Value low, IR::Value high);
	IR::Value       GetScalarAddressResource(uint32_t base);
	IR::Value       GetImageResource(const IR::MemoryInfo& memory);
	IR::Value       GetSamplerResource(const IR::MemoryInfo& memory);
	IR::Value MakeImageAddress(const Decoder::Instruction& inst, const Decoder::Operand& base);
	IR::Value ConstructU32x4(const Decoder::Operand& base, uint32_t count);
	void      WriteImageComponents(const Decoder::Operand& dst, IR::Value value,
	                               const IR::MemoryInfo& memory, uint32_t component_limit);
	IR::ValueOpcode ImageAtomicOpcode(Decoder::Opcode opcode);
	BufferAddress   ReadBufferAddress(const Decoder::Instruction& inst, uint32_t source_offset);
	IR::U32         WidenSubdword(IR::Value value, uint32_t bits, bool sign);
	IR::Value       NarrowSubdword(IR::U32 value, uint32_t bits);
	IR::ValueOpcode BufferAtomicOpcode(Decoder::Opcode opcode);
	IR::ValueOpcode SharedAtomicOpcode(Decoder::Opcode opcode);
	bool            TranslateScalarMemory(const Decoder::Instruction& inst, std::string* error);
	bool            TranslateBufferLoad(const Decoder::Instruction& inst, std::string* error);
	bool            TranslateBufferStore(const Decoder::Instruction& inst, std::string* error);
	bool            TranslateAtomicMemory(const Decoder::Instruction& inst, std::string* error);
	bool            TranslateFlatLoad(const Decoder::Instruction& inst, std::string* error);
	bool            TranslateFlatStore(const Decoder::Instruction& inst, std::string* error);
	bool            TranslateImageMemory(const Decoder::Instruction& inst, std::string* error);
	bool            TranslateSharedMemory(const Decoder::Instruction& inst, std::string* error);

	IR::F32 SelectF32(IR::U1 condition, IR::F32 true_value, IR::F32 false_value);
	IR::U32 ConvertF32ToU32Saturated(IR::F32 value, float upper_bound, float safe_upper,
	                                 uint32_t high_result);
	IR::U32 ConvertF32ToI32Saturated(IR::F32 value, float lower_bound, float upper_bound,
	                                 float safe_upper, uint32_t lower_result,
	                                 uint32_t upper_result);
	IR::U32 PackU16Lanes(IR::U32 low, IR::U32 high);

	bool   TranslateStateOperation(const Decoder::Instruction& inst);
	bool   TranslateControlOperation(const Decoder::Instruction& inst);
	bool   TranslateMove(const Decoder::Instruction& inst, std::string* error);
	bool   TranslateLaneOperation(const Decoder::Instruction& inst, std::string* error);
	bool   TranslateAttributeOperation(const Decoder::Instruction& inst, std::string* error);
	bool   TranslateMemoryOperation(const Decoder::Instruction& inst, std::string* error);
	bool   TranslateIntegerCompare(const Decoder::Instruction& inst);
	bool   TranslateInteger16Compare(const Decoder::Instruction& inst);
	bool   TranslateFloatCompare(const Decoder::Instruction& inst);
	bool   TranslateConversion(const Decoder::Instruction& inst);
	bool   TranslateInteger16Operation(const Decoder::Instruction& inst);
	bool   TranslatePackedInteger16(const Decoder::Instruction& inst);
	bool   TranslatePackedFloat16(const Decoder::Instruction& inst);
	bool   TranslateFloat16Operation(const Decoder::Instruction& inst);
	bool   TranslateFloatOperation(const Decoder::Instruction& inst);
	IR::U1 EvaluateU64Mask(const Decoder::Instruction& inst);
	bool   TranslateU64MaskOperation(const Decoder::Instruction& inst);
	bool   TranslateSimpleInteger(const Decoder::Instruction& inst);
	bool   TranslateComposedInteger(const Decoder::Instruction& inst);
	bool   TranslateExtendedInteger(const Decoder::Instruction& inst);

	IR::Program&    program;
	IR::IREmitter   ir;
	Decoder::Opcode current_opcode       = Decoder::Opcode::UNKNOWN;
	uint32_t        current_pc           = 0;
	uint32_t        current_vector_limit = 1;
	uint32_t        current_wave_size    = 64;
};

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
