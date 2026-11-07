#ifndef GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_PM4_DISPATCH_H
#define GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_PM4_DISPATCH_H

#include "graphics/guest_gpu/pm4.h"

#include <array>
#include <cstdint>

namespace Libs::Graphics {

class CommandProcessor;

using hw_sh_parser_func_t = uint32_t (*)(CommandProcessor&, uint32_t, uint32_t, const uint32_t*,
                                         uint32_t);
using cp_op_parser_func_t = uint32_t (*)(CommandProcessor&, uint32_t, const uint32_t*, uint32_t,
                                         uint32_t);
using hw_reg_func_t       = void (*)(CommandProcessor&, uint32_t, uint32_t);

extern const std::array<hw_reg_func_t, Pm4::CX_NUM>       g_hw_ctx_indirect_func;
extern const std::array<hw_sh_parser_func_t, Pm4::SH_NUM> g_hw_sh_func;
extern const std::array<hw_reg_func_t, Pm4::SH_NUM>       g_hw_sh_indirect_func;
extern const std::array<hw_reg_func_t, Pm4::UC_NUM>       g_hw_uc_indirect_func;
extern const std::array<cp_op_parser_func_t, 256>         g_cp_op_func;
extern const std::array<cp_op_parser_func_t, Pm4::R_NUM>  g_cp_op_custom_func;

uint32_t HwShSetPsUserSgpr(CommandProcessor&, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwShSetGsUserSgpr(CommandProcessor&, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwShSetHsUserSgpr(CommandProcessor&, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwShSetCsUserSgpr(CommandProcessor&, uint32_t, uint32_t, const uint32_t*, uint32_t);
uint32_t HwShIgnoreUserAccumulator(CommandProcessor&, uint32_t, uint32_t, const uint32_t*,
                                   uint32_t);
uint32_t HwShSetCsRegisters(CommandProcessor&, uint32_t, uint32_t, const uint32_t*, uint32_t);

uint32_t CpOpNop(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpContextState(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpSetBase(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpClearState(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpIndexBase(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpIndexBufferSize(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDispatchDirect(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDispatchIndirect(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDrawIndirect(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDrawIndirectMulti(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDrawIndex(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDrawIndexMultiInst(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDrawIndexOffset(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpPfpSyncMe(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpRewind(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpIndexType(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpNumInstances(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDrawIndexAuto(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpCondExec(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpSetPredication(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpWriteData(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpIndirectBuffer(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpCopyData(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpEventWrite(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDmaData(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpAcquireMem(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpReleaseMem(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpMemSemaphore(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpSetContextReg(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpSetShaderReg(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpSetUconfigReg(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpIndirectCxRegs(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpIndirectShRegs(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpIndirectUcRegs(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpGetLodStats(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpDispatchReset(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpWaitRegMem32(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpWaitFlipDone(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpPushMarker(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpPopMarker(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpWaitRegMem64(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpFlip(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);
uint32_t CpOpPssr(CommandProcessor&, uint32_t, const uint32_t*, uint32_t, uint32_t);

} // namespace Libs::Graphics

#endif // GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_PM4_DISPATCH_H
