#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"

namespace Libs::Graphics {

namespace {

constexpr auto MakeShaderDispatchTable() {
	std::array<hw_sh_parser_func_t, Pm4::SH_NUM> table {};

	for (uint32_t slot = 0; slot < 32; slot++) {
		table[Pm4::SPI_SHADER_USER_DATA_PS_0 + slot] = HwShSetPsUserSgpr;
		table[Pm4::SPI_SHADER_USER_DATA_GS_0 + slot] = HwShSetGsUserSgpr;
		table[Pm4::SPI_SHADER_USER_DATA_HS_0 + slot] = HwShSetHsUserSgpr;
	}
	for (uint32_t slot = 0; slot < 16; slot++) {
		table[Pm4::COMPUTE_USER_DATA_0 + slot] = HwShSetCsUserSgpr;
	}
	for (const auto base: {Pm4::SPI_SHADER_USER_ACCUM_PS_0, Pm4::SPI_SHADER_USER_ACCUM_ESGS_0,
	                       Pm4::SPI_SHADER_USER_ACCUM_LSHS_0, Pm4::COMPUTE_USER_ACCUM_0}) {
		for (uint32_t slot = 0; slot < 4; slot++) {
			table[base + slot] = HwShIgnoreUserAccumulator;
		}
	}
	table[Pm4::COMPUTE_PGM_LO]          = HwShSetCsRegisters;
	table[Pm4::COMPUTE_PGM_HI]          = HwShSetCsRegisters;
	table[Pm4::COMPUTE_PGM_RSRC1]       = HwShSetCsRegisters;
	table[Pm4::COMPUTE_PGM_RSRC2]       = HwShSetCsRegisters;
	table[Pm4::COMPUTE_START_X]         = HwShSetCsRegisters;
	table[Pm4::COMPUTE_START_Y]         = HwShSetCsRegisters;
	table[Pm4::COMPUTE_START_Z]         = HwShSetCsRegisters;
	table[Pm4::COMPUTE_NUM_THREAD_X]    = HwShSetCsRegisters;
	table[Pm4::COMPUTE_NUM_THREAD_Y]    = HwShSetCsRegisters;
	table[Pm4::COMPUTE_NUM_THREAD_Z]    = HwShSetCsRegisters;
	table[Pm4::COMPUTE_RESOURCE_LIMITS] = HwShSetCsRegisters;
	table[Pm4::COMPUTE_TMPRING_SIZE]    = HwShSetCsRegisters;
	table[Pm4::COMPUTE_PGM_RSRC3]       = HwShSetCsRegisters;
	table[Pm4::COMPUTE_PACE_ID]         = HwShSetCsRegisters;

	return table;
}

constexpr auto MakeOpcodeDispatchTable() {
	std::array<cp_op_parser_func_t, 256> table {};

	table[Pm4::IT_NOP]                       = CpOpNop;
	table[Pm4::IT_SET_BASE]                  = CpOpSetBase;
	table[Pm4::IT_CLEAR_STATE]               = CpOpClearState;
	table[Pm4::IT_INDEX_BASE]                = CpOpIndexBase;
	table[Pm4::IT_INDEX_BUFFER_SIZE]         = CpOpIndexBufferSize;
	table[Pm4::IT_DISPATCH_DIRECT]           = CpOpDispatchDirect;
	table[Pm4::IT_DISPATCH_INDIRECT]         = CpOpDispatchIndirect;
	table[Pm4::IT_DRAW_INDIRECT]             = CpOpDrawIndirect;
	table[Pm4::IT_DRAW_INDEX_INDIRECT]       = CpOpDrawIndirect;
	table[Pm4::IT_DRAW_INDIRECT_MULTI]       = CpOpDrawIndirectMulti;
	table[Pm4::IT_DRAW_INDEX_INDIRECT_MULTI] = CpOpDrawIndirectMulti;
	table[Pm4::IT_DRAW_INDEX_2]              = CpOpDrawIndex;
	table[Pm4::IT_DRAW_INDEX_OFFSET_2]       = CpOpDrawIndexOffset;
	table[Pm4::IT_DISPATCH_DRAW_PREAMBLE]    = CpOpDrawIndex;
	table[Pm4::IT_PFP_SYNC_ME]               = CpOpPfpSyncMe;
	table[Pm4::IT_INDEX_TYPE]                = CpOpIndexType;
	table[Pm4::IT_NUM_INSTANCES]             = CpOpNumInstances;
	table[Pm4::IT_DRAW_INDEX_AUTO]           = CpOpDrawIndexAuto;
	table[Pm4::IT_COND_EXEC]                 = CpOpCondExec;
	table[Pm4::IT_SET_PREDICATION]           = CpOpSetPredication;
	table[Pm4::IT_WRITE_DATA]                = CpOpWriteData;
	table[Pm4::IT_WAIT_REG_MEM]              = CpOpWaitRegMem32;
	table[Pm4::IT_INDIRECT_BUFFER]           = CpOpIndirectBuffer;
	table[Pm4::IT_COPY_DATA]                 = CpOpCopyData;
	table[Pm4::IT_EVENT_WRITE]               = CpOpEventWrite;
	table[Pm4::IT_DMA_DATA]                  = CpOpDmaData;
	table[Pm4::IT_ACQUIRE_MEM]               = CpOpAcquireMem;
	table[Pm4::IT_RELEASE_MEM]               = CpOpReleaseMem;
	table[Pm4::IT_MEM_SEMAPHORE]             = CpOpMemSemaphore;
	table[Pm4::IT_REWIND]                    = CpOpRewind;
	table[Pm4::IT_SET_CONTEXT_REG]           = CpOpSetContextReg;
	table[Pm4::IT_SET_SH_REG]                = CpOpSetShaderReg;
	table[Pm4::IT_SET_UCONFIG_REG]           = CpOpSetUconfigReg;
	table[Pm4::IT_SET_UCONFIG_REG_INDEX]     = CpOpSetUconfigReg;
	table[Pm4::IT_SET_CONTEXT_REG_INDIRECT]  = CpOpIndirectCxRegs;
	table[Pm4::IT_SET_SH_REG_INDIRECT]       = CpOpIndirectShRegs;
	table[Pm4::IT_SET_UCONFIG_REG_INDIRECT]  = CpOpIndirectUcRegs;
	table[Pm4::IT_GET_LOD_STATS]             = CpOpGetLodStats;
	table[Pm4::IT_WAIT_REG_MEM_64]           = CpOpWaitRegMem64;

	return table;
}

constexpr auto MakeCustomOpcodeDispatchTable() {
	std::array<cp_op_parser_func_t, Pm4::R_NUM> table {};

	table[Pm4::R_DISPATCH_RESET] = CpOpDispatchReset;
	table[Pm4::R_WAIT_FLIP_DONE] = CpOpWaitFlipDone;
	table[Pm4::R_PUSH_MARKER]    = CpOpPushMarker;
	table[Pm4::R_POP_MARKER]     = CpOpPopMarker;
	table[Pm4::R_FLIP]           = CpOpFlip;
	table[Pm4::R_CONTEXT_STATE]  = CpOpContextState;
	table[Pm4::R_PSSR]           = CpOpPssr;

	return table;
}

} // namespace

constinit const std::array<hw_sh_parser_func_t, Pm4::SH_NUM> g_hw_sh_func =
    MakeShaderDispatchTable();
constinit const std::array<cp_op_parser_func_t, 256> g_cp_op_func = MakeOpcodeDispatchTable();
constinit const std::array<cp_op_parser_func_t, Pm4::R_NUM> g_cp_op_custom_func =
    MakeCustomOpcodeDispatchTable();

} // namespace Libs::Graphics
