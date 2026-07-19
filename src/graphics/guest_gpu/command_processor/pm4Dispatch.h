#ifndef GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_PM4_DISPATCH_H
#define GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_PM4_DISPATCH_H

#include "graphics/guest_gpu/pm4.h"

#include <array>
#include <cstdint>

namespace Libs::Graphics {

class CommandProcessor;

using hw_ctx_parser_func_t   = uint32_t (*)(CommandProcessor*, uint32_t, uint32_t, const uint32_t*,
                                            uint32_t);
using hw_uc_parser_func_t    = uint32_t (*)(CommandProcessor*, uint32_t, uint32_t, const uint32_t*,
                                            uint32_t);
using hw_sh_parser_func_t    = uint32_t (*)(CommandProcessor*, uint32_t, uint32_t, const uint32_t*,
                                            uint32_t);
using cp_op_parser_func_t    = uint32_t (*)(CommandProcessor*, uint32_t, const uint32_t*, uint32_t,
                                            uint32_t);
using hw_ctx_indirect_func_t = void (*)(CommandProcessor*, uint32_t, uint32_t);
using hw_uc_indirect_func_t  = void (*)(CommandProcessor*, uint32_t, uint32_t);
using hw_sh_indirect_func_t  = void (*)(CommandProcessor*, uint32_t, uint32_t);

extern const std::array<hw_ctx_parser_func_t, Pm4::CX_NUM> g_hw_ctx_func;
extern hw_ctx_indirect_func_t                              g_hw_ctx_indirect_func[Pm4::CX_NUM];
extern const std::array<hw_sh_parser_func_t, Pm4::SH_NUM>  g_hw_sh_func;
extern hw_sh_indirect_func_t                               g_hw_sh_indirect_func[Pm4::SH_NUM];
extern const std::array<hw_uc_parser_func_t, Pm4::UC_NUM>  g_hw_uc_func;
extern hw_uc_indirect_func_t                               g_hw_uc_indirect_func[Pm4::UC_NUM];
extern const std::array<cp_op_parser_func_t, 256>          g_cp_op_func;
extern const std::array<cp_op_parser_func_t, Pm4::R_NUM>   g_cp_op_custom_func;

void GraphicsInitJmpTables();
void GraphicsInitJmpTablesCxIndirect();
void GraphicsInitJmpTablesShIndirect();
void GraphicsInitJmpTablesUcIndirect();

uint32_t HwCtxSetRenderControl(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetDepthMetadataRegisters(CommandProcessor*, uint32_t, uint32_t, const uint32_t*,
                                        uint32_t);
uint32_t HwCtxSetBorderColorTableAddr(CommandProcessor*, uint32_t, uint32_t, const uint32_t*,
                                      uint32_t);
uint32_t HwCtxSetStencilClear(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetDepthClear(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetScreenScissor(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetDepthRenderTarget(CommandProcessor*, uint32_t, uint32_t, const uint32_t*,
                                   uint32_t);
uint32_t HwCtxSetStencilInfo(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetDepthHtileSurface(CommandProcessor*, uint32_t, uint32_t, const uint32_t*,
                                   uint32_t);
uint32_t HwCtxSetHardwareScreenOffset(CommandProcessor*, uint32_t, uint32_t, const uint32_t*,
                                      uint32_t);
uint32_t HwCtxSetWindowOffset(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetWindowScissor(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetClipRect(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetRenderTargetMask(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetGenericScissor(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetBlendColor(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetCbDccControl(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetStencilControl(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetStencilMask(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetPsInput(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetSpiTmpringSize(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetDepthControl(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetEqaaControl(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetColorControl(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetDepthBounds(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetPointState(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetClipControl(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetModeControl(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetPolyOffsetRegisters(CommandProcessor*, uint32_t, uint32_t, const uint32_t*,
                                     uint32_t);
uint32_t HwCtxSetViewportTransformControl(CommandProcessor*, uint32_t, uint32_t, const uint32_t*,
                                          uint32_t);
uint32_t HwCtxSetLineControl(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetFovWindow(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetScanModeControl(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetScanModeControl1(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetAaConfig(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetAaSampleControl(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetCentroidPriority(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetAaMask(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetVtxControl(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetPaScExtendedControl(CommandProcessor*, uint32_t, uint32_t, const uint32_t*,
                                     uint32_t);
uint32_t HwCtxSetAlphaToMask(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetDrawPayloadControl(CommandProcessor*, uint32_t, uint32_t, const uint32_t*,
                                    uint32_t);
uint32_t HwCtxSetPrimitiveIdReset(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetObjprimIdControl(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetShaderStages(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetGuardBands(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetRenderTarget(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetColorInfo(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetBlendControl(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetViewportScissor(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetViewportZ(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwCtxSetViewportScaleOffset(CommandProcessor*, uint32_t, uint32_t, const uint32_t*,
                                     uint32_t);
uint32_t HwShSetPsUserSgpr(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwShSetGsUserSgpr(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwShSetHsUserSgpr(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwShSetCsUserSgpr(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwShSetPsUserAccumSgpr(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwShSetGsUserAccumSgpr(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwShSetHsUserAccumSgpr(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwShSetCsUserAccumSgpr(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwShSetCsRegisters(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwShIgnoreRegisters(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwUcSetPrimitiveType(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwUcSetIndexType(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwUcSetObjectId(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwUcSetTextureGradientFactors(CommandProcessor*, uint32_t, uint32_t, const uint32_t*,
                                       uint32_t);
uint32_t HwUcSetIaMultiVgtParam(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwUcSetMultiPrimIbReset(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwUcSetBorderColorTableAddr(CommandProcessor*, uint32_t, uint32_t, const uint32_t*,
                                     uint32_t);
uint32_t HwUcSetGeIndexOffset(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwUcSetGdsOaRegisters(CommandProcessor*, uint32_t, uint32_t, const uint32_t*, uint32_t);

uint32_t CpOpNop(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpSetBase(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpClearState(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpIndexBase(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpIndexBufferSize(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDispatchDirect(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDispatchIndirect(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDrawIndirect(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDrawIndirectMulti(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDrawIndex(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDrawIndexOffset(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpPfpSyncMe(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpIndexType(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpNumInstances(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDrawIndexAuto(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpCondExec(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpSetPredication(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpWriteData(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpIndirectBuffer(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpCopyData(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpEventWrite(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpEventWriteEop(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpEventWriteEos(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDmaData(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpAcquireMem(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpSetContextReg(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpSetShaderReg(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpSetUconfigReg(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpIndirectCxRegs(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpIndirectShRegs(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpIndirectUcRegs(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpWriteConstRam(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDumpConstRam(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpIncrementCeCounter(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpIncrementDeCounter(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpWaitOnCeCounter(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpWaitOnDeCounterDiff(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpGetLodStats(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDispatchReset(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpWaitRegMem32(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpWaitFlipDone(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpPushMarker(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpPopMarker(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpWaitRegMem64(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpFlip(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpReleaseMem(CommandProcessor*, uint32_t, const uint32_t*, uint32_t, uint32_t);

} // namespace Libs::Graphics

#endif // GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_PM4_DISPATCH_H
