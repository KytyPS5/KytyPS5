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

	void TranslateInstruction(const Decoder::Instruction& inst);
	void TranslateEmbeddedFetch(const Decoder::Instruction& inst, uint32_t attribute,
	                            uint32_t component_count);
	void AddBranchCondition(const CFG::BasicBlock& source, IR::BlockInfo& info);

private:
	struct ScalarU64 {
		std::array<IR::U32, 2> raw;
		// Raw words and Kyty's per-invocation mask model remain independent.
		// mask_valid selects invocation for logical consumers and SCC.
		IR::U1 invocation;
		IR::U1 mask_valid;
	};

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
	void                   Write16Bits(const Decoder::Operand& operand, IR::U32 value);
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
	void      WriteU32Pair(const Decoder::Operand& operand, const std::array<IR::U32, 2>& value);
	ScalarU64 ReadScalarU64(const Decoder::Operand& operand);
	ScalarU64 MakeScalarU64Mask(IR::U1 value);
	ScalarU64 MakeScalarU64Result(const std::array<IR::U32, 2>& raw, IR::U1 invocation,
	                              IR::U1 mask_valid);
	ScalarU64 SelectScalarU64(IR::U1 condition, const ScalarU64& lhs, const ScalarU64& rhs);
	ScalarU64 NotScalarU64(const ScalarU64& value);
	ScalarU64 BinaryScalarU64(const ScalarU64& lhs, const ScalarU64& rhs,
	                          IR::ValueOpcode logical_opcode, IR::ValueOpcode bit_opcode,
	                          bool negate_lhs, bool negate_rhs, bool negate_result);
	void      WriteScalarU64(const Decoder::Operand& operand, const ScalarU64& value);
	IR::U1    ScalarU64NonZero(const ScalarU64& value);
	IR::U1    ReadCondition(const Decoder::Operand& operand);
	IR::U32   ConditionBit(const Decoder::Operand& operand);
	IR::U1    ReadMask(const Decoder::Operand& operand);
	IR::U1    ReadMaskValid(const Decoder::Operand& operand);
	void      WriteMask(const Decoder::Operand& operand, IR::U1 value);
	void      WriteMask64(const Decoder::Operand& operand, IR::U1 value);
	void      WriteCompareResult(const Decoder::Operand& operand, IR::U1 value);

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
	IR::Value       GetScalarAddressResource(const Decoder::Operand& base);
	IR::Value       GetImageResource(const IR::MemoryInfo& memory);
	IR::Value       GetSamplerResource(const IR::MemoryInfo& memory);
	IR::Value     MakeImageAddress(const Decoder::Instruction& inst, const Decoder::Operand& base);
	IR::Value     ConstructU32x4(const Decoder::Operand& base, uint32_t count);
	void          WriteImageComponents(const Decoder::Operand& dst, IR::Value value,
	                                   const IR::MemoryInfo& memory, uint32_t component_limit);
	BufferAddress ReadBufferAddress(const Decoder::Instruction& inst, uint32_t source_offset);
	IR::U32       WidenSubdword(IR::Value value, uint32_t bits, bool sign);
	IR::Value     NarrowSubdword(IR::U32 value, uint32_t bits);
	bool          S_LOAD(const Decoder::Instruction& inst, bool raw);
	bool          BUFFER_LOAD(const Decoder::Instruction& inst);
	bool          BUFFER_STORE(const Decoder::Instruction& inst);
	bool          BUFFER_ATOMIC(const Decoder::Instruction& inst, IR::ValueOpcode opcode);
	bool          IMAGE_ATOMIC(const Decoder::Instruction& inst, IR::ValueOpcode opcode);
	bool DS_ATOMIC(const Decoder::Instruction& inst, IR::ValueOpcode opcode, bool returns_value);
	bool FLAT_LOAD(const Decoder::Instruction& inst);
	bool FLAT_STORE(const Decoder::Instruction& inst);
	bool IMAGE_GET_RESINFO(const Decoder::Instruction& inst);
	bool IMAGE_GET_LOD(const Decoder::Instruction& inst);
	bool IMAGE_LOAD(const Decoder::Instruction& inst);
	bool IMAGE_STORE(const Decoder::Instruction& inst);
	bool IMAGE_SAMPLE(const Decoder::Instruction& inst);
	bool IMAGE_GATHER(const Decoder::Instruction& inst);
	IR::Value LoadSharedU32(uint32_t width, IR::U32 address, const IR::MemoryInfo& memory,
	                        uint32_t pc);
	IR::Value ExtractSharedU32(IR::Value value, uint32_t width, uint32_t index);
	void WriteSharedU32(uint32_t width, IR::U32 address, const std::array<IR::Value, 4>& values,
	                    const IR::MemoryInfo& memory, uint32_t pc);
	bool DS_READ(const Decoder::Instruction& inst);
	bool DS_READ2(const Decoder::Instruction& inst);
	bool DS_WRITE(const Decoder::Instruction& inst);
	bool DS_WRITE2(const Decoder::Instruction& inst);
	bool DS_MINMAX_F32(const Decoder::Instruction& inst, IR::ValueOpcode opcode);
	bool DS_APPEND_CONSUME(const Decoder::Instruction& inst, IR::ValueOpcode opcode);
	bool DS_ADDTID(const Decoder::Instruction& inst, bool write);
	bool DS_SWIZZLE_B32(const Decoder::Instruction& inst);
	bool DS_BPERMUTE_B32(const Decoder::Instruction& inst);

	IR::F32 SelectF32(IR::U1 condition, IR::F32 true_value, IR::F32 false_value);
	IR::U32 ConvertF32ToU32Saturated(IR::F32 value, float upper_bound, float safe_upper,
	                                 uint32_t high_result);
	IR::U32 ConvertF32ToI32Saturated(IR::F32 value, float lower_bound, float upper_bound,
	                                 float safe_upper, uint32_t lower_result,
	                                 uint32_t upper_result);
	IR::U32 PackU16Lanes(IR::U32 low, IR::U32 high);
	void EmitCompareResult(const Decoder::Instruction& inst, IR::U1 value, bool scalar, bool cmpx);
	void EmitCompareConstant(const Decoder::Instruction& inst, bool value, bool scalar, bool cmpx);
	void EmitIntegerCompare(const Decoder::Instruction& inst, IR::ValueOpcode opcode, IR::Type type,
	                        bool scalar, bool cmpx);
	void EmitInteger16Compare(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
	                          bool signed_value, bool cmpx);
	void EmitFloatCompare(const Decoder::Instruction& inst, IR::ValueOpcode opcode, bool half,
	                      bool cmpx);
	void EmitFloatOrderedCompare(const Decoder::Instruction& inst, bool ordered);
	void EmitFloatClassCompare(const Decoder::Instruction& inst, bool cmpx);
	void V_CVT_F32_UBYTE(const Decoder::Instruction& inst, uint32_t byte_index);
	void V_CVT_F32_U32(const Decoder::Instruction& inst);
	void V_CVT_F32_I32(const Decoder::Instruction& inst);
	void V_CVT_U32_F32(const Decoder::Instruction& inst);
	void V_CVT_I32_F32(const Decoder::Instruction& inst);
	void V_CVT_F16_F32(const Decoder::Instruction& inst);
	void V_CVT_F32_F16(const Decoder::Instruction& inst);
	void V_CVT_F16_16(const Decoder::Instruction& inst, bool signed_value);
	void V_CVT_16_F16(const Decoder::Instruction& inst, bool signed_value);
	void V_CVT_RPI_I32_F32(const Decoder::Instruction& inst);
	void V_CVT_FLR_I32_F32(const Decoder::Instruction& inst);
	void V_FREXP_EXP_I32_F32(const Decoder::Instruction& inst);
	void V_CVT_OFF_F32_I4(const Decoder::Instruction& inst);
	void V_CVT_PKRTZ_F16_F32(const Decoder::Instruction& inst);
	void V_CVT_PKNORM_F32(const Decoder::Instruction& inst, IR::ValueOpcode opcode);
	void V_CVT_PK_U8_F32(const Decoder::Instruction& inst);
	void V_PACK_B32_F16(const Decoder::Instruction& inst);
	bool PackedFloat16(const Decoder::Instruction& inst, IR::ValueOpcode opcode, bool accumulator,
	                   bool quiet_snan);
	bool Float16Unary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
	                  bool invalid_negative);
	bool Float16Trig(const Decoder::Instruction& inst, IR::ValueOpcode opcode);
	bool Float16Binary(const Decoder::Instruction& inst, IR::ValueOpcode opcode, bool reverse);
	bool Float16Ternary(const Decoder::Instruction& inst, IR::ValueOpcode opcode, bool accumulator,
	                    bool mix);
	bool FloatUnary(const Decoder::Instruction& inst, IR::ValueOpcode opcode);
	bool FloatBinary(const Decoder::Instruction& inst, IR::ValueOpcode opcode, bool reverse);
	bool FloatTernary(const Decoder::Instruction& inst, IR::ValueOpcode opcode, bool accumulator,
	                  bool mix);
	bool V_FREXP_MANT_F32(const Decoder::Instruction& inst);
	bool V_DOT2C_F32_F16(const Decoder::Instruction& inst);
	bool V_CUBEID_F32(const Decoder::Instruction& inst);
	bool V_CUBESC_F32(const Decoder::Instruction& inst);
	bool V_CUBETC_F32(const Decoder::Instruction& inst);
	bool V_CUBEMA_F32(const Decoder::Instruction& inst);
	bool FloatCube(const Decoder::Instruction& inst, uint32_t result_kind);
	bool Integer16Shift(const Decoder::Instruction& inst, IR::ValueOpcode opcode, bool arithmetic);
	bool Integer16Binary(const Decoder::Instruction& inst, IR::ValueOpcode opcode, bool sign);
	bool V_MED3_I16(const Decoder::Instruction& inst);
	bool PackedInteger16Shift(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
	                          bool arithmetic);
	bool PackedInteger16Binary(const Decoder::Instruction& inst, IR::ValueOpcode opcode);
	bool PackedInteger16Mad(const Decoder::Instruction& inst, bool sign);
	bool PackedInteger16MinMax(const Decoder::Instruction& inst, IR::ValueOpcode opcode, bool sign);
	bool S_U64_MASK(const Decoder::Instruction& inst, IR::ValueOpcode logical_opcode,
	                IR::ValueOpcode bit_opcode, bool negate_rhs, bool negate_result, bool unary);
	bool SimpleInteger(const Decoder::Instruction& inst, IR::ValueOpcode opcode, IR::Type type,
	                   bool reverse, bool mask_shift_count, bool update_scc);
	bool ComposedIntegerBinary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
	                           bool negate_rhs, bool negate_result, bool update_scc);
	bool V_AND_OR_B32(const Decoder::Instruction& inst);
	bool V_OR3_B32(const Decoder::Instruction& inst);
	bool V_XOR3_B32(const Decoder::Instruction& inst);
	bool S_FF1_I32_B64(const Decoder::Instruction& inst);
	bool V_FFBH_32(const Decoder::Instruction& inst, bool sign);
	bool S_FLBIT_I32_B64(const Decoder::Instruction& inst);
	bool Integer24(const Decoder::Instruction& inst, bool sign, bool addend);
	bool V_MAD_U64_U32(const Decoder::Instruction& inst);
	bool V_SAD_U32(const Decoder::Instruction& inst);
	bool V_ADD3_U32(const Decoder::Instruction& inst);
	bool S_BITSET_B32(const Decoder::Instruction& inst, bool set);
	bool V_BCNT_U32_B32(const Decoder::Instruction& inst);
	bool V_MBCNT_U32_B32(const Decoder::Instruction& inst, bool low);
	bool S_BITREPLICATE_B64_B32(const Decoder::Instruction& inst);
	bool S_QUADMASK_B64(const Decoder::Instruction& inst);
	bool BFM_B32(const Decoder::Instruction& inst);
	IR::U32 RightMask32(IR::U32 count);
	IR::U64 RightMask64(IR::U32 count);
	bool    S_BFM_B64(const Decoder::Instruction& inst);
	bool    S_BFE_U32(const Decoder::Instruction& inst, bool sign);
	bool    S_BFE_U64(const Decoder::Instruction& inst);
	bool    V_BFE_U32(const Decoder::Instruction& inst, bool sign);
	bool    V_BFI_B32(const Decoder::Instruction& inst);
	bool    S_BITCMP_B32(const Decoder::Instruction& inst, bool expected);
	bool    V_ALIGNBIT_B32(const Decoder::Instruction& inst);
	bool    V_ALIGNBYTE_B32(const Decoder::Instruction& inst);
	bool    V_LSHL_ADD_U32(const Decoder::Instruction& inst);
	bool    V_ADD_LSHL_U32(const Decoder::Instruction& inst);
	bool    V_XAD_U32(const Decoder::Instruction& inst);
	bool    V_LSHL_OR_B32(const Decoder::Instruction& inst);
	bool    V_CNDMASK_B32(const Decoder::Instruction& inst);
	bool    PackB16(const Decoder::Instruction& inst, bool high0, bool high1);

	void S_SAVEEXEC(const Decoder::Instruction& inst, IR::ValueOpcode operation, bool negate_exec,
	                bool negate_source, bool write_64);
	void ADD_U32(const Decoder::Instruction& inst, bool vector, bool use_carry_in);
	void SUB_U32(const Decoder::Instruction& inst, bool vector, bool reverse);
	void SUBB_U32(const Decoder::Instruction& inst, bool vector);
	void S_ADD_SUB_I32(const Decoder::Instruction& inst, bool subtract);
	void S_LSHL_ADD_U32(const Decoder::Instruction& inst, uint32_t shift_amount);
	void ScalarMinMax32(const Decoder::Instruction& inst, IR::ValueOpcode value_opcode,
	                    IR::ValueOpcode compare_opcode);
	void EmitControlNop();
	void EmitWaitcnt();
	void S_BARRIER();
	void S_SENDMSG();
	void S_TTRACEDATA();
	void S_INST_PREFETCH();
	void S_GETPC_B64(const Decoder::Instruction& inst);
	void S_CSELECT_B32(const Decoder::Instruction& inst);
	void S_CSELECT_B64(const Decoder::Instruction& inst);
	void MOV_B32(const Decoder::Instruction& inst, bool apply_float_modifiers);
	void S_MOV_B64(const Decoder::Instruction& inst);
	void S_WQM_B64(const Decoder::Instruction& inst);
	void V_MOVRELS_B32(const Decoder::Instruction& inst);
	void V_MOVRELD_B32(const Decoder::Instruction& inst);
	void V_READFIRSTLANE_B32(const Decoder::Instruction& inst);
	void V_READLANE_B32(const Decoder::Instruction& inst);
	void V_WRITELANE_B32(const Decoder::Instruction& inst);
	void V_PERMLANE16_B32(const Decoder::Instruction& inst, bool x16);
	void V_INTERP_P1_F32();
	void V_INTERP_P2_F32(const Decoder::Instruction& inst);
	void V_INTERP_MOV_F32(const Decoder::Instruction& inst);
	void EXP(const Decoder::Instruction& inst);

	bool EmitScalar(const Decoder::Instruction& inst);
	bool EmitVector(const Decoder::Instruction& inst);
	bool EmitInterpolation(const Decoder::Instruction& inst);
	bool EmitMemory(const Decoder::Instruction& inst);

	IR::Program&    program;
	IR::IREmitter   ir;
	Decoder::Opcode current_opcode       = Decoder::Opcode::UNKNOWN;
	uint32_t        current_pc           = 0;
	uint32_t        current_vector_limit = 1;
	uint32_t        current_wave_size    = 64;
};

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
