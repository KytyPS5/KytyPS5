#include "graphics/shader/recompiler/ScalarAluOps.h"

#include <iterator>

namespace Libs::Graphics::ShaderRecompiler::Decoder {
namespace {

struct OpcodeMap {
	uint32_t opcode = 0;
	Opcode   ir     = Opcode::Unknown;
};

constexpr OpcodeMap SOP2_OPS[] = {
    {0x00u, Opcode::SAddU32},       {0x01u, Opcode::SSubU32},       {0x02u, Opcode::SAddI32},
    {0x03u, Opcode::SSubI32},       {0x04u, Opcode::SAddcU32},      {0x05u, Opcode::SSubbU32},
    {0x06u, Opcode::SMinI32},       {0x07u, Opcode::SMinU32},       {0x08u, Opcode::SMaxI32},
    {0x09u, Opcode::SMaxU32},       {0x0au, Opcode::SCselectB32},   {0x0bu, Opcode::SCselectB64},
    {0x0eu, Opcode::SAndB32},       {0x0fu, Opcode::SAndB64},       {0x10u, Opcode::SOrB32},
    {0x11u, Opcode::SOrB64},        {0x12u, Opcode::SXorB32},       {0x13u, Opcode::SXorB64},
    {0x14u, Opcode::SAndn2B32},     {0x15u, Opcode::SAndn2B64},     {0x16u, Opcode::SOrn2B32},
    {0x17u, Opcode::SOrn2B64},      {0x18u, Opcode::SNandB32},      {0x19u, Opcode::SNandB64},
    {0x1au, Opcode::SNorB32},       {0x1bu, Opcode::SNorB64},       {0x1cu, Opcode::SXnorB32},
    {0x1du, Opcode::SXnorB64},      {0x1eu, Opcode::SLshlB32},      {0x1fu, Opcode::SLshlB64},
    {0x20u, Opcode::SLshrB32},      {0x21u, Opcode::SLshrB64},      {0x22u, Opcode::SAshrI32},
    {0x24u, Opcode::SBfmB32},       {0x25u, Opcode::SBfmB64},       {0x26u, Opcode::SMulI32},
    {0x27u, Opcode::SBfeU32},       {0x29u, Opcode::SBfeU64},       {0x2eu, Opcode::SLshl1AddU32},
    {0x2fu, Opcode::SLshl2AddU32},  {0x30u, Opcode::SLshl3AddU32},  {0x31u, Opcode::SLshl4AddU32},
    {0x32u, Opcode::SPackLlB32B16}, {0x33u, Opcode::SPackLhB32B16}, {0x34u, Opcode::SPackHhB32B16},
    {0x35u, Opcode::SMulHiU32},
};

constexpr OpcodeMap SOP1_OPS[] = {
    {0x03u, Opcode::SMovB32},
    {0x04u, Opcode::SMovB64},
    {0x07u, Opcode::SNotB32},
    {0x08u, Opcode::SNotB64},
    {0x0au, Opcode::SWqmB64},
    {0x0bu, Opcode::SBrevB32},
    {0x0fu, Opcode::SBcnt1I32B32},
    {0x10u, Opcode::SBcnt1I32B64},
    {0x13u, Opcode::SFf1I32B32},
    {0x16u, Opcode::SFlbitI32B64},
    {0x1bu, Opcode::SBitset0B32},
    {0x1du, Opcode::SBitset1B32},
    {0x1fu, Opcode::SGetpcB64},
    {0x20u, Opcode::SSetpcB64},
    {0x24u, Opcode::SAndSaveexecB64},
    {0x28u, Opcode::SOrn2SaveexecB64},
    {0x34u, Opcode::SAbsI32},
    {0x37u, Opcode::SAndn1SaveexecB64},
    {0x3bu, Opcode::SBitreplicateB64B32},
    {0x3cu, Opcode::SAndSaveexecB32},
    {0x44u, Opcode::SAndn1SaveexecB32},
};

constexpr OpcodeMap SOPC_OPS[] = {
    {0x00u, Opcode::SCmpEqI32},   {0x01u, Opcode::SCmpLgI32},   {0x02u, Opcode::SCmpGtI32},
    {0x03u, Opcode::SCmpGeI32},   {0x04u, Opcode::SCmpLtI32},   {0x05u, Opcode::SCmpLeI32},
    {0x06u, Opcode::SCmpEqU32},   {0x07u, Opcode::SCmpLgU32},   {0x08u, Opcode::SCmpGtU32},
    {0x09u, Opcode::SCmpGeU32},   {0x0au, Opcode::SCmpLtU32},   {0x0bu, Opcode::SCmpLeU32},
    {0x0cu, Opcode::SBitcmp0B32}, {0x0du, Opcode::SBitcmp1B32}, {0x13u, Opcode::SCmpLgU64},
};

constexpr OpcodeMap SOPK_OPS[] = {
    {0x00u, Opcode::SMovkI32},   {0x03u, Opcode::SCmpEqI32}, {0x04u, Opcode::SCmpLgI32},
    {0x05u, Opcode::SCmpGtI32},  {0x06u, Opcode::SCmpGeI32}, {0x07u, Opcode::SCmpLtI32},
    {0x08u, Opcode::SCmpLeI32},  {0x09u, Opcode::SCmpEqU32}, {0x0au, Opcode::SCmpLgU32},
    {0x0bu, Opcode::SCmpGtU32},  {0x0cu, Opcode::SCmpGeU32}, {0x0du, Opcode::SCmpLtU32},
    {0x0eu, Opcode::SCmpLeU32},  {0x0fu, Opcode::SAddI32},   {0x10u, Opcode::SMulkI32},
    {0x13u, Opcode::SSetregB32}, {0x17u, Opcode::SWaitcnt},  {0x18u, Opcode::SWaitcnt},
    {0x19u, Opcode::SWaitcnt},   {0x1au, Opcode::SWaitcnt},
};

constexpr OpcodeMap SOPP_OPS[] = {
    {0x00u, Opcode::SNop},          {0x01u, Opcode::SEndpgm},       {0x02u, Opcode::SBranch},
    {0x04u, Opcode::SCbranchScc0},  {0x05u, Opcode::SCbranchScc1},  {0x06u, Opcode::SCbranchVccz},
    {0x07u, Opcode::SCbranchVccnz}, {0x08u, Opcode::SCbranchExecz}, {0x09u, Opcode::SCbranchExecnz},
    {0x0au, Opcode::SBarrier},      {0x0cu, Opcode::SWaitcnt},      {0x0eu, Opcode::SSleep},
    {0x10u, Opcode::SSendmsg},      {0x16u, Opcode::STtraceData},   {0x20u, Opcode::SInstPrefetch},
};

Opcode Lookup(const OpcodeMap* ops, uint32_t count, uint32_t opcode) {
	for (uint32_t i = 0; i < count; i++) {
		if (ops[i].opcode == opcode) {
			return ops[i].ir;
		}
	}
	return Opcode::Unsupported;
}

bool DecodeBinarySources(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                         Instruction* inst, uint32_t ssrc0, uint32_t ssrc1, std::string* error) {
	if (!DecodeScalarSource(ssrc0, pc, &inst->src0, error) ||
	    !DecodeScalarSource(ssrc1, pc, &inst->src1, error)) {
		return false;
	}
	inst->src_count = 2;
	return ReadLiteralOperands(code, word_index, inst, error);
}

} // namespace

bool DecodeSop1(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
                std::string* error) {
	const uint32_t word   = code[word_index];
	const uint32_t opcode = (word >> 8u) & 0xffu;
	const uint32_t ssrc0  = word & 0xffu;
	const uint32_t sdst   = (word >> 16u) & 0x7fu;

	inst->pc        = pc;
	inst->word      = word;
	inst->family    = Family::SOP1;
	inst->opcode_id = opcode;
	inst->opcode    = Lookup(SOP1_OPS, static_cast<uint32_t>(std::size(SOP1_OPS)), opcode);
	SetRawWords(inst, code, word_index, 1);

	if (inst->opcode == Opcode::Unsupported) {
		SetUnsupported(inst, Family::SOP1, opcode, "SOP1 opcode is not implemented");
		return true;
	}

	switch (inst->opcode) {
		case Opcode::SGetpcB64:
			inst->src_count = 0;
			return DecodeScalarDestination(sdst, pc, &inst->dst, error);
		case Opcode::SSetpcB64:
			inst->src_count = 1;
			inst->dst.kind  = OperandKind::Null;
			if (!DecodeScalarSource(ssrc0, pc, &inst->src0, error)) {
				return false;
			}
			return ReadLiteralOperands(code, word_index, inst, error);
		default: break;
	}

	if (!DecodeScalarSource(ssrc0, pc, &inst->src0, error) ||
	    !DecodeScalarDestination(sdst, pc, &inst->dst, error)) {
		return false;
	}
	inst->src_count = 1;
	return ReadLiteralOperands(code, word_index, inst, error);
}

bool DecodeSop2(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
                std::string* error) {
	const uint32_t word   = code[word_index];
	const uint32_t opcode = (word >> 23u) & 0x7fu;
	const uint32_t ssrc1  = (word >> 8u) & 0xffu;
	const uint32_t ssrc0  = word & 0xffu;
	const uint32_t sdst   = (word >> 16u) & 0x7fu;

	inst->pc        = pc;
	inst->word      = word;
	inst->family    = Family::SOP2;
	inst->opcode_id = opcode;
	inst->opcode    = Lookup(SOP2_OPS, static_cast<uint32_t>(std::size(SOP2_OPS)), opcode);
	SetRawWords(inst, code, word_index, 1);

	if (inst->opcode == Opcode::Unsupported) {
		SetUnsupported(inst, Family::SOP2, opcode, "SOP2 opcode is not implemented");
		return true;
	}

	if (!DecodeScalarDestination(sdst, pc, &inst->dst, error)) {
		return false;
	}
	return DecodeBinarySources(pc, code, word_index, inst, ssrc0, ssrc1, error);
}

bool DecodeSopk(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
                std::string* error) {
	const uint32_t word   = code[word_index];
	const uint32_t opcode = (word >> 23u) & 0x1fu;
	const uint32_t sdst   = (word >> 16u) & 0x7fu;
	const auto     imm    = static_cast<int16_t>(word & 0xffffu);

	(void)error;

	inst->pc              = pc;
	inst->word            = word;
	inst->family          = Family::SOPK;
	inst->opcode_id       = opcode;
	inst->opcode          = Lookup(SOPK_OPS, static_cast<uint32_t>(std::size(SOPK_OPS)), opcode);
	inst->src0.kind       = OperandKind::IntegerInlineConstant;
	inst->src0.signed_val = imm;
	inst->src0.value      = static_cast<uint32_t>(imm);
	inst->src_count       = 1;
	SetRawWords(inst, code, word_index, 1);

	if (inst->opcode == Opcode::Unsupported) {
		SetUnsupported(inst, Family::SOPK, opcode, "SOPK opcode is not implemented");
		return true;
	}

	switch (inst->opcode) {
		case Opcode::SMovkI32: return DecodeScalarDestination(sdst, pc, &inst->dst, error);
		case Opcode::SWaitcnt: {
			const uint32_t waitcnt = word & 0xffffu;
			inst->dst.kind         = OperandKind::Null;
			inst->src0.signed_val  = static_cast<int32_t>(waitcnt);
			inst->src0.value       = waitcnt;
			inst->src_count        = 1;
			return true;
		}
		case Opcode::SSetregB32:
			inst->dst.kind        = OperandKind::Null;
			inst->src1.kind       = OperandKind::LiteralConstant;
			inst->src1.value      = word & 0xffffu;
			inst->src1.signed_val = static_cast<int32_t>(imm);
			inst->src_count       = 2;
			return DecodeScalarSource(sdst, pc, &inst->src0, error);
		default: break;
	}

	inst->src1 = inst->src0;
	if (!DecodeScalarSource(sdst, pc, &inst->src0, error)) {
		return false;
	}
	if (inst->opcode == Opcode::SAddI32 || inst->opcode == Opcode::SMulkI32) {
		inst->src_count = 2;
		return DecodeScalarDestination(sdst, pc, &inst->dst, error);
	}

	inst->dst.kind  = OperandKind::Scc;
	inst->src_count = 2;
	return true;
}

bool DecodeSopc(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
                std::string* error) {
	const uint32_t word   = code[word_index];
	const uint32_t ssrc1  = (word >> 8u) & 0xffu;
	const uint32_t ssrc0  = word & 0xffu;
	const uint32_t opcode = (word >> 16u) & 0x7fu;

	inst->pc        = pc;
	inst->word      = word;
	inst->family    = Family::SOPC;
	inst->opcode_id = opcode;
	inst->opcode    = Lookup(SOPC_OPS, static_cast<uint32_t>(std::size(SOPC_OPS)), opcode);
	inst->dst.kind  = OperandKind::Scc;
	SetRawWords(inst, code, word_index, 1);

	if (inst->opcode == Opcode::Unsupported) {
		SetUnsupported(inst, Family::SOPC, opcode, "SOPC opcode is not implemented");
		return true;
	}

	return DecodeBinarySources(pc, code, word_index, inst, ssrc0, ssrc1, error);
}

bool DecodeSopp(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
                std::string* error) {
	const uint32_t word   = code[word_index];
	const uint32_t opcode = (word >> 16u) & 0x7fu;
	const uint32_t simm   = word & 0xffffu;

	(void)error;

	inst->pc              = pc;
	inst->word            = word;
	inst->family          = Family::SOPP;
	inst->opcode_id       = opcode;
	inst->opcode          = Lookup(SOPP_OPS, static_cast<uint32_t>(std::size(SOPP_OPS)), opcode);
	inst->src0.kind       = OperandKind::LiteralConstant;
	inst->src0.value      = simm;
	inst->src0.signed_val = static_cast<int16_t>(simm);
	inst->src_count = (inst->opcode == Opcode::SNop || inst->opcode == Opcode::SWaitcnt ||
	                   inst->opcode == Opcode::SSleep || inst->opcode == Opcode::SSendmsg ||
	                   inst->opcode == Opcode::STtraceData || inst->opcode == Opcode::SInstPrefetch)
	                      ? 1
	                      : 0;
	inst->branch_offset = static_cast<int32_t>(static_cast<int16_t>(simm)) * 4;
	inst->branch_target = pc + 4u + static_cast<uint32_t>(inst->branch_offset);
	SetRawWords(inst, code, word_index, 1);

	if (inst->opcode == Opcode::Unsupported) {
		SetUnsupported(inst, Family::SOPP, opcode, "SOPP control-flow opcode is not implemented");
	}
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder
