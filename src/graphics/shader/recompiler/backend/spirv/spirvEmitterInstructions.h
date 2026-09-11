#pragma once

#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
// Direct SPIR-V operations share emission as well as argument/result marshalling.
template <spv::Op opcode, IR::Type type, typename... Args>
uint32_t EmitNative(ValueEmitContext& ctx, Args... args) {
	const auto result = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction({opcode, ctx.TypeId(type), result, args...});
	return result;
}
#define EMIT_NATIVE(name, opcode, type, ...)                                                       \
	inline constexpr auto Emit##name = EmitNative<spv::opcode, IR::Type::type, __VA_ARGS__>;
EMIT_NATIVE(BitCastU16F16, OpBitcast, U32, uint32_t)
inline constexpr auto EmitBitCastF16U16 = EmitBitCastU16F16;
inline constexpr auto EmitConvertU32U16 = EmitBitCastU16F16;
inline constexpr auto EmitConvertU32U8  = EmitBitCastU16F16;
inline constexpr auto EmitBitCastU32F32 = EmitBitCastU16F16;
EMIT_NATIVE(BitCastF32U32, OpBitcast, F32, uint32_t)
uint32_t EmitConvertU16U32(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitConvertU8U32(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitConvertF16F32(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitConvertF32F16(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitConvertS32F32(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitConvertU32F32(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitConvertF32S32(ValueEmitContext& ctx, uint32_t arg0);
EMIT_NATIVE(ConvertF32U32, OpConvertUToF, F32, uint32_t)
EMIT_NATIVE(CompositeConstructU64, OpCompositeConstruct, U64, uint32_t, uint32_t)
EMIT_NATIVE(CompositeConstructU32x2, OpCompositeConstruct, U32x2, uint32_t, uint32_t)
EMIT_NATIVE(CompositeConstructU32x3, OpCompositeConstruct, U32x3, uint32_t, uint32_t, uint32_t)
EMIT_NATIVE(CompositeConstructF32x2, OpCompositeConstruct, F32x2, uint32_t, uint32_t)
EMIT_NATIVE(CompositeConstructU32x4, OpCompositeConstruct, U32x4, uint32_t, uint32_t, uint32_t,
            uint32_t)
uint32_t              EmitCompositeExtractU64(ValueEmitContext& ctx, uint32_t arg0, IR::Value arg1);
inline constexpr auto EmitCompositeExtractU32x2 = EmitCompositeExtractU64;
inline constexpr auto EmitCompositeExtractU32x3 = EmitCompositeExtractU64;
inline constexpr auto EmitCompositeExtractU32x4 = EmitCompositeExtractU64;
uint32_t              EmitPackHalf2x16(ValueEmitContext& ctx, uint32_t arg0);
uint32_t              EmitPackSnorm2x16(ValueEmitContext& ctx, uint32_t arg0);
uint32_t              EmitPackUnorm2x16(ValueEmitContext& ctx, uint32_t arg0);
uint32_t              EmitPackFloat2x16Rtz(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
uint32_t              EmitFPAbs32(ValueEmitContext& ctx, uint32_t arg0);
uint32_t              EmitFPNeg32(ValueEmitContext& ctx, uint32_t arg0);
uint32_t              EmitFPSaturate32(ValueEmitContext& ctx, uint32_t arg0);
EMIT_NATIVE(BitFieldInsert, OpBitFieldInsert, U32, uint32_t, uint32_t, uint32_t, uint32_t)
EMIT_NATIVE(BitFieldUExtract, OpBitFieldUExtract, U32, uint32_t, uint32_t, uint32_t)
EMIT_NATIVE(BitFieldSExtract, OpBitFieldSExtract, U32, uint32_t, uint32_t, uint32_t)
EMIT_NATIVE(SelectU1, OpSelect, U1, uint32_t, uint32_t, uint32_t)
EMIT_NATIVE(SelectU32, OpSelect, U32, uint32_t, uint32_t, uint32_t)
EMIT_NATIVE(SelectF32, OpSelect, F32, uint32_t, uint32_t, uint32_t)
EMIT_NATIVE(IAdd32, OpIAdd, U32, uint32_t, uint32_t)
EMIT_NATIVE(ISub32, OpISub, U32, uint32_t, uint32_t)
EMIT_NATIVE(IMul32, OpIMul, U32, uint32_t, uint32_t)
EMIT_NATIVE(UDiv32, OpUDiv, U32, uint32_t, uint32_t)
uint32_t EmitIAdd64(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
uint32_t EmitISub64(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
uint32_t EmitIMul64(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
EMIT_NATIVE(IAddCarry32, OpIAddCarry, U32x2, uint32_t, uint32_t)
uint32_t EmitSMulHi(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
uint32_t EmitUMulHi(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
uint32_t EmitIAbs32(ValueEmitContext& ctx, uint32_t arg0);
EMIT_NATIVE(ShiftLeftLogical32, OpShiftLeftLogical, U32, uint32_t, uint32_t)
EMIT_NATIVE(ShiftRightLogical32, OpShiftRightLogical, U32, uint32_t, uint32_t)
EMIT_NATIVE(ShiftRightArithmetic32, OpShiftRightArithmetic, U32, uint32_t, uint32_t)
uint32_t EmitShiftLeftLogical64(ValueEmitContext& ctx, uint32_t arg0, IR::Value arg1);
uint32_t EmitShiftRightLogical64(ValueEmitContext& ctx, uint32_t arg0, IR::Value arg1);
uint32_t EmitShiftRightArithmetic64(ValueEmitContext& ctx, uint32_t arg0, IR::Value arg1);
EMIT_NATIVE(BitwiseAnd32, OpBitwiseAnd, U32, uint32_t, uint32_t)
EMIT_NATIVE(BitwiseOr32, OpBitwiseOr, U32, uint32_t, uint32_t)
EMIT_NATIVE(BitwiseXor32, OpBitwiseXor, U32, uint32_t, uint32_t)
EMIT_NATIVE(BitwiseNot32, OpNot, U32, uint32_t)
uint32_t EmitBitwiseAnd64(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
EMIT_NATIVE(BitReverse32, OpBitReverse, U32, uint32_t)
EMIT_NATIVE(BitCount32, OpBitCount, U32, uint32_t)
uint32_t EmitBitCount64(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitFindILsb32(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitFindUMsb32(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitFindUMsb64(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitSMin32(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
uint32_t EmitSMax32(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
uint32_t EmitUMin32(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
uint32_t EmitUMax32(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
uint32_t EmitSMinTri32(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1, uint32_t arg2);
uint32_t EmitSMaxTri32(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1, uint32_t arg2);
uint32_t EmitUMinTri32(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1, uint32_t arg2);
uint32_t EmitUMaxTri32(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1, uint32_t arg2);
uint32_t EmitSMedTri32(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1, uint32_t arg2);
uint32_t EmitUMedTri32(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1, uint32_t arg2);
EMIT_NATIVE(SLessThan32, OpSLessThan, U1, uint32_t, uint32_t)
EMIT_NATIVE(ULessThan32, OpULessThan, U1, uint32_t, uint32_t)
EMIT_NATIVE(IEqual32, OpIEqual, U1, uint32_t, uint32_t)
EMIT_NATIVE(SLessThanEqual32, OpSLessThanEqual, U1, uint32_t, uint32_t)
EMIT_NATIVE(ULessThanEqual32, OpULessThanEqual, U1, uint32_t, uint32_t)
EMIT_NATIVE(SGreaterThan32, OpSGreaterThan, U1, uint32_t, uint32_t)
EMIT_NATIVE(UGreaterThan32, OpUGreaterThan, U1, uint32_t, uint32_t)
EMIT_NATIVE(INotEqual32, OpINotEqual, U1, uint32_t, uint32_t)
EMIT_NATIVE(SGreaterThanEqual32, OpSGreaterThanEqual, U1, uint32_t, uint32_t)
EMIT_NATIVE(UGreaterThanEqual32, OpUGreaterThanEqual, U1, uint32_t, uint32_t)
uint32_t EmitIEqual64(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
uint32_t EmitINotEqual64(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
uint32_t EmitULessThan64(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
uint32_t EmitSLessThan64(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
uint32_t EmitUGreaterThan64(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
EMIT_NATIVE(LogicalOr, OpLogicalOr, U1, uint32_t, uint32_t)
EMIT_NATIVE(LogicalAnd, OpLogicalAnd, U1, uint32_t, uint32_t)
EMIT_NATIVE(LogicalXor, OpLogicalNotEqual, U1, uint32_t, uint32_t)
EMIT_NATIVE(LogicalNot, OpLogicalNot, U1, uint32_t)
EMIT_NATIVE(FPOrdEqual32, OpFOrdEqual, U1, uint32_t, uint32_t)
EMIT_NATIVE(FPUnordEqual32, OpFUnordEqual, U1, uint32_t, uint32_t)
EMIT_NATIVE(FPOrdNotEqual32, OpFOrdNotEqual, U1, uint32_t, uint32_t)
EMIT_NATIVE(FPUnordNotEqual32, OpFUnordNotEqual, U1, uint32_t, uint32_t)
EMIT_NATIVE(FPOrdLessThan32, OpFOrdLessThan, U1, uint32_t, uint32_t)
EMIT_NATIVE(FPUnordLessThan32, OpFUnordLessThan, U1, uint32_t, uint32_t)
EMIT_NATIVE(FPOrdGreaterThan32, OpFOrdGreaterThan, U1, uint32_t, uint32_t)
EMIT_NATIVE(FPUnordGreaterThan32, OpFUnordGreaterThan, U1, uint32_t, uint32_t)
EMIT_NATIVE(FPOrdLessThanEqual32, OpFOrdLessThanEqual, U1, uint32_t, uint32_t)
EMIT_NATIVE(FPUnordLessThanEqual32, OpFUnordLessThanEqual, U1, uint32_t, uint32_t)
EMIT_NATIVE(FPOrdGreaterThanEqual32, OpFOrdGreaterThanEqual, U1, uint32_t, uint32_t)
EMIT_NATIVE(FPUnordGreaterThanEqual32, OpFUnordGreaterThanEqual, U1, uint32_t, uint32_t)
uint32_t EmitFPIsNan32(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitFPCmpClass32(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
EMIT_NATIVE(FPAdd32, OpFAdd, F32, uint32_t, uint32_t)
EMIT_NATIVE(FPSub32, OpFSub, F32, uint32_t, uint32_t)
EMIT_NATIVE(FPMul32, OpFMul, F32, uint32_t, uint32_t)
uint32_t EmitFPFma32(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1, uint32_t arg2);
uint32_t EmitFPMin32(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
uint32_t EmitFPMax32(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
uint32_t EmitFPMinTri32(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1, uint32_t arg2);
uint32_t EmitFPMaxTri32(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1, uint32_t arg2);
uint32_t EmitFPMedTri32(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1, uint32_t arg2);
uint32_t EmitFPRecip32(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitFPRecipIFlag32(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitFPRecipSqrt32(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitFPSqrt(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitFPExp2(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitFPLog2(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitFPLdexp(ValueEmitContext& ctx, uint32_t arg0, uint32_t arg1);
uint32_t EmitFPRoundEven32(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitFPFloor32(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitFPCeil32(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitFPTrunc32(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitFPFract32(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitFPSin(ValueEmitContext& ctx, uint32_t arg0);
uint32_t EmitFPCos(ValueEmitContext& ctx, uint32_t arg0);
#undef EMIT_NATIVE

uint32_t              EmitIdentity(ValueEmitContext& ctx, uint32_t value);
void                  EmitVoid(ValueEmitContext&);
inline constexpr auto EmitReference    = EmitVoid;
inline constexpr auto EmitReferenceU32 = EmitVoid;
inline constexpr auto EmitControlNop   = EmitVoid;
inline constexpr auto EmitWaitcnt      = EmitVoid;
inline constexpr auto EmitSendmsg      = EmitVoid;
inline constexpr auto EmitTtraceData   = EmitVoid;
inline constexpr auto EmitInstPrefetch = EmitVoid;
void                  EmitBarrier(ValueEmitContext& ctx);
void                  EmitMeshAllocate(ValueEmitContext& ctx, const IR::Inst& inst);
uint32_t              EmitMeshDrawParameter(ValueEmitContext& ctx, const IR::Inst& inst);
uint32_t              EmitGetUserData(ValueEmitContext& ctx, IR::ScalarReg reg);
uint32_t              EmitGetBuiltin(ValueEmitContext& ctx, IR::Value kind, IR::Value index);
uint32_t              EmitUndefU1(ValueEmitContext& ctx, const IR::Inst& inst);
inline constexpr auto EmitUndefU8  = EmitUndefU1;
inline constexpr auto EmitUndefU16 = EmitUndefU1;
inline constexpr auto EmitUndefU32 = EmitUndefU1;
inline constexpr auto EmitUndefU64 = EmitUndefU1;
uint32_t              EmitDppMoveU32(ValueEmitContext& ctx, const IR::Inst& inst);
uint32_t              EmitDppUpdateU32(ValueEmitContext& ctx, const IR::Inst& inst);
uint32_t              EmitWqmU64(ValueEmitContext& ctx, uint32_t value);
uint32_t              EmitLaneId(ValueEmitContext& ctx);
uint32_t              EmitBallot(ValueEmitContext& ctx, IR::Value predicate);
uint32_t              EmitReadFirstLane(ValueEmitContext& ctx, const IR::Inst& inst);
uint32_t              EmitReadLane(ValueEmitContext& ctx, const IR::Inst& inst);
uint32_t              EmitWriteLane(ValueEmitContext& ctx, const IR::Inst& inst);
uint32_t              EmitPermlane16U32(ValueEmitContext& ctx, const IR::Inst& inst);
uint32_t              EmitGetAttribute(ValueEmitContext& ctx, const IR::Inst& inst);
uint32_t              EmitGetInterpolationParameter(ValueEmitContext& ctx, const IR::Inst& inst);
void                  EmitSetAttribute(ValueEmitContext& ctx, const IR::Inst& inst);
uint32_t              EmitGetShaderBase(ValueEmitContext& ctx);
inline constexpr auto EmitGetSrtResource     = EmitVoid;
inline constexpr auto EmitGetBufferResource  = EmitGetSrtResource;
inline constexpr auto EmitGetAddressResource = EmitGetSrtResource;
inline constexpr auto EmitGetScratchResource = EmitGetSrtResource;
inline constexpr auto EmitGetImageResource   = EmitGetSrtResource;
inline constexpr auto EmitGetSamplerResource = EmitGetSrtResource;
inline constexpr auto EmitMakeImageAddress   = EmitGetSrtResource;
void                  EmitMemory(ValueEmitContext& ctx, const IR::Inst& inst);
inline constexpr auto EmitBpermuteU32           = EmitMemory;
inline constexpr auto EmitReadConst             = EmitMemory;
inline constexpr auto EmitReadConstBuffer       = EmitMemory;
inline constexpr auto EmitLoadAddressU8         = EmitMemory;
inline constexpr auto EmitLoadAddressU16        = EmitMemory;
inline constexpr auto EmitLoadAddressU32        = EmitMemory;
inline constexpr auto EmitStoreAddressU8        = EmitMemory;
inline constexpr auto EmitStoreAddressU16       = EmitMemory;
inline constexpr auto EmitStoreAddressU32       = EmitMemory;
inline constexpr auto EmitLoadBufferU8          = EmitMemory;
inline constexpr auto EmitLoadBufferU16         = EmitMemory;
inline constexpr auto EmitLoadBufferU32         = EmitMemory;
inline constexpr auto EmitLoadBufferU32x2       = EmitMemory;
inline constexpr auto EmitLoadBufferU32x3       = EmitMemory;
inline constexpr auto EmitLoadBufferU32x4       = EmitMemory;
inline constexpr auto EmitStoreBufferU8         = EmitMemory;
inline constexpr auto EmitStoreBufferU16        = EmitMemory;
inline constexpr auto EmitStoreBufferU32        = EmitMemory;
inline constexpr auto EmitStoreBufferU32x2      = EmitMemory;
inline constexpr auto EmitStoreBufferU32x3      = EmitMemory;
inline constexpr auto EmitStoreBufferU32x4      = EmitMemory;
inline constexpr auto EmitBufferAtomicSwap32    = EmitMemory;
inline constexpr auto EmitBufferAtomicCmpSwap32 = EmitMemory;
inline constexpr auto EmitBufferAtomicSwap64    = EmitMemory;
inline constexpr auto EmitBufferAtomicIAdd32    = EmitMemory;
inline constexpr auto EmitBufferAtomicISub32    = EmitMemory;
inline constexpr auto EmitBufferAtomicSMin32    = EmitMemory;
inline constexpr auto EmitBufferAtomicUMin32    = EmitMemory;
inline constexpr auto EmitBufferAtomicSMax32    = EmitMemory;
inline constexpr auto EmitBufferAtomicUMax32    = EmitMemory;
inline constexpr auto EmitBufferAtomicAnd32     = EmitMemory;
inline constexpr auto EmitBufferAtomicOr32      = EmitMemory;
inline constexpr auto EmitBufferAtomicOr64      = EmitMemory;
inline constexpr auto EmitBufferAtomicXor32     = EmitMemory;
inline constexpr auto EmitBufferAtomicFMin32    = EmitMemory;
inline constexpr auto EmitBufferAtomicFMax32    = EmitMemory;
inline constexpr auto EmitLoadSharedU8          = EmitMemory;
inline constexpr auto EmitLoadSharedU16         = EmitMemory;
inline constexpr auto EmitLoadSharedU32         = EmitMemory;
inline constexpr auto EmitLoadSharedU32x2       = EmitMemory;
inline constexpr auto EmitLoadSharedU32x3       = EmitMemory;
inline constexpr auto EmitLoadSharedU32x4       = EmitMemory;
inline constexpr auto EmitWriteSharedU8         = EmitMemory;
inline constexpr auto EmitWriteSharedU16        = EmitMemory;
inline constexpr auto EmitWriteSharedU32        = EmitMemory;
inline constexpr auto EmitWriteSharedU32x2      = EmitMemory;
inline constexpr auto EmitWriteSharedU32x3      = EmitMemory;
inline constexpr auto EmitWriteSharedU32x4      = EmitMemory;
inline constexpr auto EmitSharedAtomicFMin32    = EmitMemory;
inline constexpr auto EmitSharedAtomicFMax32    = EmitMemory;
inline constexpr auto EmitSharedAtomicSwap32    = EmitMemory;
inline constexpr auto EmitSharedAtomicIAdd32    = EmitMemory;
inline constexpr auto EmitSharedAtomicISub32    = EmitMemory;
inline constexpr auto EmitSharedAtomicInc32     = EmitMemory;
inline constexpr auto EmitSharedAtomicDec32     = EmitMemory;
inline constexpr auto EmitSharedAtomicSMin32    = EmitMemory;
inline constexpr auto EmitSharedAtomicUMin32    = EmitMemory;
inline constexpr auto EmitSharedAtomicSMax32    = EmitMemory;
inline constexpr auto EmitSharedAtomicUMax32    = EmitMemory;
inline constexpr auto EmitSharedAtomicAnd32     = EmitMemory;
inline constexpr auto EmitSharedAtomicOr32      = EmitMemory;
inline constexpr auto EmitSharedAtomicXor32     = EmitMemory;
inline constexpr auto EmitDataAppend            = EmitMemory;
inline constexpr auto EmitDataConsume           = EmitMemory;
inline constexpr auto EmitSwizzleU32            = EmitMemory;
void                  EmitImage(ValueEmitContext& ctx, const IR::Inst& inst);
inline constexpr auto EmitImageQueryDimensions = EmitImage;
inline constexpr auto EmitImageQueryLod        = EmitImage;
inline constexpr auto EmitImageRead            = EmitImage;
inline constexpr auto EmitImageWrite           = EmitImage;
inline constexpr auto EmitImageSampleRaw       = EmitImage;
inline constexpr auto EmitImageGatherRaw       = EmitImage;
inline constexpr auto EmitImageAtomicSwap32    = EmitImage;
inline constexpr auto EmitImageAtomicIAdd32    = EmitImage;
inline constexpr auto EmitImageAtomicUMin32    = EmitImage;
inline constexpr auto EmitImageAtomicUMax32    = EmitImage;
inline constexpr auto EmitImageAtomicAnd32     = EmitImage;
inline constexpr auto EmitImageAtomicOr32      = EmitImage;
inline constexpr auto EmitImageAtomicXor32     = EmitImage;
void                  EmitUnreachable(ValueEmitContext& ctx, const IR::Inst& inst);
inline constexpr auto EmitPhi                        = EmitUnreachable;
inline constexpr auto EmitGetThreadBitScalarRegister = EmitUnreachable;
inline constexpr auto EmitSetThreadBitScalarRegister = EmitUnreachable;
inline constexpr auto EmitGetScalarMaskTag           = EmitUnreachable;
inline constexpr auto EmitSetScalarMaskTag           = EmitUnreachable;
inline constexpr auto EmitGetScalarRegister          = EmitUnreachable;
inline constexpr auto EmitSetScalarRegister          = EmitUnreachable;
inline constexpr auto EmitGetVectorRegister          = EmitUnreachable;
inline constexpr auto EmitSetVectorRegister          = EmitUnreachable;
inline constexpr auto EmitGetGotoVariable            = EmitUnreachable;
inline constexpr auto EmitSetGotoVariable            = EmitUnreachable;
inline constexpr auto EmitGetScc                     = EmitUnreachable;
inline constexpr auto EmitSetScc                     = EmitUnreachable;
inline constexpr auto EmitGetExec                    = EmitUnreachable;
inline constexpr auto EmitSetExec                    = EmitUnreachable;
inline constexpr auto EmitGetExecLo                  = EmitUnreachable;
inline constexpr auto EmitSetExecLo                  = EmitUnreachable;
inline constexpr auto EmitGetExecHi                  = EmitUnreachable;
inline constexpr auto EmitSetExecHi                  = EmitUnreachable;
inline constexpr auto EmitGetVcc                     = EmitUnreachable;
inline constexpr auto EmitSetVcc                     = EmitUnreachable;
inline constexpr auto EmitGetVccLo                   = EmitUnreachable;
inline constexpr auto EmitSetVccLo                   = EmitUnreachable;
inline constexpr auto EmitGetVccHi                   = EmitUnreachable;
inline constexpr auto EmitSetVccHi                   = EmitUnreachable;
inline constexpr auto EmitGetM0                      = EmitUnreachable;
inline constexpr auto EmitSetM0                      = EmitUnreachable;

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
