#include "graphics/shader/recompiler/frontend/decode/VectorAluOps.h"

#include "graphics/shader/recompiler/frontend/decode/OpcodeTable.h"

namespace Libs::Graphics::ShaderRecompiler::Decoder {
namespace {

using Detail::OpcodeMap;

// These profiles describe the selectors that the current translator implements. They are not
// architectural SDWA legality classes.
enum class Vop2SdwaProfile {
	None,
	Float32,
	Float16,
	IntegerFullDestination,
	IntegerPartialDestination,
	ReverseRightShift,
	ReverseLogicalLeft,
	Bitwise,
	Count,
};

struct Vop2OpcodeInfo {
	uint32_t        encoding     = 0;
	Opcode          decoded      = Opcode::UNKNOWN;
	Vop2SdwaProfile sdwa_profile = Vop2SdwaProfile::None;
};

constexpr Vop2OpcodeInfo VOP2_OPCODE_LIST[] = {
    {0x01u, Opcode::V_CNDMASK_B32, Vop2SdwaProfile::IntegerPartialDestination},
    {0x02u, Opcode::V_DOT2C_F32_F16},
    {0x03u, Opcode::V_ADD_F32, Vop2SdwaProfile::Float32},
    {0x04u, Opcode::V_SUB_F32, Vop2SdwaProfile::Float32},
    {0x05u, Opcode::V_SUBREV_F32},
    {0x08u, Opcode::V_MUL_F32, Vop2SdwaProfile::Float32},
    {0x09u, Opcode::V_MUL_I32_I24, Vop2SdwaProfile::IntegerFullDestination},
    {0x0bu, Opcode::V_MUL_U32_U24, Vop2SdwaProfile::IntegerFullDestination},
    {0x0fu, Opcode::V_MIN_F32},
    {0x10u, Opcode::V_MAX_F32},
    {0x11u, Opcode::V_MIN_I32},
    {0x12u, Opcode::V_MAX_I32},
    {0x13u, Opcode::V_MIN_U32, Vop2SdwaProfile::IntegerPartialDestination},
    {0x14u, Opcode::V_MAX_U32, Vop2SdwaProfile::IntegerFullDestination},
    {0x15u, Opcode::V_LSHR_B32},
    {0x16u, Opcode::V_LSHRREV_B32, Vop2SdwaProfile::ReverseRightShift},
    {0x17u, Opcode::V_ASHR_I32},
    {0x18u, Opcode::V_ASHRREV_I32, Vop2SdwaProfile::ReverseRightShift},
    {0x19u, Opcode::V_LSHL_B32},
    {0x1au, Opcode::V_LSHLREV_B32, Vop2SdwaProfile::ReverseLogicalLeft},
    {0x1bu, Opcode::V_AND_B32, Vop2SdwaProfile::Bitwise},
    {0x1cu, Opcode::V_OR_B32, Vop2SdwaProfile::Bitwise},
    {0x1du, Opcode::V_XOR_B32, Vop2SdwaProfile::Bitwise},
    {0x1eu, Opcode::V_XNOR_B32, Vop2SdwaProfile::Bitwise},
    {0x1fu, Opcode::V_MAC_F32},
    {0x20u, Opcode::V_MADMK_F32},
    {0x21u, Opcode::V_MADAK_F32},
    {0x22u, Opcode::V_BCNT_U32_B32},
    {0x23u, Opcode::V_MBCNT_LO_U32_B32},
    {0x24u, Opcode::V_MBCNT_HI_U32_B32},
    {0x25u, Opcode::V_ADD_NC_U32, Vop2SdwaProfile::IntegerPartialDestination},
    {0x28u, Opcode::V_ADDC_U32, Vop2SdwaProfile::IntegerFullDestination},
    {0x2au, Opcode::V_SUBREV_CO_CI_U32},
    {0x26u, Opcode::V_SUB_NC_U32, Vop2SdwaProfile::IntegerPartialDestination},
    {0x27u, Opcode::V_SUBREV_NC_U32, Vop2SdwaProfile::IntegerFullDestination},
    {0x2bu, Opcode::V_MAC_F32},
    {0x2cu, Opcode::V_MADMK_F32},
    {0x2du, Opcode::V_MADAK_F32},
    {0x2fu, Opcode::V_CVT_PKRTZ_F16_F32},
    {0x32u, Opcode::V_ADD_F16, Vop2SdwaProfile::Float16},
    {0x33u, Opcode::V_SUB_F16, Vop2SdwaProfile::Float16},
    {0x34u, Opcode::V_SUBREV_F16, Vop2SdwaProfile::Float16},
    {0x35u, Opcode::V_MUL_F16, Vop2SdwaProfile::Float16},
    {0x36u, Opcode::V_FMAC_F16},
    {0x37u, Opcode::V_FMAMK_F16},
    {0x38u, Opcode::V_FMAAK_F16},
    {0x39u, Opcode::V_MAX_F16, Vop2SdwaProfile::Float16},
    {0x3au, Opcode::V_MIN_F16, Vop2SdwaProfile::Float16},
    {0x3cu, Opcode::V_PK_FMAC_F16},
};

constexpr auto VOP2_OPS = Detail::MakeOpcodeTable<0x40>(VOP2_OPCODE_LIST);

constexpr Opcode LookupVop2Opcode(uint32_t encoding) {
	return Detail::LookupOpcode(VOP2_OPS, encoding);
}

constexpr OpcodeMap VOP1_OPCODE_LIST[] = {
    {0x00u, Opcode::V_NOP},
    {0x01u, Opcode::V_MOV_B32},
    {0x02u, Opcode::V_READFIRSTLANE_B32},
    {0x05u, Opcode::V_CVT_F32_I32},
    {0x06u, Opcode::V_CVT_F32_U32},
    {0x07u, Opcode::V_CVT_U32_F32},
    {0x08u, Opcode::V_CVT_I32_F32},
    {0x0au, Opcode::V_CVT_F16_F32},
    {0x0bu, Opcode::V_CVT_F32_F16},
    {0x0cu, Opcode::V_CVT_RPI_I32_F32},
    {0x0du, Opcode::V_CVT_FLR_I32_F32},
    {0x0eu, Opcode::V_CVT_OFF_F32_I4},
    {0x11u, Opcode::V_CVT_F32_UBYTE0},
    {0x12u, Opcode::V_CVT_F32_UBYTE1},
    {0x13u, Opcode::V_CVT_F32_UBYTE2},
    {0x14u, Opcode::V_CVT_F32_UBYTE3},
    {0x2au, Opcode::V_RCP_F32},
    {0x20u, Opcode::V_FRACT_F32},
    {0x21u, Opcode::V_TRUNC_F32},
    {0x22u, Opcode::V_CEIL_F32},
    {0x23u, Opcode::V_RNDNE_F32},
    {0x24u, Opcode::V_FLOOR_F32},
    {0x25u, Opcode::V_EXP_F32},
    {0x27u, Opcode::V_LOG_F32},
    {0x2bu, Opcode::V_RCP_IFLAG_F32},
    {0x2eu, Opcode::V_RSQ_F32},
    {0x33u, Opcode::V_SQRT_F32},
    {0x35u, Opcode::V_SIN_F32},
    {0x36u, Opcode::V_COS_F32},
    {0x37u, Opcode::V_NOT_B32},
    {0x38u, Opcode::V_BFREV_B32},
    {0x39u, Opcode::V_FFBH_U32},
    {0x3au, Opcode::V_FFBL_B32},
    {0x3bu, Opcode::V_FFBH_I32},
    {0x3fu, Opcode::V_FREXP_EXP_I32_F32},
    {0x40u, Opcode::V_FREXP_MANT_F32},
    {0x42u, Opcode::V_MOVRELD_B32},
    {0x43u, Opcode::V_MOVRELS_B32},
    {0x50u, Opcode::V_CVT_F16_U16},
    {0x51u, Opcode::V_CVT_F16_I16},
    {0x52u, Opcode::V_CVT_U16_F16},
    {0x53u, Opcode::V_CVT_I16_F16},
    {0x54u, Opcode::V_RCP_F16},
    {0x55u, Opcode::V_SQRT_F16},
    {0x56u, Opcode::V_RSQ_F16},
    {0x57u, Opcode::V_LOG_F16},
    {0x58u, Opcode::V_EXP_F16},
    {0x5bu, Opcode::V_FLOOR_F16},
    {0x5cu, Opcode::V_CEIL_F16},
    {0x5du, Opcode::V_TRUNC_F16},
    {0x5eu, Opcode::V_RNDNE_F16},
    {0x61u, Opcode::V_COS_F16},
};

constexpr auto VOP1_OPS = Detail::MakeOpcodeTable<0x100>(VOP1_OPCODE_LIST);

constexpr OpcodeMap VOP3_ENCODED_VOP1_OPCODE_LIST[] = {
    {0x00u, Opcode::V_NOP},
    {0x01u, Opcode::V_MOV_B32},
    {0x02u, Opcode::V_READFIRSTLANE_B32},
    {0x05u, Opcode::V_CVT_F32_I32},
    {0x06u, Opcode::V_CVT_F32_U32},
    {0x07u, Opcode::V_CVT_U32_F32},
    {0x08u, Opcode::V_CVT_I32_F32},
    {0x0au, Opcode::V_CVT_F16_F32},
    {0x0cu, Opcode::V_CVT_RPI_I32_F32},
    {0x0du, Opcode::V_CVT_FLR_I32_F32},
    {0x0eu, Opcode::V_CVT_OFF_F32_I4},
    {0x2au, Opcode::V_RCP_F32},
    {0x20u, Opcode::V_FRACT_F32},
    {0x21u, Opcode::V_TRUNC_F32},
    {0x22u, Opcode::V_CEIL_F32},
    {0x23u, Opcode::V_RNDNE_F32},
    {0x24u, Opcode::V_FLOOR_F32},
    {0x25u, Opcode::V_EXP_F32},
    {0x27u, Opcode::V_LOG_F32},
    {0x2bu, Opcode::V_RCP_IFLAG_F32},
    {0x2eu, Opcode::V_RSQ_F32},
    {0x33u, Opcode::V_SQRT_F32},
    {0x35u, Opcode::V_SIN_F32},
    {0x36u, Opcode::V_COS_F32},
    {0x37u, Opcode::V_NOT_B32},
    {0x38u, Opcode::V_BFREV_B32},
    {0x39u, Opcode::V_FFBH_U32},
    {0x3au, Opcode::V_FFBL_B32},
    {0x3bu, Opcode::V_FFBH_I32},
    {0x3fu, Opcode::V_FREXP_EXP_I32_F32},
    {0x40u, Opcode::V_FREXP_MANT_F32},
    {0x42u, Opcode::V_MOVRELD_B32},
    {0x43u, Opcode::V_MOVRELS_B32},
    {0x50u, Opcode::V_CVT_F16_U16},
    {0x51u, Opcode::V_CVT_F16_I16},
    {0x52u, Opcode::V_CVT_U16_F16},
    {0x53u, Opcode::V_CVT_I16_F16},
    {0x54u, Opcode::V_RCP_F16},
    {0x55u, Opcode::V_SQRT_F16},
    {0x56u, Opcode::V_RSQ_F16},
    {0x57u, Opcode::V_LOG_F16},
    {0x58u, Opcode::V_EXP_F16},
    {0x5bu, Opcode::V_FLOOR_F16},
    {0x5cu, Opcode::V_CEIL_F16},
    {0x5du, Opcode::V_TRUNC_F16},
    {0x5eu, Opcode::V_RNDNE_F16},
    {0x61u, Opcode::V_COS_F16},
};

constexpr auto VOP3_ENCODED_VOP1_OPS = Detail::MakeOpcodeTable<0x80>(VOP3_ENCODED_VOP1_OPCODE_LIST);

struct VopcOpcodeInfo {
	uint32_t encoding     = 0;
	Opcode   decoded      = Opcode::UNKNOWN;
	bool     supports_dpp = true;
};

constexpr VopcOpcodeInfo VOPC_OPCODE_LIST[] = {
    {0x00u, Opcode::V_CMP_F_F32},         {0x01u, Opcode::V_CMP_LT_F32},
    {0x02u, Opcode::V_CMP_EQ_F32},        {0x03u, Opcode::V_CMP_LE_F32},
    {0x04u, Opcode::V_CMP_GT_F32},        {0x05u, Opcode::V_CMP_LG_F32},
    {0x06u, Opcode::V_CMP_GE_F32},        {0x07u, Opcode::V_CMP_O_F32},
    {0x08u, Opcode::V_CMP_U_F32},         {0x09u, Opcode::V_CMP_NGE_F32},
    {0x0au, Opcode::V_CMP_NLG_F32},       {0x0bu, Opcode::V_CMP_NGT_F32},
    {0x0cu, Opcode::V_CMP_NLE_F32},       {0x0du, Opcode::V_CMP_NEQ_F32},
    {0x0eu, Opcode::V_CMP_NLT_F32},       {0x0fu, Opcode::V_CMP_TRU_F32},
    {0x11u, Opcode::V_CMPX_LT_F32},       {0x12u, Opcode::V_CMPX_EQ_F32},
    {0x13u, Opcode::V_CMPX_LE_F32},       {0x14u, Opcode::V_CMPX_GT_F32},
    {0x15u, Opcode::V_CMPX_LG_F32},       {0x16u, Opcode::V_CMPX_GE_F32},
    {0x19u, Opcode::V_CMPX_NGE_F32},      {0x1au, Opcode::V_CMPX_NLG_F32},
    {0x1bu, Opcode::V_CMPX_NGT_F32},      {0x1cu, Opcode::V_CMPX_NLE_F32},
    {0x1du, Opcode::V_CMPX_NEQ_F32},      {0x1eu, Opcode::V_CMPX_NLT_F32},
    {0x80u, Opcode::V_CMP_F_I32},         {0x81u, Opcode::V_CMP_LT_I32},
    {0x82u, Opcode::V_CMP_EQ_I32},        {0x83u, Opcode::V_CMP_LE_I32},
    {0x84u, Opcode::V_CMP_GT_I32},        {0x85u, Opcode::V_CMP_NE_I32},
    {0x86u, Opcode::V_CMP_GE_I32},        {0x87u, Opcode::V_CMP_T_I32},
    {0x88u, Opcode::V_CMP_CLASS_F32},     {0x89u, Opcode::V_CMP_LT_I16},
    {0x8au, Opcode::V_CMP_EQ_I16},        {0x8bu, Opcode::V_CMP_LE_I16},
    {0x8cu, Opcode::V_CMP_GT_I16},        {0x8du, Opcode::V_CMP_NE_I16},
    {0x8eu, Opcode::V_CMP_GE_I16},        {0x91u, Opcode::V_CMPX_LT_I32},
    {0x92u, Opcode::V_CMPX_EQ_I32},       {0x93u, Opcode::V_CMPX_LE_I32},
    {0x94u, Opcode::V_CMPX_GT_I32},       {0x95u, Opcode::V_CMPX_NE_I32},
    {0x96u, Opcode::V_CMPX_GE_I32},       {0xa9u, Opcode::V_CMP_LT_U16},
    {0xaau, Opcode::V_CMP_EQ_U16},        {0xabu, Opcode::V_CMP_LE_U16},
    {0xacu, Opcode::V_CMP_GT_U16},        {0xadu, Opcode::V_CMP_NE_U16},
    {0xaeu, Opcode::V_CMP_GE_U16},        {0xbcu, Opcode::V_CMPX_GT_U16},
    {0xc0u, Opcode::V_CMP_F_U32},
    {0xc1u, Opcode::V_CMP_LT_U32},        {0xc2u, Opcode::V_CMP_EQ_U32},
    {0xc3u, Opcode::V_CMP_LE_U32},        {0xc4u, Opcode::V_CMP_GT_U32},
    {0xc5u, Opcode::V_CMP_NE_U32},        {0xc6u, Opcode::V_CMP_GE_U32},
    {0xc7u, Opcode::V_CMP_T_U32},         {0xa2u, Opcode::V_CMP_EQ_I64, false},
    {0xb5u, Opcode::V_CMPX_NE_I64, false},
    {0xd1u, Opcode::V_CMPX_LT_U32},       {0xd2u, Opcode::V_CMPX_EQ_U32},
    {0xd3u, Opcode::V_CMPX_LE_U32},       {0xd4u, Opcode::V_CMPX_GT_U32},
    {0xd5u, Opcode::V_CMPX_NE_U32},       {0xd6u, Opcode::V_CMPX_GE_U32},
    {0xe2u, Opcode::V_CMP_EQ_U64, false}, {0xe4u, Opcode::V_CMP_GT_U64, false},
    {0xe5u, Opcode::V_CMP_NE_U64, false},
    {0xf5u, Opcode::V_CMPX_NE_U64, false},
    {0xc9u, Opcode::V_CMP_LT_F16},        {0xcau, Opcode::V_CMP_EQ_F16},
    {0xcbu, Opcode::V_CMP_LE_F16},        {0xccu, Opcode::V_CMP_GT_F16},
    {0xcdu, Opcode::V_CMP_LG_F16},        {0xceu, Opcode::V_CMP_GE_F16},
    {0xedu, Opcode::V_CMP_NEQ_F16},       {0xd9u, Opcode::V_CMPX_LT_F16},
    {0xdau, Opcode::V_CMPX_EQ_F16},       {0xdbu, Opcode::V_CMPX_LE_F16},
    {0xdcu, Opcode::V_CMPX_GT_F16},       {0xdeu, Opcode::V_CMPX_GE_F16},
    {0xfbu, Opcode::V_CMPX_NGT_F16},      {0xfdu, Opcode::V_CMPX_NEQ_F16},
    {0xfeu, Opcode::V_CMPX_NLT_F16},
};

constexpr auto VOPC_OPS = Detail::MakeOpcodeTable<0x100>(VOPC_OPCODE_LIST);

constexpr OpcodeMap VOP3_OPCODE_LIST[] = {
    {0x141u, Opcode::V_MAD_F32},
    {0x142u, Opcode::V_MAD_I32_I24},
    {0x143u, Opcode::V_MAD_U32_U24},
    {0x176u, Opcode::V_MAD_U64_U32},
    {0x144u, Opcode::V_CUBEID_F32},
    {0x145u, Opcode::V_CUBESC_F32},
    {0x146u, Opcode::V_CUBETC_F32},
    {0x147u, Opcode::V_CUBEMA_F32},
    {0x14bu, Opcode::V_FMA_F32},
    {0x148u, Opcode::V_BFE_U32},
    {0x149u, Opcode::V_BFE_I32},
    {0x14au, Opcode::V_BFI_B32},
    {0x14eu, Opcode::V_ALIGNBIT_B32},
    {0x151u, Opcode::V_MIN3_F32},
    {0x152u, Opcode::V_MIN3_I32},
    {0x153u, Opcode::V_MIN3_U32},
    {0x351u, Opcode::V_MIN3_F16},
    {0x154u, Opcode::V_MAX3_F32},
    {0x155u, Opcode::V_MAX3_I32},
    {0x156u, Opcode::V_MAX3_U32},
    {0x354u, Opcode::V_MAX3_F16},
    {0x157u, Opcode::V_MED3_F32},
    {0x158u, Opcode::V_MED3_I32},
    {0x159u, Opcode::V_MED3_U32},
    {0x357u, Opcode::V_MED3_F16},
    {0x358u, Opcode::V_MED3_I16},
    {0x15du, Opcode::V_SAD_U32},
    {0x15eu, Opcode::V_CVT_PK_U8_F32},
    {0x178u, Opcode::V_XOR3_B32},
    {0x12fu, Opcode::V_CVT_PKRTZ_F16_F32},
    {0x169u, Opcode::V_MUL_LO_U32},
    {0x16au, Opcode::V_MUL_HI_U32},
    {0x16bu, Opcode::V_MUL_LO_I32},
    {0x16cu, Opcode::V_MUL_HI_I32},
    {0x2ffu, Opcode::V_LSHLREV_B64},
    {0x300u, Opcode::V_LSHRREV_B64},
    {0x303u, Opcode::V_ADD_NC_U16},
    {0x304u, Opcode::V_SUB_NC_U16},
    {0x307u, Opcode::V_LSHRREV_B16},
    {0x308u, Opcode::V_ASHRREV_I16},
    {0x309u, Opcode::V_MAX_U16},
    {0x30au, Opcode::V_MAX_I16},
    {0x30bu, Opcode::V_MIN_U16},
    {0x30cu, Opcode::V_MIN_I16},
    {0x30du, Opcode::V_ADD_NC_I16},
    {0x30eu, Opcode::V_SUB_NC_I16},
    {0x30fu, Opcode::V_ADD_I32},
    {0x310u, Opcode::V_SUB_I32},
    {0x311u, Opcode::V_PACK_B32_F16},
    {0x314u, Opcode::V_LSHLREV_B16},
    {0x319u, Opcode::V_SUBREV_I32},
    {0x345u, Opcode::V_XAD_U32},
    {0x346u, Opcode::V_LSHL_ADD_U32},
    {0x347u, Opcode::V_ADD_LSHL_U32},
    {0x360u, Opcode::V_READLANE_B32},
    {0x361u, Opcode::V_WRITELANE_B32},
    {0x362u, Opcode::V_LDEXP_F32},
    {0x363u, Opcode::V_BFM_B32},
    {0x364u, Opcode::V_BCNT_U32_B32},
    {0x365u, Opcode::V_MBCNT_LO_U32_B32},
    {0x366u, Opcode::V_MBCNT_HI_U32_B32},
    {0x368u, Opcode::V_CVT_PKNORM_I16_F32},
    {0x369u, Opcode::V_CVT_PKNORM_U16_F32},
    {0x36au, Opcode::V_CVT_PK_U16_U32},
    {0x36bu, Opcode::V_CVT_PK_I16_I32},
    {0x36fu, Opcode::V_LSHL_OR_B32},
    {0x371u, Opcode::V_AND_OR_B32},
    {0x372u, Opcode::V_OR3_B32},
    {0x377u, Opcode::V_PERMLANE16_B32},
    {0x378u, Opcode::V_PERMLANEX16_B32},
    {0x34bu, Opcode::V_FMA_F16},
    {0x36du, Opcode::V_ADD3_U32},
    {0x14fu, Opcode::V_ALIGNBYTE_B32},
};

constexpr auto VOP3_OPS = Detail::MakeOpcodeTable<0x400>(VOP3_OPCODE_LIST);

constexpr OpcodeMap VOP3P_OPCODE_LIST[] = {
    {0x00u, Opcode::V_PK_MAD_I16},     {0x01u, Opcode::V_PK_MUL_LO_U16},
    {0x02u, Opcode::V_PK_ADD_I16},     {0x03u, Opcode::V_PK_SUB_I16},
    {0x04u, Opcode::V_PK_LSHLREV_B16}, {0x05u, Opcode::V_PK_LSHRREV_B16},
    {0x06u, Opcode::V_PK_ASHRREV_I16}, {0x07u, Opcode::V_PK_MAX_I16},
    {0x08u, Opcode::V_PK_MIN_I16},     {0x09u, Opcode::V_PK_MAD_U16},
    {0x0au, Opcode::V_PK_ADD_U16},     {0x0bu, Opcode::V_PK_SUB_U16},
    {0x0cu, Opcode::V_PK_MAX_U16},     {0x0du, Opcode::V_PK_MIN_U16},
    {0x0eu, Opcode::V_PK_FMA_F16},     {0x0fu, Opcode::V_PK_ADD_F16},
    {0x10u, Opcode::V_PK_MUL_F16},     {0x11u, Opcode::V_PK_MIN_F16},
    {0x12u, Opcode::V_PK_MAX_F16},     {0x20u, Opcode::V_FMA_F32},
    {0x21u, Opcode::V_MAD_MIXLO_F16},  {0x22u, Opcode::V_MAD_MIXHI_F16},
};

constexpr auto VOP3P_OPS = Detail::MakeOpcodeTable<0x80>(VOP3P_OPCODE_LIST);

bool IsVop2LiteralMadOpcode(uint32_t opcode) {
	return opcode == 0x20u || opcode == 0x21u || opcode == 0x2cu || opcode == 0x2du;
}

bool IsUnsupportedVop3EncodedVop2Alias(uint32_t opcode) {
	return IsVop2LiteralMadOpcode(opcode) || opcode == 0x02u || opcode == 0x2bu ||
	       opcode == 0x39u || opcode == 0x3au;
}

Opcode LookupVop3Opcode(uint32_t opcode) {
	const auto direct = Detail::LookupOpcode(VOP3_OPS, opcode);
	if (direct != Opcode::UNSUPPORTED) {
		return direct;
	}
	if (opcode <= 0xffu) {
		return Detail::LookupOpcode(VOPC_OPS, opcode);
	}
	if (opcode >= 0x100u && opcode <= 0x13fu) {
		if (IsUnsupportedVop3EncodedVop2Alias(opcode - 0x100u)) {
			return Opcode::UNSUPPORTED;
		}
		return LookupVop2Opcode(opcode - 0x100u);
	}
	if (opcode >= 0x180u && opcode <= 0x1ffu) {
		return Detail::LookupOpcode(VOP3_ENCODED_VOP1_OPS, opcode - 0x180u);
	}
	return Opcode::UNSUPPORTED;
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
		case 0x00u: return Opcode::V_INTERP_P1_F32;
		case 0x01u: return Opcode::V_INTERP_P2_F32;
		case 0x02u: return Opcode::V_INTERP_MOV_F32;
		default: return Opcode::UNSUPPORTED;
	}
}

bool UsesScalarDestination(Opcode opcode) {
	return opcode == Opcode::V_READFIRSTLANE_B32 || opcode == Opcode::V_READLANE_B32;
}

bool IsVop3BCarryOutOpcode(Opcode opcode) {
	return opcode == Opcode::V_ADD_I32 || opcode == Opcode::V_SUB_I32 ||
	       opcode == Opcode::V_SUBREV_I32;
}

bool IsVop3BMadU64Opcode(Opcode opcode) {
	return opcode == Opcode::V_MAD_U64_U32;
}

bool IsPermlaneOpcode(Opcode opcode) {
	return opcode == Opcode::V_PERMLANE16_B32 || opcode == Opcode::V_PERMLANEX16_B32;
}

bool IsVop2LowHalfF16Opcode(Opcode opcode) {
	return opcode == Opcode::V_ADD_F16 || opcode == Opcode::V_SUB_F16 ||
	       opcode == Opcode::V_SUBREV_F16 || opcode == Opcode::V_MUL_F16 ||
	       opcode == Opcode::V_FMAC_F16 || opcode == Opcode::V_FMAMK_F16 ||
	       opcode == Opcode::V_FMAAK_F16 || opcode == Opcode::V_MAX_F16 ||
	       opcode == Opcode::V_MIN_F16;
}

bool IsNativeVop3F16TernaryOpcode(Opcode opcode) {
	return opcode == Opcode::V_MIN3_F16 || opcode == Opcode::V_MAX3_F16 ||
	       opcode == Opcode::V_MED3_F16 || opcode == Opcode::V_FMA_F16;
}

bool IsNativeVop3I16TernaryOpcode(Opcode opcode) {
	return opcode == Opcode::V_MED3_I16;
}

bool IsNativeVop3B16BinaryOpcode(Opcode opcode) {
	switch (opcode) {
		case Opcode::V_ADD_NC_U16:
		case Opcode::V_SUB_NC_U16:
		case Opcode::V_MAX_U16:
		case Opcode::V_MAX_I16:
		case Opcode::V_MIN_U16:
		case Opcode::V_MIN_I16:
		case Opcode::V_ADD_NC_I16:
		case Opcode::V_SUB_NC_I16:
		case Opcode::V_LSHLREV_B16:
		case Opcode::V_LSHRREV_B16:
		case Opcode::V_ASHRREV_I16: return true;
		default: return false;
	}
}

void ApplyDefaultVop2F16Destination(Instruction& inst) {
	if (IsVop2LowHalfF16Opcode(inst.opcode)) {
		inst.dst.sdwa_sel = 4;
	}
}

bool IsVop1FloatResultOpcode(Opcode opcode);
bool IsVopcCompareExec(Opcode opcode);

bool IsVop1FloatSourceOpcode(Opcode opcode) {
	switch (opcode) {
		case Opcode::V_MOV_B32:
		case Opcode::V_CVT_F32_F16:
		case Opcode::V_CVT_U32_F32:
		case Opcode::V_CVT_I32_F32:
		case Opcode::V_CVT_RPI_I32_F32:
		case Opcode::V_CVT_FLR_I32_F32:
		case Opcode::V_FREXP_EXP_I32_F32:
		case Opcode::V_FREXP_MANT_F32:
		case Opcode::V_CVT_F16_F32:
		case Opcode::V_RCP_F32:
		case Opcode::V_RCP_IFLAG_F32:
		case Opcode::V_FRACT_F32:
		case Opcode::V_TRUNC_F32:
		case Opcode::V_CEIL_F32:
		case Opcode::V_RNDNE_F32:
		case Opcode::V_FLOOR_F32:
		case Opcode::V_EXP_F32:
		case Opcode::V_LOG_F32:
		case Opcode::V_RSQ_F32:
		case Opcode::V_SQRT_F32:
		case Opcode::V_SQRT_F16:
		case Opcode::V_FLOOR_F16:
		case Opcode::V_CEIL_F16:
		case Opcode::V_TRUNC_F16:
		case Opcode::V_RNDNE_F16:
		case Opcode::V_COS_F16:
		case Opcode::V_SIN_F32:
		case Opcode::V_COS_F32: return true;
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
	Opcode   opcode                       = Opcode::UNKNOWN;
	uint32_t source_selectors             = SdwaSelFull();
	uint32_t partial_dst_selectors        = 0;
	uint32_t partial_dst_source_selectors = 0;
	bool     source_modifiers             = false;
};

constexpr Vop1SdwaRule VOP1_SDWA_RULES[] = {
    {Opcode::V_MOV_B32, SdwaSelBytes() | SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::V_CVT_F32_U32, SdwaSelBytes() | SdwaSelWords() | SdwaSelFull(), 0, 0, false},
    {Opcode::V_CVT_F32_I32, SdwaSelBytes() | SdwaSelWords() | SdwaSelFull(), 0, 0, false},
    {Opcode::V_CVT_F32_UBYTE0, SdwaSelBytes() | SdwaSelWords() | SdwaSelFull(), 0, 0, false},
    {Opcode::V_NOT_B32, SdwaSelBytes() | SdwaSelWords() | SdwaSelFull(), 0, 0, false},
    {Opcode::V_FFBL_B32, SdwaSelBytes() | SdwaSelWords() | SdwaSelFull(), 0, 0, false},
    {Opcode::V_CVT_F32_F16, SdwaSelWords() | SdwaSelFull(), 0, 0, true},
    {Opcode::V_CVT_F16_F32, SdwaSelFull(), SdwaSelWords(), SdwaSelFull(), false},
    {Opcode::V_CVT_F16_U16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::V_CVT_U16_F16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::V_CVT_F16_I16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::V_CVT_I16_F16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::V_RCP_F16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), true},
    {Opcode::V_SQRT_F16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::V_RSQ_F16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::V_LOG_F16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), true},
    {Opcode::V_EXP_F16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), true},
    {Opcode::V_FLOOR_F16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::V_CEIL_F16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::V_TRUNC_F16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::V_RNDNE_F16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), false},
    {Opcode::V_COS_F16, SdwaSelWords() | SdwaSelFull(), SdwaSelWords(),
     SdwaSelWords() | SdwaSelFull(), true},
    {Opcode::V_CVT_U32_F32, SdwaSelFull(), SdwaSelBytes() | SdwaSelWords(), SdwaSelFull(), false},
    {Opcode::V_CVT_I32_F32, SdwaSelFull(), SdwaSelBytes() | SdwaSelWords(), SdwaSelFull(), false},
    {Opcode::V_CVT_RPI_I32_F32, SdwaSelFull(), SdwaSelBytes() | SdwaSelWords(), SdwaSelFull(),
     false},
    {Opcode::V_CVT_FLR_I32_F32, SdwaSelFull(), SdwaSelBytes() | SdwaSelWords(), SdwaSelFull(),
     false},
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
	if ((opcode == Opcode::V_CVT_U16_F16 || opcode == Opcode::V_CVT_I16_F16) && dst_sel == 6u) {
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
		case Opcode::V_CVT_U16_F16:
		case Opcode::V_CVT_I16_F16:
		case Opcode::V_SQRT_F16:
		case Opcode::V_RSQ_F16: return src_neg || src_abs;
		case Opcode::V_LOG_F16: return src_neg;
		default: return false;
	}
}

bool ValidateVop1Sdwa(Instruction& inst, uint32_t opcode, uint32_t modifier) {
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
	if ((clamp != 0u || omod != 0u) && !IsVop1FloatResultOpcode(inst.opcode)) {
		SetUnsupported(inst, Family::VOP1, opcode, "VOP1 SDWA output modifiers are not supported");
		return false;
	}
	if (!IsVop1SdwaDestinationSupported(inst.opcode, dst_sel, dst_u, src0_sel)) {
		SetUnsupported(inst, Family::VOP1, opcode,
		               "VOP1 SDWA destination selector is not supported");
		return false;
	}
	if (HasUnsupportedVop1SdwaSourceModifiers(inst.opcode, src0_neg != 0u, src0_abs != 0u)) {
		SetUnsupported(inst, Family::VOP1, opcode, "VOP1 SDWA source modifiers are not supported");
		return false;
	}
	if (!IsVop1SdwaSourceSupported(inst.opcode, src0_sel, src0_neg != 0u, src0_abs != 0u)) {
		SetUnsupported(inst, Family::VOP1, opcode, "VOP1 SDWA source selector is not supported");
		return false;
	}
	return true;
}

bool DecodeVop1Sdwa(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                    uint32_t opcode, uint32_t vdst, Instruction& inst, std::string* error) {
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

	const bool scalar_dst = UsesScalarDestination(inst.opcode);
	if (scalar_dst) {
		if (!DecodeScalarDestination(vdst, pc, inst.dst, error)) {
			return false;
		}
	} else {
		DecodeVectorGpr(vdst, inst.dst);
	}
	if (!DecodeScalarSource(src0 + (s0 == 0u ? 256u : 0u), pc, inst.src0, error)) {
		return false;
	}
	inst.dst.sdwa_sel        = dst_sel;
	inst.dst.sdwa_dst_unused = dst_u;
	inst.dst.clamp           = clamp != 0u;
	inst.dst.omod            = omod;
	inst.src0.sdwa_sel       = src0_sel;
	inst.src0.sdwa_sext      = src0_sext != 0u;
	inst.src0.negate         = src0_neg != 0u;
	inst.src0.absolute       = src0_abs != 0u;
	inst.src_count           = 1;
	ReadLiteralOperands(code, word_index, inst);
	return true;
}

void ApplyDppModifier(Operand& operand, uint32_t modifier) {
	operand.negate             = ((modifier >> 20u) & 0x1u) != 0u;
	operand.absolute           = ((modifier >> 21u) & 0x1u) != 0u;
	operand.dpp                = true;
	operand.dpp_ctrl           = (modifier >> 8u) & 0x1ffu;
	operand.dpp_fetch_inactive = ((modifier >> 18u) & 0x1u) != 0u;
	operand.dpp_bound_ctrl     = ((modifier >> 19u) & 0x1u) != 0u;
	operand.dpp_bank_mask      = (modifier >> 24u) & 0xfu;
	operand.dpp_row_mask       = (modifier >> 28u) & 0xfu;
}

bool DecodeVop1Dpp(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                   uint32_t opcode, uint32_t vdst, Instruction& inst, std::string* error) {
	const auto modifier = code[word_index + 1u];
	const auto src0     = modifier & 0xffu;
	SetRawWords(inst, code, word_index, 2);

	const bool scalar_dst = UsesScalarDestination(inst.opcode);
	if (scalar_dst) {
		if (!DecodeScalarDestination(vdst, pc, inst.dst, error)) {
			return false;
		}
	} else {
		DecodeVectorGpr(vdst, inst.dst);
	}
	if (!DecodeScalarSource(src0 + 256u, pc, inst.src0, error)) {
		return false;
	}
	ApplyDppModifier(inst.src0, modifier);
	inst.src_count = 1;

	if (!IsVop1FloatSourceOpcode(inst.opcode) && (inst.src0.negate || inst.src0.absolute)) {
		SetUnsupported(inst, Family::VOP1, opcode,
		               "VOP1 DPP integer source modifiers are not supported");
		return true;
	}
	ReadLiteralOperands(code, word_index, inst);
	return true;
}

using Vop1ModifierDecodeFn = bool (*)(uint32_t pc, std::span<const uint32_t> code,
                                      uint32_t word_index, uint32_t opcode, uint32_t vdst,
                                      Instruction& inst, std::string* error);

struct Vop1ModifierDecoder {
	uint32_t             escape = 0;
	Vop1ModifierDecodeFn decode = nullptr;
};

constexpr Vop1ModifierDecoder VOP1_MODIFIER_DECODERS[] = {
    {249u, DecodeVop1Sdwa},
    {250u, DecodeVop1Dpp},
};

bool TryDecodeVop1Modifier(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                           uint32_t src0, uint32_t opcode, uint32_t vdst, Instruction& inst,
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
		case Opcode::V_ADD_F32:
		case Opcode::V_SUB_F32:
		case Opcode::V_SUBREV_F32:
		case Opcode::V_MUL_F32:
		case Opcode::V_MIN_F32:
		case Opcode::V_MAX_F32:
		case Opcode::V_MAC_F32:
		case Opcode::V_MADMK_F32:
		case Opcode::V_MADAK_F32:
		case Opcode::V_ADD_F16:
		case Opcode::V_SUB_F16:
		case Opcode::V_SUBREV_F16:
		case Opcode::V_MUL_F16:
		case Opcode::V_FMAC_F16:
		case Opcode::V_FMAMK_F16:
		case Opcode::V_FMAAK_F16:
		case Opcode::V_MAX_F16:
		case Opcode::V_MIN_F16: return true;
		default: return false;
	}
}

bool IsVop2FloatResultOpcode(Opcode opcode) {
	switch (opcode) {
		case Opcode::V_ADD_F32:
		case Opcode::V_SUB_F32:
		case Opcode::V_SUBREV_F32:
		case Opcode::V_MUL_F32:
		case Opcode::V_MIN_F32:
		case Opcode::V_MAX_F32:
		case Opcode::V_MAC_F32:
		case Opcode::V_MADMK_F32:
		case Opcode::V_MADAK_F32:
		case Opcode::V_ADD_F16:
		case Opcode::V_SUB_F16:
		case Opcode::V_SUBREV_F16:
		case Opcode::V_MUL_F16:
		case Opcode::V_FMAC_F16:
		case Opcode::V_FMAMK_F16:
		case Opcode::V_FMAAK_F16:
		case Opcode::V_MAX_F16:
		case Opcode::V_MIN_F16: return true;
		default: return false;
	}
}

bool IsVop1FloatResultOpcode(Opcode opcode) {
	switch (opcode) {
		case Opcode::V_CVT_F32_I32:
		case Opcode::V_CVT_F32_U32:
		case Opcode::V_CVT_F32_F16:
		case Opcode::V_CVT_OFF_F32_I4:
		case Opcode::V_CVT_F32_UBYTE0:
		case Opcode::V_CVT_F32_UBYTE1:
		case Opcode::V_CVT_F32_UBYTE2:
		case Opcode::V_CVT_F32_UBYTE3:
		case Opcode::V_RCP_F32:
		case Opcode::V_RCP_IFLAG_F32:
		case Opcode::V_FRACT_F32:
		case Opcode::V_TRUNC_F32:
		case Opcode::V_CEIL_F32:
		case Opcode::V_RNDNE_F32:
		case Opcode::V_FLOOR_F32:
		case Opcode::V_FREXP_MANT_F32:
		case Opcode::V_EXP_F32:
		case Opcode::V_LOG_F32:
		case Opcode::V_RSQ_F32:
		case Opcode::V_SQRT_F32:
		case Opcode::V_RCP_F16:
		case Opcode::V_RSQ_F16:
		case Opcode::V_LOG_F16:
		case Opcode::V_EXP_F16:
		case Opcode::V_COS_F16:
		case Opcode::V_SIN_F32:
		case Opcode::V_COS_F32: return true;
		default: return false;
	}
}

bool IsVopcFloatCompareOpcode(Opcode opcode) {
	switch (opcode) {
		case Opcode::V_CMP_F_F32:
		case Opcode::V_CMP_LT_F32:
		case Opcode::V_CMP_EQ_F32:
		case Opcode::V_CMP_LE_F32:
		case Opcode::V_CMP_GT_F32:
		case Opcode::V_CMP_LG_F32:
		case Opcode::V_CMP_GE_F32:
		case Opcode::V_CMP_O_F32:
		case Opcode::V_CMP_U_F32:
		case Opcode::V_CMP_NGE_F32:
		case Opcode::V_CMP_NLG_F32:
		case Opcode::V_CMP_NGT_F32:
		case Opcode::V_CMP_NLE_F32:
		case Opcode::V_CMP_NEQ_F32:
		case Opcode::V_CMP_NLT_F32:
		case Opcode::V_CMP_TRU_F32:
		case Opcode::V_CMPX_LT_F32:
		case Opcode::V_CMPX_EQ_F32:
		case Opcode::V_CMPX_LE_F32:
		case Opcode::V_CMPX_GT_F32:
		case Opcode::V_CMPX_LG_F32:
		case Opcode::V_CMPX_GE_F32:
		case Opcode::V_CMPX_NGE_F32:
		case Opcode::V_CMPX_NLG_F32:
		case Opcode::V_CMPX_NGT_F32:
		case Opcode::V_CMPX_NLE_F32:
		case Opcode::V_CMPX_NEQ_F32:
		case Opcode::V_CMPX_NLT_F32:
		case Opcode::V_CMP_LT_F16:
		case Opcode::V_CMP_EQ_F16:
		case Opcode::V_CMP_LE_F16:
		case Opcode::V_CMP_GT_F16:
		case Opcode::V_CMP_LG_F16:
		case Opcode::V_CMP_GE_F16:
		case Opcode::V_CMP_NEQ_F16:
		case Opcode::V_CMPX_LT_F16:
		case Opcode::V_CMPX_EQ_F16:
		case Opcode::V_CMPX_LE_F16:
		case Opcode::V_CMPX_GT_F16:
		case Opcode::V_CMPX_GE_F16:
		case Opcode::V_CMPX_NGT_F16:
		case Opcode::V_CMPX_NEQ_F16:
		case Opcode::V_CMPX_NLT_F16:
		case Opcode::V_CMP_CLASS_F32: return true;
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
    {},
    {SdwaSelFull(), SdwaSelFull(), SdwaSelFull(), false, true},
    {SdwaSelWords() | SdwaSelFull(), SdwaSelWords() | SdwaSelFull(), SdwaSelWords() | SdwaSelFull(),
     true, true},
    {SdwaSelFull(), SdwaSelAll(), SdwaSelAll(), false, false},
    {SdwaSelAll(), SdwaSelAll(), SdwaSelAll(), true, false},
    {SdwaSelFull(), SdwaSelAll(), SdwaSelWords() | SdwaSelFull(), false, false},
    {SdwaSelFull(), SdwaSelAll(), SdwaSelAll(), false, false},
    {SdwaSelWords() | SdwaSelFull(), SdwaSelAll(), SdwaSelAll(), true, false},
};
static_assert(sizeof(VOP2_SDWA_RULES) / sizeof(VOP2_SDWA_RULES[0]) ==
              static_cast<size_t>(Vop2SdwaProfile::Count));

const Vop2SdwaRule* FindVop2SdwaRule(uint32_t encoding) {
	const auto* info = Detail::FindOpcode(VOP2_OPS, encoding);
	if (info == nullptr || info->sdwa_profile == Vop2SdwaProfile::None) {
		return nullptr;
	}
	return &VOP2_SDWA_RULES[static_cast<size_t>(info->sdwa_profile)];
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

bool ValidateVop2Sdwa(Instruction& inst, uint32_t opcode, const Vop2SdwaFields& fields) {
	if (fields.src0_sel > 6u || fields.src1_sel > 6u || fields.dst_sel > 6u) {
		SetUnsupported(inst, Family::VOP2, opcode, "VOP2 SDWA selector is invalid");
		return false;
	}
	if ((fields.clamp != 0u || fields.omod != 0u) && !IsVop2FloatResultOpcode(inst.opcode)) {
		SetUnsupported(inst, Family::VOP2, opcode, "VOP2 SDWA output modifiers are not supported");
		return false;
	}
	if (IsFullWidthVop2Sdwa(fields)) {
		if (!IsVop2FloatSourceOpcode(inst.opcode) && inst.opcode != Opcode::V_CNDMASK_B32 &&
		    (fields.src0_neg != 0u || fields.src0_abs != 0u || fields.src1_neg != 0u ||
		     fields.src1_abs != 0u)) {
			SetUnsupported(inst, Family::VOP2, opcode,
			               "VOP2 SDWA source modifiers are not supported");
			return false;
		}
		return true;
	}

	const auto* rule = FindVop2SdwaRule(opcode);
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
	    inst.opcode == Opcode::V_CNDMASK_B32 && fields.src0_sel == 6u && fields.src1_sel == 6u;
	if (!rule->source_modifiers && has_source_modifiers && !cndmask_float_modifiers) {
		SetUnsupported(inst, Family::VOP2, opcode, "VOP2 SDWA source modifiers are not supported");
		return false;
	}
	return true;
}

bool FinalizeVop2Instruction(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                             Instruction& inst, std::string* error) {
	(void)pc;
	switch (inst.opcode) {
		case Opcode::V_MADMK_F32:
		case Opcode::V_FMAMK_F16:
			inst.src2      = inst.src1;
			inst.src1      = {};
			inst.src1.kind = OperandKind::LiteralConstant;
			inst.src_count = 3;
			break;
		case Opcode::V_MADAK_F32:
		case Opcode::V_FMAAK_F16:
			inst.src2      = {};
			inst.src2.kind = OperandKind::LiteralConstant;
			inst.src_count = 3;
			break;
		case Opcode::V_ADDC_U32:
		case Opcode::V_SUBREV_CO_CI_U32:
			inst.dst2.kind = OperandKind::VccLo;
			inst.src2.kind = OperandKind::VccLo;
			inst.src_count = 3;
			break;
		default: inst.src_count = 2; break;
	}
	ReadLiteralOperands(code, word_index, inst);
	return true;
}

bool DecodeVop2Sdwa(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                    uint32_t opcode, uint32_t vdst, uint32_t vsrc1, Instruction& inst,
                    std::string* error) {
	const auto modifier = code[word_index + 1u];
	const auto fields   = DecodeVop2SdwaFields(modifier);
	SetRawWords(inst, code, word_index, 2);
	if (!ValidateVop2Sdwa(inst, opcode, fields)) {
		return true;
	}

	DecodeVectorGpr(vdst, inst.dst);
	if (!DecodeScalarSource(fields.src0 + (fields.s0 == 0u ? 256u : 0u), pc, inst.src0, error) ||
	    !DecodeScalarSource(vsrc1 + (fields.s1 == 0u ? 256u : 0u), pc, inst.src1, error)) {
		return false;
	}
	inst.dst.sdwa_sel        = fields.dst_sel;
	inst.dst.sdwa_dst_unused = fields.dst_u;
	inst.dst.clamp           = fields.clamp != 0u;
	inst.dst.omod            = fields.omod;
	inst.src0.sdwa_sel       = fields.src0_sel;
	inst.src0.sdwa_sext      = fields.src0_sext != 0u;
	inst.src0.negate         = fields.src0_neg != 0u;
	inst.src0.absolute       = fields.src0_abs != 0u;
	inst.src1.sdwa_sel       = fields.src1_sel;
	inst.src1.sdwa_sext      = fields.src1_sext != 0u;
	inst.src1.negate         = fields.src1_neg != 0u;
	inst.src1.absolute       = fields.src1_abs != 0u;
	return FinalizeVop2Instruction(pc, code, word_index, inst, error);
}

bool DecodeVop2Dpp(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                   uint32_t opcode, uint32_t vdst, uint32_t vsrc1, Instruction& inst,
                   std::string* error) {
	const auto modifier = code[word_index + 1u];
	const auto src0     = modifier & 0xffu;
	SetRawWords(inst, code, word_index, 2);

	DecodeVectorGpr(vdst, inst.dst);
	DecodeVectorGpr(vsrc1, inst.src1);
	if (!DecodeScalarSource(src0 + 256u, pc, inst.src0, error)) {
		return false;
	}
	ApplyDefaultVop2F16Destination(inst);
	ApplyDppModifier(inst.src0, modifier);
	inst.src1.negate   = ((modifier >> 22u) & 0x1u) != 0u;
	inst.src1.absolute = ((modifier >> 23u) & 0x1u) != 0u;

	if (!IsVop2FloatSourceOpcode(inst.opcode) &&
	    (inst.src0.negate || inst.src0.absolute || inst.src1.negate || inst.src1.absolute)) {
		SetUnsupported(inst, Family::VOP2, opcode,
		               "VOP2 DPP integer source modifiers are not supported");
		return true;
	}
	return FinalizeVop2Instruction(pc, code, word_index, inst, error);
}

using Vop2ModifierDecodeFn = bool (*)(uint32_t pc, std::span<const uint32_t> code,
                                      uint32_t word_index, uint32_t opcode, uint32_t vdst,
                                      uint32_t vsrc1, Instruction& inst, std::string* error);

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
                           Instruction& inst, std::string* error, bool* handled) {
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
	return opcode != Opcode::UNSUPPORTED;
}

bool DecodeVopcSdwa(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                    uint32_t opcode, uint32_t vsrc1, Instruction& inst, std::string* error) {
	const auto modifier = code[word_index + 1u];
	const auto fields   = DecodeVopcSdwaFields(modifier);
	SetRawWords(inst, code, word_index, 2);
	if (fields.src0_sel > 6u || fields.src1_sel > 6u) {
		SetUnsupported(inst, Family::VOPC, opcode, "VOPC SDWA selector is invalid");
		return true;
	}
	if (!SupportsVopcSdwa(inst.opcode)) {
		SetUnsupported(inst, Family::VOPC, opcode,
		               "VOPC SDWA modifier is not supported for opcode");
		return true;
	}

	if (!DecodeScalarSource(fields.src0 + (fields.s0 == 0u ? 256u : 0u), pc, inst.src0, error) ||
	    !DecodeScalarSource(vsrc1 + (fields.s1 == 0u ? 256u : 0u), pc, inst.src1, error)) {
		return false;
	}
	if (IsVopcCompareExec(inst.opcode)) {
		inst.dst.kind = OperandKind::ExecLo;
	} else if (fields.sd == 0u) {
		inst.dst.kind = OperandKind::VccLo;
	} else if (!DecodeScalarDestination(fields.sdst, pc, inst.dst, error)) {
		return false;
	}
	inst.src0.sdwa_sel  = fields.src0_sel;
	inst.src0.sdwa_sext = fields.src0_sext != 0u;
	inst.src0.negate    = fields.src0_neg != 0u;
	inst.src0.absolute  = fields.src0_abs != 0u;
	inst.src1.sdwa_sel  = fields.src1_sel;
	inst.src1.sdwa_sext = fields.src1_sext != 0u;
	inst.src1.negate    = fields.src1_neg != 0u;
	inst.src1.absolute  = fields.src1_abs != 0u;
	inst.src_count      = 2;
	ReadLiteralOperands(code, word_index, inst);
	return true;
}

bool DecodeVopcDpp(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                   uint32_t opcode, uint32_t vsrc1, Instruction& inst, std::string* error) {
	const auto modifier = code[word_index + 1u];
	const auto src0     = modifier & 0xffu;
	SetRawWords(inst, code, word_index, 2);
	if (inst.opcode == Opcode::UNSUPPORTED) {
		SetUnsupported(inst, Family::VOPC, opcode, "VOPC opcode is not implemented");
		return true;
	}
	const auto* info = Detail::FindOpcode(VOPC_OPS, opcode);
	if (info == nullptr || !info->supports_dpp) {
		SetUnsupported(inst, Family::VOPC, opcode, "VOPC DPP modifier is not supported for opcode");
		return true;
	}
	DecodeVectorGpr(vsrc1, inst.src1);
	if (!DecodeScalarSource(src0 + 256u, pc, inst.src0, error)) {
		return false;
	}
	inst.dst.kind = IsVopcCompareExec(inst.opcode) ? OperandKind::ExecLo : OperandKind::VccLo;
	ApplyDppModifier(inst.src0, modifier);
	inst.src1.negate   = ((modifier >> 22u) & 0x1u) != 0u;
	inst.src1.absolute = ((modifier >> 23u) & 0x1u) != 0u;
	inst.src_count     = 2;
	ReadLiteralOperands(code, word_index, inst);
	return true;
}

uint32_t NativeVop3SourceCount(Opcode opcode) {
	switch (opcode) {
		case Opcode::V_MUL_LO_U32:
		case Opcode::V_MUL_HI_U32:
		case Opcode::V_MUL_LO_I32:
		case Opcode::V_MUL_HI_I32:
		case Opcode::V_MUL_I32_I24:
		case Opcode::V_ADD_NC_U16:
		case Opcode::V_SUB_NC_U16:
		case Opcode::V_MAX_U16:
		case Opcode::V_MAX_I16:
		case Opcode::V_MIN_U16:
		case Opcode::V_MIN_I16:
		case Opcode::V_ADD_NC_I16:
		case Opcode::V_SUB_NC_I16:
		case Opcode::V_LSHLREV_B64:
		case Opcode::V_LSHRREV_B64:
		case Opcode::V_LSHLREV_B16:
		case Opcode::V_LSHRREV_B16:
		case Opcode::V_ASHRREV_I16:
		case Opcode::V_ADD_I32:
		case Opcode::V_SUB_I32:
		case Opcode::V_SUBREV_I32:
		case Opcode::V_PACK_B32_F16:
		case Opcode::V_READLANE_B32:
		case Opcode::V_WRITELANE_B32:
		case Opcode::V_CVT_PKRTZ_F16_F32:
		case Opcode::V_LDEXP_F32:
		case Opcode::V_BFM_B32:
		case Opcode::V_BCNT_U32_B32:
		case Opcode::V_CVT_PKNORM_I16_F32:
		case Opcode::V_CVT_PKNORM_U16_F32:
		case Opcode::V_CVT_PK_U16_U32:
		case Opcode::V_CVT_PK_I16_I32: return 2;
		default: return 3;
	}
}

uint32_t Vop3pSourceCount(Opcode opcode) {
	switch (opcode) {
		case Opcode::V_PK_MUL_LO_U16:
		case Opcode::V_PK_ADD_I16:
		case Opcode::V_PK_SUB_I16:
		case Opcode::V_PK_LSHLREV_B16:
		case Opcode::V_PK_LSHRREV_B16:
		case Opcode::V_PK_ASHRREV_I16:
		case Opcode::V_PK_MAX_I16:
		case Opcode::V_PK_MIN_I16:
		case Opcode::V_PK_ADD_U16:
		case Opcode::V_PK_SUB_U16:
		case Opcode::V_PK_MAX_U16:
		case Opcode::V_PK_MIN_U16:
		case Opcode::V_PK_ADD_F16:
		case Opcode::V_PK_MUL_F16:
		case Opcode::V_PK_MIN_F16:
		case Opcode::V_PK_MAX_F16: return 2;
		default: return 3;
	}
}

bool IsPackedVop3p(Opcode opcode) {
	switch (opcode) {
		case Opcode::V_PK_ADD_F16:
		case Opcode::V_PK_MUL_F16:
		case Opcode::V_PK_MIN_F16:
		case Opcode::V_PK_MAX_F16:
		case Opcode::V_PK_FMA_F16: return true;
		default: return false;
	}
}

bool IsMadMixF16(Opcode opcode) {
	return opcode == Opcode::V_MAD_MIXLO_F16 || opcode == Opcode::V_MAD_MIXHI_F16;
}

void ApplyVop3pSourceModifiers(Instruction& inst, uint32_t op_sel, uint32_t op_sel_hi, uint32_t neg,
                               uint32_t neg_hi) {
	Operand* sources[] = {&inst.src0, &inst.src1, &inst.src2};
	for (uint32_t i = 0; i < 3u; i++) {
		sources[i]->op_sel    = ((op_sel >> i) & 1u) != 0;
		sources[i]->op_sel_hi = ((op_sel_hi >> i) & 1u) != 0;
		sources[i]->negate    = ((neg >> i) & 1u) != 0;
		sources[i]->negate_hi = ((neg_hi >> i) & 1u) != 0;
	}
}

void ApplyVop3pMixAbsModifiers(Instruction& inst) {
	Operand* sources[] = {&inst.src0, &inst.src1, &inst.src2};
	for (auto* source: sources) {
		// RDNA2 MIX opcodes reuse the VOP3P NEG_HI bits as source absolute modifiers.
		source->absolute  = source->negate_hi;
		source->negate_hi = false;
	}
}

void ApplyNativeVop3TernaryModifiers(Instruction& inst, uint32_t op_sel, uint32_t abs,
                                     uint32_t neg) {
	Operand* sources[] = {&inst.src0, &inst.src1, &inst.src2};
	for (uint32_t i = 0; i < 3u; i++) {
		sources[i]->op_sel    = ((op_sel >> i) & 1u) != 0;
		sources[i]->op_sel_hi = true;
		sources[i]->negate    = ((neg >> i) & 1u) != 0;
		sources[i]->absolute  = ((abs >> i) & 1u) != 0;
	}
	inst.dst.sdwa_sel = ((op_sel & 0x8u) != 0) ? 5u : 4u;
}

void ApplyNativeVop3I16TernarySelectors(Instruction& inst, uint32_t op_sel) {
	Operand* sources[] = {&inst.src0, &inst.src1, &inst.src2};
	for (uint32_t i = 0; i < 3u; i++) {
		sources[i]->op_sel = ((op_sel >> i) & 1u) != 0;
	}
	inst.dst.sdwa_sel = (op_sel & 0x8u) != 0 ? 5u : 4u;
}

void ApplyNativeVop3B16BinaryModifiers(Instruction& inst, uint32_t op_sel) {
	inst.src0.op_sel  = (op_sel & 0x1u) != 0;
	inst.src1.op_sel  = (op_sel & 0x2u) != 0;
	inst.dst.sdwa_sel = (op_sel & 0x8u) != 0 ? 5u : 4u;
}

void ApplyNativeVop3PackB32F16Modifiers(Instruction& inst, uint32_t op_sel, uint32_t abs,
                                        uint32_t neg) {
	inst.src0.op_sel   = (op_sel & 0x1u) != 0;
	inst.src1.op_sel   = (op_sel & 0x2u) != 0;
	inst.src0.negate   = (neg & 0x1u) != 0;
	inst.src1.negate   = (neg & 0x2u) != 0;
	inst.src0.absolute = (abs & 0x1u) != 0;
	inst.src1.absolute = (abs & 0x2u) != 0;
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
		case Opcode::V_CNDMASK_B32:
		case Opcode::V_ADD_F32:
		case Opcode::V_SUB_F32:
		case Opcode::V_SUBREV_F32:
		case Opcode::V_MUL_F32:
		case Opcode::V_MIN_F32:
		case Opcode::V_MAX_F32:
		case Opcode::V_MAC_F32:
		case Opcode::V_MAD_F32:
		case Opcode::V_FMA_F32:
		case Opcode::V_PACK_B32_F16:
		case Opcode::V_CUBEID_F32:
		case Opcode::V_CUBESC_F32:
		case Opcode::V_CUBETC_F32:
		case Opcode::V_CUBEMA_F32:
		case Opcode::V_CVT_PKRTZ_F16_F32:
		case Opcode::V_CVT_PKNORM_I16_F32:
		case Opcode::V_CVT_PKNORM_U16_F32:
		case Opcode::V_MIN3_F32:
		case Opcode::V_MAX3_F32:
		case Opcode::V_MED3_F32:
		case Opcode::V_LDEXP_F32: return true;
		default: return false;
	}
}

bool SupportsNativeVop3ResultModifiers(Opcode opcode) {
	if (IsVop1FloatResultOpcode(opcode)) {
		return true;
	}
	switch (opcode) {
		case Opcode::V_ADD_F32:
		case Opcode::V_SUB_F32:
		case Opcode::V_SUBREV_F32:
		case Opcode::V_MUL_F32:
		case Opcode::V_MIN_F32:
		case Opcode::V_MAX_F32:
		case Opcode::V_MAC_F32:
		case Opcode::V_MAD_F32:
		case Opcode::V_FMA_F32:
		case Opcode::V_FMA_F16:
		case Opcode::V_LDEXP_F32:
		case Opcode::V_MIN3_F32:
		case Opcode::V_MAX3_F32:
		case Opcode::V_MED3_F32: return true;
		default: return false;
	}
}

bool HasUnsupportedNativeVop3Modifiers(Opcode opcode, bool permlane, bool mad_mix,
                                       bool carry_in_out, bool scalar_dst, uint32_t abs,
                                       uint32_t op_sel, uint32_t clamp, uint32_t omod,
                                       uint32_t neg) {
	const bool source_modifiers = SupportsNativeVop3SourceModifiers(opcode);
	const bool result_modifiers = SupportsNativeVop3ResultModifiers(opcode);

	if (permlane) {
		return abs != 0u || (op_sel & ~0x3u) != 0u || clamp != 0u || omod != 0u || neg != 0u;
	}
	if (mad_mix) {
		return clamp != 0u || omod != 0u;
	}
	if (IsNativeVop3F16TernaryOpcode(opcode)) {
		return opcode != Opcode::V_FMA_F16 && (clamp != 0u || omod != 0u);
	}
	if (IsNativeVop3I16TernaryOpcode(opcode)) {
		return abs != 0u || clamp != 0u || omod != 0u || neg != 0u;
	}
	if (IsNativeVop3B16BinaryOpcode(opcode)) {
		return abs != 0u || clamp != 0u || omod != 0u || neg != 0u;
	}
	if (carry_in_out || scalar_dst) {
		return clamp != 0u || omod != 0u || neg != 0u;
	}
	switch (opcode) {
		case Opcode::V_LDEXP_F32: return (abs & ~1u) != 0u || op_sel != 0u || (neg & ~1u) != 0u;
		case Opcode::V_CNDMASK_B32:
			return (abs & ~0x3u) != 0u || op_sel != 0u || clamp != 0u || omod != 0u ||
			       (neg & ~0x3u) != 0u;
		case Opcode::V_PACK_B32_F16:
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

void ApplyNativeVop3SourceModifiers(Instruction& inst, uint32_t abs, uint32_t neg) {
	Operand* sources[] = {&inst.src0, &inst.src1, &inst.src2};
	for (uint32_t i = 0; i < inst.src_count && i < 3u; i++) {
		sources[i]->absolute = ((abs >> i) & 1u) != 0;
		sources[i]->negate   = ((neg >> i) & 1u) != 0;
	}
}

bool IsVopcCompareExec(Opcode opcode) {
	switch (opcode) {
		case Opcode::V_CMPX_LT_F32:
		case Opcode::V_CMPX_EQ_F32:
		case Opcode::V_CMPX_LE_F32:
		case Opcode::V_CMPX_GT_F32:
		case Opcode::V_CMPX_LG_F32:
		case Opcode::V_CMPX_GE_F32:
		case Opcode::V_CMPX_NGE_F32:
		case Opcode::V_CMPX_NLG_F32:
		case Opcode::V_CMPX_NGT_F32:
		case Opcode::V_CMPX_NLE_F32:
		case Opcode::V_CMPX_NEQ_F32:
		case Opcode::V_CMPX_NLT_F32:
		case Opcode::V_CMPX_LT_I32:
		case Opcode::V_CMPX_EQ_I32:
		case Opcode::V_CMPX_LE_I32:
		case Opcode::V_CMPX_GT_I32:
		case Opcode::V_CMPX_NE_I32:
		case Opcode::V_CMPX_GE_I32:
		case Opcode::V_CMPX_LT_U32:
		case Opcode::V_CMPX_EQ_U32:
		case Opcode::V_CMPX_LE_U32:
		case Opcode::V_CMPX_GT_U32:
		case Opcode::V_CMPX_NE_U32:
		case Opcode::V_CMPX_GE_U32:
		case Opcode::V_CMPX_NE_I64:
		case Opcode::V_CMPX_NE_U64:
		case Opcode::V_CMPX_GT_U16:
		case Opcode::V_CMPX_LT_F16:
		case Opcode::V_CMPX_EQ_F16:
		case Opcode::V_CMPX_LE_F16:
		case Opcode::V_CMPX_GT_F16:
		case Opcode::V_CMPX_GE_F16:
		case Opcode::V_CMPX_NGT_F16:
		case Opcode::V_CMPX_NEQ_F16:
		case Opcode::V_CMPX_NLT_F16: return true;
		default: return false;
	}
}

} // namespace

bool DecodeVop2(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction& inst,
                std::string* error) {
	const uint32_t word   = code[word_index];
	const uint32_t opcode = (word >> 25u) & 0x3fu;
	const uint32_t vdst   = (word >> 17u) & 0xffu;
	const uint32_t src0   = word & 0x1ffu;
	const uint32_t vsrc1  = (word >> 9u) & 0xffu;

	inst.pc        = pc;
	inst.word      = word;
	inst.family    = Family::VOP2;
	inst.opcode_id = opcode;
	inst.opcode    = LookupVop2Opcode(opcode);
	SetRawWords(inst, code, word_index, 1);

	if (inst.opcode == Opcode::UNSUPPORTED) {
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

	DecodeVectorGpr(vdst, inst.dst);
	DecodeVectorGpr(vsrc1, inst.src1);
	if (!DecodeScalarSource(src0, pc, inst.src0, error)) {
		return false;
	}
	ApplyDefaultVop2F16Destination(inst);
	return FinalizeVop2Instruction(pc, code, word_index, inst, error);
}

bool DecodeVop1(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction& inst,
                std::string* error) {
	const uint32_t word   = code[word_index];
	const uint32_t opcode = (word >> 9u) & 0xffu;
	const uint32_t src0   = word & 0x1ffu;
	const uint32_t vdst   = (word >> 17u) & 0xffu;

	inst.pc        = pc;
	inst.word      = word;
	inst.family    = Family::VOP1;
	inst.opcode_id = opcode;
	inst.opcode    = Detail::LookupOpcode(VOP1_OPS, opcode);
	SetRawWords(inst, code, word_index, 1);

	if (inst.opcode == Opcode::UNSUPPORTED) {
		SetUnsupported(inst, Family::VOP1, opcode, "VOP1 opcode is not implemented");
		return true;
	}
	if (inst.opcode == Opcode::V_NOP) {
		inst.dst.kind  = OperandKind::Null;
		inst.src_count = 0;
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
	const bool scalar_dst = UsesScalarDestination(inst.opcode);
	if (scalar_dst) {
		if (!DecodeScalarDestination(vdst, pc, inst.dst, error)) {
			return false;
		}
	} else {
		DecodeVectorGpr(vdst, inst.dst);
	}
	if (!DecodeScalarSource(src0, pc, inst.src0, error)) {
		return false;
	}
	inst.src_count = 1;
	ReadLiteralOperands(code, word_index, inst);
	return true;
}

bool DecodeVopc(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction& inst,
                std::string* error) {
	const uint32_t word   = code[word_index];
	const uint32_t opcode = (word >> 17u) & 0xffu;
	const uint32_t src0   = word & 0x1ffu;
	const uint32_t vsrc1  = (word >> 9u) & 0xffu;

	inst.pc        = pc;
	inst.word      = word;
	inst.family    = Family::VOPC;
	inst.opcode_id = opcode;
	inst.opcode    = Detail::LookupOpcode(VOPC_OPS, opcode);
	inst.dst.kind  = IsVopcCompareExec(inst.opcode) ? OperandKind::ExecLo : OperandKind::VccLo;
	SetRawWords(inst, code, word_index, 1);

	switch (src0) {
		case 249u: return DecodeVopcSdwa(pc, code, word_index, opcode, vsrc1, inst, error);
		case 250u: return DecodeVopcDpp(pc, code, word_index, opcode, vsrc1, inst, error);
		default: break;
	}
	if (inst.opcode == Opcode::UNSUPPORTED) {
		SetUnsupported(inst, Family::VOPC, opcode, "VOPC opcode is not implemented");
		return true;
	}
	DecodeVectorGpr(vsrc1, inst.src1);
	if (!DecodeScalarSource(src0, pc, inst.src0, error)) {
		return false;
	}
	inst.src_count = 2;
	ReadLiteralOperands(code, word_index, inst);
	return true;
}

bool DecodeVop3(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction& inst,
                std::string* error) {
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

	inst.pc        = pc;
	inst.word      = word0;
	inst.family    = Family::VOP3;
	inst.opcode_id = opcode;
	inst.opcode    = LookupVop3Opcode(opcode);
	SetRawWords(inst, code, word_index, 2);

	const bool carry_in_out    = (inst.opcode == Opcode::V_ADDC_U32 && opcode == 0x128u) ||
	                             (inst.opcode == Opcode::V_SUBREV_CO_CI_U32 && opcode == 0x12au);
	const bool vop3b_carry_out = IsVop3BCarryOutOpcode(inst.opcode);
	const bool vop3b_mad_u64   = IsVop3BMadU64Opcode(inst.opcode);
	const bool vop3b_uses_sdst = carry_in_out || vop3b_carry_out || vop3b_mad_u64;
	const bool mad_mix         = false;
	const bool f16_ternary     = IsNativeVop3F16TernaryOpcode(inst.opcode);
	const bool i16_ternary     = IsNativeVop3I16TernaryOpcode(inst.opcode);
	const bool b16_binary      = IsNativeVop3B16BinaryOpcode(inst.opcode);
	const bool pack_b32_f16    = inst.opcode == Opcode::V_PACK_B32_F16;
	const bool permlane        = IsPermlaneOpcode(inst.opcode);
	const bool vop3_vopc       = IsVop3EncodedVopc(opcode);
	const bool compare_exec    = vop3_vopc && IsVopcCompareExec(inst.opcode);
	const bool scalar_dst      = vop3_vopc || UsesScalarDestination(inst.opcode);
	const bool scalar_modifier_limits  = UsesScalarDestination(inst.opcode);
	const bool native_source_modifiers = SupportsNativeVop3SourceModifiers(inst.opcode);
	const bool native_result_modifiers = SupportsNativeVop3ResultModifiers(inst.opcode);
	const bool modifiers =
	    HasUnsupportedNativeVop3Modifiers(inst.opcode, permlane, mad_mix, vop3b_uses_sdst,
	                                      scalar_modifier_limits, abs, op_sel, clamp, omod, neg);
	if (modifiers) {
		const char* reason = "VOP3 source modifiers are not implemented";
		if (permlane && (op_sel & ~0x3u) != 0) {
			reason = "VOP3 perm-lane op_sel bits are not implemented";
		} else if (mad_mix) {
			reason = "VOP3 mad-mix clamp/omod is not implemented";
		}
		SetUnsupported(inst, Family::VOP3, opcode, reason);
		return true;
	}
	if (inst.opcode == Opcode::UNSUPPORTED) {
		SetUnsupported(inst, Family::VOP3, opcode, "VOP3 opcode is not implemented");
		return true;
	}
	if (inst.opcode == Opcode::V_NOP) {
		inst.dst.kind  = OperandKind::Null;
		inst.src_count = 0;
		return true;
	}
	bool dst_ok = true;
	if (compare_exec) {
		inst.dst.kind = OperandKind::ExecLo;
	} else if (scalar_dst) {
		// VOP3A uses VDST for VOPC and the scalar-destination lane-read opcodes.
		dst_ok = DecodeScalarDestination(vdst, pc, inst.dst, error);
	} else {
		DecodeVectorGpr(vdst, inst.dst);
	}
	if (!dst_ok || !DecodeScalarSource(src0, pc, inst.src0, error)) {
		return false;
	}
	inst.dst.clamp = native_result_modifiers && clamp != 0u;
	inst.dst.omod  = native_result_modifiers ? omod : 0u;
	if (permlane) {
		inst.dst.op_sel    = (op_sel & 0x1u) != 0u;
		inst.dst.op_sel_hi = (op_sel & 0x2u) != 0u;
	}
	if (vop3_vopc) {
		if (!DecodeScalarSource(src1, pc, inst.src1, error)) {
			return false;
		}
		inst.src_count = 2;
		if (native_source_modifiers) {
			ApplyNativeVop3SourceModifiers(inst, abs, neg);
		}
		ReadLiteralOperands(code, word_index, inst);
		return true;
	}
	if (IsVop3EncodedVop1(opcode)) {
		inst.src_count = 1;
		if (native_source_modifiers) {
			ApplyNativeVop3SourceModifiers(inst, abs, neg);
		}
		ReadLiteralOperands(code, word_index, inst);
		return true;
	}
	if (carry_in_out) {
		if (!DecodeScalarDestination(sdst, pc, inst.dst2, error) ||
		    !DecodeScalarSource(src1, pc, inst.src1, error) ||
		    !DecodeScalarSource(src2, pc, inst.src2, error)) {
			return false;
		}
		inst.src_count = 3;
		ReadLiteralOperands(code, word_index, inst);
		return true;
	}
	if (vop3b_carry_out) {
		if (!DecodeScalarDestination(sdst, pc, inst.dst2, error) ||
		    !DecodeScalarSource(src1, pc, inst.src1, error)) {
			return false;
		}
		inst.src_count = 2;
		ReadLiteralOperands(code, word_index, inst);
		return true;
	}
	if (vop3b_mad_u64) {
		if (!DecodeScalarDestination(sdst, pc, inst.dst2, error) ||
		    !DecodeScalarSource(src1, pc, inst.src1, error) ||
		    !DecodeScalarSource(src2, pc, inst.src2, error)) {
			return false;
		}
		inst.src_count = 3;
		ReadLiteralOperands(code, word_index, inst);
		return true;
	}
	if (IsVop3EncodedVop2(opcode)) {
		if (!DecodeScalarSource(src1, pc, inst.src1, error)) {
			return false;
		}
		if (inst.opcode == Opcode::V_CNDMASK_B32) {
			if (!DecodeScalarSource(src2, pc, inst.src2, error)) {
				return false;
			}
			inst.src_count = 3;
		} else {
			inst.src_count = 2;
		}
		if (native_source_modifiers) {
			ApplyNativeVop3SourceModifiers(inst, abs, neg);
		}
		ReadLiteralOperands(code, word_index, inst);
		return true;
	}
	if (!DecodeScalarSource(src1, pc, inst.src1, error)) {
		return false;
	}
	inst.src_count = NativeVop3SourceCount(inst.opcode);
	if (inst.src_count > 2u && !DecodeScalarSource(src2, pc, inst.src2, error)) {
		return false;
	}
	if (mad_mix) {
		ApplyNativeVop3TernaryModifiers(inst, op_sel, abs, neg);
	} else if (f16_ternary) {
		ApplyNativeVop3TernaryModifiers(inst, op_sel, abs, neg);
	} else if (i16_ternary) {
		ApplyNativeVop3I16TernarySelectors(inst, op_sel);
	} else if (b16_binary) {
		ApplyNativeVop3B16BinaryModifiers(inst, op_sel);
	} else if (pack_b32_f16) {
		ApplyNativeVop3PackB32F16Modifiers(inst, op_sel, abs, neg);
	} else if (native_source_modifiers) {
		ApplyNativeVop3SourceModifiers(inst, abs, neg);
	}
	ReadLiteralOperands(code, word_index, inst);
	return true;
}

bool DecodeVop3p(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                 Instruction& inst, std::string* error) {
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

	inst.pc        = pc;
	inst.word      = word0;
	inst.family    = Family::VOP3P;
	inst.opcode_id = opcode;
	inst.opcode    = Detail::LookupOpcode(VOP3P_OPS, opcode);
	SetRawWords(inst, code, word_index, 2);

	if (inst.opcode == Opcode::UNSUPPORTED) {
		SetUnsupported(inst, Family::VOP3P, opcode, "VOP3P opcode is not implemented");
		return true;
	}
	inst.src_count = Vop3pSourceCount(inst.opcode);
	DecodeVectorGpr(vdst, inst.dst);
	if (!DecodeScalarSource(src0, pc, inst.src0, error) ||
	    !DecodeScalarSource(src1, pc, inst.src1, error)) {
		return false;
	}
	if (inst.src_count > 2u && !DecodeScalarSource(src2, pc, inst.src2, error)) {
		return false;
	}
	if (inst.opcode == Opcode::V_FMA_F32) {
		inst.dst.clamp = clamp != 0u;
	} else if (IsMadMixF16(inst.opcode)) {
		inst.dst.clamp = clamp != 0u;
	} else if (IsPackedVop3p(inst.opcode)) {
		inst.dst.clamp = clamp != 0u;
	} else if (clamp != 0u) {
		SetUnsupported(inst, Family::VOP3P, opcode, "VOP3P integer clamp is not implemented");
		return true;
	}
	ApplyVop3pSourceModifiers(inst, op_sel, op_sel_hi, neg, neg_hi);
	if (inst.opcode == Opcode::V_MAD_MIXLO_F16) {
		ApplyVop3pMixAbsModifiers(inst);
		inst.dst.sdwa_sel = 4;
	} else if (inst.opcode == Opcode::V_MAD_MIXHI_F16) {
		ApplyVop3pMixAbsModifiers(inst);
		inst.dst.sdwa_sel = 5;
	} else if (inst.opcode == Opcode::V_FMA_F32) {
		ApplyVop3pMixAbsModifiers(inst);
	}
	ReadLiteralOperands(code, word_index, inst);
	return true;
}

void DecodeVintrp(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                  Instruction& inst) {
	const uint32_t word   = code[word_index];
	const uint32_t opcode = (word >> 16u) & 0x3u;
	const uint32_t vdst   = (word >> 18u) & 0xffu;
	const uint32_t attr   = (word >> 10u) & 0x3fu;
	const uint32_t chan   = (word >> 8u) & 0x3u;
	const uint32_t vsrc   = word & 0xffu;

	inst.pc         = pc;
	inst.word       = word;
	inst.word_count = 1;
	inst.family     = Family::VINTRP;
	inst.opcode_id  = opcode;
	inst.opcode     = LookupVintrpOpcode(opcode);
	SetRawWords(inst, code, word_index, 1);
	if (inst.opcode == Opcode::UNSUPPORTED) {
		SetUnsupported(inst, Family::VINTRP, opcode, "VINTRP opcode is not implemented");
		return;
	}

	DecodeVectorGpr(vdst, inst.dst);
	if (inst.opcode == Opcode::V_INTERP_MOV_F32) {
		inst.src0.kind       = OperandKind::IntegerInlineConstant;
		inst.src0.value      = vsrc & 0x3u;
		inst.src0.signed_val = static_cast<int32_t>(inst.src0.value);
	} else {
		DecodeVectorGpr(vsrc, inst.src0);
	}
	inst.src1.kind       = OperandKind::IntegerInlineConstant;
	inst.src1.value      = attr;
	inst.src1.signed_val = static_cast<int32_t>(attr);
	inst.src2.kind       = OperandKind::IntegerInlineConstant;
	inst.src2.value      = chan;
	inst.src2.signed_val = static_cast<int32_t>(chan);
	inst.src_count       = 3;
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder
