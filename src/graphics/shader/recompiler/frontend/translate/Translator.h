#pragma once

#include "graphics/shader/recompiler/frontend/translate/Translate.h"
#include "graphics/shader/recompiler/ir/IREmitter.h"

#include <array>

namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail {

struct ScalarMemorySourceValues {
	IR::Value resource;
	IR::U32   offset;
};

class Translator {
public:
	Translator(IR::ValueProgram& program, IR::Block* block, uint32_t vector_limit,
	           uint32_t wave_size, bool per_invocation_masks)
	    : value_program(program), ir(block), current_vector_limit(vector_limit),
	      current_wave_size(wave_size), current_per_invocation_masks(per_invocation_masks) {}

	bool TranslateBlock(const IR::BasicBlock& source, std::string* error);
	bool AddBranchCondition(const IR::BasicBlock& source, IR::ValueBlockInfo& info,
	                        std::string* error);

private:
	IR::Operand            OffsetOperand(const IR::Operand& operand, uint32_t offset);
	IR::Operand            PlainOperand(const IR::Operand& operand);
	IR::U32                ReadPcRelativeU32(const IR::Operand& operand);
	std::array<IR::U32, 2> BallotMask(IR::U1 value);
	IR::U32                ReadRawU32(const IR::Operand& operand);
	IR::U32                ReadScalarCode(uint32_t code);
	IR::U32                ApplyBitSourceModifiers(const IR::Operand& operand, IR::U32 value);
	IR::Value              ReadOperand(const IR::Operand& operand, IR::Type type);
	IR::U1                 ThreadBit(IR::U32 low, IR::U32 high);
	void                   WriteRawU32(const IR::Operand& operand, IR::U32 value);
	IR::U64                ExpandWholeQuadMask(IR::U64 value);
	IR::F32                ApplyF32ResultModifiers(const IR::Operand& operand, IR::F32 value);
	void                   WriteOperand(const IR::Operand& operand, IR::Value value);
	IR::U32                PackHalf2x16(IR::F32 low, IR::F32 high);
	void                   WriteF16(const IR::Operand& operand, IR::F32 value);
	void                   WriteU16(const IR::Operand& operand, IR::U32 value);
	IR::U32                ReadU32(const IR::Operand& operand);
	IR::U64                ReadU64(const IR::Operand& operand);
	IR::F32                ReadF16LaneAsF32(const IR::Operand& operand, bool high_lane);
	IR::F32                ReadF16AsF32(const IR::Operand& operand);
	IR::F32                ReadMixF32(const IR::Operand& operand);
	IR::U32                ReadU16LaneRaw(const IR::Operand& operand, bool high_lane);
	IR::U32 ReadU16LaneAsU32(const IR::Operand& operand, bool high_lane, bool sign_extend);
	IR::U32 ReadU16AsU32(const IR::Operand& operand, bool sign_extend);
	IR::U32 ReadF16LaneBits(const IR::Operand& operand, bool high_lane);
	std::array<IR::U32, 2> UnpackU64(IR::U64 value);
	IR::U1                 ReadCondition(const IR::Operand& operand);
	IR::U32                ConditionBit(const IR::Operand& operand);
	IR::U1                 ReadMask(const IR::Operand& operand);
	void                   WriteMask(const IR::Operand& operand, IR::U1 value);
	void                   WriteMask64(const IR::Operand& operand, IR::U1 value);
	void                   WriteCompareResult(const IR::Operand& operand, IR::U1 value);

	IR::MemoryFlags AddMemoryInfo(const IR::Instruction& inst);
	IR::ExportFlags AddExportInfo(const IR::Instruction& inst);
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
	AddressOperands ReadAddressOperands(const IR::Instruction& inst, uint32_t first_source);
	IR::U32         GetResourceDword(uint32_t index, uint32_t dword);
	IR::Value       GetBufferResource(const IR::MemoryInfo& memory);
	IR::Value       GetAddressResource(IR::Value low, IR::Value high);
	IR::Value       GetScalarAddressResource(uint32_t base);
	IR::Value       GetImageResource(const IR::MemoryInfo& memory);
	IR::Value       GetSamplerResource(const IR::MemoryInfo& memory);
	IR::Value       GetSharedResource(IR::ResourceKind kind);
	IR::Value       MakeImageAddress(const IR::Instruction& inst, const IR::Operand& base);
	IR::Value       ConstructU32x4(const IR::Operand& base, uint32_t count);
	void            WriteImageComponents(const IR::Operand& dst, IR::Value value, uint32_t dmask,
	                                     uint32_t component_limit);
	IR::ValueOpcode ImageAtomicOpcode(IR::Opcode opcode);
	BufferAddress   ReadBufferAddress(const IR::Instruction& inst, uint32_t source_offset);
	IR::U32         WidenSubdword(IR::Value value, uint32_t bits, bool sign);
	IR::Value       NarrowSubdword(IR::U32 value, uint32_t bits);
	IR::ValueOpcode BufferAtomicOpcode(IR::Opcode opcode);
	IR::ValueOpcode SharedAtomicOpcode(IR::Opcode opcode, bool gds);
	bool            IsScalarMemoryLoadOperation(IR::Opcode opcode);
	ScalarMemorySourceValues ReadScalarMemorySource(const IR::Instruction& inst);
	bool                     TranslateScalarMemory(const IR::Instruction&          inst,
	                                               const ScalarMemorySourceValues* source_snapshot);
	bool                     TranslateBufferLoad(const IR::Instruction& inst);
	bool                     TranslateBufferStore(const IR::Instruction& inst);
	bool                     TranslateAtomicMemory(const IR::Instruction& inst);
	bool                     TranslateFlatLoad(const IR::Instruction& inst);
	bool                     TranslateFlatStore(const IR::Instruction& inst);
	bool                     TranslateImageMemory(const IR::Instruction& inst);
	bool                     TranslateSharedMemory(const IR::Instruction& inst);

	IR::F32 SelectF32(IR::U1 condition, IR::F32 true_value, IR::F32 false_value);
	IR::U32 ConvertF32ToU32Saturated(IR::F32 value, float upper_bound, float safe_upper,
	                                 uint32_t high_result);
	IR::U32 ConvertF32ToI32Saturated(IR::F32 value, float lower_bound, float upper_bound,
	                                 float safe_upper, uint32_t lower_result,
	                                 uint32_t upper_result);
	IR::U32 PackU16Lanes(IR::U32 low, IR::U32 high);

	bool TranslateInstruction(const IR::Instruction&          inst,
	                          const ScalarMemorySourceValues* scalar_source_snapshot);
	bool TranslateStateOperation(const IR::Instruction& inst);
	bool TranslateControlOperation(const IR::Instruction& inst);
	bool TranslateMove(const IR::Instruction& inst);
	bool TranslateLaneOperation(const IR::Instruction& inst);
	bool TranslateAttributeOperation(const IR::Instruction& inst);
	bool TranslateMemoryOperation(const IR::Instruction&          inst,
	                              const ScalarMemorySourceValues* scalar_source_snapshot);
	bool TranslateIntegerCompare(const IR::Instruction& inst);
	bool TranslateInteger16Compare(const IR::Instruction& inst);
	bool TranslateFloatCompare(const IR::Instruction& inst);
	bool TranslateConversion(const IR::Instruction& inst);
	bool TranslateInteger16Operation(const IR::Instruction& inst);
	bool TranslatePackedInteger16(const IR::Instruction& inst);
	bool TranslatePackedFloat16(const IR::Instruction& inst);
	bool TranslateFloat16Operation(const IR::Instruction& inst);
	bool TranslateFloatOperation(const IR::Instruction& inst);
	bool TranslatePerInvocationU64Mask(const IR::Instruction& inst);
	bool TranslateU64MaskOperation(const IR::Instruction& inst);
	bool TranslateSimpleInteger(const IR::Instruction& inst);
	bool TranslateComposedInteger(const IR::Instruction& inst);
	bool TranslateExtendedInteger(const IR::Instruction& inst);

	IR::ValueProgram& value_program;
	IR::IREmitter     ir;
	IR::Opcode        current_opcode               = IR::Opcode::ControlNop;
	uint32_t          current_pc                   = 0;
	uint32_t          current_vector_limit         = 1;
	uint32_t          current_wave_size            = 64;
	bool              current_per_invocation_masks = false;
};

} // namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail
