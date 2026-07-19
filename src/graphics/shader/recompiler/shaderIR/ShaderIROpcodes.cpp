#include "common/assert.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ShaderIR.h"
#include "graphics/shader/recompiler/shaderIR/ShaderIRInternal.h"

#include <algorithm>
#include <fmt/format.h>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

struct LowerMap {
	Decoder::Opcode decoded = Decoder::Opcode::Unknown;
	Opcode          ir      = Opcode::MoveU32;
};

constexpr LowerMap LOWER_OPS[] = {
    {Decoder::Opcode::SMovB32, Opcode::MoveU32},
    {Decoder::Opcode::SMovB64, Opcode::MoveU64},
    {Decoder::Opcode::SWqmB64, Opcode::WqmB64},
    {Decoder::Opcode::SMovkI32, Opcode::MoveU32},
    {Decoder::Opcode::SAbsI32, Opcode::AbsI32},
    {Decoder::Opcode::SBrevB32, Opcode::BitReverseU32},
    {Decoder::Opcode::SBcnt1I32B32, Opcode::BitCountU32},
    {Decoder::Opcode::SBcnt1I32B64, Opcode::BitCountU64},
    {Decoder::Opcode::SFf1I32B32, Opcode::FindLsbU32},
    {Decoder::Opcode::SFlbitI32B64, Opcode::FindMsbFromHighU64},
    {Decoder::Opcode::SBitreplicateB64B32, Opcode::BitReplicateB64B32},
    {Decoder::Opcode::SNotB32, Opcode::BitwiseNotU32},
    {Decoder::Opcode::SNotB64, Opcode::BitwiseNotU64},
    {Decoder::Opcode::SAddU32, Opcode::IAddU32},
    {Decoder::Opcode::SSubU32, Opcode::ISubU32},
    {Decoder::Opcode::SAddI32, Opcode::IAddU32},
    {Decoder::Opcode::SSubI32, Opcode::ISubU32},
    {Decoder::Opcode::SMinI32, Opcode::IMinI32},
    {Decoder::Opcode::SMaxI32, Opcode::IMaxI32},
    {Decoder::Opcode::SMinU32, Opcode::UMinU32},
    {Decoder::Opcode::SMaxU32, Opcode::UMaxU32},
    {Decoder::Opcode::SAndB32, Opcode::BitwiseAndU32},
    {Decoder::Opcode::SAndB64, Opcode::BitwiseAndU64},
    {Decoder::Opcode::SAndn2B32, Opcode::BitwiseAndNotU32},
    {Decoder::Opcode::SAndn2B64, Opcode::BitwiseAndNotU64},
    {Decoder::Opcode::SOrB32, Opcode::BitwiseOrU32},
    {Decoder::Opcode::SOrB64, Opcode::BitwiseOrU64},
    {Decoder::Opcode::SOrn2B32, Opcode::BitwiseOrNotU32},
    {Decoder::Opcode::SOrn2B64, Opcode::BitwiseOrNotU64},
    {Decoder::Opcode::SXorB32, Opcode::BitwiseXorU32},
    {Decoder::Opcode::SXorB64, Opcode::BitwiseXorU64},
    {Decoder::Opcode::SNandB32, Opcode::BitwiseNandU32},
    {Decoder::Opcode::SNandB64, Opcode::BitwiseNandU64},
    {Decoder::Opcode::SNorB32, Opcode::BitwiseNorU32},
    {Decoder::Opcode::SNorB64, Opcode::BitwiseNorU64},
    {Decoder::Opcode::SXnorB32, Opcode::BitwiseXnorU32},
    {Decoder::Opcode::SXnorB64, Opcode::BitwiseXnorU64},
    {Decoder::Opcode::SLshlB32, Opcode::ShiftLeftLogicalU32},
    {Decoder::Opcode::SLshlB64, Opcode::ShiftLeftLogicalU64},
    {Decoder::Opcode::SLshrB32, Opcode::ShiftRightLogicalU32},
    {Decoder::Opcode::SLshrB64, Opcode::ShiftRightLogicalU64},
    {Decoder::Opcode::SAshrI32, Opcode::ShiftRightArithmeticI32},
    {Decoder::Opcode::SMulI32, Opcode::IMulU32},
    {Decoder::Opcode::SMulHiU32, Opcode::UMulHighU32},
    {Decoder::Opcode::SMulkI32, Opcode::IMulU32},
    {Decoder::Opcode::SBfeU32, Opcode::BitFieldExtractU32},
    {Decoder::Opcode::SBfeU64, Opcode::BitFieldExtractU64},
    {Decoder::Opcode::SBfmB32, Opcode::BitFieldMaskU32},
    {Decoder::Opcode::SBfmB64, Opcode::BitFieldMaskU64},
    {Decoder::Opcode::SBitcmp0B32, Opcode::BitCompare0B32},
    {Decoder::Opcode::SBitcmp1B32, Opcode::BitCompare1B32},
    {Decoder::Opcode::SPackLlB32B16, Opcode::PackLowLowU16},
    {Decoder::Opcode::SPackLhB32B16, Opcode::PackLowHighU16},
    {Decoder::Opcode::SPackHhB32B16, Opcode::PackHighHighU16},
    {Decoder::Opcode::SCmpEqI32, Opcode::CompareEqI32},
    {Decoder::Opcode::SCmpLgI32, Opcode::CompareNeI32},
    {Decoder::Opcode::SCmpGtI32, Opcode::CompareGtI32},
    {Decoder::Opcode::SCmpGeI32, Opcode::CompareGeI32},
    {Decoder::Opcode::SCmpLtI32, Opcode::CompareLtI32},
    {Decoder::Opcode::SCmpLeI32, Opcode::CompareLeI32},
    {Decoder::Opcode::SCmpEqU32, Opcode::CompareEqU32},
    {Decoder::Opcode::SCmpLgU32, Opcode::CompareNeU32},
    {Decoder::Opcode::SCmpGtU32, Opcode::CompareGtU32},
    {Decoder::Opcode::SCmpGeU32, Opcode::CompareGeU32},
    {Decoder::Opcode::SCmpLtU32, Opcode::CompareLtU32},
    {Decoder::Opcode::SCmpLeU32, Opcode::CompareLeU32},
    {Decoder::Opcode::SCmpLgU64, Opcode::CompareNeU64},
    {Decoder::Opcode::VReadfirstlaneB32, Opcode::ReadFirstLaneU32},
    {Decoder::Opcode::VReadlaneB32, Opcode::ReadLaneU32},
    {Decoder::Opcode::VWritelaneB32, Opcode::WriteLaneU32},
    {Decoder::Opcode::VPermlane16B32, Opcode::Permlane16B32},
    {Decoder::Opcode::VPermlanex16B32, Opcode::Permlanex16B32},
    {Decoder::Opcode::VCvtF32I32, Opcode::ConvertI32ToF32},
    {Decoder::Opcode::VCvtF32U32, Opcode::ConvertU32ToF32},
    {Decoder::Opcode::VCvtU32F32, Opcode::ConvertF32ToU32},
    {Decoder::Opcode::VCvtI32F32, Opcode::ConvertF32ToI32},
    {Decoder::Opcode::VCvtF16F32, Opcode::ConvertF32ToF16},
    {Decoder::Opcode::VCvtF32F16, Opcode::ConvertF16ToF32},
    {Decoder::Opcode::VCvtF16U16, Opcode::ConvertU16ToF16},
    {Decoder::Opcode::VCvtU16F16, Opcode::ConvertF16ToU16},
    {Decoder::Opcode::VCvtF16I16, Opcode::ConvertI16ToF16},
    {Decoder::Opcode::VCvtI16F16, Opcode::ConvertF16ToI16},
    {Decoder::Opcode::VCvtRpiI32F32, Opcode::ConvertRoundPlusInfF32ToI32},
    {Decoder::Opcode::VCvtFlrI32F32, Opcode::ConvertFloorF32ToI32},
    {Decoder::Opcode::VCvtOffF32I4, Opcode::ConvertI4ToOffsetF32},
    {Decoder::Opcode::VRcpF32, Opcode::RcpF32},
    {Decoder::Opcode::VFractF32, Opcode::FractF32},
    {Decoder::Opcode::VTruncF32, Opcode::TruncF32},
    {Decoder::Opcode::VCeilF32, Opcode::CeilF32},
    {Decoder::Opcode::VRndneF32, Opcode::RoundEvenF32},
    {Decoder::Opcode::VFloorF32, Opcode::FloorF32},
    {Decoder::Opcode::VExpF32, Opcode::Exp2F32},
    {Decoder::Opcode::VLogF32, Opcode::Log2F32},
    {Decoder::Opcode::VRsqF32, Opcode::InverseSqrtF32},
    {Decoder::Opcode::VSqrtF32, Opcode::SqrtF32},
    {Decoder::Opcode::VRcpF16, Opcode::RcpF16},
    {Decoder::Opcode::VSqrtF16, Opcode::SqrtF16},
    {Decoder::Opcode::VRsqF16, Opcode::InverseSqrtF16},
    {Decoder::Opcode::VLogF16, Opcode::Log2F16},
    {Decoder::Opcode::VExpF16, Opcode::Exp2F16},
    {Decoder::Opcode::VFloorF16, Opcode::FloorF16},
    {Decoder::Opcode::VCeilF16, Opcode::CeilF16},
    {Decoder::Opcode::VTruncF16, Opcode::TruncF16},
    {Decoder::Opcode::VRndneF16, Opcode::RoundEvenF16},
    {Decoder::Opcode::VSinF32, Opcode::SinF32},
    {Decoder::Opcode::VCosF32, Opcode::CosF32},
    {Decoder::Opcode::VNotB32, Opcode::BitwiseNotU32},
    {Decoder::Opcode::VBfrevB32, Opcode::BitReverseU32},
    {Decoder::Opcode::VFfblB32, Opcode::FindLsbU32},
    {Decoder::Opcode::VFfbhU32, Opcode::FindMsbFromHighU32},
    {Decoder::Opcode::VAddF32, Opcode::FAddF32},
    {Decoder::Opcode::VSubF32, Opcode::FSubF32},
    {Decoder::Opcode::VSubrevF32, Opcode::FSubF32},
    {Decoder::Opcode::VMulF32, Opcode::FMulF32},
    {Decoder::Opcode::VMacF32, Opcode::FMadF32},
    {Decoder::Opcode::VMadmkF32, Opcode::FMadF32},
    {Decoder::Opcode::VMadakF32, Opcode::FMadF32},
    {Decoder::Opcode::VMinF32, Opcode::FMinF32},
    {Decoder::Opcode::VMaxF32, Opcode::FMaxF32},
    {Decoder::Opcode::VMinI32, Opcode::IMinI32},
    {Decoder::Opcode::VMaxI32, Opcode::IMaxI32},
    {Decoder::Opcode::VMadF32, Opcode::FMadF32},
    {Decoder::Opcode::VMadI32I24, Opcode::IMadI24U32},
    {Decoder::Opcode::VMadU32U24, Opcode::UMadU24U32},
    {Decoder::Opcode::VCubeidF32, Opcode::CubeIdF32},
    {Decoder::Opcode::VCubescF32, Opcode::CubeScF32},
    {Decoder::Opcode::VCubetcF32, Opcode::CubeTcF32},
    {Decoder::Opcode::VCubemaF32, Opcode::CubeMaF32},
    {Decoder::Opcode::VFmaF32, Opcode::FMadF32},
    {Decoder::Opcode::VBfeU32, Opcode::BitFieldExtract3U32},
    {Decoder::Opcode::VBfeI32, Opcode::BitFieldExtract3I32},
    {Decoder::Opcode::VBfiB32, Opcode::BitFieldInsertSelectU32},
    {Decoder::Opcode::VAlignbitB32, Opcode::AlignBitU32},
    {Decoder::Opcode::VMin3F32, Opcode::FMin3F32},
    {Decoder::Opcode::VMin3I32, Opcode::IMin3I32},
    {Decoder::Opcode::VMin3U32, Opcode::UMin3U32},
    {Decoder::Opcode::VMin3F16, Opcode::Min3F16},
    {Decoder::Opcode::VMax3F32, Opcode::FMax3F32},
    {Decoder::Opcode::VMax3I32, Opcode::IMax3I32},
    {Decoder::Opcode::VMax3U32, Opcode::UMax3U32},
    {Decoder::Opcode::VMax3F16, Opcode::Max3F16},
    {Decoder::Opcode::VMed3F32, Opcode::FMed3F32},
    {Decoder::Opcode::VMed3I32, Opcode::IMed3I32},
    {Decoder::Opcode::VMed3U32, Opcode::UMed3U32},
    {Decoder::Opcode::VMed3F16, Opcode::Med3F16},
    {Decoder::Opcode::VSadU32, Opcode::SadU32},
    {Decoder::Opcode::VAdd3U32, Opcode::IAdd3U32},
    {Decoder::Opcode::VLshlAddU32, Opcode::ShiftLeftAddU32},
    {Decoder::Opcode::VAddLshlU32, Opcode::AddShiftLeftU32},
    {Decoder::Opcode::VXadU32, Opcode::XorAddU32},
    {Decoder::Opcode::VLshlOrB32, Opcode::ShiftLeftOrU32},
    {Decoder::Opcode::VAndOrB32, Opcode::BitwiseAndOrU32},
    {Decoder::Opcode::VOr3B32, Opcode::BitwiseOr3U32},
    {Decoder::Opcode::VXor3B32, Opcode::BitwiseXor3U32},
    {Decoder::Opcode::VAddNcU32, Opcode::IAddU32},
    {Decoder::Opcode::VSubNcU32, Opcode::ISubU32},
    {Decoder::Opcode::VSubrevNcU32, Opcode::ISubU32},
    {Decoder::Opcode::VAddNcU16, Opcode::IAddU16},
    {Decoder::Opcode::VSubNcU16, Opcode::ISubI16},
    {Decoder::Opcode::VMaxU16, Opcode::UMaxU16},
    {Decoder::Opcode::VMaxI16, Opcode::IMaxI16},
    {Decoder::Opcode::VMinU16, Opcode::UMinU16},
    {Decoder::Opcode::VMinI16, Opcode::IMinI16},
    {Decoder::Opcode::VAddNcI16, Opcode::IAddU16},
    {Decoder::Opcode::VSubNcI16, Opcode::ISubI16},
    {Decoder::Opcode::VMulI32I24, Opcode::IMulI24U32},
    {Decoder::Opcode::VMulU32U24, Opcode::UMulU24U32},
    {Decoder::Opcode::VMulLoU32, Opcode::IMulU32},
    {Decoder::Opcode::VMulHiU32, Opcode::UMulHighU32},
    {Decoder::Opcode::VMulLoI32, Opcode::IMulU32},
    {Decoder::Opcode::VMulHiI32, Opcode::SMulHighI32},
    {Decoder::Opcode::VAddI32, Opcode::IAddU32},
    {Decoder::Opcode::VSubI32, Opcode::ISubU32},
    {Decoder::Opcode::VSubrevI32, Opcode::ISubU32},
    {Decoder::Opcode::VBfmB32, Opcode::BitFieldMaskU32},
    {Decoder::Opcode::VCvtPkrtzF16F32, Opcode::PackF32ToF16Rtz},
    {Decoder::Opcode::VLdexpF32, Opcode::LdexpF32},
    {Decoder::Opcode::VCvtPknormI16F32, Opcode::PackSnorm2x16F32},
    {Decoder::Opcode::VCvtPknormU16F32, Opcode::PackUnorm2x16F32},
    {Decoder::Opcode::VCvtPkU16U32, Opcode::PackU16U32},
    {Decoder::Opcode::VCvtPkU8F32, Opcode::PackU8F32},
    {Decoder::Opcode::VPackB32F16, Opcode::PackB32F16},
    {Decoder::Opcode::VPkMadI16, Opcode::PackedMadI16},
    {Decoder::Opcode::VPkMulLoU16, Opcode::PackedMulLoU16},
    {Decoder::Opcode::VPkAddI16, Opcode::PackedAddI16},
    {Decoder::Opcode::VPkSubI16, Opcode::PackedSubI16},
    {Decoder::Opcode::VPkLshlrevB16, Opcode::PackedLshlrevB16},
    {Decoder::Opcode::VPkLshrrevB16, Opcode::PackedLshrrevB16},
    {Decoder::Opcode::VPkAshrrevI16, Opcode::PackedAshrrevI16},
    {Decoder::Opcode::VPkMaxI16, Opcode::PackedMaxI16},
    {Decoder::Opcode::VPkMinI16, Opcode::PackedMinI16},
    {Decoder::Opcode::VPkMadU16, Opcode::PackedMadU16},
    {Decoder::Opcode::VPkAddU16, Opcode::PackedAddU16},
    {Decoder::Opcode::VPkSubU16, Opcode::PackedSubU16},
    {Decoder::Opcode::VPkMaxU16, Opcode::PackedMaxU16},
    {Decoder::Opcode::VPkMinU16, Opcode::PackedMinU16},
    {Decoder::Opcode::VPkAddF16, Opcode::PackedAddF16},
    {Decoder::Opcode::VPkMulF16, Opcode::PackedMulF16},
    {Decoder::Opcode::VPkMinF16, Opcode::PackedMinF16},
    {Decoder::Opcode::VPkMaxF16, Opcode::PackedMaxF16},
    {Decoder::Opcode::VPkFmaF16, Opcode::PackedFmaF16},
    {Decoder::Opcode::VPkFmacF16, Opcode::PackedFmaF16},
    {Decoder::Opcode::VAddF16, Opcode::AddF16},
    {Decoder::Opcode::VSubF16, Opcode::SubF16},
    {Decoder::Opcode::VSubrevF16, Opcode::SubF16},
    {Decoder::Opcode::VMulF16, Opcode::MulF16},
    {Decoder::Opcode::VFmacF16, Opcode::FmaF16},
    {Decoder::Opcode::VFmamkF16, Opcode::FmaF16},
    {Decoder::Opcode::VFmaakF16, Opcode::FmaF16},
    {Decoder::Opcode::VFmaF16, Opcode::FmaF16},
    {Decoder::Opcode::VMadMixloF16, Opcode::MadMixF16},
    {Decoder::Opcode::VMadMixhiF16, Opcode::MadMixF16},
    {Decoder::Opcode::VAndB32, Opcode::BitwiseAndU32},
    {Decoder::Opcode::VOrB32, Opcode::BitwiseOrU32},
    {Decoder::Opcode::VXorB32, Opcode::BitwiseXorU32},
    {Decoder::Opcode::VXnorB32, Opcode::BitwiseXnorU32},
    {Decoder::Opcode::VBcntU32B32, Opcode::BitCountAddU32},
    {Decoder::Opcode::VMbcntLoU32B32, Opcode::MaskedBitCountLowU32},
    {Decoder::Opcode::VMbcntHiU32B32, Opcode::MaskedBitCountHighU32},
    {Decoder::Opcode::VLshlB32, Opcode::ShiftLeftLogicalU32},
    {Decoder::Opcode::VLshlrevB32, Opcode::ShiftLeftLogicalU32},
    {Decoder::Opcode::VLshlrevB16, Opcode::ShiftLeftLogicalU16},
    {Decoder::Opcode::VLshrB32, Opcode::ShiftRightLogicalU32},
    {Decoder::Opcode::VLshrrevB32, Opcode::ShiftRightLogicalU32},
    {Decoder::Opcode::VLshrrevB16, Opcode::ShiftRightLogicalU16},
    {Decoder::Opcode::VAshrI32, Opcode::ShiftRightArithmeticI32},
    {Decoder::Opcode::VAshrrevI32, Opcode::ShiftRightArithmeticI32},
    {Decoder::Opcode::VAshrrevI16, Opcode::ShiftRightArithmeticI16},
    {Decoder::Opcode::VMinU32, Opcode::UMinU32},
    {Decoder::Opcode::VMaxU32, Opcode::UMaxU32},
    {Decoder::Opcode::VMaxF16, Opcode::MaxF16},
    {Decoder::Opcode::VMinF16, Opcode::MinF16},
    {Decoder::Opcode::VCmpFF32, Opcode::CompareFalse},
    {Decoder::Opcode::VCmpLtF32, Opcode::CompareLtF32},
    {Decoder::Opcode::VCmpEqF32, Opcode::CompareEqF32},
    {Decoder::Opcode::VCmpLeF32, Opcode::CompareLeF32},
    {Decoder::Opcode::VCmpGtF32, Opcode::CompareGtF32},
    {Decoder::Opcode::VCmpLgF32, Opcode::CompareNeF32},
    {Decoder::Opcode::VCmpGeF32, Opcode::CompareGeF32},
    {Decoder::Opcode::VCmpOF32, Opcode::CompareOrderedF32},
    {Decoder::Opcode::VCmpUF32, Opcode::CompareUnorderedF32},
    {Decoder::Opcode::VCmpNgeF32, Opcode::CompareUnordLtF32},
    {Decoder::Opcode::VCmpNlgF32, Opcode::CompareUnordEqF32},
    {Decoder::Opcode::VCmpNgtF32, Opcode::CompareUnordLeF32},
    {Decoder::Opcode::VCmpNleF32, Opcode::CompareUnordGtF32},
    {Decoder::Opcode::VCmpNeqF32, Opcode::CompareUnordNeF32},
    {Decoder::Opcode::VCmpNltF32, Opcode::CompareUnordGeF32},
    {Decoder::Opcode::VCmpTruF32, Opcode::CompareTrue},
    {Decoder::Opcode::VCmpClassF32, Opcode::CompareClassF32},
    {Decoder::Opcode::VCmpLtF16, Opcode::CompareLtF16},
    {Decoder::Opcode::VCmpEqF16, Opcode::CompareEqF16},
    {Decoder::Opcode::VCmpLeF16, Opcode::CompareLeF16},
    {Decoder::Opcode::VCmpGtF16, Opcode::CompareGtF16},
    {Decoder::Opcode::VCmpLgF16, Opcode::CompareNeF16},
    {Decoder::Opcode::VCmpGeF16, Opcode::CompareGeF16},
    {Decoder::Opcode::VCmpNeqF16, Opcode::CompareUnordNeF16},
    {Decoder::Opcode::VCmpxLtF16, Opcode::CompareMaskLtF16},
    {Decoder::Opcode::VCmpxEqF16, Opcode::CompareMaskEqF16},
    {Decoder::Opcode::VCmpxLeF16, Opcode::CompareMaskLeF16},
    {Decoder::Opcode::VCmpxGtF16, Opcode::CompareMaskGtF16},
    {Decoder::Opcode::VCmpxGeF16, Opcode::CompareMaskGeF16},
    {Decoder::Opcode::VCmpxNeqF16, Opcode::CompareMaskUnordNeF16},
    {Decoder::Opcode::VCmpxNltF16, Opcode::CompareMaskUnordGeF16},
    {Decoder::Opcode::VCmpxLtF32, Opcode::CompareMaskLtF32},
    {Decoder::Opcode::VCmpxEqF32, Opcode::CompareMaskEqF32},
    {Decoder::Opcode::VCmpxLeF32, Opcode::CompareMaskLeF32},
    {Decoder::Opcode::VCmpxGtF32, Opcode::CompareMaskGtF32},
    {Decoder::Opcode::VCmpxLgF32, Opcode::CompareMaskNeF32},
    {Decoder::Opcode::VCmpxGeF32, Opcode::CompareMaskGeF32},
    {Decoder::Opcode::VCmpxNgeF32, Opcode::CompareMaskUnordLtF32},
    {Decoder::Opcode::VCmpxNlgF32, Opcode::CompareMaskUnordEqF32},
    {Decoder::Opcode::VCmpxNgtF32, Opcode::CompareMaskUnordLeF32},
    {Decoder::Opcode::VCmpxNleF32, Opcode::CompareMaskUnordGtF32},
    {Decoder::Opcode::VCmpxNeqF32, Opcode::CompareMaskUnordNeF32},
    {Decoder::Opcode::VCmpxNltF32, Opcode::CompareMaskUnordGeF32},
    {Decoder::Opcode::VCmpFI32, Opcode::CompareFalse},
    {Decoder::Opcode::VCmpLtI32, Opcode::CompareLtI32},
    {Decoder::Opcode::VCmpEqI32, Opcode::CompareEqI32},
    {Decoder::Opcode::VCmpLeI32, Opcode::CompareLeI32},
    {Decoder::Opcode::VCmpGtI32, Opcode::CompareGtI32},
    {Decoder::Opcode::VCmpNeI32, Opcode::CompareNeI32},
    {Decoder::Opcode::VCmpGeI32, Opcode::CompareGeI32},
    {Decoder::Opcode::VCmpTI32, Opcode::CompareTrue},
    {Decoder::Opcode::VCmpLtI16, Opcode::CompareLtI16},
    {Decoder::Opcode::VCmpEqI16, Opcode::CompareEqI16},
    {Decoder::Opcode::VCmpLeI16, Opcode::CompareLeI16},
    {Decoder::Opcode::VCmpGtI16, Opcode::CompareGtI16},
    {Decoder::Opcode::VCmpNeI16, Opcode::CompareNeI16},
    {Decoder::Opcode::VCmpGeI16, Opcode::CompareGeI16},
    {Decoder::Opcode::VCmpxLtI32, Opcode::CompareMaskLtI32},
    {Decoder::Opcode::VCmpxEqI32, Opcode::CompareMaskEqI32},
    {Decoder::Opcode::VCmpxLeI32, Opcode::CompareMaskLeI32},
    {Decoder::Opcode::VCmpxGtI32, Opcode::CompareMaskGtI32},
    {Decoder::Opcode::VCmpxNeI32, Opcode::CompareMaskNeI32},
    {Decoder::Opcode::VCmpxGeI32, Opcode::CompareMaskGeI32},
    {Decoder::Opcode::VCmpLtU16, Opcode::CompareLtU16},
    {Decoder::Opcode::VCmpEqU16, Opcode::CompareEqU16},
    {Decoder::Opcode::VCmpLeU16, Opcode::CompareLeU16},
    {Decoder::Opcode::VCmpGtU16, Opcode::CompareGtU16},
    {Decoder::Opcode::VCmpNeU16, Opcode::CompareNeU16},
    {Decoder::Opcode::VCmpGeU16, Opcode::CompareGeU16},
    {Decoder::Opcode::VCmpFU32, Opcode::CompareFalse},
    {Decoder::Opcode::VCmpLtU32, Opcode::CompareLtU32},
    {Decoder::Opcode::VCmpEqU32, Opcode::CompareEqU32},
    {Decoder::Opcode::VCmpLeU32, Opcode::CompareLeU32},
    {Decoder::Opcode::VCmpGtU32, Opcode::CompareGtU32},
    {Decoder::Opcode::VCmpNeU32, Opcode::CompareNeU32},
    {Decoder::Opcode::VCmpGeU32, Opcode::CompareGeU32},
    {Decoder::Opcode::VCmpTU32, Opcode::CompareTrue},
    {Decoder::Opcode::VCmpNeU64, Opcode::CompareNeU64},
    {Decoder::Opcode::VCmpxLtU32, Opcode::CompareMaskLtU32},
    {Decoder::Opcode::VCmpxEqU32, Opcode::CompareMaskEqU32},
    {Decoder::Opcode::VCmpxLeU32, Opcode::CompareMaskLeU32},
    {Decoder::Opcode::VCmpxGtU32, Opcode::CompareMaskGtU32},
    {Decoder::Opcode::VCmpxNeU32, Opcode::CompareMaskNeU32},
    {Decoder::Opcode::VCmpxGeU32, Opcode::CompareMaskGeU32},
    {Decoder::Opcode::SLoadDword, Opcode::SLoadDword},
    {Decoder::Opcode::SBufferLoadDword, Opcode::SBufferLoadDword},
    {Decoder::Opcode::BufferLoadDword, Opcode::BufferLoadDword},
    {Decoder::Opcode::BufferLoadSbyte, Opcode::BufferLoadSbyte},
    {Decoder::Opcode::BufferLoadSshort, Opcode::BufferLoadSshort},
    {Decoder::Opcode::BufferStoreByte, Opcode::BufferStoreByte},
    {Decoder::Opcode::BufferStoreShort, Opcode::BufferStoreShort},
    {Decoder::Opcode::BufferStoreDword, Opcode::BufferStoreDword},
    {Decoder::Opcode::BufferAtomicSwap, Opcode::AtomicSwapU32},
    {Decoder::Opcode::BufferAtomicAdd, Opcode::AtomicAddU32},
    {Decoder::Opcode::BufferAtomicSub, Opcode::AtomicSubU32},
    {Decoder::Opcode::BufferAtomicSMin, Opcode::AtomicSMinI32},
    {Decoder::Opcode::BufferAtomicUMin, Opcode::AtomicUMinU32},
    {Decoder::Opcode::BufferAtomicSMax, Opcode::AtomicSMaxI32},
    {Decoder::Opcode::BufferAtomicUMax, Opcode::AtomicUMaxU32},
    {Decoder::Opcode::BufferAtomicAnd, Opcode::AtomicAndU32},
    {Decoder::Opcode::BufferAtomicOr, Opcode::AtomicOrU32},
    {Decoder::Opcode::BufferAtomicXor, Opcode::AtomicXorU32},
    {Decoder::Opcode::FlatLoadUbyte, Opcode::FlatLoadUbyte},
    {Decoder::Opcode::FlatLoadSbyte, Opcode::FlatLoadSbyte},
    {Decoder::Opcode::FlatLoadSshort, Opcode::FlatLoadSshort},
    {Decoder::Opcode::FlatStoreByte, Opcode::FlatStoreByte},
    {Decoder::Opcode::FlatStoreShort, Opcode::FlatStoreShort},
    {Decoder::Opcode::FlatStoreDword, Opcode::FlatStoreDword},
    {Decoder::Opcode::DsAddU32, Opcode::AtomicAddU32},
    {Decoder::Opcode::DsAddRtnU32, Opcode::AtomicAddU32},
    {Decoder::Opcode::DsSubU32, Opcode::AtomicSubU32},
    {Decoder::Opcode::DsSubRtnU32, Opcode::AtomicSubU32},
    {Decoder::Opcode::DsMinI32, Opcode::AtomicSMinI32},
    {Decoder::Opcode::DsMinRtnI32, Opcode::AtomicSMinI32},
    {Decoder::Opcode::DsMaxI32, Opcode::AtomicSMaxI32},
    {Decoder::Opcode::DsMaxRtnI32, Opcode::AtomicSMaxI32},
    {Decoder::Opcode::DsMinU32, Opcode::AtomicUMinU32},
    {Decoder::Opcode::DsMinRtnU32, Opcode::AtomicUMinU32},
    {Decoder::Opcode::DsMaxU32, Opcode::AtomicUMaxU32},
    {Decoder::Opcode::DsMaxRtnU32, Opcode::AtomicUMaxU32},
    {Decoder::Opcode::DsAndB32, Opcode::AtomicAndU32},
    {Decoder::Opcode::DsAndRtnB32, Opcode::AtomicAndU32},
    {Decoder::Opcode::DsOrB32, Opcode::AtomicOrU32},
    {Decoder::Opcode::DsOrRtnB32, Opcode::AtomicOrU32},
    {Decoder::Opcode::DsXorB32, Opcode::AtomicXorU32},
    {Decoder::Opcode::DsXorRtnB32, Opcode::AtomicXorU32},
    {Decoder::Opcode::DsWrxchgRtnB32, Opcode::AtomicSwapU32},
    {Decoder::Opcode::DsReadSbyte, Opcode::DsReadSbyte},
    {Decoder::Opcode::DsReadUbyte, Opcode::DsReadUbyte},
    {Decoder::Opcode::DsReadSshort, Opcode::DsReadSshort},
    {Decoder::Opcode::DsReadUshort, Opcode::DsReadUshort},
    {Decoder::Opcode::DsRead2B32, Opcode::DsReadB32},
    {Decoder::Opcode::DsReadB32, Opcode::DsReadB32},
    {Decoder::Opcode::DsReadB64, Opcode::DsReadB32},
    {Decoder::Opcode::DsRead2B64, Opcode::DsReadB32},
    {Decoder::Opcode::DsRead2St64B64, Opcode::DsReadB32},
    {Decoder::Opcode::DsReadB96, Opcode::DsReadB32},
    {Decoder::Opcode::DsReadB128, Opcode::DsReadB32},
    {Decoder::Opcode::DsWriteByte, Opcode::DsWriteByte},
    {Decoder::Opcode::DsWriteShort, Opcode::DsWriteShort},
    {Decoder::Opcode::DsWrite2B32, Opcode::DsWriteB32},
    {Decoder::Opcode::DsWrite2St64B32, Opcode::DsWriteB32},
    {Decoder::Opcode::DsWrite2B64, Opcode::DsWriteB32},
    {Decoder::Opcode::DsWrite2St64B64, Opcode::DsWriteB32},
    {Decoder::Opcode::DsWriteB32, Opcode::DsWriteB32},
    {Decoder::Opcode::DsWriteB64, Opcode::DsWriteB32},
    {Decoder::Opcode::DsWriteB96, Opcode::DsWriteB32},
    {Decoder::Opcode::DsWriteB128, Opcode::DsWriteB32},
    {Decoder::Opcode::DsMinF32, Opcode::DsMinF32},
    {Decoder::Opcode::DsMaxF32, Opcode::DsMaxF32},
    {Decoder::Opcode::DsConsume, Opcode::DsConsume},
    {Decoder::Opcode::DsAppend, Opcode::DsAppend},
    {Decoder::Opcode::DsWriteAddtidB32, Opcode::DsWriteAddtidB32},
    {Decoder::Opcode::DsReadAddtidB32, Opcode::DsReadAddtidB32},
    {Decoder::Opcode::ImageGetResinfo, Opcode::ImageGetResinfo},
    {Decoder::Opcode::ImageGetLod, Opcode::ImageGetLod},
    {Decoder::Opcode::ImageLoad, Opcode::ImageLoad},
    {Decoder::Opcode::ImageLoadMip, Opcode::ImageLoad},
    {Decoder::Opcode::ImageStore, Opcode::ImageStore},
    {Decoder::Opcode::ImageStoreMip, Opcode::ImageStore},
    {Decoder::Opcode::ImageAtomicAdd, Opcode::AtomicAddU32},
    {Decoder::Opcode::ImageAtomicUMin, Opcode::AtomicUMinU32},
    {Decoder::Opcode::ImageAtomicUMax, Opcode::AtomicUMaxU32},
    {Decoder::Opcode::ImageAtomicAnd, Opcode::AtomicAndU32},
    {Decoder::Opcode::ImageAtomicOr, Opcode::AtomicOrU32},
    {Decoder::Opcode::ImageAtomicXor, Opcode::AtomicXorU32},
    {Decoder::Opcode::ImageSample, Opcode::ImageSample},
    {Decoder::Opcode::ImageGather4Lz, Opcode::ImageGather4},
    {Decoder::Opcode::ImageGather4C, Opcode::ImageGather4},
    {Decoder::Opcode::ImageGather4CLz, Opcode::ImageGather4},
    {Decoder::Opcode::ImageGather4LzO, Opcode::ImageGather4},
    {Decoder::Opcode::ImageGather4CO, Opcode::ImageGather4},
    {Decoder::Opcode::ImageGather4CLzO, Opcode::ImageGather4},
    {Decoder::Opcode::ImageGather4H, Opcode::ImageGather4},
};

} // namespace

void SetError(std::string* error, const char* message) {
	if (error != nullptr) {
		*error = message;
	}
}

Opcode LookupIrOpcode(Decoder::Opcode opcode) {
	for (const auto& op: LOWER_OPS) {
		if (op.decoded == opcode) {
			return op.ir;
		}
	}
	return Opcode::MoveU32;
}

bool IsImplemented(Decoder::Opcode opcode) {
	for (const auto& op: LOWER_OPS) {
		if (op.decoded == opcode) {
			return true;
		}
	}
	return false;
}

bool IsReversedBinary(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::VSubrevF32:
		case Decoder::Opcode::VSubrevF16:
		case Decoder::Opcode::VSubrevNcU32:
		case Decoder::Opcode::VLshlrevB32:
		case Decoder::Opcode::VLshrrevB32:
		case Decoder::Opcode::VAshrrevI32:
		case Decoder::Opcode::VLshlrevB16:
		case Decoder::Opcode::VLshrrevB16:
		case Decoder::Opcode::VAshrrevI16:
		case Decoder::Opcode::VSubrevI32: return true;
		default: return false;
	}
}

bool IsVectorCarryOutOpcode(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::VAddI32:
		case Decoder::Opcode::VSubI32:
		case Decoder::Opcode::VSubrevI32: return true;
		default: return false;
	}
}

bool ScalarResultWritesSccNonZero(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::SAbsI32:
		case Decoder::Opcode::SBcnt1I32B32:
		case Decoder::Opcode::SBcnt1I32B64:
		case Decoder::Opcode::SAndB32:
		case Decoder::Opcode::SAndn2B32:
		case Decoder::Opcode::SOrB32:
		case Decoder::Opcode::SOrn2B32:
		case Decoder::Opcode::SXorB32:
		case Decoder::Opcode::SNandB32:
		case Decoder::Opcode::SNorB32:
		case Decoder::Opcode::SXnorB32:
		case Decoder::Opcode::SNotB32:
		case Decoder::Opcode::SLshlB32:
		case Decoder::Opcode::SLshrB32:
		case Decoder::Opcode::SAshrI32:
		case Decoder::Opcode::SBfeU32:
		case Decoder::Opcode::SAndB64:
		case Decoder::Opcode::SAndn2B64:
		case Decoder::Opcode::SNotB64:
		case Decoder::Opcode::SOrB64:
		case Decoder::Opcode::SOrn2B64:
		case Decoder::Opcode::SXorB64:
		case Decoder::Opcode::SNandB64:
		case Decoder::Opcode::SNorB64:
		case Decoder::Opcode::SXnorB64:
		case Decoder::Opcode::SLshlB64:
		case Decoder::Opcode::SLshrB64:
		case Decoder::Opcode::SBfeU64:
		case Decoder::Opcode::SWqmB64: return true;
		default: return false;
	}
}

bool ScalarResultIs64Bit(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::SAndB64:
		case Decoder::Opcode::SAndn2B64:
		case Decoder::Opcode::SNotB64:
		case Decoder::Opcode::SOrB64:
		case Decoder::Opcode::SOrn2B64:
		case Decoder::Opcode::SXorB64:
		case Decoder::Opcode::SNandB64:
		case Decoder::Opcode::SNorB64:
		case Decoder::Opcode::SXnorB64:
		case Decoder::Opcode::SLshlB64:
		case Decoder::Opcode::SLshrB64:
		case Decoder::Opcode::SBfeU64:
		case Decoder::Opcode::SWqmB64: return true;
		default: return false;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
