#include "graphics/shader/recompiler/VectorAluOps.h"

#include <fmt/format.h>
#include <iterator>

namespace Libs::Graphics::ShaderRecompiler::Decoder {
namespace {

struct OpcodeMap {
	uint32_t opcode = 0;
	Opcode   ir     = Opcode::Unknown;
};

constexpr OpcodeMap VOP2_OPS[] = {
    {0x01u, Opcode::VCndmaskB32},    {0x02u, Opcode::VDot2cF32F16},
    {0x03u, Opcode::VAddF32},        {0x04u, Opcode::VSubF32},
    {0x05u, Opcode::VSubrevF32},     {0x08u, Opcode::VMulF32},
    {0x09u, Opcode::VMulI32I24},     {0x0bu, Opcode::VMulU32U24},
    {0x0fu, Opcode::VMinF32},        {0x10u, Opcode::VMaxF32},
    {0x11u, Opcode::VMinI32},        {0x12u, Opcode::VMaxI32},
    {0x13u, Opcode::VMinU32},        {0x14u, Opcode::VMaxU32},
    {0x15u, Opcode::VLshrB32},       {0x16u, Opcode::VLshrrevB32},
    {0x17u, Opcode::VAshrI32},       {0x18u, Opcode::VAshrrevI32},
    {0x19u, Opcode::VLshlB32},       {0x1au, Opcode::VLshlrevB32},
    {0x1bu, Opcode::VAndB32},        {0x1cu, Opcode::VOrB32},
    {0x1du, Opcode::VXorB32},        {0x1eu, Opcode::VXnorB32},
    {0x1fu, Opcode::VMacF32},        {0x20u, Opcode::VMadmkF32},
    {0x21u, Opcode::VMadakF32},      {0x22u, Opcode::VBcntU32B32},
    {0x23u, Opcode::VMbcntLoU32B32}, {0x24u, Opcode::VMbcntHiU32B32},
    {0x25u, Opcode::VAddNcU32},      {0x28u, Opcode::VAddcU32},
    {0x26u, Opcode::VSubNcU32},      {0x27u, Opcode::VSubrevNcU32},
    {0x2bu, Opcode::VMacF32},        {0x2cu, Opcode::VMadmkF32},
    {0x2du, Opcode::VMadakF32},      {0x2fu, Opcode::VCvtPkrtzF16F32},
    {0x32u, Opcode::VAddF16},        {0x33u, Opcode::VSubF16},
    {0x34u, Opcode::VSubrevF16},     {0x35u, Opcode::VMulF16},
    {0x36u, Opcode::VFmacF16},       {0x37u, Opcode::VFmamkF16},
    {0x38u, Opcode::VFmaakF16},      {0x39u, Opcode::VMaxF16},
    {0x3au, Opcode::VMinF16},        {0x3cu, Opcode::VPkFmacF16},
};

constexpr OpcodeMap VOP1_OPS[] = {
    {0x00u, Opcode::VNop},
    {0x01u, Opcode::VMovB32},
    {0x02u, Opcode::VReadfirstlaneB32},
    {0x05u, Opcode::VCvtF32I32},
    {0x06u, Opcode::VCvtF32U32},
    {0x07u, Opcode::VCvtU32F32},
    {0x08u, Opcode::VCvtI32F32},
    {0x0au, Opcode::VCvtF16F32},
    {0x0bu, Opcode::VCvtF32F16},
    {0x0cu, Opcode::VCvtRpiI32F32},
    {0x0du, Opcode::VCvtFlrI32F32},
    {0x0eu, Opcode::VCvtOffF32I4},
    {0x11u, Opcode::VCvtF32Ubyte0},
    {0x12u, Opcode::VCvtF32Ubyte1},
    {0x13u, Opcode::VCvtF32Ubyte2},
    {0x14u, Opcode::VCvtF32Ubyte3},
    {0x2au, Opcode::VRcpF32},
    {0x20u, Opcode::VFractF32},
    {0x21u, Opcode::VTruncF32},
    {0x22u, Opcode::VCeilF32},
    {0x23u, Opcode::VRndneF32},
    {0x24u, Opcode::VFloorF32},
    {0x25u, Opcode::VExpF32},
    {0x27u, Opcode::VLogF32},
    {0x2bu, Opcode::VRcpF32},
    {0x2eu, Opcode::VRsqF32},
    {0x33u, Opcode::VSqrtF32},
    {0x35u, Opcode::VSinF32},
    {0x36u, Opcode::VCosF32},
    {0x37u, Opcode::VNotB32},
    {0x38u, Opcode::VBfrevB32},
    {0x39u, Opcode::VFfbhU32},
    {0x3au, Opcode::VFfblB32},
    {0x42u, Opcode::VMovreldB32},
    {0x43u, Opcode::VMovrelsB32},
    {0x50u, Opcode::VCvtF16U16},
    {0x51u, Opcode::VCvtF16I16},
    {0x52u, Opcode::VCvtU16F16},
    {0x53u, Opcode::VCvtI16F16},
    {0x54u, Opcode::VRcpF16},
    {0x55u, Opcode::VSqrtF16},
    {0x56u, Opcode::VRsqF16},
    {0x57u, Opcode::VLogF16},
    {0x58u, Opcode::VExpF16},
    {0x5bu, Opcode::VFloorF16},
    {0x5cu, Opcode::VCeilF16},
    {0x5du, Opcode::VTruncF16},
    {0x5eu, Opcode::VRndneF16},
};

constexpr OpcodeMap VOP3_ENCODED_VOP1_OPS[] = {
    {0x00u, Opcode::VNop},
    {0x01u, Opcode::VMovB32},
    {0x02u, Opcode::VReadfirstlaneB32},
    {0x05u, Opcode::VCvtF32I32},
    {0x06u, Opcode::VCvtF32U32},
    {0x07u, Opcode::VCvtU32F32},
    {0x08u, Opcode::VCvtI32F32},
    {0x0au, Opcode::VCvtF16F32},
    {0x0cu, Opcode::VCvtRpiI32F32},
    {0x0du, Opcode::VCvtFlrI32F32},
    {0x0eu, Opcode::VCvtOffF32I4},
    {0x2au, Opcode::VRcpF32},
    {0x20u, Opcode::VFractF32},
    {0x21u, Opcode::VTruncF32},
    {0x22u, Opcode::VCeilF32},
    {0x23u, Opcode::VRndneF32},
    {0x24u, Opcode::VFloorF32},
    {0x25u, Opcode::VExpF32},
    {0x27u, Opcode::VLogF32},
    {0x2bu, Opcode::VRcpF32},
    {0x2eu, Opcode::VRsqF32},
    {0x33u, Opcode::VSqrtF32},
    {0x35u, Opcode::VSinF32},
    {0x36u, Opcode::VCosF32},
    {0x37u, Opcode::VNotB32},
    {0x38u, Opcode::VBfrevB32},
    {0x39u, Opcode::VFfbhU32},
    {0x3au, Opcode::VFfblB32},
    {0x42u, Opcode::VMovreldB32},
    {0x43u, Opcode::VMovrelsB32},
    {0x50u, Opcode::VCvtF16U16},
    {0x51u, Opcode::VCvtF16I16},
    {0x52u, Opcode::VCvtU16F16},
    {0x53u, Opcode::VCvtI16F16},
    {0x54u, Opcode::VRcpF16},
    {0x55u, Opcode::VSqrtF16},
    {0x56u, Opcode::VRsqF16},
    {0x57u, Opcode::VLogF16},
    {0x58u, Opcode::VExpF16},
    {0x5bu, Opcode::VFloorF16},
    {0x5cu, Opcode::VCeilF16},
    {0x5du, Opcode::VTruncF16},
    {0x5eu, Opcode::VRndneF16},
};

constexpr OpcodeMap VOPC_OPS[] = {
    {0x00u, Opcode::VCmpFF32},     {0x01u, Opcode::VCmpLtF32},   {0x02u, Opcode::VCmpEqF32},
    {0x03u, Opcode::VCmpLeF32},    {0x04u, Opcode::VCmpGtF32},   {0x05u, Opcode::VCmpLgF32},
    {0x06u, Opcode::VCmpGeF32},    {0x07u, Opcode::VCmpOF32},    {0x08u, Opcode::VCmpUF32},
    {0x09u, Opcode::VCmpNgeF32},   {0x0au, Opcode::VCmpNlgF32},  {0x0bu, Opcode::VCmpNgtF32},
    {0x0cu, Opcode::VCmpNleF32},   {0x0du, Opcode::VCmpNeqF32},  {0x0eu, Opcode::VCmpNltF32},
    {0x0fu, Opcode::VCmpTruF32},   {0x11u, Opcode::VCmpxLtF32},  {0x12u, Opcode::VCmpxEqF32},
    {0x13u, Opcode::VCmpxLeF32},   {0x14u, Opcode::VCmpxGtF32},  {0x15u, Opcode::VCmpxLgF32},
    {0x16u, Opcode::VCmpxGeF32},   {0x19u, Opcode::VCmpxNgeF32}, {0x1au, Opcode::VCmpxNlgF32},
    {0x1bu, Opcode::VCmpxNgtF32},  {0x1cu, Opcode::VCmpxNleF32}, {0x1du, Opcode::VCmpxNeqF32},
    {0x1eu, Opcode::VCmpxNltF32},  {0x80u, Opcode::VCmpFI32},    {0x81u, Opcode::VCmpLtI32},
    {0x82u, Opcode::VCmpEqI32},    {0x83u, Opcode::VCmpLeI32},   {0x84u, Opcode::VCmpGtI32},
    {0x85u, Opcode::VCmpNeI32},    {0x86u, Opcode::VCmpGeI32},   {0x87u, Opcode::VCmpTI32},
    {0x88u, Opcode::VCmpClassF32}, {0x89u, Opcode::VCmpLtI16},   {0x8au, Opcode::VCmpEqI16},
    {0x8bu, Opcode::VCmpLeI16},    {0x8cu, Opcode::VCmpGtI16},   {0x8du, Opcode::VCmpNeI16},
    {0x8eu, Opcode::VCmpGeI16},    {0x91u, Opcode::VCmpxLtI32},  {0x92u, Opcode::VCmpxEqI32},
    {0x93u, Opcode::VCmpxLeI32},   {0x94u, Opcode::VCmpxGtI32},  {0x95u, Opcode::VCmpxNeI32},
    {0x96u, Opcode::VCmpxGeI32},   {0xa9u, Opcode::VCmpLtU16},   {0xaau, Opcode::VCmpEqU16},
    {0xabu, Opcode::VCmpLeU16},    {0xacu, Opcode::VCmpGtU16},   {0xadu, Opcode::VCmpNeU16},
    {0xaeu, Opcode::VCmpGeU16},    {0xc0u, Opcode::VCmpFU32},    {0xc1u, Opcode::VCmpLtU32},
    {0xc2u, Opcode::VCmpEqU32},    {0xc3u, Opcode::VCmpLeU32},   {0xc4u, Opcode::VCmpGtU32},
    {0xc5u, Opcode::VCmpNeU32},    {0xc6u, Opcode::VCmpGeU32},   {0xc7u, Opcode::VCmpTU32},
    {0xd1u, Opcode::VCmpxLtU32},   {0xd2u, Opcode::VCmpxEqU32},  {0xd3u, Opcode::VCmpxLeU32},
    {0xd4u, Opcode::VCmpxGtU32},   {0xd5u, Opcode::VCmpxNeU32},  {0xd6u, Opcode::VCmpxGeU32},
    {0xe5u, Opcode::VCmpNeU64},    {0xc9u, Opcode::VCmpLtF16},   {0xcau, Opcode::VCmpEqF16},
    {0xcbu, Opcode::VCmpLeF16},    {0xccu, Opcode::VCmpGtF16},   {0xcdu, Opcode::VCmpLgF16},
    {0xceu, Opcode::VCmpGeF16},    {0xedu, Opcode::VCmpNeqF16},  {0xd9u, Opcode::VCmpxLtF16},
    {0xdau, Opcode::VCmpxEqF16},   {0xdbu, Opcode::VCmpxLeF16},  {0xdcu, Opcode::VCmpxGtF16},
    {0xdeu, Opcode::VCmpxGeF16},   {0xfdu, Opcode::VCmpxNeqF16}, {0xfeu, Opcode::VCmpxNltF16},
};

constexpr OpcodeMap VOP3_OPS[] = {
    {0x141u, Opcode::VMadF32},          {0x142u, Opcode::VMadI32I24},
    {0x143u, Opcode::VMadU32U24},       {0x176u, Opcode::VMadU64U32},
    {0x144u, Opcode::VCubeidF32},       {0x145u, Opcode::VCubescF32},
    {0x146u, Opcode::VCubetcF32},       {0x147u, Opcode::VCubemaF32},
    {0x14bu, Opcode::VFmaF32},          {0x148u, Opcode::VBfeU32},
    {0x149u, Opcode::VBfeI32},          {0x14au, Opcode::VBfiB32},
    {0x14eu, Opcode::VAlignbitB32},     {0x151u, Opcode::VMin3F32},
    {0x152u, Opcode::VMin3I32},         {0x153u, Opcode::VMin3U32},
    {0x351u, Opcode::VMin3F16},         {0x154u, Opcode::VMax3F32},
    {0x155u, Opcode::VMax3I32},         {0x156u, Opcode::VMax3U32},
    {0x354u, Opcode::VMax3F16},         {0x157u, Opcode::VMed3F32},
    {0x158u, Opcode::VMed3I32},         {0x159u, Opcode::VMed3U32},
    {0x357u, Opcode::VMed3F16},         {0x15du, Opcode::VSadU32},
    {0x15eu, Opcode::VCvtPkU8F32},      {0x178u, Opcode::VXor3B32},
    {0x12fu, Opcode::VCvtPkrtzF16F32},  {0x169u, Opcode::VMulLoU32},
    {0x16au, Opcode::VMulHiU32},        {0x16bu, Opcode::VMulLoI32},
    {0x16cu, Opcode::VMulHiI32},        {0x303u, Opcode::VAddNcU16},
    {0x304u, Opcode::VSubNcU16},        {0x307u, Opcode::VLshrrevB16},
    {0x308u, Opcode::VAshrrevI16},      {0x309u, Opcode::VMaxU16},
    {0x30au, Opcode::VMaxI16},          {0x30bu, Opcode::VMinU16},
    {0x30cu, Opcode::VMinI16},          {0x30du, Opcode::VAddNcI16},
    {0x30eu, Opcode::VSubNcI16},        {0x30fu, Opcode::VAddI32},
    {0x310u, Opcode::VSubI32},          {0x311u, Opcode::VPackB32F16},
    {0x314u, Opcode::VLshlrevB16},      {0x319u, Opcode::VSubrevI32},
    {0x345u, Opcode::VXadU32},          {0x346u, Opcode::VLshlAddU32},
    {0x347u, Opcode::VAddLshlU32},      {0x360u, Opcode::VReadlaneB32},
    {0x361u, Opcode::VWritelaneB32},    {0x362u, Opcode::VLdexpF32},
    {0x363u, Opcode::VBfmB32},          {0x364u, Opcode::VBcntU32B32},
    {0x365u, Opcode::VMbcntLoU32B32},   {0x366u, Opcode::VMbcntHiU32B32},
    {0x368u, Opcode::VCvtPknormI16F32}, {0x369u, Opcode::VCvtPknormU16F32},
    {0x36au, Opcode::VCvtPkU16U32},     {0x36fu, Opcode::VLshlOrB32},
    {0x371u, Opcode::VAndOrB32},        {0x372u, Opcode::VOr3B32},
    {0x377u, Opcode::VPermlane16B32},   {0x378u, Opcode::VPermlanex16B32},
    {0x34bu, Opcode::VFmaF16},          {0x36du, Opcode::VAdd3U32},
};

constexpr OpcodeMap VOP3P_OPS[] = {
    {0x00u, Opcode::VPkMadI16},     {0x01u, Opcode::VPkMulLoU16},   {0x02u, Opcode::VPkAddI16},
    {0x03u, Opcode::VPkSubI16},     {0x04u, Opcode::VPkLshlrevB16}, {0x05u, Opcode::VPkLshrrevB16},
    {0x06u, Opcode::VPkAshrrevI16}, {0x07u, Opcode::VPkMaxI16},     {0x08u, Opcode::VPkMinI16},
    {0x09u, Opcode::VPkMadU16},     {0x0au, Opcode::VPkAddU16},     {0x0bu, Opcode::VPkSubU16},
    {0x0cu, Opcode::VPkMaxU16},     {0x0du, Opcode::VPkMinU16},     {0x0eu, Opcode::VPkFmaF16},
    {0x0fu, Opcode::VPkAddF16},     {0x10u, Opcode::VPkMulF16},     {0x11u, Opcode::VPkMinF16},
    {0x12u, Opcode::VPkMaxF16},     {0x20u, Opcode::VFmaF32},       {0x21u, Opcode::VMadMixloF16},
    {0x22u, Opcode::VMadMixhiF16},
};

Opcode Lookup(const OpcodeMap* ops, uint32_t count, uint32_t opcode) {
	for (uint32_t i = 0; i < count; i++) {
		const auto& op = ops[i];
		if (op.opcode == opcode) {
			return op.ir;
		}
	}
	return Opcode::Unsupported;
}

bool IsVop2LiteralMadOpcode(uint32_t opcode) {
	return opcode == 0x20u || opcode == 0x21u || opcode == 0x2cu || opcode == 0x2du;
}

bool IsUnsupportedVop3EncodedVop2Alias(uint32_t opcode) {
	return IsVop2LiteralMadOpcode(opcode) || opcode == 0x02u || opcode == 0x2bu ||
	       opcode == 0x39u || opcode == 0x3au;
}

Opcode LookupVop3Opcode(uint32_t opcode) {
	const auto direct = Lookup(VOP3_OPS, static_cast<uint32_t>(std::size(VOP3_OPS)), opcode);
	if (direct != Opcode::Unsupported) {
		return direct;
	}
	if (opcode <= 0xffu) {
		return Lookup(VOPC_OPS, static_cast<uint32_t>(std::size(VOPC_OPS)), opcode);
	}
	if (opcode >= 0x100u && opcode <= 0x13fu) {
		if (IsUnsupportedVop3EncodedVop2Alias(opcode - 0x100u)) {
			return Opcode::Unsupported;
		}
		return Lookup(VOP2_OPS, static_cast<uint32_t>(std::size(VOP2_OPS)), opcode - 0x100u);
	}
	if (opcode >= 0x180u && opcode <= 0x1ffu) {
		return Lookup(VOP3_ENCODED_VOP1_OPS,
		              static_cast<uint32_t>(std::size(VOP3_ENCODED_VOP1_OPS)), opcode - 0x180u);
	}
	return Lookup(VOP3_OPS, static_cast<uint32_t>(std::size(VOP3_OPS)), opcode);
}

bool IsVop3EncodedVopc(uint32_t opcode) {
	return opcode <= 0xffu;
}

bool IsVop3EncodedVop2(uint32_t opcode) {
	return opcode >= 0x100u && opcode <= 0x13fu;
}

bool IsVop3EncodedVop1(uint32_t opcode) {
	return opcode >= 0x180u && opcode <= 0x1ffu;
}

Opcode LookupVintrpOpcode(uint32_t opcode) {
	switch (opcode) {
		case 0x00u: return Opcode::VInterpP1F32;
		case 0x01u: return Opcode::VInterpP2F32;
		case 0x02u: return Opcode::VInterpMovF32;
		default: return Opcode::Unsupported;
	}
}

bool UsesScalarDestination(Opcode opcode) {
	return opcode == Opcode::VReadfirstlaneB32 || opcode == Opcode::VReadlaneB32;
}

bool IsVop3BCarryOutOpcode(Opcode opcode) {
	return opcode == Opcode::VAddI32 || opcode == Opcode::VSubI32 || opcode == Opcode::VSubrevI32;
}

bool IsVop3BMadU64Opcode(Opcode opcode) {
	return opcode == Opcode::VMadU64U32;
}

bool IsPermlaneOpcode(Opcode opcode) {
	return opcode == Opcode::VPermlane16B32 || opcode == Opcode::VPermlanex16B32;
}

bool IsVop2LowHalfF16Opcode(Opcode opcode) {
	return opcode == Opcode::VAddF16 || opcode == Opcode::VSubF16 || opcode == Opcode::VSubrevF16 ||
	       opcode == Opcode::VMulF16 || opcode == Opcode::VFmacF16 || opcode == Opcode::VFmamkF16 ||
	       opcode == Opcode::VFmaakF16 || opcode == Opcode::VMaxF16 || opcode == Opcode::VMinF16;
}

bool IsNativeVop3F16TernaryOpcode(Opcode opcode) {
	return opcode == Opcode::VMin3F16 || opcode == Opcode::VMax3F16 || opcode == Opcode::VMed3F16 ||
	       opcode == Opcode::VFmaF16;
}

bool IsNativeVop3B16BinaryOpcode(Opcode opcode) {
	switch (opcode) {
		case Opcode::VAddNcU16:
		case Opcode::VSubNcU16:
		case Opcode::VMaxU16:
		case Opcode::VMaxI16:
		case Opcode::VMinU16:
		case Opcode::VMinI16:
		case Opcode::VAddNcI16:
		case Opcode::VSubNcI16:
		case Opcode::VLshlrevB16:
		case Opcode::VLshrrevB16:
		case Opcode::VAshrrevI16: return true;
		default: return false;
	}
}

void ApplyDefaultVop2F16Destination(Instruction* inst) {
	if (inst != nullptr && IsVop2LowHalfF16Opcode(inst->opcode)) {
		inst->dst.sdwa_sel = 4;
	}
}

bool IsVop1FloatResultOpcode(Opcode opcode);
bool IsVopcCompareExec(Opcode opcode);

bool IsVop1FloatSourceOpcode(Opcode opcode) {
	switch (opcode) {
		case Opcode::VMovB32:
		case Opcode::VCvtF32F16:
		case Opcode::VCvtU32F32:
		case Opcode::VCvtI32F32:
		case Opcode::VCvtRpiI32F32:
		case Opcode::VCvtFlrI32F32:
		case Opcode::VCvtF16F32:
		case Opcode::VRcpF32:
		case Opcode::VFractF32:
		case Opcode::VTruncF32:
		case Opcode::VCeilF32:
		case Opcode::VRndneF32:
		case Opcode::VFloorF32:
		case Opcode::VExpF32:
		case Opcode::VLogF32:
		case Opcode::VRsqF32:
		case Opcode::VSqrtF32:
		case Opcode::VSqrtF16:
		case Opcode::VFloorF16:
		case Opcode::VCeilF16:
		case Opcode::VTruncF16:
		case Opcode::VRndneF16:
		case Opcode::VSinF32:
		case Opcode::VCosF32: return true;
		default: return false;
	}
}

constexpr uint32_t SdwaSel(uint32_t sel) {
	return 1u << sel;
}

constexpr uint32_t SdwaSelBytes() {
	return SdwaSel(0) | SdwaSel(1) | SdwaSel(2) | SdwaSel(3);
}

constexpr uint32_t SdwaSelWords() {
	return SdwaSel(4) | SdwaSel(5);
}

constexpr uint32_t SdwaSelFull() {
	return SdwaSel(6);
}

struct Vop1SdwaRule {
	Opcode   opcode                       = Opcode::Unknown;
	uint32_t source_selectors             = SdwaSelFull();
	uint32_t partial_dst_selectors        = 0;
	uint32_t partial_dst_source_selectors = 0;
	bool     source_modifiers             = false;
};

constexpr Vop1SdwaRule VOP1_SDWA_RULES[] = {
    {Opcode::VMovB32, SdwaSelBytes() | SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::VCvtF32U32, SdwaSelBytes() | SdwaSelWords() | SdwaSelFull(), 0, 0, false},
    {Opcode::VCvtF32I32, SdwaSelBytes() | SdwaSelWords() | SdwaSelFull(), 0, 0, false},
    {Opcode::VCvtF32F16, SdwaSelWords() | SdwaSelFull(), 0, 0, true},
    {Opcode::VCvtF16F32, SdwaSelFull(), SdwaSelWords(), SdwaSelFull(), false},
    {Opcode::VCvtF16U16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::VCvtU16F16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::VCvtF16I16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::VCvtI16F16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::VRcpF16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), true},
    {Opcode::VSqrtF16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::VRsqF16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::VLogF16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), true},
    {Opcode::VExpF16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), true},
    {Opcode::VFloorF16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::VCeilF16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::VTruncF16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::VRndneF16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::VCvtU32F32, SdwaSelFull(), SdwaSelBytes() | SdwaSelWords(), SdwaSelFull(), false},
    {Opcode::VCvtI32F32, SdwaSelFull(), SdwaSelBytes() | SdwaSelWords(), SdwaSelFull(), false},
    {Opcode::VCvtRpiI32F32, SdwaSelFull(), SdwaSelBytes() | SdwaSelWords(), SdwaSelFull(), false},
    {Opcode::VCvtFlrI32F32, SdwaSelFull(), SdwaSelBytes() | SdwaSelWords(), SdwaSelFull(), false},
};

const Vop1SdwaRule* FindVop1SdwaRule(Opcode opcode) {
	for (const auto& rule: VOP1_SDWA_RULES) {
		if (rule.opcode == opcode) {
			return &rule;
		}
	}
	return nullptr;
}

bool HasSdwaSelector(uint32_t mask, uint32_t selector) {
	return selector <= 6u && (mask & SdwaSel(selector)) != 0;
}

bool IsValidFullSdwaDestinationUnused(uint32_t dst_u) {
	return dst_u != 3u;
}

bool IsVop1SdwaSourceSupported(Opcode opcode, uint32_t src_sel, bool src_neg, bool src_abs) {
	if (src_sel == 6u) {
		return true;
	}
	const auto* rule = FindVop1SdwaRule(opcode);
	if (rule == nullptr || !HasSdwaSelector(rule->source_selectors, src_sel)) {
		return false;
	}
	return rule->source_modifiers || (!src_neg && !src_abs);
}

bool IsVop1SdwaDestinationSupported(Opcode opcode, uint32_t dst_sel, uint32_t dst_u,
                                    uint32_t src_sel) {
	if ((opcode == Opcode::VCvtU16F16 || opcode == Opcode::VCvtI16F16) && dst_sel == 6u) {
		return false;
	}
	if (dst_sel == 6u) {
		return IsValidFullSdwaDestinationUnused(dst_u);
	}
	const auto* rule = FindVop1SdwaRule(opcode);
	if (rule == nullptr || dst_u != 2u) {
		return false;
	}
	return HasSdwaSelector(rule->partial_dst_selectors, dst_sel) &&
	       HasSdwaSelector(rule->partial_dst_source_selectors, src_sel);
}

bool HasUnsupportedVop1SdwaSourceModifiers(Opcode opcode, bool src_neg, bool src_abs) {
	switch (opcode) {
		case Opcode::VCvtU16F16:
		case Opcode::VCvtI16F16:
		case Opcode::VSqrtF16:
		case Opcode::VRsqF16: return src_neg || src_abs;
		case Opcode::VLogF16: return src_neg;
		default: return false;
	}
}

bool ValidateVop1Sdwa(Instruction* inst, uint32_t opcode, uint32_t modifier) {
	const auto dst_sel  = (modifier >> 8u) & 0x7u;
	const auto dst_u    = (modifier >> 11u) & 0x3u;
	const auto clamp    = (modifier >> 13u) & 0x1u;
	const auto omod     = (modifier >> 14u) & 0x3u;
	const auto src0_sel = (modifier >> 16u) & 0x7u;
	const auto src0_neg = (modifier >> 20u) & 0x1u;
	const auto src0_abs = (modifier >> 21u) & 0x1u;

	if (src0_sel > 6u || dst_sel > 6u) {
		SetUnsupported(inst, Family::VOP1, opcode, "VOP1 SDWA selector is invalid");
		return false;
	}
	if ((clamp != 0u || omod != 0u) && !IsVop1FloatResultOpcode(inst->opcode)) {
		SetUnsupported(inst, Family::VOP1, opcode, "VOP1 SDWA output modifiers are not supported");
		return false;
	}
	if (!IsVop1SdwaDestinationSupported(inst->opcode, dst_sel, dst_u, src0_sel)) {
		SetUnsupported(inst, Family::VOP1, opcode,
		               "VOP1 SDWA destination selector is not supported");
		return false;
	}
	if (HasUnsupportedVop1SdwaSourceModifiers(inst->opcode, src0_neg != 0u, src0_abs != 0u)) {
		SetUnsupported(inst, Family::VOP1, opcode, "VOP1 SDWA source modifiers are not supported");
		return false;
	}
	if (!IsVop1SdwaSourceSupported(inst->opcode, src0_sel, src0_neg != 0u, src0_abs != 0u)) {
		SetUnsupported(inst, Family::VOP1, opcode, "VOP1 SDWA source selector is not supported");
		return false;
	}
	return true;
}

bool DecodeVop1Sdwa(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                    uint32_t opcode, uint32_t vdst, Instruction* inst, std::string* error) {
	if (word_index + 1u >= code.size()) {
		if (error != nullptr) {
			*error = fmt::format("truncated VOP1 SDWA instruction at pc 0x{:08x}", pc);
		}
		return false;
	}

	const auto modifier  = code[word_index + 1u];
	const auto src0      = modifier & 0xffu;
	const auto dst_sel   = (modifier >> 8u) & 0x7u;
	const auto dst_u     = (modifier >> 11u) & 0x3u;
	const auto clamp     = (modifier >> 13u) & 0x1u;
	const auto omod      = (modifier >> 14u) & 0x3u;
	const auto src0_sel  = (modifier >> 16u) & 0x7u;
	const auto src0_sext = (modifier >> 19u) & 0x1u;
	const auto src0_neg  = (modifier >> 20u) & 0x1u;
	const auto src0_abs  = (modifier >> 21u) & 0x1u;
	const auto s0        = (modifier >> 23u) & 0x1u;

	SetRawWords(inst, code, word_index, 2);
	if (!ValidateVop1Sdwa(inst, opcode, modifier)) {
		return true;
	}

	const bool scalar_dst = UsesScalarDestination(inst->opcode);
	if (!(scalar_dst ? DecodeScalarDestination(vdst, pc, &inst->dst, error)
	                 : DecodeVectorGpr(vdst, &inst->dst, error)) ||
	    !DecodeScalarSource(src0 + (s0 == 0u ? 256u : 0u), pc, &inst->src0, error)) {
		return false;
	}
	inst->dst.sdwa_sel        = dst_sel;
	inst->dst.sdwa_dst_unused = dst_u;
	inst->dst.clamp           = clamp != 0u;
	inst->dst.omod            = omod;
	inst->src0.sdwa_sel       = src0_sel;
	inst->src0.sdwa_sext      = src0_sext != 0u;
	inst->src0.negate         = src0_neg != 0u;
	inst->src0.absolute       = src0_abs != 0u;
	inst->src_count           = 1;
	return ReadLiteralOperands(code, word_index, inst, error);
}

bool DecodeVop1Dpp(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                   uint32_t opcode, uint32_t vdst, Instruction* inst, std::string* error) {
	if (word_index + 1u >= code.size()) {
		if (error != nullptr) {
			*error = fmt::format("truncated VOP1 DPP instruction at pc 0x{:08x}", pc);
		}
		return false;
	}

	const auto modifier = code[word_index + 1u];
	const auto src0     = modifier & 0xffu;
	SetRawWords(inst, code, word_index, 2);

	const bool scalar_dst = UsesScalarDestination(inst->opcode);
	if (!(scalar_dst ? DecodeScalarDestination(vdst, pc, &inst->dst, error)
	                 : DecodeVectorGpr(vdst, &inst->dst, error)) ||
	    !DecodeScalarSource(src0 + 256u, pc, &inst->src0, error)) {
		return false;
	}
	inst->src0.negate             = ((modifier >> 20u) & 0x1u) != 0u;
	inst->src0.absolute           = ((modifier >> 21u) & 0x1u) != 0u;
	inst->src0.dpp                = true;
	inst->src0.dpp_ctrl           = (modifier >> 8u) & 0x1ffu;
	inst->src0.dpp_fetch_inactive = ((modifier >> 18u) & 0x1u) != 0u;
	inst->src0.dpp_bound_ctrl     = ((modifier >> 19u) & 0x1u) != 0u;
	inst->src0.dpp_bank_mask      = (modifier >> 24u) & 0xfu;
	inst->src0.dpp_row_mask       = (modifier >> 28u) & 0xfu;
	inst->src_count               = 1;

	if (!IsVop1FloatSourceOpcode(inst->opcode) && (inst->src0.negate || inst->src0.absolute)) {
		SetUnsupported(inst, Family::VOP1, opcode,
		               "VOP1 DPP integer source modifiers are not supported");
		return true;
	}
	return ReadLiteralOperands(code, word_index, inst, error);
}

using Vop1ModifierDecodeFn = bool (*)(uint32_t pc, std::span<const uint32_t> code,
                                      uint32_t word_index, uint32_t opcode, uint32_t vdst,
                                      Instruction* inst, std::string* error);

struct Vop1ModifierDecoder {
	uint32_t             escape = 0;
	Vop1ModifierDecodeFn decode = nullptr;
};

constexpr Vop1ModifierDecoder VOP1_MODIFIER_DECODERS[] = {
    {249u, DecodeVop1Sdwa},
    {250u, DecodeVop1Dpp},
};

bool TryDecodeVop1Modifier(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                           uint32_t src0, uint32_t opcode, uint32_t vdst, Instruction* inst,
                           std::string* error, bool* handled) {
	if (handled != nullptr) {
		*handled = false;
	}
	for (const auto& decoder: VOP1_MODIFIER_DECODERS) {
		if (decoder.escape == src0) {
			if (handled != nullptr) {
				*handled = true;
			}
			return decoder.decode(pc, code, word_index, opcode, vdst, inst, error);
		}
	}
	return true;
}

bool IsVop2FloatSourceOpcode(Opcode opcode) {
	switch (opcode) {
		case Opcode::VAddF32:
		case Opcode::VSubF32:
		case Opcode::VSubrevF32:
		case Opcode::VMulF32:
		case Opcode::VMinF32:
		case Opcode::VMaxF32:
		case Opcode::VMacF32:
		case Opcode::VMadmkF32:
		case Opcode::VMadakF32:
		case Opcode::VAddF16:
		case Opcode::VSubF16:
		case Opcode::VSubrevF16:
		case Opcode::VMulF16:
		case Opcode::VFmacF16:
		case Opcode::VFmamkF16:
		case Opcode::VFmaakF16:
		case Opcode::VMaxF16:
		case Opcode::VMinF16: return true;
		default: return false;
	}
}

bool IsVop2FloatResultOpcode(Opcode opcode) {
	switch (opcode) {
		case Opcode::VAddF32:
		case Opcode::VSubF32:
		case Opcode::VSubrevF32:
		case Opcode::VMulF32:
		case Opcode::VMinF32:
		case Opcode::VMaxF32:
		case Opcode::VMacF32:
		case Opcode::VMadmkF32:
		case Opcode::VMadakF32:
		case Opcode::VAddF16:
		case Opcode::VSubF16:
		case Opcode::VSubrevF16:
		case Opcode::VMulF16:
		case Opcode::VFmacF16:
		case Opcode::VFmamkF16:
		case Opcode::VFmaakF16:
		case Opcode::VMaxF16:
		case Opcode::VMinF16: return true;
		default: return false;
	}
}

bool IsVop1FloatResultOpcode(Opcode opcode) {
	switch (opcode) {
		case Opcode::VCvtF32I32:
		case Opcode::VCvtF32U32:
		case Opcode::VCvtF32F16:
		case Opcode::VCvtOffF32I4:
		case Opcode::VCvtF32Ubyte0:
		case Opcode::VCvtF32Ubyte1:
		case Opcode::VCvtF32Ubyte2:
		case Opcode::VCvtF32Ubyte3:
		case Opcode::VRcpF32:
		case Opcode::VFractF32:
		case Opcode::VTruncF32:
		case Opcode::VCeilF32:
		case Opcode::VRndneF32:
		case Opcode::VFloorF32:
		case Opcode::VExpF32:
		case Opcode::VLogF32:
		case Opcode::VRsqF32:
		case Opcode::VSqrtF32:
		case Opcode::VRcpF16:
		case Opcode::VRsqF16:
		case Opcode::VLogF16:
		case Opcode::VExpF16:
		case Opcode::VSinF32:
		case Opcode::VCosF32: return true;
		default: return false;
	}
}

bool IsVopcFloatCompareOpcode(Opcode opcode) {
	switch (opcode) {
		case Opcode::VCmpFF32:
		case Opcode::VCmpLtF32:
		case Opcode::VCmpEqF32:
		case Opcode::VCmpLeF32:
		case Opcode::VCmpGtF32:
		case Opcode::VCmpLgF32:
		case Opcode::VCmpGeF32:
		case Opcode::VCmpOF32:
		case Opcode::VCmpUF32:
		case Opcode::VCmpNgeF32:
		case Opcode::VCmpNlgF32:
		case Opcode::VCmpNgtF32:
		case Opcode::VCmpNleF32:
		case Opcode::VCmpNeqF32:
		case Opcode::VCmpNltF32:
		case Opcode::VCmpTruF32:
		case Opcode::VCmpxLtF32:
		case Opcode::VCmpxEqF32:
		case Opcode::VCmpxLeF32:
		case Opcode::VCmpxGtF32:
		case Opcode::VCmpxLgF32:
		case Opcode::VCmpxGeF32:
		case Opcode::VCmpxNgeF32:
		case Opcode::VCmpxNlgF32:
		case Opcode::VCmpxNgtF32:
		case Opcode::VCmpxNleF32:
		case Opcode::VCmpxNeqF32:
		case Opcode::VCmpxNltF32:
		case Opcode::VCmpLtF16:
		case Opcode::VCmpEqF16:
		case Opcode::VCmpLeF16:
		case Opcode::VCmpGtF16:
		case Opcode::VCmpLgF16:
		case Opcode::VCmpGeF16:
		case Opcode::VCmpNeqF16:
		case Opcode::VCmpxLtF16:
		case Opcode::VCmpxEqF16:
		case Opcode::VCmpxLeF16:
		case Opcode::VCmpxGtF16:
		case Opcode::VCmpxGeF16:
		case Opcode::VCmpxNeqF16:
		case Opcode::VCmpxNltF16:
		case Opcode::VCmpClassF32: return true;
		default: return false;
	}
}

struct Vop2SdwaFields {
	uint32_t src0      = 0;
	uint32_t dst_sel   = 6;
	uint32_t dst_u     = 0;
	uint32_t clamp     = 0;
	uint32_t omod      = 0;
	uint32_t src0_sel  = 6;
	uint32_t src0_sext = 0;
	uint32_t src0_neg  = 0;
	uint32_t src0_abs  = 0;
	uint32_t s0        = 0;
	uint32_t src1_sel  = 6;
	uint32_t src1_sext = 0;
	uint32_t src1_neg  = 0;
	uint32_t src1_abs  = 0;
	uint32_t s1        = 0;
};

Vop2SdwaFields DecodeVop2SdwaFields(uint32_t modifier) {
	Vop2SdwaFields fields;
	fields.src0      = modifier & 0xffu;
	fields.dst_sel   = (modifier >> 8u) & 0x7u;
	fields.dst_u     = (modifier >> 11u) & 0x3u;
	fields.clamp     = (modifier >> 13u) & 0x1u;
	fields.omod      = (modifier >> 14u) & 0x3u;
	fields.src0_sel  = (modifier >> 16u) & 0x7u;
	fields.src0_sext = (modifier >> 19u) & 0x1u;
	fields.src0_neg  = (modifier >> 20u) & 0x1u;
	fields.src0_abs  = (modifier >> 21u) & 0x1u;
	fields.s0        = (modifier >> 23u) & 0x1u;
	fields.src1_sel  = (modifier >> 24u) & 0x7u;
	fields.src1_sext = (modifier >> 27u) & 0x1u;
	fields.src1_neg  = (modifier >> 28u) & 0x1u;
	fields.src1_abs  = (modifier >> 29u) & 0x1u;
	fields.s1        = (modifier >> 31u) & 0x1u;
	return fields;
}

struct Vop2SdwaRule {
	Opcode   opcode           = Opcode::Unknown;
	uint32_t dst_selectors    = SdwaSelFull();
	uint32_t src0_selectors   = SdwaSelFull();
	uint32_t src1_selectors   = SdwaSelFull();
	bool     partial_dst      = false;
	bool     source_modifiers = false;
};

constexpr uint32_t SdwaSelAll() {
	return SdwaSelBytes() | SdwaSelWords() | SdwaSelFull();
}

constexpr Vop2SdwaRule VOP2_SDWA_RULES[] = {
    {Opcode::VCndmaskB32, SdwaSelWords(), SdwaSelWords() | SdwaSelFull(),
     SdwaSelWords() | SdwaSelFull(), true, false},
    {Opcode::VAddF32, SdwaSelFull(), SdwaSelFull(), SdwaSelFull(), false, true},
    {Opcode::VSubF32, SdwaSelFull(), SdwaSelFull(), SdwaSelFull(), false, true},
    {Opcode::VMulF32, SdwaSelFull(), SdwaSelFull(), SdwaSelFull(), false, true},
    {Opcode::VAddF16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords() | SdwaSelFull(),
     SdwaSelWords() | SdwaSelFull(), true, true},
    {Opcode::VSubF16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords() | SdwaSelFull(),
     SdwaSelWords() | SdwaSelFull(), true, true},
    {Opcode::VSubrevF16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords() | SdwaSelFull(),
     SdwaSelWords() | SdwaSelFull(), true, true},
    {Opcode::VMulF16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords() | SdwaSelFull(),
     SdwaSelWords() | SdwaSelFull(), true, true},
    {Opcode::VMaxF16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords() | SdwaSelFull(),
     SdwaSelWords() | SdwaSelFull(), true, true},
    {Opcode::VMinF16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords() | SdwaSelFull(),
     SdwaSelWords() | SdwaSelFull(), true, true},
    {Opcode::VMulI32I24, SdwaSelFull(), SdwaSelAll(), SdwaSelAll(), false, false},
    {Opcode::VMulU32U24, SdwaSelFull(), SdwaSelAll(), SdwaSelAll(), false, false},
    {Opcode::VMinU32, SdwaSelFull(), SdwaSelAll(), SdwaSelAll(), false, false},
    {Opcode::VMaxU32, SdwaSelFull(), SdwaSelAll(), SdwaSelAll(), false, false},
    {Opcode::VLshrrevB32, SdwaSelFull(), SdwaSelFull(), SdwaSelWords() | SdwaSelFull(), false,
     false},
    {Opcode::VLshlrevB32, SdwaSelFull(), SdwaSelFull(), SdwaSelAll(), false, false},
    {Opcode::VAndB32, SdwaSelWords() | SdwaSelFull(), SdwaSelAll(), SdwaSelAll(), true, false},
    {Opcode::VOrB32, SdwaSelWords() | SdwaSelFull(), SdwaSelAll(), SdwaSelAll(), true, false},
    {Opcode::VXorB32, SdwaSelWords() | SdwaSelFull(), SdwaSelAll(), SdwaSelAll(), true, false},
    {Opcode::VXnorB32, SdwaSelWords() | SdwaSelFull(), SdwaSelAll(), SdwaSelAll(), true, false},
    {Opcode::VAddNcU32, SdwaSelFull(), SdwaSelAll(), SdwaSelAll(), false, false},
    {Opcode::VSubNcU32, SdwaSelFull(), SdwaSelAll(), SdwaSelAll(), false, false},
    {Opcode::VSubrevNcU32, SdwaSelFull(), SdwaSelAll(), SdwaSelAll(), false, false},
};

const Vop2SdwaRule* FindVop2SdwaRule(Opcode opcode) {
	for (const auto& rule: VOP2_SDWA_RULES) {
		if (rule.opcode == opcode) {
			return &rule;
		}
	}
	return nullptr;
}

bool IsVop2SdwaDestinationSupported(const Vop2SdwaRule& rule, const Vop2SdwaFields& fields) {
	if (!HasSdwaSelector(rule.dst_selectors, fields.dst_sel)) {
		return false;
	}
	if (fields.dst_sel == 6u) {
		return IsValidFullSdwaDestinationUnused(fields.dst_u);
	}
	return fields.dst_u == 2u && rule.partial_dst;
}

bool IsFullWidthVop2Sdwa(const Vop2SdwaFields& fields) {
	return fields.dst_sel == 6u && fields.dst_u == 0u && fields.src0_sel == 6u &&
	       fields.src1_sel == 6u;
}

bool ValidateVop2Sdwa(Instruction* inst, uint32_t opcode, const Vop2SdwaFields& fields) {
	if (fields.src0_sel > 6u || fields.src1_sel > 6u || fields.dst_sel > 6u) {
		SetUnsupported(inst, Family::VOP2, opcode, "VOP2 SDWA selector is invalid");
		return false;
	}
	if ((fields.clamp != 0u || fields.omod != 0u) && !IsVop2FloatResultOpcode(inst->opcode)) {
		SetUnsupported(inst, Family::VOP2, opcode, "VOP2 SDWA output modifiers are not supported");
		return false;
	}
	if (IsFullWidthVop2Sdwa(fields)) {
		if (!IsVop2FloatSourceOpcode(inst->opcode) && inst->opcode != Opcode::VCndmaskB32 &&
		    (fields.src0_neg != 0u || fields.src0_abs != 0u || fields.src1_neg != 0u ||
		     fields.src1_abs != 0u)) {
			SetUnsupported(inst, Family::VOP2, opcode,
			               "VOP2 SDWA source modifiers are not supported");
			return false;
		}
		return true;
	}

	const auto* rule = FindVop2SdwaRule(inst->opcode);
	if (rule == nullptr) {
		SetUnsupported(inst, Family::VOP2, opcode,
		               "VOP2 SDWA modifier is not supported for opcode");
		return false;
	}
	if (!IsVop2SdwaDestinationSupported(*rule, fields)) {
		SetUnsupported(inst, Family::VOP2, opcode,
		               "VOP2 SDWA destination selector is not supported");
		return false;
	}
	if (!HasSdwaSelector(rule->src0_selectors, fields.src0_sel) ||
	    !HasSdwaSelector(rule->src1_selectors, fields.src1_sel)) {
		SetUnsupported(inst, Family::VOP2, opcode, "VOP2 SDWA source selector is not supported");
		return false;
	}
	const bool has_source_modifiers = (fields.src0_neg != 0u || fields.src0_abs != 0u ||
	                                   fields.src1_neg != 0u || fields.src1_abs != 0u);
	const bool cndmask_float_modifiers =
	    inst->opcode == Opcode::VCndmaskB32 && fields.src0_sel == 6u && fields.src1_sel == 6u;
	if (!rule->source_modifiers && has_source_modifiers && !cndmask_float_modifiers) {
		SetUnsupported(inst, Family::VOP2, opcode, "VOP2 SDWA source modifiers are not supported");
		return false;
	}
	return true;
}

bool FinalizeVop2Instruction(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                             Instruction* inst, std::string* error) {
	(void)pc;
	switch (inst->opcode) {
		case Opcode::VMadmkF32:
		case Opcode::VFmamkF16:
			inst->src2      = inst->src1;
			inst->src1      = {};
			inst->src1.kind = OperandKind::LiteralConstant;
			inst->src_count = 3;
			break;
		case Opcode::VMadakF32:
		case Opcode::VFmaakF16:
			inst->src2      = {};
			inst->src2.kind = OperandKind::LiteralConstant;
			inst->src_count = 3;
			break;
		case Opcode::VAddcU32:
			inst->dst2.kind = OperandKind::VccLo;
			inst->src2.kind = OperandKind::VccLo;
			inst->src_count = 3;
			break;
		default: inst->src_count = 2; break;
	}
	return ReadLiteralOperands(code, word_index, inst, error);
}

bool DecodeVop2Sdwa(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                    uint32_t opcode, uint32_t vdst, uint32_t vsrc1, Instruction* inst,
                    std::string* error) {
	if (word_index + 1u >= code.size()) {
		if (error != nullptr) {
			*error = fmt::format("truncated VOP2 SDWA instruction at pc 0x{:08x}", pc);
		}
		return false;
	}

	const auto modifier = code[word_index + 1u];
	const auto fields   = DecodeVop2SdwaFields(modifier);
	SetRawWords(inst, code, word_index, 2);
	if (!ValidateVop2Sdwa(inst, opcode, fields)) {
		return true;
	}

	if (!DecodeVectorGpr(vdst, &inst->dst, error) ||
	    !DecodeScalarSource(fields.src0 + (fields.s0 == 0u ? 256u : 0u), pc, &inst->src0, error) ||
	    !DecodeScalarSource(vsrc1 + (fields.s1 == 0u ? 256u : 0u), pc, &inst->src1, error)) {
		return false;
	}
	inst->dst.sdwa_sel        = fields.dst_sel;
	inst->dst.sdwa_dst_unused = fields.dst_u;
	inst->dst.clamp           = fields.clamp != 0u;
	inst->dst.omod            = fields.omod;
	inst->src0.sdwa_sel       = fields.src0_sel;
	inst->src0.sdwa_sext      = fields.src0_sext != 0u;
	inst->src0.negate         = fields.src0_neg != 0u;
	inst->src0.absolute       = fields.src0_abs != 0u;
	inst->src1.sdwa_sel       = fields.src1_sel;
	inst->src1.sdwa_sext      = fields.src1_sext != 0u;
	inst->src1.negate         = fields.src1_neg != 0u;
	inst->src1.absolute       = fields.src1_abs != 0u;
	return FinalizeVop2Instruction(pc, code, word_index, inst, error);
}

bool DecodeVop2Dpp(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                   uint32_t opcode, uint32_t vdst, uint32_t vsrc1, Instruction* inst,
                   std::string* error) {
	if (word_index + 1u >= code.size()) {
		if (error != nullptr) {
			*error = fmt::format("truncated VOP2 DPP instruction at pc 0x{:08x}", pc);
		}
		return false;
	}

	const auto modifier = code[word_index + 1u];
	const auto src0     = modifier & 0xffu;
	SetRawWords(inst, code, word_index, 2);

	if (!DecodeVectorGpr(vdst, &inst->dst, error) ||
	    !DecodeScalarSource(src0 + 256u, pc, &inst->src0, error) ||
	    !DecodeVectorGpr(vsrc1, &inst->src1, error)) {
		return false;
	}
	ApplyDefaultVop2F16Destination(inst);
	inst->src0.negate             = ((modifier >> 20u) & 0x1u) != 0u;
	inst->src0.absolute           = ((modifier >> 21u) & 0x1u) != 0u;
	inst->src0.dpp                = true;
	inst->src0.dpp_ctrl           = (modifier >> 8u) & 0x1ffu;
	inst->src0.dpp_fetch_inactive = ((modifier >> 18u) & 0x1u) != 0u;
	inst->src0.dpp_bound_ctrl     = ((modifier >> 19u) & 0x1u) != 0u;
	inst->src0.dpp_bank_mask      = (modifier >> 24u) & 0xfu;
	inst->src0.dpp_row_mask       = (modifier >> 28u) & 0xfu;
	inst->src1.negate             = ((modifier >> 22u) & 0x1u) != 0u;
	inst->src1.absolute           = ((modifier >> 23u) & 0x1u) != 0u;

	if (!IsVop2FloatSourceOpcode(inst->opcode) &&
	    (inst->src0.negate || inst->src0.absolute || inst->src1.negate || inst->src1.absolute)) {
		SetUnsupported(inst, Family::VOP2, opcode,
		               "VOP2 DPP integer source modifiers are not supported");
		return true;
	}
	return FinalizeVop2Instruction(pc, code, word_index, inst, error);
}

using Vop2ModifierDecodeFn = bool (*)(uint32_t pc, std::span<const uint32_t> code,
                                      uint32_t word_index, uint32_t opcode, uint32_t vdst,
                                      uint32_t vsrc1, Instruction* inst, std::string* error);

struct Vop2ModifierDecoder {
	uint32_t             escape = 0;
	Vop2ModifierDecodeFn decode = nullptr;
};

constexpr Vop2ModifierDecoder VOP2_MODIFIER_DECODERS[] = {
    {249u, DecodeVop2Sdwa},
    {250u, DecodeVop2Dpp},
};

bool TryDecodeVop2Modifier(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                           uint32_t src0, uint32_t opcode, uint32_t vdst, uint32_t vsrc1,
                           Instruction* inst, std::string* error, bool* handled) {
	if (handled != nullptr) {
		*handled = false;
	}
	for (const auto& decoder: VOP2_MODIFIER_DECODERS) {
		if (decoder.escape == src0) {
			if (handled != nullptr) {
				*handled = true;
			}
			return decoder.decode(pc, code, word_index, opcode, vdst, vsrc1, inst, error);
		}
	}
	return true;
}

struct VopcSdwaFields {
	uint32_t src0      = 0;
	uint32_t sdst      = 0;
	uint32_t sd        = 0;
	uint32_t src0_sel  = 6;
	uint32_t src0_sext = 0;
	uint32_t src0_neg  = 0;
	uint32_t src0_abs  = 0;
	uint32_t s0        = 0;
	uint32_t src1_sel  = 6;
	uint32_t src1_sext = 0;
	uint32_t src1_neg  = 0;
	uint32_t src1_abs  = 0;
	uint32_t s1        = 0;
};

VopcSdwaFields DecodeVopcSdwaFields(uint32_t modifier) {
	VopcSdwaFields fields;
	fields.src0      = modifier & 0xffu;
	fields.sdst      = (modifier >> 8u) & 0x7fu;
	fields.sd        = (modifier >> 15u) & 0x1u;
	fields.src0_sel  = (modifier >> 16u) & 0x7u;
	fields.src0_sext = (modifier >> 19u) & 0x1u;
	fields.src0_neg  = (modifier >> 20u) & 0x1u;
	fields.src0_abs  = (modifier >> 21u) & 0x1u;
	fields.s0        = (modifier >> 23u) & 0x1u;
	fields.src1_sel  = (modifier >> 24u) & 0x7u;
	fields.src1_sext = (modifier >> 27u) & 0x1u;
	fields.src1_neg  = (modifier >> 28u) & 0x1u;
	fields.src1_abs  = (modifier >> 29u) & 0x1u;
	fields.s1        = (modifier >> 31u) & 0x1u;
	return fields;
}

bool SupportsVopcSdwa(Opcode opcode) {
	return opcode != Opcode::Unsupported;
}

bool DecodeVopcSdwa(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                    uint32_t opcode, uint32_t vsrc1, Instruction* inst, std::string* error) {
	if (word_index + 1u >= code.size()) {
		if (error != nullptr) {
			*error = fmt::format("truncated VOPC SDWA instruction at pc 0x{:08x}", pc);
		}
		return false;
	}

	const auto modifier = code[word_index + 1u];
	const auto fields   = DecodeVopcSdwaFields(modifier);
	SetRawWords(inst, code, word_index, 2);
	if (fields.src0_sel > 6u || fields.src1_sel > 6u) {
		SetUnsupported(inst, Family::VOPC, opcode, "VOPC SDWA selector is invalid");
		return true;
	}
	if (!SupportsVopcSdwa(inst->opcode)) {
		SetUnsupported(inst, Family::VOPC, opcode,
		               "VOPC SDWA modifier is not supported for opcode");
		return true;
	}

	if (!DecodeScalarSource(fields.src0 + (fields.s0 == 0u ? 256u : 0u), pc, &inst->src0, error) ||
	    !DecodeScalarSource(vsrc1 + (fields.s1 == 0u ? 256u : 0u), pc, &inst->src1, error)) {
		return false;
	}
	if (IsVopcCompareExec(inst->opcode)) {
		inst->dst.kind = OperandKind::ExecLo;
	} else if (fields.sd == 0u) {
		inst->dst.kind = OperandKind::VccLo;
	} else if (!DecodeScalarDestination(fields.sdst, pc, &inst->dst, error)) {
		return false;
	}
	inst->src0.sdwa_sel  = fields.src0_sel;
	inst->src0.sdwa_sext = fields.src0_sext != 0u;
	inst->src0.negate    = fields.src0_neg != 0u;
	inst->src0.absolute  = fields.src0_abs != 0u;
	inst->src1.sdwa_sel  = fields.src1_sel;
	inst->src1.sdwa_sext = fields.src1_sext != 0u;
	inst->src1.negate    = fields.src1_neg != 0u;
	inst->src1.absolute  = fields.src1_abs != 0u;
	inst->src_count      = 2;
	return ReadLiteralOperands(code, word_index, inst, error);
}

bool DecodeVopcDpp(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                   uint32_t opcode, uint32_t vsrc1, Instruction* inst, std::string* error) {
	if (word_index + 1u >= code.size()) {
		if (error != nullptr) {
			*error = fmt::format("truncated VOPC DPP instruction at pc 0x{:08x}", pc);
		}
		return false;
	}

	const auto modifier = code[word_index + 1u];
	const auto src0     = modifier & 0xffu;
	SetRawWords(inst, code, word_index, 2);
	if (inst->opcode == Opcode::Unsupported) {
		SetUnsupported(inst, Family::VOPC, opcode, "VOPC opcode is not implemented");
		return true;
	}
	if (!DecodeScalarSource(src0 + 256u, pc, &inst->src0, error) ||
	    !DecodeVectorGpr(vsrc1, &inst->src1, error)) {
		return false;
	}
	inst->dst.kind    = IsVopcCompareExec(inst->opcode) ? OperandKind::ExecLo : OperandKind::VccLo;
	inst->src0.negate = ((modifier >> 20u) & 0x1u) != 0u;
	inst->src0.absolute           = ((modifier >> 21u) & 0x1u) != 0u;
	inst->src0.dpp                = true;
	inst->src0.dpp_ctrl           = (modifier >> 8u) & 0x1ffu;
	inst->src0.dpp_fetch_inactive = ((modifier >> 18u) & 0x1u) != 0u;
	inst->src0.dpp_bound_ctrl     = ((modifier >> 19u) & 0x1u) != 0u;
	inst->src0.dpp_bank_mask      = (modifier >> 24u) & 0xfu;
	inst->src0.dpp_row_mask       = (modifier >> 28u) & 0xfu;
	inst->src1.negate             = ((modifier >> 22u) & 0x1u) != 0u;
	inst->src1.absolute           = ((modifier >> 23u) & 0x1u) != 0u;
	inst->src_count               = 2;
	return ReadLiteralOperands(code, word_index, inst, error);
}

uint32_t NativeVop3SourceCount(Opcode opcode) {
	switch (opcode) {
		case Opcode::VMulLoU32:
		case Opcode::VMulHiU32:
		case Opcode::VMulLoI32:
		case Opcode::VMulHiI32:
		case Opcode::VMulI32I24:
		case Opcode::VAddNcU16:
		case Opcode::VSubNcU16:
		case Opcode::VMaxU16:
		case Opcode::VMaxI16:
		case Opcode::VMinU16:
		case Opcode::VMinI16:
		case Opcode::VAddNcI16:
		case Opcode::VSubNcI16:
		case Opcode::VLshlrevB16:
		case Opcode::VLshrrevB16:
		case Opcode::VAshrrevI16:
		case Opcode::VAddI32:
		case Opcode::VSubI32:
		case Opcode::VSubrevI32:
		case Opcode::VPackB32F16:
		case Opcode::VReadlaneB32:
		case Opcode::VWritelaneB32:
		case Opcode::VCvtPkrtzF16F32:
		case Opcode::VLdexpF32:
		case Opcode::VBfmB32:
		case Opcode::VBcntU32B32:
		case Opcode::VCvtPknormI16F32:
		case Opcode::VCvtPknormU16F32:
		case Opcode::VCvtPkU16U32: return 2;
		default: return 3;
	}
}

uint32_t Vop3pSourceCount(Opcode opcode) {
	switch (opcode) {
		case Opcode::VPkMulLoU16:
		case Opcode::VPkAddI16:
		case Opcode::VPkSubI16:
		case Opcode::VPkLshlrevB16:
		case Opcode::VPkLshrrevB16:
		case Opcode::VPkAshrrevI16:
		case Opcode::VPkMaxI16:
		case Opcode::VPkMinI16:
		case Opcode::VPkAddU16:
		case Opcode::VPkSubU16:
		case Opcode::VPkMaxU16:
		case Opcode::VPkMinU16:
		case Opcode::VPkAddF16:
		case Opcode::VPkMulF16:
		case Opcode::VPkMinF16:
		case Opcode::VPkMaxF16: return 2;
		default: return 3;
	}
}

bool IsPackedVop3p(Opcode opcode) {
	switch (opcode) {
		case Opcode::VPkAddF16:
		case Opcode::VPkMulF16:
		case Opcode::VPkMinF16:
		case Opcode::VPkMaxF16:
		case Opcode::VPkFmaF16: return true;
		default: return false;
	}
}

bool IsMadMixF16(Opcode opcode) {
	return opcode == Opcode::VMadMixloF16 || opcode == Opcode::VMadMixhiF16;
}

void ApplyVop3pSourceModifiers(Instruction* inst, uint32_t op_sel, uint32_t op_sel_hi, uint32_t neg,
                               uint32_t neg_hi) {
	Operand* sources[] = {&inst->src0, &inst->src1, &inst->src2};
	for (uint32_t i = 0; i < 3u; i++) {
		sources[i]->op_sel    = ((op_sel >> i) & 1u) != 0;
		sources[i]->op_sel_hi = ((op_sel_hi >> i) & 1u) != 0;
		sources[i]->negate    = ((neg >> i) & 1u) != 0;
		sources[i]->negate_hi = ((neg_hi >> i) & 1u) != 0;
	}
}

void ApplyVop3pMixAbsModifiers(Instruction* inst) {
	Operand* sources[] = {&inst->src0, &inst->src1, &inst->src2};
	for (auto* source: sources) {
		// RDNA2 MIX opcodes reuse the VOP3P NEG_HI bits as source absolute modifiers.
		source->absolute  = source->negate_hi;
		source->negate_hi = false;
	}
}

void ApplyNativeVop3MadMixModifiers(Instruction* inst, uint32_t op_sel, uint32_t abs,
                                    uint32_t neg) {
	Operand* sources[] = {&inst->src0, &inst->src1, &inst->src2};
	for (uint32_t i = 0; i < 3u; i++) {
		sources[i]->op_sel    = ((op_sel >> i) & 1u) != 0;
		sources[i]->op_sel_hi = true;
		sources[i]->negate    = ((neg >> i) & 1u) != 0;
		sources[i]->absolute  = ((abs >> i) & 1u) != 0;
	}
	inst->dst.sdwa_sel = ((op_sel & 0x8u) != 0) ? 5u : 4u;
}

void ApplyNativeVop3F16TernaryModifiers(Instruction* inst, uint32_t op_sel, uint32_t abs,
                                        uint32_t neg) {
	Operand* sources[] = {&inst->src0, &inst->src1, &inst->src2};
	for (uint32_t i = 0; i < 3u; i++) {
		sources[i]->op_sel    = ((op_sel >> i) & 1u) != 0;
		sources[i]->op_sel_hi = true;
		sources[i]->negate    = ((neg >> i) & 1u) != 0;
		sources[i]->absolute  = ((abs >> i) & 1u) != 0;
	}
	inst->dst.sdwa_sel = ((op_sel & 0x8u) != 0) ? 5u : 4u;
}

void ApplyNativeVop3B16BinaryModifiers(Instruction* inst, uint32_t op_sel) {
	inst->src0.op_sel  = (op_sel & 0x1u) != 0;
	inst->src1.op_sel  = (op_sel & 0x2u) != 0;
	inst->dst.sdwa_sel = (op_sel & 0x8u) != 0 ? 5u : 4u;
}

void ApplyNativeVop3PackB32F16Modifiers(Instruction* inst, uint32_t op_sel, uint32_t abs,
                                        uint32_t neg) {
	inst->src0.op_sel   = (op_sel & 0x1u) != 0;
	inst->src1.op_sel   = (op_sel & 0x2u) != 0;
	inst->src0.negate   = (neg & 0x1u) != 0;
	inst->src1.negate   = (neg & 0x2u) != 0;
	inst->src0.absolute = (abs & 0x1u) != 0;
	inst->src1.absolute = (abs & 0x2u) != 0;
}

bool SupportsNativeVop3SourceModifiers(Opcode opcode) {
	if (IsVop1FloatSourceOpcode(opcode)) {
		return true;
	}
	if (IsVopcFloatCompareOpcode(opcode)) {
		return true;
	}
	if (IsNativeVop3F16TernaryOpcode(opcode)) {
		return true;
	}
	switch (opcode) {
		case Opcode::VCndmaskB32:
		case Opcode::VAddF32:
		case Opcode::VSubF32:
		case Opcode::VSubrevF32:
		case Opcode::VMulF32:
		case Opcode::VMinF32:
		case Opcode::VMaxF32:
		case Opcode::VMacF32:
		case Opcode::VMadF32:
		case Opcode::VFmaF32:
		case Opcode::VPackB32F16:
		case Opcode::VCubeidF32:
		case Opcode::VCubescF32:
		case Opcode::VCubetcF32:
		case Opcode::VCubemaF32:
		case Opcode::VCvtPkrtzF16F32:
		case Opcode::VCvtPknormI16F32:
		case Opcode::VCvtPknormU16F32:
		case Opcode::VMin3F32:
		case Opcode::VMax3F32:
		case Opcode::VMed3F32:
		case Opcode::VLdexpF32: return true;
		default: return false;
	}
}

bool SupportsNativeVop3ResultModifiers(Opcode opcode) {
	if (IsVop1FloatResultOpcode(opcode)) {
		return true;
	}
	switch (opcode) {
		case Opcode::VAddF32:
		case Opcode::VSubF32:
		case Opcode::VSubrevF32:
		case Opcode::VMulF32:
		case Opcode::VMinF32:
		case Opcode::VMaxF32:
		case Opcode::VMacF32:
		case Opcode::VMadF32:
		case Opcode::VFmaF32:
		case Opcode::VFmaF16:
		case Opcode::VLdexpF32:
		case Opcode::VMin3F32:
		case Opcode::VMax3F32:
		case Opcode::VMed3F32: return true;
		default: return false;
	}
}

bool HasUnsupportedNativeVop3Modifiers(Opcode opcode, bool permlane, bool mad_mix, bool addc,
                                       bool scalar_dst, uint32_t abs, uint32_t op_sel,
                                       uint32_t clamp, uint32_t omod, uint32_t neg) {
	const bool source_modifiers = SupportsNativeVop3SourceModifiers(opcode);
	const bool result_modifiers = SupportsNativeVop3ResultModifiers(opcode);

	if (permlane) {
		return abs != 0u || (op_sel & ~0x3u) != 0u || clamp != 0u || omod != 0u || neg != 0u;
	}
	if (mad_mix) {
		return clamp != 0u || omod != 0u;
	}
	if (IsNativeVop3F16TernaryOpcode(opcode)) {
		return opcode != Opcode::VFmaF16 && (clamp != 0u || omod != 0u);
	}
	if (IsNativeVop3B16BinaryOpcode(opcode)) {
		return abs != 0u || clamp != 0u || omod != 0u || neg != 0u;
	}
	if (addc || scalar_dst) {
		return clamp != 0u || omod != 0u || neg != 0u;
	}
	switch (opcode) {
		case Opcode::VLdexpF32: return (abs & ~1u) != 0u || op_sel != 0u || (neg & ~1u) != 0u;
		case Opcode::VCndmaskB32:
			return (abs & ~0x3u) != 0u || op_sel != 0u || clamp != 0u || omod != 0u ||
			       (neg & ~0x3u) != 0u;
		case Opcode::VPackB32F16:
			return (abs & ~0x3u) != 0u || (op_sel & ~0x3u) != 0u || clamp != 0u || omod != 0u ||
			       (neg & ~0x3u) != 0u;
		default: break;
	}
	if (source_modifiers) {
		return op_sel != 0u || (omod != 0u && !result_modifiers) ||
		       (clamp != 0u && !result_modifiers);
	}
	if (result_modifiers) {
		return abs != 0u || op_sel != 0u || neg != 0u;
	}
	return abs != 0u || op_sel != 0u || clamp != 0u || omod != 0u || neg != 0u;
}

void ApplyNativeVop3SourceModifiers(Instruction* inst, uint32_t abs, uint32_t neg) {
	Operand* sources[] = {&inst->src0, &inst->src1, &inst->src2};
	for (uint32_t i = 0; i < inst->src_count && i < 3u; i++) {
		sources[i]->absolute = ((abs >> i) & 1u) != 0;
		sources[i]->negate   = ((neg >> i) & 1u) != 0;
	}
}

bool IsVopcCompareExec(Opcode opcode) {
	switch (opcode) {
		case Opcode::VCmpxLtF32:
		case Opcode::VCmpxEqF32:
		case Opcode::VCmpxLeF32:
		case Opcode::VCmpxGtF32:
		case Opcode::VCmpxLgF32:
		case Opcode::VCmpxGeF32:
		case Opcode::VCmpxNgeF32:
		case Opcode::VCmpxNlgF32:
		case Opcode::VCmpxNgtF32:
		case Opcode::VCmpxNleF32:
		case Opcode::VCmpxNeqF32:
		case Opcode::VCmpxNltF32:
		case Opcode::VCmpxLtI32:
		case Opcode::VCmpxEqI32:
		case Opcode::VCmpxLeI32:
		case Opcode::VCmpxGtI32:
		case Opcode::VCmpxNeI32:
		case Opcode::VCmpxGeI32:
		case Opcode::VCmpxLtU32:
		case Opcode::VCmpxEqU32:
		case Opcode::VCmpxLeU32:
		case Opcode::VCmpxGtU32:
		case Opcode::VCmpxNeU32:
		case Opcode::VCmpxGeU32:
		case Opcode::VCmpxLtF16:
		case Opcode::VCmpxEqF16:
		case Opcode::VCmpxLeF16:
		case Opcode::VCmpxGtF16:
		case Opcode::VCmpxGeF16:
		case Opcode::VCmpxNeqF16:
		case Opcode::VCmpxNltF16: return true;
		default: return false;
	}
}

} // namespace

bool DecodeVop2(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
                std::string* error) {
	const uint32_t word   = code[word_index];
	const uint32_t opcode = (word >> 25u) & 0x3fu;
	const uint32_t vdst   = (word >> 17u) & 0xffu;
	const uint32_t src0   = word & 0x1ffu;
	const uint32_t vsrc1  = (word >> 9u) & 0xffu;

	inst->pc        = pc;
	inst->word      = word;
	inst->family    = Family::VOP2;
	inst->opcode_id = opcode;
	inst->opcode    = Lookup(VOP2_OPS, static_cast<uint32_t>(std::size(VOP2_OPS)), opcode);
	SetRawWords(inst, code, word_index, 1);

	switch (opcode) {
		case 0x3eu: return DecodeVopc(pc, code, word_index, inst, error);
		case 0x3fu: return DecodeVop1(pc, code, word_index, inst, error);
		default: break;
	}
	if (inst->opcode == Opcode::Unsupported) {
		SetUnsupported(inst, Family::VOP2, opcode, "VOP2 opcode is not implemented");
		return true;
	}
	bool modifier_handled = false;
	if (!TryDecodeVop2Modifier(pc, code, word_index, src0, opcode, vdst, vsrc1, inst, error,
	                           &modifier_handled)) {
		return false;
	}
	if (modifier_handled) {
		return true;
	}

	if (!DecodeVectorGpr(vdst, &inst->dst, error) ||
	    !DecodeScalarSource(src0, pc, &inst->src0, error) ||
	    !DecodeVectorGpr(vsrc1, &inst->src1, error)) {
		return false;
	}
	ApplyDefaultVop2F16Destination(inst);
	return FinalizeVop2Instruction(pc, code, word_index, inst, error);
}

bool DecodeVop1(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
                std::string* error) {
	const uint32_t word   = code[word_index];
	const uint32_t opcode = (word >> 9u) & 0xffu;
	const uint32_t src0   = word & 0x1ffu;
	const uint32_t vdst   = (word >> 17u) & 0xffu;

	inst->pc        = pc;
	inst->word      = word;
	inst->family    = Family::VOP1;
	inst->opcode_id = opcode;
	inst->opcode    = Lookup(VOP1_OPS, static_cast<uint32_t>(std::size(VOP1_OPS)), opcode);
	SetRawWords(inst, code, word_index, 1);

	if (inst->opcode == Opcode::Unsupported) {
		SetUnsupported(inst, Family::VOP1, opcode, "VOP1 opcode is not implemented");
		return true;
	}
	if (inst->opcode == Opcode::VNop) {
		inst->dst.kind  = OperandKind::Null;
		inst->src_count = 0;
		return true;
	}
	bool modifier_handled = false;
	if (!TryDecodeVop1Modifier(pc, code, word_index, src0, opcode, vdst, inst, error,
	                           &modifier_handled)) {
		return false;
	}
	if (modifier_handled) {
		return true;
	}
	const bool scalar_dst = UsesScalarDestination(inst->opcode);
	if (!(scalar_dst ? DecodeScalarDestination(vdst, pc, &inst->dst, error)
	                 : DecodeVectorGpr(vdst, &inst->dst, error)) ||
	    !DecodeScalarSource(src0, pc, &inst->src0, error)) {
		return false;
	}
	inst->src_count = 1;
	return ReadLiteralOperands(code, word_index, inst, error);
}

bool DecodeVopc(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
                std::string* error) {
	const uint32_t word   = code[word_index];
	const uint32_t opcode = (word >> 17u) & 0xffu;
	const uint32_t src0   = word & 0x1ffu;
	const uint32_t vsrc1  = (word >> 9u) & 0xffu;

	inst->pc        = pc;
	inst->word      = word;
	inst->family    = Family::VOPC;
	inst->opcode_id = opcode;
	inst->opcode    = Lookup(VOPC_OPS, static_cast<uint32_t>(std::size(VOPC_OPS)), opcode);
	inst->dst.kind  = IsVopcCompareExec(inst->opcode) ? OperandKind::ExecLo : OperandKind::VccLo;
	SetRawWords(inst, code, word_index, 1);

	switch (src0) {
		case 249u: return DecodeVopcSdwa(pc, code, word_index, opcode, vsrc1, inst, error);
		case 250u: return DecodeVopcDpp(pc, code, word_index, opcode, vsrc1, inst, error);
		default: break;
	}
	if (inst->opcode == Opcode::Unsupported) {
		SetUnsupported(inst, Family::VOPC, opcode, "VOPC opcode is not implemented");
		return true;
	}
	if (!DecodeScalarSource(src0, pc, &inst->src0, error) ||
	    !DecodeVectorGpr(vsrc1, &inst->src1, error)) {
		return false;
	}
	inst->src_count = 2;
	return ReadLiteralOperands(code, word_index, inst, error);
}

bool DecodeVop3(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
                std::string* error) {
	if (word_index + 1u >= code.size()) {
		if (error != nullptr) {
			*error = fmt::format("truncated VOP3 instruction at pc 0x{:08x}", pc);
		}
		return false;
	}

	const uint32_t word0  = code[word_index];
	const uint32_t word1  = code[word_index + 1u];
	const uint32_t opcode = (word0 >> 16u) & 0x3ffu;
	const uint32_t vdst   = word0 & 0xffu;
	const uint32_t sdst   = (word0 >> 8u) & 0x7fu;
	const uint32_t src0   = word1 & 0x1ffu;
	const uint32_t src1   = (word1 >> 9u) & 0x1ffu;
	const uint32_t src2   = (word1 >> 18u) & 0x1ffu;
	const uint32_t abs    = (word0 >> 8u) & 0x7u;
	const uint32_t op_sel = (word0 >> 11u) & 0xfu;
	const uint32_t clamp  = (word0 >> 15u) & 0x1u;
	const uint32_t omod   = (word1 >> 27u) & 0x3u;
	const uint32_t neg    = (word1 >> 29u) & 0x7u;

	inst->pc        = pc;
	inst->word      = word0;
	inst->family    = Family::VOP3;
	inst->opcode_id = opcode;
	inst->opcode    = LookupVop3Opcode(opcode);
	SetRawWords(inst, code, word_index, 2);

	const bool addc                    = inst->opcode == Opcode::VAddcU32 && opcode == 0x128u;
	const bool vop3b_carry_out         = IsVop3BCarryOutOpcode(inst->opcode);
	const bool vop3b_mad_u64           = IsVop3BMadU64Opcode(inst->opcode);
	const bool vop3b_uses_sdst         = addc || vop3b_carry_out || vop3b_mad_u64;
	const bool mad_mix                 = false;
	const bool f16_ternary             = IsNativeVop3F16TernaryOpcode(inst->opcode);
	const bool b16_binary              = IsNativeVop3B16BinaryOpcode(inst->opcode);
	const bool pack_b32_f16            = inst->opcode == Opcode::VPackB32F16;
	const bool permlane                = IsPermlaneOpcode(inst->opcode);
	const bool vop3_vopc               = IsVop3EncodedVopc(opcode);
	const bool compare_exec            = vop3_vopc && IsVopcCompareExec(inst->opcode);
	const bool scalar_dst              = vop3_vopc || UsesScalarDestination(inst->opcode);
	const bool scalar_modifier_limits  = UsesScalarDestination(inst->opcode);
	const bool native_source_modifiers = SupportsNativeVop3SourceModifiers(inst->opcode);
	const bool native_result_modifiers = SupportsNativeVop3ResultModifiers(inst->opcode);
	const bool modifiers =
	    HasUnsupportedNativeVop3Modifiers(inst->opcode, permlane, mad_mix, vop3b_uses_sdst,
	                                      scalar_modifier_limits, abs, op_sel, clamp, omod, neg);
	if (modifiers) {
		SetUnsupported(inst, Family::VOP3, opcode,
		               permlane && (op_sel & ~0x3u) != 0
		                   ? "VOP3 perm-lane op_sel bits are not implemented"
		               : mad_mix ? "VOP3 mad-mix clamp/omod is not implemented"
		                         : "VOP3 source modifiers are not implemented");
		return true;
	}
	if (inst->opcode == Opcode::Unsupported) {
		SetUnsupported(inst, Family::VOP3, opcode, "VOP3 opcode is not implemented");
		return true;
	}
	if (inst->opcode == Opcode::VNop) {
		inst->dst.kind  = OperandKind::Null;
		inst->src_count = 0;
		return true;
	}
	bool dst_ok = true;
	if (compare_exec) {
		inst->dst.kind = OperandKind::ExecLo;
	} else if (scalar_dst) {
		// VOP3A uses VDST for VOPC and the scalar-destination lane-read opcodes.
		dst_ok = DecodeScalarDestination(vdst, pc, &inst->dst, error);
	} else {
		dst_ok = DecodeVectorGpr(vdst, &inst->dst, error);
	}
	if (!dst_ok || !DecodeScalarSource(src0, pc, &inst->src0, error)) {
		return false;
	}
	inst->dst.clamp = native_result_modifiers && clamp != 0u;
	inst->dst.omod  = native_result_modifiers ? omod : 0u;
	if (permlane) {
		inst->dst.op_sel    = (op_sel & 0x1u) != 0u;
		inst->dst.op_sel_hi = (op_sel & 0x2u) != 0u;
	}
	if (vop3_vopc) {
		if (!DecodeScalarSource(src1, pc, &inst->src1, error)) {
			return false;
		}
		inst->src_count = 2;
		if (native_source_modifiers) {
			ApplyNativeVop3SourceModifiers(inst, abs, neg);
		}
		return ReadLiteralOperands(code, word_index, inst, error);
	}
	if (IsVop3EncodedVop1(opcode)) {
		inst->src_count = 1;
		if (native_source_modifiers) {
			ApplyNativeVop3SourceModifiers(inst, abs, neg);
		}
		return ReadLiteralOperands(code, word_index, inst, error);
	}
	if (addc) {
		if (!DecodeScalarDestination(sdst, pc, &inst->dst2, error) ||
		    !DecodeScalarSource(src1, pc, &inst->src1, error) ||
		    !DecodeScalarSource(src2, pc, &inst->src2, error)) {
			return false;
		}
		inst->src_count = 3;
		return ReadLiteralOperands(code, word_index, inst, error);
	}
	if (vop3b_carry_out) {
		if (!DecodeScalarDestination(sdst, pc, &inst->dst2, error) ||
		    !DecodeScalarSource(src1, pc, &inst->src1, error)) {
			return false;
		}
		inst->src_count = 2;
		return ReadLiteralOperands(code, word_index, inst, error);
	}
	if (vop3b_mad_u64) {
		if (!DecodeScalarDestination(sdst, pc, &inst->dst2, error) ||
		    !DecodeScalarSource(src1, pc, &inst->src1, error) ||
		    !DecodeScalarSource(src2, pc, &inst->src2, error)) {
			return false;
		}
		inst->src_count = 3;
		return ReadLiteralOperands(code, word_index, inst, error);
	}
	if (IsVop3EncodedVop2(opcode)) {
		if (!DecodeScalarSource(src1, pc, &inst->src1, error)) {
			return false;
		}
		if (inst->opcode == Opcode::VCndmaskB32) {
			if (!DecodeScalarSource(src2, pc, &inst->src2, error)) {
				return false;
			}
			inst->src_count = 3;
		} else {
			inst->src_count = 2;
		}
		if (native_source_modifiers) {
			ApplyNativeVop3SourceModifiers(inst, abs, neg);
		}
		return ReadLiteralOperands(code, word_index, inst, error);
	}
	if (!DecodeScalarSource(src1, pc, &inst->src1, error)) {
		return false;
	}
	inst->src_count = NativeVop3SourceCount(inst->opcode);
	if (inst->src_count > 2u && !DecodeScalarSource(src2, pc, &inst->src2, error)) {
		return false;
	}
	if (mad_mix) {
		ApplyNativeVop3MadMixModifiers(inst, op_sel, abs, neg);
	} else if (f16_ternary) {
		ApplyNativeVop3F16TernaryModifiers(inst, op_sel, abs, neg);
	} else if (b16_binary) {
		ApplyNativeVop3B16BinaryModifiers(inst, op_sel);
	} else if (pack_b32_f16) {
		ApplyNativeVop3PackB32F16Modifiers(inst, op_sel, abs, neg);
	} else if (native_source_modifiers) {
		ApplyNativeVop3SourceModifiers(inst, abs, neg);
	}
	return ReadLiteralOperands(code, word_index, inst, error);
}

bool DecodeVop3p(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                 Instruction* inst, std::string* error) {
	if (word_index + 1u >= code.size()) {
		if (error != nullptr) {
			*error = fmt::format("truncated VOP3P instruction at pc 0x{:08x}", pc);
		}
		return false;
	}

	const uint32_t word0       = code[word_index];
	const uint32_t word1       = code[word_index + 1u];
	const uint32_t opcode      = (word0 >> 16u) & 0x7fu;
	const uint32_t vdst        = word0 & 0xffu;
	const uint32_t neg_hi      = (word0 >> 8u) & 0x7u;
	const uint32_t op_sel      = (word0 >> 11u) & 0x7u;
	const uint32_t op_sel_hi_2 = (word0 >> 14u) & 0x1u;
	const uint32_t clamp       = (word0 >> 15u) & 0x1u;
	const uint32_t src0        = word1 & 0x1ffu;
	const uint32_t src1        = (word1 >> 9u) & 0x1ffu;
	const uint32_t src2        = (word1 >> 18u) & 0x1ffu;
	const uint32_t op_sel_hi   = ((word1 >> 27u) & 0x3u) | (op_sel_hi_2 << 2u);
	const uint32_t neg         = (word1 >> 29u) & 0x7u;

	inst->pc        = pc;
	inst->word      = word0;
	inst->family    = Family::VOP3P;
	inst->opcode_id = opcode;
	inst->opcode    = Lookup(VOP3P_OPS, static_cast<uint32_t>(std::size(VOP3P_OPS)), opcode);
	SetRawWords(inst, code, word_index, 2);

	if (inst->opcode == Opcode::Unsupported) {
		SetUnsupported(inst, Family::VOP3P, opcode, "VOP3P opcode is not implemented");
		return true;
	}
	inst->src_count = Vop3pSourceCount(inst->opcode);
	if (!DecodeVectorGpr(vdst, &inst->dst, error) ||
	    !DecodeScalarSource(src0, pc, &inst->src0, error) ||
	    !DecodeScalarSource(src1, pc, &inst->src1, error)) {
		return false;
	}
	if (inst->src_count > 2u && !DecodeScalarSource(src2, pc, &inst->src2, error)) {
		return false;
	}
	if (inst->opcode == Opcode::VFmaF32) {
		inst->dst.clamp = clamp != 0u;
	} else if (IsMadMixF16(inst->opcode)) {
		inst->dst.clamp = clamp != 0u;
	} else if (IsPackedVop3p(inst->opcode)) {
		inst->dst.clamp = clamp != 0u;
	} else if (clamp != 0u) {
		SetUnsupported(inst, Family::VOP3P, opcode, "VOP3P integer clamp is not implemented");
		return true;
	}
	ApplyVop3pSourceModifiers(inst, op_sel, op_sel_hi, neg, neg_hi);
	if (inst->opcode == Opcode::VMadMixloF16) {
		ApplyVop3pMixAbsModifiers(inst);
		inst->dst.sdwa_sel = 4;
	} else if (inst->opcode == Opcode::VMadMixhiF16) {
		ApplyVop3pMixAbsModifiers(inst);
		inst->dst.sdwa_sel = 5;
	} else if (inst->opcode == Opcode::VFmaF32) {
		ApplyVop3pMixAbsModifiers(inst);
	}
	return ReadLiteralOperands(code, word_index, inst, error);
}

bool DecodeVintrp(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                  Instruction* inst, std::string* error) {
	if (word_index >= code.size()) {
		if (error != nullptr) {
			*error = fmt::format("truncated VINTRP instruction at pc 0x{:08x}", pc);
		}
		return false;
	}

	const uint32_t word   = code[word_index];
	const uint32_t opcode = (word >> 16u) & 0x3u;
	const uint32_t vdst   = (word >> 18u) & 0xffu;
	const uint32_t attr   = (word >> 10u) & 0x3fu;
	const uint32_t chan   = (word >> 8u) & 0x3u;
	const uint32_t vsrc   = word & 0xffu;

	inst->pc         = pc;
	inst->word       = word;
	inst->word_count = 1;
	inst->family     = Family::VINTRP;
	inst->opcode_id  = opcode;
	inst->opcode     = LookupVintrpOpcode(opcode);
	SetRawWords(inst, code, word_index, 1);
	if (inst->opcode == Opcode::Unsupported) {
		SetUnsupported(inst, Family::VINTRP, opcode, "VINTRP opcode is not implemented");
		return true;
	}

	if (!DecodeVectorGpr(vdst, &inst->dst, error)) {
		return false;
	}
	if (inst->opcode == Opcode::VInterpMovF32) {
		inst->src0.kind       = OperandKind::IntegerInlineConstant;
		inst->src0.value      = vsrc & 0x3u;
		inst->src0.signed_val = static_cast<int32_t>(inst->src0.value);
	} else if (!DecodeVectorGpr(vsrc, &inst->src0, error)) {
		return false;
	}
	inst->src1.kind       = OperandKind::IntegerInlineConstant;
	inst->src1.value      = attr;
	inst->src1.signed_val = static_cast<int32_t>(attr);
	inst->src2.kind       = OperandKind::IntegerInlineConstant;
	inst->src2.value      = chan;
	inst->src2.signed_val = static_cast<int32_t>(chan);
	inst->src_count       = 3;
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder
