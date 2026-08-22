#include "common/assert.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/ShaderIRInternal.h"

#include <algorithm>
#include <fmt/format.h>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

struct LowerMap {
	Decoder::Opcode decoded = Decoder::Opcode::UNKNOWN;
	Opcode          ir      = Opcode::MoveU32;
};

constexpr LowerMap LOWER_OPS[] = {
    {Decoder::Opcode::S_MOV_B32, Opcode::MoveU32},
    {Decoder::Opcode::S_MOV_B64, Opcode::MoveU64},
    {Decoder::Opcode::S_WQM_B64, Opcode::WqmB64},
    {Decoder::Opcode::S_MOVK_I32, Opcode::MoveU32},
    {Decoder::Opcode::S_ABS_I32, Opcode::AbsI32},
    {Decoder::Opcode::S_BREV_B32, Opcode::BitReverseU32},
    {Decoder::Opcode::S_BCNT1_I32_B32, Opcode::BitCountU32},
    {Decoder::Opcode::S_BCNT1_I32_B64, Opcode::BitCountU64},
    {Decoder::Opcode::S_FF1_I32_B32, Opcode::FindLsbU32},
    {Decoder::Opcode::S_FF1_I32_B64, Opcode::FindLsbU64},
    {Decoder::Opcode::S_FLBIT_I32_B64, Opcode::FindMsbFromHighU64},
    {Decoder::Opcode::S_BITREPLICATE_B64_B32, Opcode::BitReplicateB64B32},
    {Decoder::Opcode::S_QUADMASK_B64, Opcode::QuadmaskB64},
    {Decoder::Opcode::S_NOT_B32, Opcode::BitwiseNotU32},
    {Decoder::Opcode::S_NOT_B64, Opcode::BitwiseNotU64},
    {Decoder::Opcode::S_ADD_U32, Opcode::IAddU32},
    {Decoder::Opcode::S_SUB_U32, Opcode::ISubU32},
    {Decoder::Opcode::S_ADD_I32, Opcode::IAddU32},
    {Decoder::Opcode::S_SUB_I32, Opcode::ISubU32},
    {Decoder::Opcode::S_MIN_I32, Opcode::IMinI32},
    {Decoder::Opcode::S_MAX_I32, Opcode::IMaxI32},
    {Decoder::Opcode::S_MIN_U32, Opcode::UMinU32},
    {Decoder::Opcode::S_MAX_U32, Opcode::UMaxU32},
    {Decoder::Opcode::S_AND_B32, Opcode::BitwiseAndU32},
    {Decoder::Opcode::S_AND_B64, Opcode::BitwiseAndU64},
    {Decoder::Opcode::S_ANDN2_B32, Opcode::BitwiseAndNotU32},
    {Decoder::Opcode::S_ANDN2_B64, Opcode::BitwiseAndNotU64},
    {Decoder::Opcode::S_OR_B32, Opcode::BitwiseOrU32},
    {Decoder::Opcode::S_OR_B64, Opcode::BitwiseOrU64},
    {Decoder::Opcode::S_ORN2_B32, Opcode::BitwiseOrNotU32},
    {Decoder::Opcode::S_ORN2_B64, Opcode::BitwiseOrNotU64},
    {Decoder::Opcode::S_XOR_B32, Opcode::BitwiseXorU32},
    {Decoder::Opcode::S_XOR_B64, Opcode::BitwiseXorU64},
    {Decoder::Opcode::S_NAND_B32, Opcode::BitwiseNandU32},
    {Decoder::Opcode::S_NAND_B64, Opcode::BitwiseNandU64},
    {Decoder::Opcode::S_NOR_B32, Opcode::BitwiseNorU32},
    {Decoder::Opcode::S_NOR_B64, Opcode::BitwiseNorU64},
    {Decoder::Opcode::S_XNOR_B32, Opcode::BitwiseXnorU32},
    {Decoder::Opcode::S_XNOR_B64, Opcode::BitwiseXnorU64},
    {Decoder::Opcode::S_LSHL_B32, Opcode::ShiftLeftLogicalU32},
    {Decoder::Opcode::S_LSHL_B64, Opcode::ShiftLeftLogicalU64},
    {Decoder::Opcode::S_LSHR_B32, Opcode::ShiftRightLogicalU32},
    {Decoder::Opcode::S_LSHR_B64, Opcode::ShiftRightLogicalU64},
    {Decoder::Opcode::S_ASHR_I32, Opcode::ShiftRightArithmeticI32},
    {Decoder::Opcode::S_MUL_I32, Opcode::IMulU32},
    {Decoder::Opcode::S_MUL_HI_U32, Opcode::UMulHighU32},
    {Decoder::Opcode::S_MULK_I32, Opcode::IMulU32},
    {Decoder::Opcode::S_BFE_U32, Opcode::BitFieldExtractU32},
    {Decoder::Opcode::S_BFE_I32, Opcode::BitFieldExtractI32},
    {Decoder::Opcode::S_BFE_U64, Opcode::BitFieldExtractU64},
    {Decoder::Opcode::S_BFM_B32, Opcode::BitFieldMaskU32},
    {Decoder::Opcode::S_BFM_B64, Opcode::BitFieldMaskU64},
    {Decoder::Opcode::S_BITCMP0_B32, Opcode::BitCompare0B32},
    {Decoder::Opcode::S_BITCMP1_B32, Opcode::BitCompare1B32},
    {Decoder::Opcode::S_PACK_LL_B32_B16, Opcode::PackLowLowU16},
    {Decoder::Opcode::S_PACK_LH_B32_B16, Opcode::PackLowHighU16},
    {Decoder::Opcode::S_PACK_HH_B32_B16, Opcode::PackHighHighU16},
    {Decoder::Opcode::S_CMP_EQ_I32, Opcode::CompareEqI32},
    {Decoder::Opcode::S_CMP_LG_I32, Opcode::CompareNeI32},
    {Decoder::Opcode::S_CMP_GT_I32, Opcode::CompareGtI32},
    {Decoder::Opcode::S_CMP_GE_I32, Opcode::CompareGeI32},
    {Decoder::Opcode::S_CMP_LT_I32, Opcode::CompareLtI32},
    {Decoder::Opcode::S_CMP_LE_I32, Opcode::CompareLeI32},
    {Decoder::Opcode::S_CMP_EQ_U32, Opcode::CompareEqU32},
    {Decoder::Opcode::S_CMP_LG_U32, Opcode::CompareNeU32},
    {Decoder::Opcode::S_CMP_GT_U32, Opcode::CompareGtU32},
    {Decoder::Opcode::S_CMP_GE_U32, Opcode::CompareGeU32},
    {Decoder::Opcode::S_CMP_LT_U32, Opcode::CompareLtU32},
    {Decoder::Opcode::S_CMP_LE_U32, Opcode::CompareLeU32},
    {Decoder::Opcode::S_CMP_EQ_U64, Opcode::CompareEqU64},
    {Decoder::Opcode::S_CMP_LG_U64, Opcode::CompareNeU64},
    {Decoder::Opcode::V_READFIRSTLANE_B32, Opcode::ReadFirstLaneU32},
    {Decoder::Opcode::V_READLANE_B32, Opcode::ReadLaneU32},
    {Decoder::Opcode::V_WRITELANE_B32, Opcode::WriteLaneU32},
    {Decoder::Opcode::V_PERMLANE16_B32, Opcode::Permlane16B32},
    {Decoder::Opcode::V_PERMLANEX16_B32, Opcode::Permlanex16B32},
    {Decoder::Opcode::V_CVT_F32_I32, Opcode::ConvertI32ToF32},
    {Decoder::Opcode::V_CVT_F32_U32, Opcode::ConvertU32ToF32},
    {Decoder::Opcode::V_CVT_U32_F32, Opcode::ConvertF32ToU32},
    {Decoder::Opcode::V_CVT_I32_F32, Opcode::ConvertF32ToI32},
    {Decoder::Opcode::V_CVT_F16_F32, Opcode::ConvertF32ToF16},
    {Decoder::Opcode::V_CVT_F32_F16, Opcode::ConvertF16ToF32},
    {Decoder::Opcode::V_CVT_F16_U16, Opcode::ConvertU16ToF16},
    {Decoder::Opcode::V_CVT_U16_F16, Opcode::ConvertF16ToU16},
    {Decoder::Opcode::V_CVT_F16_I16, Opcode::ConvertI16ToF16},
    {Decoder::Opcode::V_CVT_I16_F16, Opcode::ConvertF16ToI16},
    {Decoder::Opcode::V_CVT_RPI_I32_F32, Opcode::ConvertRoundPlusInfF32ToI32},
    {Decoder::Opcode::V_CVT_FLR_I32_F32, Opcode::ConvertFloorF32ToI32},
    {Decoder::Opcode::V_FREXP_EXP_I32_F32, Opcode::FrexpExpI32F32},
    {Decoder::Opcode::V_FREXP_MANT_F32, Opcode::FrexpMantF32},
    {Decoder::Opcode::V_CVT_OFF_F32_I4, Opcode::ConvertI4ToOffsetF32},
    {Decoder::Opcode::V_RCP_F32, Opcode::RcpF32},
    {Decoder::Opcode::V_RCP_IFLAG_F32, Opcode::RcpIflagF32},
    {Decoder::Opcode::V_FRACT_F32, Opcode::FractF32},
    {Decoder::Opcode::V_TRUNC_F32, Opcode::TruncF32},
    {Decoder::Opcode::V_CEIL_F32, Opcode::CeilF32},
    {Decoder::Opcode::V_RNDNE_F32, Opcode::RoundEvenF32},
    {Decoder::Opcode::V_FLOOR_F32, Opcode::FloorF32},
    {Decoder::Opcode::V_EXP_F32, Opcode::Exp2F32},
    {Decoder::Opcode::V_LOG_F32, Opcode::Log2F32},
    {Decoder::Opcode::V_RSQ_F32, Opcode::InverseSqrtF32},
    {Decoder::Opcode::V_SQRT_F32, Opcode::SqrtF32},
    {Decoder::Opcode::V_RCP_F16, Opcode::RcpF16},
    {Decoder::Opcode::V_SQRT_F16, Opcode::SqrtF16},
    {Decoder::Opcode::V_RSQ_F16, Opcode::InverseSqrtF16},
    {Decoder::Opcode::V_LOG_F16, Opcode::Log2F16},
    {Decoder::Opcode::V_EXP_F16, Opcode::Exp2F16},
    {Decoder::Opcode::V_FLOOR_F16, Opcode::FloorF16},
    {Decoder::Opcode::V_CEIL_F16, Opcode::CeilF16},
    {Decoder::Opcode::V_TRUNC_F16, Opcode::TruncF16},
    {Decoder::Opcode::V_RNDNE_F16, Opcode::RoundEvenF16},
    {Decoder::Opcode::V_SIN_F32, Opcode::SinF32},
    {Decoder::Opcode::V_COS_F32, Opcode::CosF32},
    {Decoder::Opcode::V_NOT_B32, Opcode::BitwiseNotU32},
    {Decoder::Opcode::V_BFREV_B32, Opcode::BitReverseU32},
    {Decoder::Opcode::V_FFBL_B32, Opcode::FindLsbU32},
    {Decoder::Opcode::V_FFBH_U32, Opcode::FindMsbFromHighU32},
    {Decoder::Opcode::V_ADD_F32, Opcode::FAddF32},
    {Decoder::Opcode::V_SUB_F32, Opcode::FSubF32},
    {Decoder::Opcode::V_SUBREV_F32, Opcode::FSubF32},
    {Decoder::Opcode::V_MUL_F32, Opcode::FMulF32},
    {Decoder::Opcode::V_MAC_F32, Opcode::FMadF32},
    {Decoder::Opcode::V_MADMK_F32, Opcode::FMadF32},
    {Decoder::Opcode::V_MADAK_F32, Opcode::FMadF32},
    {Decoder::Opcode::V_MIN_F32, Opcode::FMinF32},
    {Decoder::Opcode::V_MAX_F32, Opcode::FMaxF32},
    {Decoder::Opcode::V_MIN_I32, Opcode::IMinI32},
    {Decoder::Opcode::V_MAX_I32, Opcode::IMaxI32},
    {Decoder::Opcode::V_MAD_F32, Opcode::FMadF32},
    {Decoder::Opcode::V_MAD_I32_I24, Opcode::IMadI24U32},
    {Decoder::Opcode::V_MAD_U32_U24, Opcode::UMadU24U32},
    {Decoder::Opcode::V_CUBEID_F32, Opcode::CubeIdF32},
    {Decoder::Opcode::V_CUBESC_F32, Opcode::CubeScF32},
    {Decoder::Opcode::V_CUBETC_F32, Opcode::CubeTcF32},
    {Decoder::Opcode::V_CUBEMA_F32, Opcode::CubeMaF32},
    {Decoder::Opcode::V_FMA_F32, Opcode::FMadF32},
    {Decoder::Opcode::V_BFE_U32, Opcode::BitFieldExtract3U32},
    {Decoder::Opcode::V_BFE_I32, Opcode::BitFieldExtract3I32},
    {Decoder::Opcode::V_BFI_B32, Opcode::BitFieldInsertSelectU32},
    {Decoder::Opcode::V_ALIGNBIT_B32, Opcode::AlignBitU32},
    {Decoder::Opcode::V_ALIGNBYTE_B32, Opcode::AlignByteU32},
    {Decoder::Opcode::V_MIN3_F32, Opcode::FMin3F32},
    {Decoder::Opcode::V_MIN3_I32, Opcode::IMin3I32},
    {Decoder::Opcode::V_MIN3_U32, Opcode::UMin3U32},
    {Decoder::Opcode::V_MIN3_F16, Opcode::Min3F16},
    {Decoder::Opcode::V_MAX3_F32, Opcode::FMax3F32},
    {Decoder::Opcode::V_MAX3_I32, Opcode::IMax3I32},
    {Decoder::Opcode::V_MAX3_U32, Opcode::UMax3U32},
    {Decoder::Opcode::V_MAX3_F16, Opcode::Max3F16},
    {Decoder::Opcode::V_MED3_F32, Opcode::FMed3F32},
    {Decoder::Opcode::V_MED3_I32, Opcode::IMed3I32},
    {Decoder::Opcode::V_MED3_U32, Opcode::UMed3U32},
    {Decoder::Opcode::V_MED3_F16, Opcode::Med3F16},
    {Decoder::Opcode::V_MED3_I16, Opcode::IMed3I16},
    {Decoder::Opcode::V_SAD_U32, Opcode::SadU32},
    {Decoder::Opcode::V_ADD3_U32, Opcode::IAdd3U32},
    {Decoder::Opcode::V_LSHL_ADD_U32, Opcode::ShiftLeftAddU32},
    {Decoder::Opcode::V_ADD_LSHL_U32, Opcode::AddShiftLeftU32},
    {Decoder::Opcode::V_XAD_U32, Opcode::XorAddU32},
    {Decoder::Opcode::V_LSHL_OR_B32, Opcode::ShiftLeftOrU32},
    {Decoder::Opcode::V_AND_OR_B32, Opcode::BitwiseAndOrU32},
    {Decoder::Opcode::V_OR3_B32, Opcode::BitwiseOr3U32},
    {Decoder::Opcode::V_XOR3_B32, Opcode::BitwiseXor3U32},
    {Decoder::Opcode::V_ADD_NC_U32, Opcode::IAddU32},
    {Decoder::Opcode::V_SUB_NC_U32, Opcode::ISubU32},
    {Decoder::Opcode::V_SUBREV_NC_U32, Opcode::ISubU32},
    {Decoder::Opcode::V_ADD_NC_U16, Opcode::IAddU16},
    {Decoder::Opcode::V_SUB_NC_U16, Opcode::ISubI16},
    {Decoder::Opcode::V_MAX_U16, Opcode::UMaxU16},
    {Decoder::Opcode::V_MAX_I16, Opcode::IMaxI16},
    {Decoder::Opcode::V_MIN_U16, Opcode::UMinU16},
    {Decoder::Opcode::V_MIN_I16, Opcode::IMinI16},
    {Decoder::Opcode::V_ADD_NC_I16, Opcode::IAddU16},
    {Decoder::Opcode::V_SUB_NC_I16, Opcode::ISubI16},
    {Decoder::Opcode::V_MUL_I32_I24, Opcode::IMulI24U32},
    {Decoder::Opcode::V_MUL_U32_U24, Opcode::UMulU24U32},
    {Decoder::Opcode::V_MUL_LO_U32, Opcode::IMulU32},
    {Decoder::Opcode::V_MUL_HI_U32, Opcode::UMulHighU32},
    {Decoder::Opcode::V_MUL_LO_I32, Opcode::IMulU32},
    {Decoder::Opcode::V_MUL_HI_I32, Opcode::SMulHighI32},
    {Decoder::Opcode::V_ADD_I32, Opcode::IAddU32},
    {Decoder::Opcode::V_SUB_I32, Opcode::ISubU32},
    {Decoder::Opcode::V_SUBREV_I32, Opcode::ISubU32},
    {Decoder::Opcode::V_BFM_B32, Opcode::BitFieldMaskU32},
    {Decoder::Opcode::V_CVT_PKRTZ_F16_F32, Opcode::PackF32ToF16Rtz},
    {Decoder::Opcode::V_LDEXP_F32, Opcode::LdexpF32},
    {Decoder::Opcode::V_CVT_PKNORM_I16_F32, Opcode::PackSnorm2x16F32},
    {Decoder::Opcode::V_CVT_PKNORM_U16_F32, Opcode::PackUnorm2x16F32},
    {Decoder::Opcode::V_CVT_PK_U16_U32, Opcode::PackU16U32},
    {Decoder::Opcode::V_CVT_PK_I16_I32, Opcode::PackU16U32},
    {Decoder::Opcode::V_CVT_PK_U8_F32, Opcode::PackU8F32},
    {Decoder::Opcode::V_PACK_B32_F16, Opcode::PackB32F16},
    {Decoder::Opcode::V_PK_MAD_I16, Opcode::PackedMadI16},
    {Decoder::Opcode::V_PK_MUL_LO_U16, Opcode::PackedMulLoU16},
    {Decoder::Opcode::V_PK_ADD_I16, Opcode::PackedAddI16},
    {Decoder::Opcode::V_PK_SUB_I16, Opcode::PackedSubI16},
    {Decoder::Opcode::V_PK_LSHLREV_B16, Opcode::PackedLshlrevB16},
    {Decoder::Opcode::V_PK_LSHRREV_B16, Opcode::PackedLshrrevB16},
    {Decoder::Opcode::V_PK_ASHRREV_I16, Opcode::PackedAshrrevI16},
    {Decoder::Opcode::V_PK_MAX_I16, Opcode::PackedMaxI16},
    {Decoder::Opcode::V_PK_MIN_I16, Opcode::PackedMinI16},
    {Decoder::Opcode::V_PK_MAD_U16, Opcode::PackedMadU16},
    {Decoder::Opcode::V_PK_ADD_U16, Opcode::PackedAddU16},
    {Decoder::Opcode::V_PK_SUB_U16, Opcode::PackedSubU16},
    {Decoder::Opcode::V_PK_MAX_U16, Opcode::PackedMaxU16},
    {Decoder::Opcode::V_PK_MIN_U16, Opcode::PackedMinU16},
    {Decoder::Opcode::V_PK_ADD_F16, Opcode::PackedAddF16},
    {Decoder::Opcode::V_PK_MUL_F16, Opcode::PackedMulF16},
    {Decoder::Opcode::V_PK_MIN_F16, Opcode::PackedMinF16},
    {Decoder::Opcode::V_PK_MAX_F16, Opcode::PackedMaxF16},
    {Decoder::Opcode::V_PK_FMA_F16, Opcode::PackedFmaF16},
    {Decoder::Opcode::V_PK_FMAC_F16, Opcode::PackedFmaF16},
    {Decoder::Opcode::V_ADD_F16, Opcode::AddF16},
    {Decoder::Opcode::V_SUB_F16, Opcode::SubF16},
    {Decoder::Opcode::V_SUBREV_F16, Opcode::SubF16},
    {Decoder::Opcode::V_MUL_F16, Opcode::MulF16},
    {Decoder::Opcode::V_FMAC_F16, Opcode::FmaF16},
    {Decoder::Opcode::V_FMAMK_F16, Opcode::FmaF16},
    {Decoder::Opcode::V_FMAAK_F16, Opcode::FmaF16},
    {Decoder::Opcode::V_FMA_F16, Opcode::FmaF16},
    {Decoder::Opcode::V_MAD_MIXLO_F16, Opcode::MadMixF16},
    {Decoder::Opcode::V_MAD_MIXHI_F16, Opcode::MadMixF16},
    {Decoder::Opcode::V_AND_B32, Opcode::BitwiseAndU32},
    {Decoder::Opcode::V_OR_B32, Opcode::BitwiseOrU32},
    {Decoder::Opcode::V_XOR_B32, Opcode::BitwiseXorU32},
    {Decoder::Opcode::V_XNOR_B32, Opcode::BitwiseXnorU32},
    {Decoder::Opcode::V_BCNT_U32_B32, Opcode::BitCountAddU32},
    {Decoder::Opcode::V_MBCNT_LO_U32_B32, Opcode::MaskedBitCountLowU32},
    {Decoder::Opcode::V_MBCNT_HI_U32_B32, Opcode::MaskedBitCountHighU32},
    {Decoder::Opcode::V_LSHL_B32, Opcode::ShiftLeftLogicalU32},
    {Decoder::Opcode::V_LSHLREV_B32, Opcode::ShiftLeftLogicalU32},
    {Decoder::Opcode::V_LSHLREV_B16, Opcode::ShiftLeftLogicalU16},
    {Decoder::Opcode::V_LSHR_B32, Opcode::ShiftRightLogicalU32},
    {Decoder::Opcode::V_LSHRREV_B32, Opcode::ShiftRightLogicalU32},
    {Decoder::Opcode::V_LSHRREV_B16, Opcode::ShiftRightLogicalU16},
    {Decoder::Opcode::V_ASHR_I32, Opcode::ShiftRightArithmeticI32},
    {Decoder::Opcode::V_ASHRREV_I32, Opcode::ShiftRightArithmeticI32},
    {Decoder::Opcode::V_ASHRREV_I16, Opcode::ShiftRightArithmeticI16},
    {Decoder::Opcode::V_MIN_U32, Opcode::UMinU32},
    {Decoder::Opcode::V_MAX_U32, Opcode::UMaxU32},
    {Decoder::Opcode::V_MAX_F16, Opcode::MaxF16},
    {Decoder::Opcode::V_MIN_F16, Opcode::MinF16},
    {Decoder::Opcode::V_CMP_F_F32, Opcode::CompareFalse},
    {Decoder::Opcode::V_CMP_LT_F32, Opcode::CompareLtF32},
    {Decoder::Opcode::V_CMP_EQ_F32, Opcode::CompareEqF32},
    {Decoder::Opcode::V_CMP_LE_F32, Opcode::CompareLeF32},
    {Decoder::Opcode::V_CMP_GT_F32, Opcode::CompareGtF32},
    {Decoder::Opcode::V_CMP_LG_F32, Opcode::CompareNeF32},
    {Decoder::Opcode::V_CMP_GE_F32, Opcode::CompareGeF32},
    {Decoder::Opcode::V_CMP_O_F32, Opcode::CompareOrderedF32},
    {Decoder::Opcode::V_CMP_U_F32, Opcode::CompareUnorderedF32},
    {Decoder::Opcode::V_CMP_NGE_F32, Opcode::CompareUnordLtF32},
    {Decoder::Opcode::V_CMP_NLG_F32, Opcode::CompareUnordEqF32},
    {Decoder::Opcode::V_CMP_NGT_F32, Opcode::CompareUnordLeF32},
    {Decoder::Opcode::V_CMP_NLE_F32, Opcode::CompareUnordGtF32},
    {Decoder::Opcode::V_CMP_NEQ_F32, Opcode::CompareUnordNeF32},
    {Decoder::Opcode::V_CMP_NLT_F32, Opcode::CompareUnordGeF32},
    {Decoder::Opcode::V_CMP_TRU_F32, Opcode::CompareTrue},
    {Decoder::Opcode::V_CMP_CLASS_F32, Opcode::CompareClassF32},
    {Decoder::Opcode::V_CMP_LT_F16, Opcode::CompareLtF16},
    {Decoder::Opcode::V_CMP_EQ_F16, Opcode::CompareEqF16},
    {Decoder::Opcode::V_CMP_LE_F16, Opcode::CompareLeF16},
    {Decoder::Opcode::V_CMP_GT_F16, Opcode::CompareGtF16},
    {Decoder::Opcode::V_CMP_LG_F16, Opcode::CompareNeF16},
    {Decoder::Opcode::V_CMP_GE_F16, Opcode::CompareGeF16},
    {Decoder::Opcode::V_CMP_NEQ_F16, Opcode::CompareUnordNeF16},
    {Decoder::Opcode::V_CMPX_LT_F16, Opcode::CompareMaskLtF16},
    {Decoder::Opcode::V_CMPX_EQ_F16, Opcode::CompareMaskEqF16},
    {Decoder::Opcode::V_CMPX_LE_F16, Opcode::CompareMaskLeF16},
    {Decoder::Opcode::V_CMPX_GT_F16, Opcode::CompareMaskGtF16},
    {Decoder::Opcode::V_CMPX_GE_F16, Opcode::CompareMaskGeF16},
    {Decoder::Opcode::V_CMPX_NEQ_F16, Opcode::CompareMaskUnordNeF16},
    {Decoder::Opcode::V_CMPX_NLT_F16, Opcode::CompareMaskUnordGeF16},
    {Decoder::Opcode::V_CMPX_LT_F32, Opcode::CompareMaskLtF32},
    {Decoder::Opcode::V_CMPX_EQ_F32, Opcode::CompareMaskEqF32},
    {Decoder::Opcode::V_CMPX_LE_F32, Opcode::CompareMaskLeF32},
    {Decoder::Opcode::V_CMPX_GT_F32, Opcode::CompareMaskGtF32},
    {Decoder::Opcode::V_CMPX_LG_F32, Opcode::CompareMaskNeF32},
    {Decoder::Opcode::V_CMPX_GE_F32, Opcode::CompareMaskGeF32},
    {Decoder::Opcode::V_CMPX_NGE_F32, Opcode::CompareMaskUnordLtF32},
    {Decoder::Opcode::V_CMPX_NLG_F32, Opcode::CompareMaskUnordEqF32},
    {Decoder::Opcode::V_CMPX_NGT_F32, Opcode::CompareMaskUnordLeF32},
    {Decoder::Opcode::V_CMPX_NLE_F32, Opcode::CompareMaskUnordGtF32},
    {Decoder::Opcode::V_CMPX_NEQ_F32, Opcode::CompareMaskUnordNeF32},
    {Decoder::Opcode::V_CMPX_NLT_F32, Opcode::CompareMaskUnordGeF32},
    {Decoder::Opcode::V_CMP_F_I32, Opcode::CompareFalse},
    {Decoder::Opcode::V_CMP_LT_I32, Opcode::CompareLtI32},
    {Decoder::Opcode::V_CMP_EQ_I32, Opcode::CompareEqI32},
    {Decoder::Opcode::V_CMP_LE_I32, Opcode::CompareLeI32},
    {Decoder::Opcode::V_CMP_GT_I32, Opcode::CompareGtI32},
    {Decoder::Opcode::V_CMP_NE_I32, Opcode::CompareNeI32},
    {Decoder::Opcode::V_CMP_GE_I32, Opcode::CompareGeI32},
    {Decoder::Opcode::V_CMP_T_I32, Opcode::CompareTrue},
    {Decoder::Opcode::V_CMP_LT_I16, Opcode::CompareLtI16},
    {Decoder::Opcode::V_CMP_EQ_I16, Opcode::CompareEqI16},
    {Decoder::Opcode::V_CMP_LE_I16, Opcode::CompareLeI16},
    {Decoder::Opcode::V_CMP_GT_I16, Opcode::CompareGtI16},
    {Decoder::Opcode::V_CMP_NE_I16, Opcode::CompareNeI16},
    {Decoder::Opcode::V_CMP_GE_I16, Opcode::CompareGeI16},
    {Decoder::Opcode::V_CMPX_LT_I32, Opcode::CompareMaskLtI32},
    {Decoder::Opcode::V_CMPX_EQ_I32, Opcode::CompareMaskEqI32},
    {Decoder::Opcode::V_CMPX_LE_I32, Opcode::CompareMaskLeI32},
    {Decoder::Opcode::V_CMPX_GT_I32, Opcode::CompareMaskGtI32},
    {Decoder::Opcode::V_CMPX_NE_I32, Opcode::CompareMaskNeI32},
    {Decoder::Opcode::V_CMPX_GE_I32, Opcode::CompareMaskGeI32},
    {Decoder::Opcode::V_CMP_LT_U16, Opcode::CompareLtU16},
    {Decoder::Opcode::V_CMP_EQ_U16, Opcode::CompareEqU16},
    {Decoder::Opcode::V_CMP_LE_U16, Opcode::CompareLeU16},
    {Decoder::Opcode::V_CMP_GT_U16, Opcode::CompareGtU16},
    {Decoder::Opcode::V_CMP_NE_U16, Opcode::CompareNeU16},
    {Decoder::Opcode::V_CMP_GE_U16, Opcode::CompareGeU16},
    {Decoder::Opcode::V_CMP_F_U32, Opcode::CompareFalse},
    {Decoder::Opcode::V_CMP_LT_U32, Opcode::CompareLtU32},
    {Decoder::Opcode::V_CMP_EQ_U32, Opcode::CompareEqU32},
    {Decoder::Opcode::V_CMP_LE_U32, Opcode::CompareLeU32},
    {Decoder::Opcode::V_CMP_GT_U32, Opcode::CompareGtU32},
    {Decoder::Opcode::V_CMP_NE_U32, Opcode::CompareNeU32},
    {Decoder::Opcode::V_CMP_GE_U32, Opcode::CompareGeU32},
    {Decoder::Opcode::V_CMP_T_U32, Opcode::CompareTrue},
    {Decoder::Opcode::V_CMP_EQ_I64, Opcode::CompareEqU64},
    {Decoder::Opcode::V_CMP_GT_U64, Opcode::CompareGtU64},
    {Decoder::Opcode::V_CMP_NE_U64, Opcode::CompareNeU64},
    {Decoder::Opcode::V_CMPX_NE_I64, Opcode::CompareNeU64},
    {Decoder::Opcode::V_CMPX_NE_U64, Opcode::CompareNeU64},
    {Decoder::Opcode::V_CMPX_LT_U32, Opcode::CompareMaskLtU32},
    {Decoder::Opcode::V_CMPX_EQ_U32, Opcode::CompareMaskEqU32},
    {Decoder::Opcode::V_CMPX_LE_U32, Opcode::CompareMaskLeU32},
    {Decoder::Opcode::V_CMPX_GT_U32, Opcode::CompareMaskGtU32},
    {Decoder::Opcode::V_CMPX_NE_U32, Opcode::CompareMaskNeU32},
    {Decoder::Opcode::V_CMPX_GE_U32, Opcode::CompareMaskGeU32},
    {Decoder::Opcode::S_LOAD_DWORD, Opcode::SLoadDword},
    {Decoder::Opcode::S_BUFFER_LOAD_DWORD, Opcode::SBufferLoadDword},
    {Decoder::Opcode::BUFFER_LOAD_DWORD, Opcode::BufferLoadDword},
    {Decoder::Opcode::BUFFER_LOAD_SBYTE, Opcode::BufferLoadSbyte},
    {Decoder::Opcode::BUFFER_LOAD_SSHORT, Opcode::BufferLoadSshort},
    {Decoder::Opcode::BUFFER_STORE_BYTE, Opcode::BufferStoreByte},
    {Decoder::Opcode::BUFFER_STORE_SHORT, Opcode::BufferStoreShort},
    {Decoder::Opcode::BUFFER_STORE_DWORD, Opcode::BufferStoreDword},
    {Decoder::Opcode::BUFFER_ATOMIC_SWAP, Opcode::AtomicSwapU32},
    {Decoder::Opcode::BUFFER_ATOMIC_ADD, Opcode::AtomicAddU32},
    {Decoder::Opcode::BUFFER_ATOMIC_SUB, Opcode::AtomicSubU32},
    {Decoder::Opcode::BUFFER_ATOMIC_SMIN, Opcode::AtomicSMinI32},
    {Decoder::Opcode::BUFFER_ATOMIC_UMIN, Opcode::AtomicUMinU32},
    {Decoder::Opcode::BUFFER_ATOMIC_SMAX, Opcode::AtomicSMaxI32},
    {Decoder::Opcode::BUFFER_ATOMIC_UMAX, Opcode::AtomicUMaxU32},
    {Decoder::Opcode::BUFFER_ATOMIC_AND, Opcode::AtomicAndU32},
    {Decoder::Opcode::BUFFER_ATOMIC_OR, Opcode::AtomicOrU32},
    {Decoder::Opcode::BUFFER_ATOMIC_XOR, Opcode::AtomicXorU32},
    {Decoder::Opcode::BUFFER_ATOMIC_FMIN, Opcode::AtomicFMinF32},
    {Decoder::Opcode::BUFFER_ATOMIC_FMAX, Opcode::AtomicFMaxF32},
    {Decoder::Opcode::FLAT_LOAD_UBYTE, Opcode::FlatLoadUbyte},
    {Decoder::Opcode::FLAT_LOAD_SBYTE, Opcode::FlatLoadSbyte},
    {Decoder::Opcode::FLAT_LOAD_SSHORT, Opcode::FlatLoadSshort},
    {Decoder::Opcode::FLAT_STORE_BYTE, Opcode::FlatStoreByte},
    {Decoder::Opcode::FLAT_STORE_SHORT, Opcode::FlatStoreShort},
    {Decoder::Opcode::FLAT_STORE_DWORD, Opcode::FlatStoreDword},
    {Decoder::Opcode::DS_ADD_U32, Opcode::AtomicAddU32},
    {Decoder::Opcode::DS_ADD_RTN_U32, Opcode::AtomicAddU32},
    {Decoder::Opcode::DS_SUB_U32, Opcode::AtomicSubU32},
    {Decoder::Opcode::DS_SUB_RTN_U32, Opcode::AtomicSubU32},
    {Decoder::Opcode::DS_MIN_I32, Opcode::AtomicSMinI32},
    {Decoder::Opcode::DS_MIN_RTN_I32, Opcode::AtomicSMinI32},
    {Decoder::Opcode::DS_MAX_I32, Opcode::AtomicSMaxI32},
    {Decoder::Opcode::DS_MAX_RTN_I32, Opcode::AtomicSMaxI32},
    {Decoder::Opcode::DS_MIN_U32, Opcode::AtomicUMinU32},
    {Decoder::Opcode::DS_MIN_RTN_U32, Opcode::AtomicUMinU32},
    {Decoder::Opcode::DS_MAX_U32, Opcode::AtomicUMaxU32},
    {Decoder::Opcode::DS_MAX_RTN_U32, Opcode::AtomicUMaxU32},
    {Decoder::Opcode::DS_AND_B32, Opcode::AtomicAndU32},
    {Decoder::Opcode::DS_AND_RTN_B32, Opcode::AtomicAndU32},
    {Decoder::Opcode::DS_OR_B32, Opcode::AtomicOrU32},
    {Decoder::Opcode::DS_OR_RTN_B32, Opcode::AtomicOrU32},
    {Decoder::Opcode::DS_XOR_B32, Opcode::AtomicXorU32},
    {Decoder::Opcode::DS_XOR_RTN_B32, Opcode::AtomicXorU32},
    {Decoder::Opcode::DS_WRXCHG_RTN_B32, Opcode::AtomicSwapU32},
    {Decoder::Opcode::DS_READ_I8, Opcode::DsReadSbyte},
    {Decoder::Opcode::DS_READ_U8, Opcode::DsReadUbyte},
    {Decoder::Opcode::DS_READ_I16, Opcode::DsReadSshort},
    {Decoder::Opcode::DS_READ_U16, Opcode::DsReadUshort},
    {Decoder::Opcode::DS_READ2_B32, Opcode::DsRead2B32},
    {Decoder::Opcode::DS_READ2ST64_B32, Opcode::DsRead2B32},
    {Decoder::Opcode::DS_READ_B32, Opcode::DsReadB32},
    {Decoder::Opcode::DS_READ_B64, Opcode::DsReadB32},
    {Decoder::Opcode::DS_READ2_B64, Opcode::DsRead2B32},
    {Decoder::Opcode::DS_READ2ST64_B64, Opcode::DsRead2B32},
    {Decoder::Opcode::DS_READ_B96, Opcode::DsReadB32},
    {Decoder::Opcode::DS_READ_B128, Opcode::DsReadB32},
    {Decoder::Opcode::DS_WRITE_B8, Opcode::DsWriteByte},
    {Decoder::Opcode::DS_WRITE_B16, Opcode::DsWriteShort},
    {Decoder::Opcode::DS_WRITE2_B32, Opcode::DsWrite2B32},
    {Decoder::Opcode::DS_WRITE2ST64_B32, Opcode::DsWrite2B32},
    {Decoder::Opcode::DS_WRITE2_B64, Opcode::DsWrite2B32},
    {Decoder::Opcode::DS_WRITE2ST64_B64, Opcode::DsWrite2B32},
    {Decoder::Opcode::DS_WRITE_B32, Opcode::DsWriteB32},
    {Decoder::Opcode::DS_WRITE_B64, Opcode::DsWriteB32},
    {Decoder::Opcode::DS_WRITE_B96, Opcode::DsWriteB32},
    {Decoder::Opcode::DS_WRITE_B128, Opcode::DsWriteB32},
    {Decoder::Opcode::DS_MIN_F32, Opcode::DsMinF32},
    {Decoder::Opcode::DS_MAX_F32, Opcode::DsMaxF32},
    {Decoder::Opcode::DS_CONSUME, Opcode::DsConsume},
    {Decoder::Opcode::DS_APPEND, Opcode::DsAppend},
    {Decoder::Opcode::DS_WRITE_ADDTID_B32, Opcode::DsWriteAddtidB32},
    {Decoder::Opcode::DS_READ_ADDTID_B32, Opcode::DsReadAddtidB32},
    {Decoder::Opcode::IMAGE_GET_RESINFO, Opcode::ImageGetResinfo},
    {Decoder::Opcode::IMAGE_GET_LOD, Opcode::ImageGetLod},
    {Decoder::Opcode::IMAGE_LOAD, Opcode::ImageLoad},
    {Decoder::Opcode::IMAGE_LOAD_MIP, Opcode::ImageLoad},
    {Decoder::Opcode::IMAGE_STORE, Opcode::ImageStore},
    {Decoder::Opcode::IMAGE_STORE_MIP, Opcode::ImageStore},
    {Decoder::Opcode::IMAGE_ATOMIC_ADD, Opcode::AtomicAddU32},
    {Decoder::Opcode::IMAGE_ATOMIC_UMIN, Opcode::AtomicUMinU32},
    {Decoder::Opcode::IMAGE_ATOMIC_UMAX, Opcode::AtomicUMaxU32},
    {Decoder::Opcode::IMAGE_ATOMIC_AND, Opcode::AtomicAndU32},
    {Decoder::Opcode::IMAGE_ATOMIC_OR, Opcode::AtomicOrU32},
    {Decoder::Opcode::IMAGE_ATOMIC_XOR, Opcode::AtomicXorU32},
    {Decoder::Opcode::IMAGE_SAMPLE, Opcode::ImageSample},
    {Decoder::Opcode::IMAGE_GATHER4_LZ, Opcode::ImageGather4},
    {Decoder::Opcode::IMAGE_GATHER4_C, Opcode::ImageGather4},
    {Decoder::Opcode::IMAGE_GATHER4_C_LZ, Opcode::ImageGather4},
    {Decoder::Opcode::IMAGE_GATHER4_LZ_O, Opcode::ImageGather4},
    {Decoder::Opcode::IMAGE_GATHER4_C_O, Opcode::ImageGather4},
    {Decoder::Opcode::IMAGE_GATHER4_C_LZ_O, Opcode::ImageGather4},
    {Decoder::Opcode::IMAGE_GATHER4H, Opcode::ImageGather4},
};

} // namespace

void SetError(std::string* error, const char* message) {
	if (error != nullptr) {
		*error = message;
	}
}

std::optional<Opcode> LookupIrOpcode(Decoder::Opcode opcode) {
	for (const auto& op: LOWER_OPS) {
		if (op.decoded == opcode) {
			return op.ir;
		}
	}
	return std::nullopt;
}

bool IsReversedBinary(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::V_SUBREV_F32:
		case Decoder::Opcode::V_SUBREV_F16:
		case Decoder::Opcode::V_SUBREV_NC_U32:
		case Decoder::Opcode::V_LSHLREV_B32:
		case Decoder::Opcode::V_LSHRREV_B32:
		case Decoder::Opcode::V_ASHRREV_I32:
		case Decoder::Opcode::V_LSHLREV_B16:
		case Decoder::Opcode::V_LSHRREV_B16:
		case Decoder::Opcode::V_ASHRREV_I16:
		case Decoder::Opcode::V_SUBREV_I32: return true;
		default: return false;
	}
}

bool IsVectorCarryOutOpcode(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::V_ADD_I32:
		case Decoder::Opcode::V_SUB_I32:
		case Decoder::Opcode::V_SUBREV_I32: return true;
		default: return false;
	}
}

bool ScalarResultWritesSccNonZero(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::S_ABS_I32:
		case Decoder::Opcode::S_BCNT1_I32_B32:
		case Decoder::Opcode::S_BCNT1_I32_B64:
		case Decoder::Opcode::S_AND_B32:
		case Decoder::Opcode::S_ANDN2_B32:
		case Decoder::Opcode::S_OR_B32:
		case Decoder::Opcode::S_ORN2_B32:
		case Decoder::Opcode::S_XOR_B32:
		case Decoder::Opcode::S_NAND_B32:
		case Decoder::Opcode::S_NOR_B32:
		case Decoder::Opcode::S_XNOR_B32:
		case Decoder::Opcode::S_NOT_B32:
		case Decoder::Opcode::S_LSHL_B32:
		case Decoder::Opcode::S_LSHR_B32:
		case Decoder::Opcode::S_ASHR_I32:
		case Decoder::Opcode::S_BFE_U32:
		case Decoder::Opcode::S_BFE_I32:
		case Decoder::Opcode::S_AND_B64:
		case Decoder::Opcode::S_ANDN2_B64:
		case Decoder::Opcode::S_NOT_B64:
		case Decoder::Opcode::S_OR_B64:
		case Decoder::Opcode::S_ORN2_B64:
		case Decoder::Opcode::S_XOR_B64:
		case Decoder::Opcode::S_NAND_B64:
		case Decoder::Opcode::S_NOR_B64:
		case Decoder::Opcode::S_XNOR_B64:
		case Decoder::Opcode::S_LSHL_B64:
		case Decoder::Opcode::S_LSHR_B64:
		case Decoder::Opcode::S_BFE_U64:
		case Decoder::Opcode::S_WQM_B64:
		case Decoder::Opcode::S_QUADMASK_B64: return true;
		default: return false;
	}
}

bool ScalarResultIs64Bit(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::S_AND_B64:
		case Decoder::Opcode::S_ANDN2_B64:
		case Decoder::Opcode::S_NOT_B64:
		case Decoder::Opcode::S_OR_B64:
		case Decoder::Opcode::S_ORN2_B64:
		case Decoder::Opcode::S_XOR_B64:
		case Decoder::Opcode::S_NAND_B64:
		case Decoder::Opcode::S_NOR_B64:
		case Decoder::Opcode::S_XNOR_B64:
		case Decoder::Opcode::S_LSHL_B64:
		case Decoder::Opcode::S_LSHR_B64:
		case Decoder::Opcode::S_BFE_U64:
		case Decoder::Opcode::S_WQM_B64:
		case Decoder::Opcode::S_QUADMASK_B64: return true;
		default: return false;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
