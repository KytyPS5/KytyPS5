#include "graphics/shader/recompiler/frontend/decode/ScalarAluOps.h"

#include "graphics/shader/recompiler/frontend/decode/OpcodeTable.h"

namespace Libs::Graphics::ShaderRecompiler::Decoder {
namespace {

using Detail::OpcodeMap;

constexpr OpcodeMap SOP2_OPCODE_LIST[] = {
    {0x00u, Opcode::S_ADD_U32},         {0x01u, Opcode::S_SUB_U32},
    {0x02u, Opcode::S_ADD_I32},         {0x03u, Opcode::S_SUB_I32},
    {0x04u, Opcode::S_ADDC_U32},        {0x05u, Opcode::S_SUBB_U32},
    {0x06u, Opcode::S_MIN_I32},         {0x07u, Opcode::S_MIN_U32},
    {0x08u, Opcode::S_MAX_I32},         {0x09u, Opcode::S_MAX_U32},
    {0x0au, Opcode::S_CSELECT_B32},     {0x0bu, Opcode::S_CSELECT_B64},
    {0x0eu, Opcode::S_AND_B32},         {0x0fu, Opcode::S_AND_B64},
    {0x10u, Opcode::S_OR_B32},          {0x11u, Opcode::S_OR_B64},
    {0x12u, Opcode::S_XOR_B32},         {0x13u, Opcode::S_XOR_B64},
    {0x14u, Opcode::S_ANDN2_B32},       {0x15u, Opcode::S_ANDN2_B64},
    {0x16u, Opcode::S_ORN2_B32},        {0x17u, Opcode::S_ORN2_B64},
    {0x18u, Opcode::S_NAND_B32},        {0x19u, Opcode::S_NAND_B64},
    {0x1au, Opcode::S_NOR_B32},         {0x1bu, Opcode::S_NOR_B64},
    {0x1cu, Opcode::S_XNOR_B32},        {0x1du, Opcode::S_XNOR_B64},
    {0x1eu, Opcode::S_LSHL_B32},        {0x1fu, Opcode::S_LSHL_B64},
    {0x20u, Opcode::S_LSHR_B32},        {0x21u, Opcode::S_LSHR_B64},
    {0x22u, Opcode::S_ASHR_I32},        {0x24u, Opcode::S_BFM_B32},
    {0x25u, Opcode::S_BFM_B64},         {0x26u, Opcode::S_MUL_I32},
    {0x27u, Opcode::S_BFE_U32},         {0x28u, Opcode::S_BFE_I32},
    {0x29u, Opcode::S_BFE_U64},         {0x2cu, Opcode::S_ABSDIFF_I32},
    {0x2eu, Opcode::S_LSHL1_ADD_U32},
    {0x2fu, Opcode::S_LSHL2_ADD_U32},   {0x30u, Opcode::S_LSHL3_ADD_U32},
    {0x31u, Opcode::S_LSHL4_ADD_U32},   {0x32u, Opcode::S_PACK_LL_B32_B16},
    {0x33u, Opcode::S_PACK_LH_B32_B16}, {0x34u, Opcode::S_PACK_HH_B32_B16},
    {0x35u, Opcode::S_MUL_HI_U32},
};

constexpr OpcodeMap SOP1_OPCODE_LIST[] = {
    {0x03u, Opcode::S_MOV_B32},
    {0x04u, Opcode::S_MOV_B64},
    {0x06u, Opcode::S_CMOV_B64},
    {0x07u, Opcode::S_NOT_B32},
    {0x08u, Opcode::S_NOT_B64},
    // Wave32 sibling of S_WQM_B64 below (reproduced in ASTRO's Playroom, 2026-09-09) -- the codebase
    // already retrofits wave32 siblings for other originally-64-bit mask ops (S_AND_SAVEEXEC_B32,
    // S_ANDN1_SAVEEXEC_B32 further down this table), this is the same pattern.
    {0x09u, Opcode::S_WQM_B32},
    {0x0au, Opcode::S_WQM_B64},
    {0x0bu, Opcode::S_BREV_B32},
    {0x0fu, Opcode::S_BCNT1_I32_B32},
    {0x10u, Opcode::S_BCNT1_I32_B64},
    {0x13u, Opcode::S_FF1_I32_B32},
    {0x14u, Opcode::S_FF1_I32_B64},
    {0x15u, Opcode::S_FLBIT_I32_B32},
    {0x16u, Opcode::S_FLBIT_I32_B64},
    {0x1bu, Opcode::S_BITSET0_B32},
    {0x1cu, Opcode::S_BITSET0_B64},
    {0x1du, Opcode::S_BITSET1_B32},
    {0x1eu, Opcode::S_BITSET1_B64},
    {0x1fu, Opcode::S_GETPC_B64},
    {0x20u, Opcode::S_SETPC_B64},
    {0x24u, Opcode::S_AND_SAVEEXEC_B64},
    {0x28u, Opcode::S_ORN2_SAVEEXEC_B64},
    {0x2du, Opcode::S_QUADMASK_B64},
    {0x34u, Opcode::S_ABS_I32},
    {0x37u, Opcode::S_ANDN1_SAVEEXEC_B64},
    {0x3bu, Opcode::S_BITREPLICATE_B64_B32},
    {0x3cu, Opcode::S_AND_SAVEEXEC_B32},
    // Confirmed against the RDNA2 ISA manual's SOP1 opcode table (page 251: decimal opcode 64 =
    // S_ORN2_SAVEEXEC_B32), docling-converted rather than trusted from a raw pymupdf read
    // (reproduced in ASTRO's Playroom, 2026-09-09: Astro's Playroom hits this decoding an unrelated shader).
    // Independently landed upstream the same day as 32ea086 -- the opcode number matches exactly.
    {0x40u, Opcode::S_ORN2_SAVEEXEC_B32},
    {0x44u, Opcode::S_ANDN1_SAVEEXEC_B32},
};

constexpr OpcodeMap SOPC_OPCODE_LIST[] = {
    {0x00u, Opcode::S_CMP_EQ_I32},  {0x01u, Opcode::S_CMP_LG_I32},  {0x02u, Opcode::S_CMP_GT_I32},
    {0x03u, Opcode::S_CMP_GE_I32},  {0x04u, Opcode::S_CMP_LT_I32},  {0x05u, Opcode::S_CMP_LE_I32},
    {0x06u, Opcode::S_CMP_EQ_U32},  {0x07u, Opcode::S_CMP_LG_U32},  {0x08u, Opcode::S_CMP_GT_U32},
    {0x09u, Opcode::S_CMP_GE_U32},  {0x0au, Opcode::S_CMP_LT_U32},  {0x0bu, Opcode::S_CMP_LE_U32},
    {0x0cu, Opcode::S_BITCMP0_B32}, {0x0du, Opcode::S_BITCMP1_B32}, {0x12u, Opcode::S_CMP_EQ_U64},
    {0x13u, Opcode::S_CMP_LG_U64},
};

constexpr OpcodeMap SOPK_OPCODE_LIST[] = {
    {0x00u, Opcode::S_MOVK_I32},   {0x03u, Opcode::S_CMP_EQ_I32}, {0x04u, Opcode::S_CMP_LG_I32},
    {0x05u, Opcode::S_CMP_GT_I32}, {0x06u, Opcode::S_CMP_GE_I32}, {0x07u, Opcode::S_CMP_LT_I32},
    {0x08u, Opcode::S_CMP_LE_I32}, {0x09u, Opcode::S_CMP_EQ_U32}, {0x0au, Opcode::S_CMP_LG_U32},
    {0x0bu, Opcode::S_CMP_GT_U32}, {0x0cu, Opcode::S_CMP_GE_U32}, {0x0du, Opcode::S_CMP_LT_U32},
    {0x0eu, Opcode::S_CMP_LE_U32}, {0x0fu, Opcode::S_ADD_I32},    {0x10u, Opcode::S_MULK_I32},
    {0x13u, Opcode::S_SETREG_B32}, {0x17u, Opcode::S_WAITCNT},    {0x18u, Opcode::S_WAITCNT},
    {0x19u, Opcode::S_WAITCNT},    {0x1au, Opcode::S_WAITCNT},
    // RDNA2 ISA manual (docling-converted, page 118, decimal 27/28): a GCN-hardware-only
    // register-pressure workaround that runs a code region in two 32-lane passes when a wave64
    // shader needs more VGPRs than a single wave64 pass affords, saving/restoring EXEC_HI/LO
    // through D0 (SDST) across passes. Mesa ACO's own docs (README-ISA.md, "RDNA subvector
    // mode") say the ISA text on addressing is unclear and that they treat it like S_CBRANCH.
    // Translated as a plain no-op here (reproduced in ASTRO's Playroom, 2026-09-09: Astro's Playroom) --
    // this codebase's SPIR-V backend models per-invocation (per-lane) execution, not a literal
    // fixed-width physical register file, so the entire two-pass split is redundant plumbing
    // that dissolves away on this target, the same way the ESGS ring buffer did for the ES/GS
    // merged-shader case. Confirmed safe for this specific occurrence: the very next instruction
    // in the real decoded stream (V_CMPX_GT_U32) unconditionally recomputes EXEC from scratch,
    // so whatever this instruction would have done to EXEC/D0 is overwritten immediately after.
    {0x1bu, Opcode::S_SUBVECTOR_LOOP_BEGIN}, {0x1cu, Opcode::S_SUBVECTOR_LOOP_END},
};

constexpr OpcodeMap SOPP_OPCODE_LIST[] = {
    {0x00u, Opcode::S_NOP},
    {0x01u, Opcode::S_ENDPGM},
    {0x02u, Opcode::S_BRANCH},
    {0x04u, Opcode::S_CBRANCH_SCC0},
    {0x05u, Opcode::S_CBRANCH_SCC1},
    {0x06u, Opcode::S_CBRANCH_VCCZ},
    {0x07u, Opcode::S_CBRANCH_VCCNZ},
    {0x08u, Opcode::S_CBRANCH_EXECZ},
    {0x09u, Opcode::S_CBRANCH_EXECNZ},
    {0x0au, Opcode::S_BARRIER},
    {0x0cu, Opcode::S_WAITCNT},
    {0x0eu, Opcode::S_SLEEP},
    {0x0fu, Opcode::S_SETPRIO},
    {0x10u, Opcode::S_SENDMSG},
    {0x12u, Opcode::S_TRAP},
    {0x16u, Opcode::S_TTRACEDATA},
    {0x20u, Opcode::S_INST_PREFETCH},
    {0x23u, Opcode::S_WAITCNT_DEPCTR},
};

constexpr auto SOP1_OPS = Detail::MakeOpcodeTable<0x100>(SOP1_OPCODE_LIST);
constexpr auto SOP2_OPS = Detail::MakeOpcodeTable<0x80>(SOP2_OPCODE_LIST);
constexpr auto SOPK_OPS = Detail::MakeOpcodeTable<0x20>(SOPK_OPCODE_LIST);
constexpr auto SOPC_OPS = Detail::MakeOpcodeTable<0x80>(SOPC_OPCODE_LIST);
constexpr auto SOPP_OPS = Detail::MakeOpcodeTable<0x80>(SOPP_OPCODE_LIST);

void DecodeBinarySources(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                         Instruction& inst, uint32_t ssrc0, uint32_t ssrc1) {
	DecodeScalarSource(ssrc0, pc, inst.src0);
	DecodeScalarSource(ssrc1, pc, inst.src1);
	inst.src_count = 2;
	ReadLiteralOperands(code, word_index, inst);
}

} // namespace

void DecodeSop1(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                Instruction& inst) {
	const uint32_t word   = code[word_index];
	const uint32_t opcode = (word >> 8u) & 0xffu;
	const uint32_t ssrc0  = word & 0xffu;
	const uint32_t sdst   = (word >> 16u) & 0x7fu;

	inst.pc        = pc;
	inst.family    = Family::SOP1;
	inst.opcode_id = opcode;
	inst.opcode    = Detail::LookupOpcode(SOP1_OPS, opcode);
	SetRawWords(inst, code, word_index, 1);

	if (inst.opcode == Opcode::UNSUPPORTED) {
		SetUnsupported(inst, Family::SOP1, opcode, "SOP1 opcode is not implemented");
		return;
	}

	switch (inst.opcode) {
		case Opcode::S_GETPC_B64:
			inst.src_count = 0;
			DecodeScalarDestination(sdst, pc, inst.dst);
			return;
		case Opcode::S_SETPC_B64:
			inst.src_count = 1;
			inst.dst.kind  = OperandKind::Null;
			DecodeScalarSource(ssrc0, pc, inst.src0);
			ReadLiteralOperands(code, word_index, inst);
			return;
		default: break;
	}

	DecodeScalarSource(ssrc0, pc, inst.src0);
	DecodeScalarDestination(sdst, pc, inst.dst);
	inst.src_count = 1;
	ReadLiteralOperands(code, word_index, inst);
}

void DecodeSop2(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                Instruction& inst) {
	const uint32_t word   = code[word_index];
	const uint32_t opcode = (word >> 23u) & 0x7fu;
	const uint32_t ssrc1  = (word >> 8u) & 0xffu;
	const uint32_t ssrc0  = word & 0xffu;
	const uint32_t sdst   = (word >> 16u) & 0x7fu;

	inst.pc        = pc;
	inst.family    = Family::SOP2;
	inst.opcode_id = opcode;
	inst.opcode    = Detail::LookupOpcode(SOP2_OPS, opcode);
	SetRawWords(inst, code, word_index, 1);

	if (inst.opcode == Opcode::UNSUPPORTED) {
		SetUnsupported(inst, Family::SOP2, opcode, "SOP2 opcode is not implemented");
		return;
	}

	DecodeScalarDestination(sdst, pc, inst.dst);
	DecodeBinarySources(pc, code, word_index, inst, ssrc0, ssrc1);
}

void DecodeSopk(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                Instruction& inst) {
	const uint32_t word   = code[word_index];
	const uint32_t opcode = (word >> 23u) & 0x1fu;
	const uint32_t sdst   = (word >> 16u) & 0x7fu;
	const auto     imm    = opcode >= 0x09u && opcode <= 0x0eu
	                           ? static_cast<int32_t>(word & 0xffffu)
	                           : static_cast<int32_t>(static_cast<int16_t>(word & 0xffffu));

	inst.pc              = pc;
	inst.family          = Family::SOPK;
	inst.opcode_id       = opcode;
	inst.opcode          = Detail::LookupOpcode(SOPK_OPS, opcode);
	inst.src0.kind       = OperandKind::IntegerInlineConstant;
	inst.src0.signed_val = imm;
	inst.src0.value      = static_cast<uint32_t>(imm);
	inst.src_count       = 1;
	SetRawWords(inst, code, word_index, 1);

	if (inst.opcode == Opcode::UNSUPPORTED) {
		SetUnsupported(inst, Family::SOPK, opcode, "SOPK opcode is not implemented");
		return;
	}

	switch (inst.opcode) {
		case Opcode::S_MOVK_I32: DecodeScalarDestination(sdst, pc, inst.dst); return;
		case Opcode::S_SUBVECTOR_LOOP_BEGIN:
		case Opcode::S_SUBVECTOR_LOOP_END:
			// D0 (SDST) and the SIMM16 branch-shaped literal decoded for disassembly legibility
			// only -- this codebase translates both opcodes as no-ops (see the table comment
			// above), so branch_target is never consulted by CFG building. Computed with the same
			// pc+4+offset*4 formula DecodeSopp uses for real SOPP branches (Mesa ACO: "is
			// equivalent to an S_CBRANCH with extra math").
			DecodeScalarDestination(sdst, pc, inst.dst);
			inst.branch_target = pc + 4u + static_cast<uint32_t>(imm * 4);
			return;
		case Opcode::S_WAITCNT: {
			const uint32_t waitcnt = word & 0xffffu;
			inst.dst.kind          = OperandKind::Null;
			inst.src0.signed_val   = static_cast<int32_t>(waitcnt);
			inst.src0.value        = waitcnt;
			inst.src_count         = 1;
			return;
		}
		case Opcode::S_SETREG_B32:
			inst.dst.kind        = OperandKind::Null;
			inst.src1.kind       = OperandKind::LiteralConstant;
			inst.src1.value      = word & 0xffffu;
			inst.src1.signed_val = static_cast<int32_t>(imm);
			inst.src_count       = 2;
			DecodeScalarSource(sdst, pc, inst.src0);
			return;
		default: break;
	}

	inst.src1 = inst.src0;
	DecodeScalarSource(sdst, pc, inst.src0);
	if (inst.opcode == Opcode::S_ADD_I32 || inst.opcode == Opcode::S_MULK_I32) {
		inst.src_count = 2;
		DecodeScalarDestination(sdst, pc, inst.dst);
		return;
	}

	inst.dst.kind  = OperandKind::Scc;
	inst.src_count = 2;
}

void DecodeSopc(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                Instruction& inst) {
	const uint32_t word   = code[word_index];
	const uint32_t ssrc1  = (word >> 8u) & 0xffu;
	const uint32_t ssrc0  = word & 0xffu;
	const uint32_t opcode = (word >> 16u) & 0x7fu;

	inst.pc        = pc;
	inst.family    = Family::SOPC;
	inst.opcode_id = opcode;
	inst.opcode    = Detail::LookupOpcode(SOPC_OPS, opcode);
	inst.dst.kind  = OperandKind::Scc;
	SetRawWords(inst, code, word_index, 1);

	if (inst.opcode == Opcode::UNSUPPORTED) {
		SetUnsupported(inst, Family::SOPC, opcode, "SOPC opcode is not implemented");
		return;
	}

	DecodeBinarySources(pc, code, word_index, inst, ssrc0, ssrc1);
}

void DecodeSopp(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                Instruction& inst) {
	const uint32_t word   = code[word_index];
	const uint32_t opcode = (word >> 16u) & 0x7fu;
	const uint32_t simm   = word & 0xffffu;

	inst.pc              = pc;
	inst.family          = Family::SOPP;
	inst.opcode_id       = opcode;
	inst.opcode          = Detail::LookupOpcode(SOPP_OPS, opcode);
	inst.src0.kind       = OperandKind::LiteralConstant;
	inst.src0.value      = inst.opcode == Opcode::S_TRAP ? simm & 0xffu : simm;
	inst.src0.signed_val = inst.opcode == Opcode::S_TRAP
	                           ? static_cast<int32_t>(inst.src0.value)
	                           : static_cast<int32_t>(static_cast<int16_t>(simm));
	inst.src_count = (inst.opcode == Opcode::S_NOP || inst.opcode == Opcode::S_WAITCNT ||
	                  inst.opcode == Opcode::S_WAITCNT_DEPCTR || inst.opcode == Opcode::S_SLEEP ||
	                  inst.opcode == Opcode::S_SETPRIO ||
	                  inst.opcode == Opcode::S_SENDMSG || inst.opcode == Opcode::S_TRAP ||
	                  inst.opcode == Opcode::S_TTRACEDATA || inst.opcode == Opcode::S_INST_PREFETCH)
	                     ? 1
	                     : 0;
	const auto branch_offset = static_cast<int32_t>(static_cast<int16_t>(simm)) * 4;
	inst.branch_target = pc + 4u + static_cast<uint32_t>(branch_offset);
	SetRawWords(inst, code, word_index, 1);

	if (inst.opcode == Opcode::UNSUPPORTED) {
		SetUnsupported(inst, Family::SOPP, opcode, "SOPP control-flow opcode is not implemented");
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder
