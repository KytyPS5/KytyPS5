#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"

namespace Libs::Graphics {

hw_ctx_indirect_func_t g_hw_ctx_indirect_func[Pm4::CX_NUM] = {};
hw_sh_indirect_func_t  g_hw_sh_indirect_func[Pm4::SH_NUM]  = {};
hw_uc_indirect_func_t  g_hw_uc_indirect_func[Pm4::UC_NUM]  = {};

namespace {

constexpr auto MakeContextDispatchTable() {
	std::array<hw_ctx_parser_func_t, Pm4::CX_NUM> g_hw_ctx_func {};

	g_hw_ctx_func[Pm4::DB_RENDER_CONTROL]            = HwCtxSetRenderControl;
	g_hw_ctx_func[Pm4::DB_COUNT_CONTROL]             = HwCtxSetDepthMetadataRegisters;
	g_hw_ctx_func[Pm4::DB_RENDER_OVERRIDE]           = HwCtxSetDepthMetadataRegisters;
	g_hw_ctx_func[Pm4::DB_RENDER_OVERRIDE2]          = HwCtxSetDepthMetadataRegisters;
	g_hw_ctx_func[Pm4::DB_DFSM_CONTROL]              = HwCtxSetDepthMetadataRegisters;
	g_hw_ctx_func[Pm4::DB_RMI_L2_CACHE_CONTROL]      = HwCtxSetDepthMetadataRegisters;
	g_hw_ctx_func[Pm4::CB_RMI_GL2_CACHE_CONTROL]     = HwCtxSetDepthMetadataRegisters;
	g_hw_ctx_func[Pm4::TA_BC_BASE_ADDR]              = HwCtxSetBorderColorTableAddr;
	g_hw_ctx_func[Pm4::TA_BC_BASE_ADDR_HI]           = HwCtxSetBorderColorTableAddr;
	g_hw_ctx_func[Pm4::DB_STENCIL_CLEAR]             = HwCtxSetStencilClear;
	g_hw_ctx_func[Pm4::DB_DEPTH_CLEAR]               = HwCtxSetDepthClear;
	g_hw_ctx_func[Pm4::PA_SC_SCREEN_SCISSOR_TL]      = HwCtxSetScreenScissor;
	g_hw_ctx_func[Pm4::DB_Z_INFO]                    = HwCtxSetDepthRenderTarget;
	g_hw_ctx_func[Pm4::DB_STENCIL_INFO]              = HwCtxSetStencilInfo;
	g_hw_ctx_func[Pm4::DB_HTILE_SURFACE]             = HwCtxSetDepthHtileSurface;
	g_hw_ctx_func[Pm4::PA_SU_HARDWARE_SCREEN_OFFSET] = HwCtxSetHardwareScreenOffset;
	g_hw_ctx_func[Pm4::PA_SC_WINDOW_OFFSET]          = HwCtxSetWindowOffset;
	g_hw_ctx_func[Pm4::PA_SC_WINDOW_SCISSOR_TL]      = HwCtxSetWindowScissor;
	for (auto cmd_offset = Pm4::PA_SC_CLIPRECT_RULE; cmd_offset < Pm4::PA_SC_CLIPRECT_0_TL + 8u;
	     cmd_offset++) {
		g_hw_ctx_func[cmd_offset] = HwCtxSetClipRect;
	}
	g_hw_ctx_func[Pm4::CB_TARGET_MASK]                 = HwCtxSetRenderTargetMask;
	g_hw_ctx_func[Pm4::PA_SC_GENERIC_SCISSOR_TL]       = HwCtxSetGenericScissor;
	g_hw_ctx_func[Pm4::CB_BLEND_RED]                   = HwCtxSetBlendColor;
	g_hw_ctx_func[Pm4::CB_DCC_CONTROL]                 = HwCtxSetCbDccControl;
	g_hw_ctx_func[Pm4::DB_STENCIL_CONTROL]             = HwCtxSetStencilControl;
	g_hw_ctx_func[Pm4::DB_STENCILREFMASK]              = HwCtxSetStencilMask;
	g_hw_ctx_func[Pm4::SPI_PS_INPUT_CNTL_0]            = HwCtxSetPsInput;
	g_hw_ctx_func[Pm4::SPI_TMPRING_SIZE]               = HwCtxSetSpiTmpringSize;
	g_hw_ctx_func[Pm4::DB_DEPTH_CONTROL]               = HwCtxSetDepthControl;
	g_hw_ctx_func[Pm4::DB_EQAA]                        = HwCtxSetEqaaControl;
	g_hw_ctx_func[Pm4::CB_COLOR_CONTROL]               = HwCtxSetColorControl;
	g_hw_ctx_func[Pm4::DB_DEPTH_BOUNDS_MIN]            = HwCtxSetDepthBounds;
	g_hw_ctx_func[Pm4::DB_DEPTH_BOUNDS_MAX]            = HwCtxSetDepthBounds;
	g_hw_ctx_func[Pm4::PA_SU_POINT_SIZE]               = HwCtxSetPointState;
	g_hw_ctx_func[Pm4::PA_SU_POINT_MINMAX]             = HwCtxSetPointState;
	g_hw_ctx_func[Pm4::PA_CL_CLIP_CNTL]                = HwCtxSetClipControl;
	g_hw_ctx_func[Pm4::PA_SU_SC_MODE_CNTL]             = HwCtxSetModeControl;
	g_hw_ctx_func[Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL]  = HwCtxSetPolyOffsetRegisters;
	g_hw_ctx_func[Pm4::PA_SU_POLY_OFFSET_CLAMP]        = HwCtxSetPolyOffsetRegisters;
	g_hw_ctx_func[Pm4::PA_SU_POLY_OFFSET_FRONT_SCALE]  = HwCtxSetPolyOffsetRegisters;
	g_hw_ctx_func[Pm4::PA_SU_POLY_OFFSET_FRONT_OFFSET] = HwCtxSetPolyOffsetRegisters;
	g_hw_ctx_func[Pm4::PA_SU_POLY_OFFSET_BACK_SCALE]   = HwCtxSetPolyOffsetRegisters;
	g_hw_ctx_func[Pm4::PA_SU_POLY_OFFSET_BACK_OFFSET]  = HwCtxSetPolyOffsetRegisters;
	g_hw_ctx_func[Pm4::PA_CL_VTE_CNTL]                 = HwCtxSetViewportTransformControl;
	g_hw_ctx_func[Pm4::PA_SU_LINE_CNTL]                = HwCtxSetLineControl;
	g_hw_ctx_func[Pm4::PA_SC_FOV_WINDOW_LR]            = HwCtxSetFovWindow;
	g_hw_ctx_func[Pm4::PA_SC_FOV_WINDOW_TB]            = HwCtxSetFovWindow;
	g_hw_ctx_func[Pm4::PA_SC_MODE_CNTL_0]              = HwCtxSetScanModeControl;
	g_hw_ctx_func[Pm4::PA_SC_MODE_CNTL_1]              = HwCtxSetScanModeControl1;
	g_hw_ctx_func[Pm4::PA_SC_AA_CONFIG]                = HwCtxSetAaConfig;
	for (auto cmd_offset = Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0;
	     cmd_offset < Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0 + 16; cmd_offset++) {
		g_hw_ctx_func[cmd_offset] = HwCtxSetAaSampleControl;
	}
	g_hw_ctx_func[Pm4::PA_SC_CENTROID_PRIORITY_0]             = HwCtxSetCentroidPriority;
	g_hw_ctx_func[Pm4::PA_SC_CENTROID_PRIORITY_1]             = HwCtxSetCentroidPriority;
	g_hw_ctx_func[Pm4::PA_SC_AA_MASK_X0Y0_X1Y0]               = HwCtxSetAaMask;
	g_hw_ctx_func[Pm4::PA_SC_AA_MASK_X0Y1_X1Y1]               = HwCtxSetAaMask;
	g_hw_ctx_func[Pm4::PA_SU_VTX_CNTL]                        = HwCtxSetVtxControl;
	g_hw_ctx_func[Pm4::PS_SHADER_SAMPLE_EXCLUSION_MASK]       = HwCtxSetAaMask;
	g_hw_ctx_func[Pm4::PA_SC_BINNER_CNTL_0]                   = HwCtxSetPaScExtendedControl;
	g_hw_ctx_func[Pm4::PA_SC_BINNER_CNTL_1]                   = HwCtxSetPaScExtendedControl;
	g_hw_ctx_func[Pm4::PA_SC_CONSERVATIVE_RASTERIZATION_CNTL] = HwCtxSetPaScExtendedControl;
	g_hw_ctx_func[Pm4::PA_SC_NGG_MODE_CNTL]                   = HwCtxSetPaScExtendedControl;
	g_hw_ctx_func[Pm4::DB_ALPHA_TO_MASK]                      = HwCtxSetAlphaToMask;
	g_hw_ctx_func[Pm4::VGT_DRAW_PAYLOAD_CNTL]                 = HwCtxSetDrawPayloadControl;
	g_hw_ctx_func[Pm4::VGT_PRIMITIVEID_RESET]                 = HwCtxSetPrimitiveIdReset;
	g_hw_ctx_func[Pm4::PA_CL_OBJPRIM_ID_CNTL]                 = HwCtxSetObjprimIdControl;
	g_hw_ctx_func[Pm4::VGT_SHADER_STAGES_EN]                  = HwCtxSetShaderStages;
	g_hw_ctx_func[Pm4::PA_CL_GB_VERT_CLIP_ADJ]                = HwCtxSetGuardBands;

	for (uint32_t slot = 0; slot < 8; slot++) {
		g_hw_ctx_func[Pm4::CB_COLOR0_BASE + slot * 15] = HwCtxSetRenderTarget;
		g_hw_ctx_func[Pm4::CB_COLOR0_INFO + slot * 15] = HwCtxSetColorInfo;

		g_hw_ctx_func[Pm4::CB_BLEND0_CONTROL + slot * 1] = HwCtxSetBlendControl;
	}

	for (uint32_t viewport = 0; viewport < 16; viewport++) {
		g_hw_ctx_func[Pm4::PA_SC_VPORT_SCISSOR_0_TL + viewport * 2] = HwCtxSetViewportScissor;
		g_hw_ctx_func[Pm4::PA_SC_VPORT_SCISSOR_0_BR + viewport * 2] = HwCtxSetViewportScissor;
		g_hw_ctx_func[Pm4::PA_SC_VPORT_ZMIN_0 + viewport * 2]       = HwCtxSetViewportZ;
		g_hw_ctx_func[Pm4::PA_SC_VPORT_ZMAX_0 + viewport * 2]       = HwCtxSetViewportZ;
		for (uint32_t field = 0; field < 6; field++) {
			g_hw_ctx_func[Pm4::PA_CL_VPORT_XSCALE + viewport * 6 + field] =
			    HwCtxSetViewportScaleOffset;
		}
	}

	return g_hw_ctx_func;
}

constexpr auto MakeShaderDispatchTable() {
	std::array<hw_sh_parser_func_t, Pm4::SH_NUM> g_hw_sh_func {};

	for (uint32_t slot = 0; slot < 32; slot++) {
		g_hw_sh_func[Pm4::SPI_SHADER_USER_DATA_PS_0 + slot * 1] = HwShSetPsUserSgpr;
		g_hw_sh_func[Pm4::SPI_SHADER_USER_DATA_GS_0 + slot * 1] = HwShSetGsUserSgpr;
		g_hw_sh_func[Pm4::SPI_SHADER_USER_DATA_HS_0 + slot * 1] = HwShSetHsUserSgpr;
	}
	for (uint32_t slot = 0; slot < 16; slot++) {
		g_hw_sh_func[Pm4::COMPUTE_USER_DATA_0 + slot * 1] = HwShSetCsUserSgpr;
	}

	for (uint32_t slot = 0; slot < 4; slot++) {
		g_hw_sh_func[Pm4::SPI_SHADER_USER_ACCUM_PS_0 + slot] = HwShSetPsUserAccumSgpr;
	}

	for (uint32_t slot = 0; slot < 4; slot++) {
		g_hw_sh_func[Pm4::SPI_SHADER_USER_ACCUM_ESGS_0 + slot] = HwShSetGsUserAccumSgpr;
	}
	for (uint32_t slot = 0; slot < 4; slot++) {
		g_hw_sh_func[Pm4::SPI_SHADER_USER_ACCUM_LSHS_0 + slot] = HwShSetHsUserAccumSgpr;
	}

	for (uint32_t slot = 0; Pm4::COMPUTE_USER_ACCUM_0 + slot < Pm4::COMPUTE_USER_DATA_0; slot++) {
		g_hw_sh_func[Pm4::COMPUTE_USER_ACCUM_0 + slot] = HwShSetCsUserAccumSgpr;
	}
	g_hw_sh_func[Pm4::COMPUTE_PGM_LO]                  = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_PGM_HI]                  = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_TBA_LO]                  = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_TBA_HI]                  = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_TMA_LO]                  = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_TMA_HI]                  = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_PGM_RSRC1]               = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_PGM_RSRC2]               = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_START_X]                 = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_START_Y]                 = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_START_Z]                 = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_NUM_THREAD_X]            = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_NUM_THREAD_Y]            = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_NUM_THREAD_Z]            = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_RESOURCE_LIMITS]         = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_DESTINATION_EN_SE0]      = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_DESTINATION_EN_SE1]      = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_TMPRING_SIZE]            = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_DESTINATION_EN_SE2]      = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_DESTINATION_EN_SE3]      = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_PGM_RSRC3]               = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_SHADER_CHKSUM]           = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::COMPUTE_DISPATCH_TUNNEL]         = HwShSetCsRegisters;
	g_hw_sh_func[Pm4::SPI_SHADER_TBA_LO_PS]            = HwShIgnoreRegisters;
	g_hw_sh_func[Pm4::SPI_SHADER_PGM_RSRC3_PS]         = HwShIgnoreRegisters;
	g_hw_sh_func[Pm4::SPI_SHADER_PGM_RSRC4_PS]         = HwShIgnoreRegisters;
	g_hw_sh_func[Pm4::SPI_SHADER_TMA_LO_PS]            = HwShIgnoreRegisters;
	g_hw_sh_func[Pm4::SPI_SHADER_TMA_HI_PS]            = HwShIgnoreRegisters;
	g_hw_sh_func[Pm4::SPI_SHADER_REQ_CTRL_PS]          = HwShIgnoreRegisters;
	g_hw_sh_func[Pm4::SPI_SHADER_PGM_RSRC3_GS]         = HwShIgnoreRegisters;
	g_hw_sh_func[Pm4::SPI_SHADER_PGM_RSRC4_GS]         = HwShIgnoreRegisters;
	g_hw_sh_func[Pm4::SPI_SHADER_USER_DATA_ADDR_LO_GS] = HwShIgnoreRegisters;
	g_hw_sh_func[Pm4::SPI_SHADER_USER_DATA_ADDR_HI_GS] = HwShIgnoreRegisters;
	g_hw_sh_func[Pm4::SPI_SHADER_REQ_CTRL_ESGS]        = HwShIgnoreRegisters;
	g_hw_sh_func[Pm4::SPI_SHADER_PGM_RSRC3_HS]         = HwShIgnoreRegisters;
	g_hw_sh_func[Pm4::SPI_SHADER_PGM_RSRC4_HS]         = HwShIgnoreRegisters;
	g_hw_sh_func[Pm4::SPI_SHADER_USER_DATA_ADDR_LO_HS] = HwShIgnoreRegisters;
	g_hw_sh_func[Pm4::SPI_SHADER_USER_DATA_ADDR_HI_HS] = HwShIgnoreRegisters;
	g_hw_sh_func[Pm4::SPI_SHADER_REQ_CTRL_LSHS]        = HwShIgnoreRegisters;

	return g_hw_sh_func;
}

constexpr auto MakeUconfigDispatchTable() {
	std::array<hw_uc_parser_func_t, Pm4::UC_NUM> g_hw_uc_func {};

	g_hw_uc_func[Pm4::VGT_PRIMITIVE_TYPE]        = HwUcSetPrimitiveType;
	g_hw_uc_func[Pm4::VGT_INDEX_TYPE]            = HwUcSetIndexType;
	g_hw_uc_func[Pm4::VGT_OBJECT_ID]             = HwUcSetObjectId;
	g_hw_uc_func[Pm4::TEXTURE_GRADIENT_FACTORS]  = HwUcSetTextureGradientFactors;
	g_hw_uc_func[Pm4::IA_MULTI_VGT_PARAM]        = HwUcSetIaMultiVgtParam;
	g_hw_uc_func[Pm4::GE_MULTI_PRIM_IB_RESET_EN] = HwUcSetMultiPrimIbReset;
	g_hw_uc_func[Pm4::TA_CS_BC_BASE_ADDR]        = HwUcSetBorderColorTableAddr;
	g_hw_uc_func[Pm4::TA_CS_BC_BASE_ADDR_HI]     = HwUcSetBorderColorTableAddr;
	g_hw_uc_func[Pm4::GE_INDX_OFFSET]            = HwUcSetGeIndexOffset;
	g_hw_uc_func[Pm4::GDS_OA_CNTL]               = HwUcSetGdsOaRegisters;
	g_hw_uc_func[Pm4::GDS_OA_COUNTER]            = HwUcSetGdsOaRegisters;
	g_hw_uc_func[Pm4::GDS_OA_ADDRESS]            = HwUcSetGdsOaRegisters;

	return g_hw_uc_func;
}

constexpr auto MakeOpcodeDispatchTable() {
	std::array<cp_op_parser_func_t, 256> g_cp_op_func {};

	g_cp_op_func[Pm4::IT_NOP]                       = CpOpNop;
	g_cp_op_func[Pm4::IT_SET_BASE]                  = CpOpSetBase;
	g_cp_op_func[Pm4::IT_CLEAR_STATE]               = CpOpClearState;
	g_cp_op_func[Pm4::IT_INDEX_BASE]                = CpOpIndexBase;
	g_cp_op_func[Pm4::IT_INDEX_BUFFER_SIZE]         = CpOpIndexBufferSize;
	g_cp_op_func[Pm4::IT_DISPATCH_DIRECT]           = CpOpDispatchDirect;
	g_cp_op_func[Pm4::IT_DISPATCH_INDIRECT]         = CpOpDispatchIndirect;
	g_cp_op_func[Pm4::IT_DRAW_INDIRECT]             = CpOpDrawIndirect;
	g_cp_op_func[Pm4::IT_DRAW_INDEX_INDIRECT]       = CpOpDrawIndirect;
	g_cp_op_func[Pm4::IT_DRAW_INDIRECT_MULTI]       = CpOpDrawIndirectMulti;
	g_cp_op_func[Pm4::IT_DRAW_INDEX_INDIRECT_MULTI] = CpOpDrawIndirectMulti;
	g_cp_op_func[Pm4::IT_DRAW_INDEX_2]              = CpOpDrawIndex;
	g_cp_op_func[Pm4::IT_DRAW_INDEX_OFFSET_2]       = CpOpDrawIndexOffset;
	g_cp_op_func[Pm4::IT_DISPATCH_DRAW_PREAMBLE]    = CpOpDrawIndex;
	g_cp_op_func[Pm4::IT_PFP_SYNC_ME]               = CpOpPfpSyncMe;
	g_cp_op_func[Pm4::IT_INDEX_TYPE]                = CpOpIndexType;
	g_cp_op_func[Pm4::IT_NUM_INSTANCES]             = CpOpNumInstances;
	g_cp_op_func[Pm4::IT_DRAW_INDEX_AUTO]           = CpOpDrawIndexAuto;
	g_cp_op_func[Pm4::IT_COND_EXEC]                 = CpOpCondExec;
	g_cp_op_func[Pm4::IT_SET_PREDICATION]           = CpOpSetPredication;
	g_cp_op_func[Pm4::IT_WRITE_DATA]                = CpOpWriteData;
	g_cp_op_func[Pm4::IT_INDIRECT_BUFFER]           = CpOpIndirectBuffer;
	g_cp_op_func[Pm4::IT_COPY_DATA]                 = CpOpCopyData;
	g_cp_op_func[Pm4::IT_EVENT_WRITE]               = CpOpEventWrite;
	g_cp_op_func[Pm4::IT_EVENT_WRITE_EOP]           = CpOpEventWriteEop;
	g_cp_op_func[Pm4::IT_EVENT_WRITE_EOS]           = CpOpEventWriteEos;
	g_cp_op_func[Pm4::IT_DMA_DATA]                  = CpOpDmaData;
	g_cp_op_func[Pm4::IT_ACQUIRE_MEM]               = CpOpAcquireMem;
	g_cp_op_func[Pm4::IT_SET_CONTEXT_REG]           = CpOpSetContextReg;
	g_cp_op_func[Pm4::IT_SET_SH_REG]                = CpOpSetShaderReg;
	g_cp_op_func[Pm4::IT_SET_UCONFIG_REG]           = CpOpSetUconfigReg;
	g_cp_op_func[Pm4::IT_SET_UCONFIG_REG_INDEX]     = CpOpSetUconfigReg;
	g_cp_op_func[Pm4::IT_SET_CONTEXT_REG_INDIRECT]  = CpOpIndirectCxRegs;
	g_cp_op_func[Pm4::IT_SET_SH_REG_INDIRECT]       = CpOpIndirectShRegs;
	g_cp_op_func[Pm4::IT_SET_UCONFIG_REG_INDIRECT]  = CpOpIndirectUcRegs;
	g_cp_op_func[Pm4::IT_WRITE_CONST_RAM]           = CpOpWriteConstRam;
	g_cp_op_func[Pm4::IT_DUMP_CONST_RAM]            = CpOpDumpConstRam;
	g_cp_op_func[Pm4::IT_INCREMENT_CE_COUNTER]      = CpOpIncrementCeCounter;
	g_cp_op_func[Pm4::IT_INCREMENT_DE_COUNTER]      = CpOpIncrementDeCounter;
	g_cp_op_func[Pm4::IT_WAIT_ON_CE_COUNTER]        = CpOpWaitOnCeCounter;
	g_cp_op_func[Pm4::IT_WAIT_ON_DE_COUNTER_DIFF]   = CpOpWaitOnDeCounterDiff;
	g_cp_op_func[Pm4::IT_GET_LOD_STATS]             = CpOpGetLodStats;

	return g_cp_op_func;
}

constexpr auto MakeCustomOpcodeDispatchTable() {
	std::array<cp_op_parser_func_t, Pm4::R_NUM> g_cp_op_custom_func {};

	g_cp_op_custom_func[Pm4::R_DISPATCH_RESET] = CpOpDispatchReset;
	g_cp_op_custom_func[Pm4::R_WAIT_MEM_32]    = CpOpWaitRegMem32;
	g_cp_op_custom_func[Pm4::R_WAIT_FLIP_DONE] = CpOpWaitFlipDone;
	g_cp_op_custom_func[Pm4::R_PUSH_MARKER]    = CpOpPushMarker;
	g_cp_op_custom_func[Pm4::R_POP_MARKER]     = CpOpPopMarker;
	g_cp_op_custom_func[Pm4::R_ACQUIRE_MEM]    = CpOpAcquireMem;
	g_cp_op_custom_func[Pm4::R_WAIT_MEM_64]    = CpOpWaitRegMem64;
	g_cp_op_custom_func[Pm4::R_FLIP]           = CpOpFlip;
	g_cp_op_custom_func[Pm4::R_RELEASE_MEM]    = CpOpReleaseMem;

	return g_cp_op_custom_func;
}

} // namespace

constinit const std::array<hw_ctx_parser_func_t, Pm4::CX_NUM> g_hw_ctx_func =
    MakeContextDispatchTable();
constinit const std::array<hw_sh_parser_func_t, Pm4::SH_NUM> g_hw_sh_func =
    MakeShaderDispatchTable();
constinit const std::array<hw_uc_parser_func_t, Pm4::UC_NUM> g_hw_uc_func =
    MakeUconfigDispatchTable();
constinit const std::array<cp_op_parser_func_t, 256> g_cp_op_func = MakeOpcodeDispatchTable();
constinit const std::array<cp_op_parser_func_t, Pm4::R_NUM> g_cp_op_custom_func =
    MakeCustomOpcodeDispatchTable();

void GraphicsInitJmpTables() {
	GraphicsInitJmpTablesCxIndirect();
	GraphicsInitJmpTablesShIndirect();
	GraphicsInitJmpTablesUcIndirect();
}

} // namespace Libs::Graphics
