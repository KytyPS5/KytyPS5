#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/objects/label.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"
#include "libs/agc.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <vector>

#define KYTY_HW_CTX_PARSER_ARGS                                                                    \
	[[maybe_unused]] CommandProcessor *cp, uint32_t cmd_id, [[maybe_unused]] uint32_t cmd_offset,  \
	    const uint32_t *buffer, [[maybe_unused]] uint32_t dw
#define KYTY_HW_UC_PARSER_ARGS                                                                     \
	[[maybe_unused]] CommandProcessor *cp, uint32_t cmd_id, [[maybe_unused]] uint32_t cmd_offset,  \
	    const uint32_t *buffer, [[maybe_unused]] uint32_t dw
#define KYTY_HW_SH_PARSER_ARGS                                                                     \
	[[maybe_unused]] CommandProcessor *cp, uint32_t cmd_id, [[maybe_unused]] uint32_t cmd_offset,  \
	    const uint32_t *buffer, [[maybe_unused]] uint32_t dw

#define KYTY_HW_CTX_PARSER(f) uint32_t f(KYTY_HW_CTX_PARSER_ARGS)
#define KYTY_HW_UC_PARSER(f)  uint32_t f(KYTY_HW_UC_PARSER_ARGS)
#define KYTY_HW_SH_PARSER(f)  uint32_t f(KYTY_HW_SH_PARSER_ARGS)

#define KYTY_HW_CTX_INDIRECT_ARGS                                                                  \
	CommandProcessor *cp, [[maybe_unused]] uint32_t cmd_offset, uint32_t value
#define KYTY_HW_UC_INDIRECT_ARGS                                                                   \
	CommandProcessor *cp, [[maybe_unused]] uint32_t cmd_offset, uint32_t value
#define KYTY_HW_SH_INDIRECT_ARGS                                                                   \
	CommandProcessor *cp, [[maybe_unused]] uint32_t cmd_offset, uint32_t value

#define KYTY_CP_OP_PARSER_ARGS                                                                     \
	[[maybe_unused]] CommandProcessor *cp, [[maybe_unused]] uint32_t cmd_id,                       \
	    [[maybe_unused]] const uint32_t *buffer, [[maybe_unused]] uint32_t dw,                     \
	    [[maybe_unused]] uint32_t num_dw
#define KYTY_CP_OP_PARSER(f) uint32_t f(KYTY_CP_OP_PARSER_ARGS)

namespace Libs::Graphics {

namespace {

constexpr uint32_t GcrGl2MetadataInvalidate = 1u << 1u;
constexpr uint32_t GcrGl0VectorInvalidate   = 1u << 2u;
constexpr uint32_t GcrGl1Invalidate         = 1u << 3u;
constexpr uint32_t GcrGl2Unshared           = 1u << 4u;
constexpr uint32_t GcrGl2Invalidate         = 1u << 8u;
constexpr uint32_t GcrGl2Writeback          = 1u << 9u;
constexpr uint32_t GcrOrder012              = 1u << 10u;
constexpr uint32_t GcrOrder210              = 2u << 10u;
constexpr uint32_t GcrKnownMask             = GcrGl2MetadataInvalidate | GcrGl0VectorInvalidate |
                                              GcrGl1Invalidate | GcrGl2Unshared | GcrGl2Invalidate |
                                              GcrGl2Writeback | GcrOrder012 | GcrOrder210;
constexpr uint32_t RegisterSelectorMask     = 0x70000000u;

uint32_t NormalizeRegisterOffset(uint32_t raw_offset) {
	return (raw_offset & ~RegisterSelectorMask);
}

bool ReleaseMemGcrNeedsBarrier(uint32_t eop_event_type, uint32_t gcr_cntl) {
	return eop_event_type != 0x28u ||
	       (gcr_cntl & (GcrGl2MetadataInvalidate | GcrGl0VectorInvalidate | GcrGl1Invalidate |
	                    GcrGl2Invalidate | GcrGl2Writeback)) != 0;
}

uint32_t ReleaseMemCacheActionFromGcr(uint32_t gcr_cntl) {
	return ((gcr_cntl & GcrGl2Writeback) != 0 ? 0x38u : 0x00u);
}

void LogUnknownReleaseMemGcr(uint32_t gcr_cntl) {
	const uint32_t unknown = (gcr_cntl & ~GcrKnownMask);
	if (unknown != 0) {
		LOGF("\t warning: release_mem uses unknown GCR bits: 0x%04" PRIx32 "\n", unknown);
	}
}

} // namespace

static HW::AaConfig ParseAaConfig(uint32_t value) {
	HW::AaConfig r;

	r.msaa_num_samples      = KYTY_PM4_GET(value, PA_SC_AA_CONFIG, MSAA_NUM_SAMPLES);
	r.aa_mask_centroid_dtmn = KYTY_PM4_GET(value, PA_SC_AA_CONFIG, AA_MASK_CENTROID_DTMN) != 0;
	r.max_sample_dist       = KYTY_PM4_GET(value, PA_SC_AA_CONFIG, MAX_SAMPLE_DIST);
	r.msaa_exposed_samples  = KYTY_PM4_GET(value, PA_SC_AA_CONFIG, MSAA_EXPOSED_SAMPLES);

	return r;
}

KYTY_HW_CTX_PARSER(HwCtxSetAaConfig) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::PA_SC_AA_CONFIG);

	cp->GetCtx()->SetAaConfig(ParseAaConfig(buffer[0]));

	return 1;
}

KYTY_HW_CTX_PARSER(HwCtxSetAaSampleControl) {
	if (cmd_id == 0xc0016900 && cmd_offset >= Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0 &&
	    cmd_offset < Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0 + 16) {
		auto r = cp->GetCtx()->GetAaSampleControl();
		r.locations[cmd_offset - Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0] = buffer[0];
		cp->GetCtx()->SetAaSampleControl(r);
		return 1;
	}

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0106900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0);

	uint32_t count = 1;

	if (dw >= 20 && buffer[16] == 0xc0026900 && buffer[17] == Pm4::PA_SC_CENTROID_PRIORITY_0) {
		count = 20;

		HW::AaSampleControl r;

		memcpy(r.locations, buffer, static_cast<size_t>(16) * 4);

		r.centroid_priority =
		    static_cast<uint64_t>(buffer[18]) | (static_cast<uint64_t>(buffer[19]) << 32u);

		cp->GetCtx()->SetAaSampleControl(r);
	} else {
		KYTY_NOT_IMPLEMENTED;
	}

	return count;
}

KYTY_HW_CTX_PARSER(HwCtxSetCentroidPriority) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;
	auto r          = cp->GetCtx()->GetAaSampleControl();

	for (uint32_t i = 0; i < num_values; i++) {
		auto offset = cmd_offset + i;
		switch (offset) {
			case Pm4::PA_SC_CENTROID_PRIORITY_0:
				r.centroid_priority = (r.centroid_priority & 0xffffffff00000000ull) | buffer[i];
				break;
			case Pm4::PA_SC_CENTROID_PRIORITY_1:
				r.centroid_priority = (r.centroid_priority & 0x00000000ffffffffull) |
				                      (static_cast<uint64_t>(buffer[i]) << 32u);
				break;
			default: break;
		}
	}

	cp->GetCtx()->SetAaSampleControl(r);

	return num_values;
}

static void HwCtxIgnoreAaMaskRegister([[maybe_unused]] uint32_t cmd_offset,
                                      [[maybe_unused]] uint32_t value) {}

static void HwCtxIgnoreAlphaToMaskRegister([[maybe_unused]] uint32_t value) {}

static void HwCtxIgnoreDrawPayloadControl([[maybe_unused]] uint32_t value) {}

static void HwCtxIgnoreObjprimIdControl([[maybe_unused]] uint32_t value) {}

static void HwCtxIgnorePrimitiveIdReset([[maybe_unused]] uint32_t value) {}

static void HwCtxIgnoreVtxControl([[maybe_unused]] uint32_t value) {}

static void HwCtxSetDepthBoundsRegister(HW::Context* ctx, uint32_t cmd_offset, uint32_t value) {
	EXIT_IF(ctx == nullptr);

	auto fvalue = *reinterpret_cast<const float*>(&value);

	switch (cmd_offset) {
		case Pm4::DB_DEPTH_BOUNDS_MIN: ctx->SetDepthBoundsMin(fvalue); break;
		case Pm4::DB_DEPTH_BOUNDS_MAX: ctx->SetDepthBoundsMax(fvalue); break;
		default: EXIT("unknown depth bounds register: 0x%08" PRIx32 "\n", cmd_offset);
	}
}

static void HwCtxIgnoreDepthMetadataRegister([[maybe_unused]] uint32_t cmd_offset,
                                             [[maybe_unused]] uint32_t value) {}

static void HwCtxIgnoreFovWindow([[maybe_unused]] uint32_t value) {}

static void HwCtxIgnoreFsrRegister([[maybe_unused]] uint32_t cmd_offset,
                                   [[maybe_unused]] uint32_t value) {}

static void HwCtxIgnoreScanModeControl1([[maybe_unused]] uint32_t value) {}

static void HwCtxIgnorePaScExtendedControl([[maybe_unused]] uint32_t cmd_offset,
                                           [[maybe_unused]] uint32_t value) {}

static void HwCtxIgnoreCbDccControl([[maybe_unused]] uint32_t value) {}

static void HwCtxIgnorePointState([[maybe_unused]] uint32_t cmd_offset,
                                  [[maybe_unused]] uint32_t value) {}

static void HwCtxIgnoreBorderColorTableAddr([[maybe_unused]] uint32_t cmd_offset,
                                            [[maybe_unused]] uint32_t value) {}

static void HwCtxIgnoreSpiTmpringSize(uint32_t value) {
	static std::atomic<uint32_t> log_count {0};
	if (log_count.fetch_add(1, std::memory_order_relaxed) < 4) {
		LOGF("\t temporary: accepting SPI_TMPRING_SIZE = 0x%08" PRIx32 "\n", value);
	}
}

KYTY_HW_CTX_PARSER(HwCtxSetSpiTmpringSize) {
	auto reg_num = (cmd_id >> 16u) & 0x3fffu;

	EXIT_NOT_IMPLEMENTED(reg_num != 1);

	HwCtxIgnoreSpiTmpringSize(buffer[0]);

	return 1;
}

static bool HwCtxIsFakeRegister(uint32_t cmd_offset) {
	switch (cmd_offset) {
		case 0x18:
		case 0x19:
		case Pm4::CX_NOP: return true;
		default: return false;
	}
}

static void HwCtxIgnoreFakeRegister([[maybe_unused]] uint32_t cmd_offset,
                                    [[maybe_unused]] uint32_t value) {}

static bool HwCtxTrySetFakeRegister(uint32_t cmd_offset, uint32_t value) {
	if (!HwCtxIsFakeRegister(cmd_offset)) {
		return false;
	}

	if (cmd_offset != Pm4::CX_NOP) {
		HwCtxIgnoreFakeRegister(cmd_offset, value);
	}

	return true;
}

KYTY_HW_CTX_PARSER(HwCtxSetDepthBounds) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwCtxSetDepthBoundsRegister(cp->GetCtx(), cmd_offset + i, buffer[i]);
	}

	return num_values;
}

KYTY_HW_CTX_PARSER(HwCtxSetDepthMetadataRegisters) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwCtxIgnoreDepthMetadataRegister(cmd_offset + i, buffer[i]);
	}

	return num_values;
}

KYTY_HW_CTX_PARSER(HwCtxSetBorderColorTableAddr) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwCtxIgnoreBorderColorTableAddr(cmd_offset + i, buffer[i]);
	}

	return num_values;
}

KYTY_HW_CTX_PARSER(HwCtxSetPaScExtendedControl) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwCtxIgnorePaScExtendedControl(cmd_offset + i, buffer[i]);
	}

	return num_values;
}

KYTY_HW_CTX_PARSER(HwCtxSetCbDccControl) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwCtxIgnoreCbDccControl(buffer[i]);
	}

	return num_values;
}

KYTY_HW_CTX_PARSER(HwCtxSetPointState) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwCtxIgnorePointState(cmd_offset + i, buffer[i]);
	}

	return num_values;
}

KYTY_HW_CTX_PARSER(HwCtxSetAlphaToMask) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwCtxIgnoreAlphaToMaskRegister(buffer[i]);
	}

	return num_values;
}

KYTY_HW_CTX_PARSER(HwCtxSetDrawPayloadControl) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwCtxIgnoreDrawPayloadControl(buffer[i]);
	}

	return num_values;
}

KYTY_HW_CTX_PARSER(HwCtxSetObjprimIdControl) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwCtxIgnoreObjprimIdControl(buffer[i]);
	}

	return num_values;
}

KYTY_HW_CTX_PARSER(HwCtxSetPrimitiveIdReset) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwCtxIgnorePrimitiveIdReset(buffer[i]);
	}

	return num_values;
}

KYTY_HW_CTX_PARSER(HwCtxSetVtxControl) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwCtxIgnoreVtxControl(buffer[i]);
	}

	return num_values;
}

KYTY_HW_CTX_PARSER(HwCtxSetFovWindow) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwCtxIgnoreFovWindow(buffer[i]);
	}

	return num_values;
}

KYTY_HW_CTX_PARSER(HwCtxSetScanModeControl1) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwCtxIgnoreScanModeControl1(buffer[i]);
	}

	return num_values;
}

KYTY_HW_CTX_PARSER(HwCtxSetAaMask) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwCtxIgnoreAaMaskRegister(cmd_offset + i, buffer[i]);
	}

	return num_values;
}

KYTY_HW_CTX_PARSER(HwCtxSetBlendColor) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0046900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::CB_BLEND_RED);

	HW::BlendColor r;

	r.red   = *reinterpret_cast<const float*>(&buffer[0]);
	r.green = *reinterpret_cast<const float*>(&buffer[1]);
	r.blue  = *reinterpret_cast<const float*>(&buffer[2]);
	r.alpha = *reinterpret_cast<const float*>(&buffer[3]);

	cp->GetCtx()->SetBlendColor(r);

	return 4;
}

KYTY_HW_CTX_PARSER(HwCtxSetBlendControl) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);

	uint32_t param = (cmd_offset - Pm4::CB_BLEND0_CONTROL) / 1;

	HW::BlendControl r;

	r.color_srcblend       = (buffer[0] >> Pm4::CB_BLEND0_CONTROL_COLOR_SRCBLEND_SHIFT) &
	                         Pm4::CB_BLEND0_CONTROL_COLOR_SRCBLEND_MASK;
	r.color_comb_fcn       = (buffer[0] >> Pm4::CB_BLEND0_CONTROL_COLOR_COMB_FCN_SHIFT) &
	                         Pm4::CB_BLEND0_CONTROL_COLOR_COMB_FCN_MASK;
	r.color_destblend      = (buffer[0] >> Pm4::CB_BLEND0_CONTROL_COLOR_DESTBLEND_SHIFT) &
	                         Pm4::CB_BLEND0_CONTROL_COLOR_DESTBLEND_MASK;
	r.alpha_srcblend       = (buffer[0] >> Pm4::CB_BLEND0_CONTROL_ALPHA_SRCBLEND_SHIFT) &
	                         Pm4::CB_BLEND0_CONTROL_ALPHA_SRCBLEND_MASK;
	r.alpha_comb_fcn       = (buffer[0] >> Pm4::CB_BLEND0_CONTROL_ALPHA_COMB_FCN_SHIFT) &
	                         Pm4::CB_BLEND0_CONTROL_ALPHA_COMB_FCN_MASK;
	r.alpha_destblend      = (buffer[0] >> Pm4::CB_BLEND0_CONTROL_ALPHA_DESTBLEND_SHIFT) &
	                         Pm4::CB_BLEND0_CONTROL_ALPHA_DESTBLEND_MASK;
	r.separate_alpha_blend = ((buffer[0] >> Pm4::CB_BLEND0_CONTROL_SEPARATE_ALPHA_BLEND_SHIFT) &
	                          Pm4::CB_BLEND0_CONTROL_SEPARATE_ALPHA_BLEND_MASK) != 0;
	r.enable               = ((buffer[0] >> Pm4::CB_BLEND0_CONTROL_ENABLE_SHIFT) &
	                          Pm4::CB_BLEND0_CONTROL_ENABLE_MASK) != 0;

	cp->GetCtx()->SetBlendControl(param, r);

	return 1;
}

KYTY_HW_CTX_PARSER(HwCtxSetClipControl) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::PA_CL_CLIP_CNTL);

	HW::ClipControl r;

	//	r.user_clip_planes                    = buffer[0] & 0x3fu;
	//	r.user_clip_plane_mode                = (buffer[0] >> 14u) & 0x3u;
	//	r.clip_space                          = (buffer[0] >> 19u) & 0x1u;
	//	r.vertex_kill_mode                    = (buffer[0] >> 21u) & 0x1u;
	//	r.min_z_clip_enable                   = (buffer[0] >> 26u) & 0x1u;
	//	r.max_z_clip_enable                   = (buffer[0] >> 27u) & 0x1u;
	//	r.user_clip_plane_negate_y            = (buffer[0] & 0x00002000u) != 0;
	//	r.clip_enable                         = (buffer[0] & 0x00010000u) != 0;
	//	r.user_clip_plane_cull_only           = (buffer[0] & 0x00020000u) != 0;
	//	r.cull_on_clipping_error_disable      = (buffer[0] & 0x00100000u) != 0;
	//	r.linear_attribute_clip_enable        = (buffer[0] & 0x01000000u) != 0;
	//	r.force_viewport_index_from_vs_enable = (buffer[0] & 0x02000000u) != 0;

	r.user_clip_planes          = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, UCP_ENA);
	r.user_clip_plane_mode      = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, PS_UCP_MODE);
	r.dx_clip_space             = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, DX_CLIP_SPACE_DEF) != 0;
	r.vertex_kill_any           = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, VTX_KILL_OR) != 0;
	r.min_z_clip_disable        = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, ZCLIP_NEAR_DISABLE) != 0;
	r.max_z_clip_disable        = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, ZCLIP_FAR_DISABLE) != 0;
	r.user_clip_plane_negate_y  = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, PS_UCP_Y_SCALE_NEG) != 0;
	r.clip_disable              = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, CLIP_DISABLE) != 0;
	r.user_clip_plane_cull_only = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, UCP_CULL_ONLY_ENA) != 0;
	r.cull_on_clipping_error_disable =
	    KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, DIS_CLIP_ERR_DETECT) != 0;
	r.linear_attribute_clip_enable =
	    KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, DX_LINEAR_ATTR_CLIP_ENA) != 0;
	r.force_viewport_index_from_vs_enable =
	    KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, VTE_VPORT_PROVOKE_DISABLE) != 0;

	cp->GetCtx()->SetClipControl(r);

	return 1;
}

KYTY_HW_CTX_PARSER(HwCtxSetColorControl) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::CB_COLOR_CONTROL);

	HW::ColorControl r;

	r.mode = KYTY_PM4_GET(buffer[0], CB_COLOR_CONTROL, MODE);
	r.op   = KYTY_PM4_GET(buffer[0], CB_COLOR_CONTROL, ROP3);

	cp->GetCtx()->SetColorControl(r);

	return 1;
}

KYTY_HW_CTX_PARSER(HwCtxSetColorInfo) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0016900);

	uint32_t param = (cmd_offset - Pm4::CB_COLOR0_INFO) / 15;

	HW::ColorInfo r;

	//	r.fmask_compression_enable = (buffer[4] & 0x4000u) != 0;
	//	// r.fmask_compression_mode   = (buffer[4] >> 26u) & 0x3u;
	//	r.fmask_data_compression_disable = ((buffer[4] >> 26u) & 0x1u) != 0;
	//	r.fmask_one_frag_mode            = ((buffer[4] >> 27u) & 0x1u) != 0;
	//	r.cmask_fast_clear_enable        = (buffer[4] & 0x2000u) != 0;
	//	r.dcc_compression_enable         = (buffer[4] & 0x10000000u) != 0;
	//	r.cmask_is_linear                = (buffer[4] >> 19u) & 0x1u;
	//	r.cmask_addr_type                = (buffer[4] >> 29u) & 0x3u;
	//	r.alt_tile_mode                  = ((buffer[4] >> 31u) & 0x1u) != 0;
	//	r.format                         = (buffer[4] >> 2u) & 0x1fu;
	//	r.channel_type                   = (buffer[4] >> 8u) & 0x7u;
	//	r.channel_order                  = (buffer[4] >> 11u) & 0x3u;
	r.format                   = KYTY_PM4_GET(buffer[0], CB_COLOR0_INFO, FORMAT);
	r.channel_type             = KYTY_PM4_GET(buffer[0], CB_COLOR0_INFO, NUMBER_TYPE);
	r.channel_order            = KYTY_PM4_GET(buffer[0], CB_COLOR0_INFO, COMP_SWAP);
	r.cmask_fast_clear_enable  = KYTY_PM4_GET(buffer[0], CB_COLOR0_INFO, FAST_CLEAR) != 0;
	r.fmask_compression_enable = KYTY_PM4_GET(buffer[0], CB_COLOR0_INFO, COMPRESSION) != 0;
	r.blend_clamp              = KYTY_PM4_GET(buffer[0], CB_COLOR0_INFO, BLEND_CLAMP) != 0;
	r.blend_bypass             = KYTY_PM4_GET(buffer[0], CB_COLOR0_INFO, BLEND_BYPASS) != 0;
	r.round_mode               = KYTY_PM4_GET(buffer[0], CB_COLOR0_INFO, ROUND_MODE) != 0;
	r.cmask_is_linear          = KYTY_PM4_GET(buffer[0], CB_COLOR0_INFO, CMASK_IS_LINEAR);
	r.fmask_data_compression_disable =
	    KYTY_PM4_GET(buffer[0], CB_COLOR0_INFO, FMASK_COMPRESSION_DISABLE) != 0;
	r.fmask_one_frag_mode = KYTY_PM4_GET(buffer[0], CB_COLOR0_INFO, FMASK_COMPRESS_1FRAG_ONLY) != 0;
	r.dcc_compression_enable = KYTY_PM4_GET(buffer[0], CB_COLOR0_INFO, DCC_ENABLE) != 0;
	r.cmask_addr_type        = KYTY_PM4_GET(buffer[0], CB_COLOR0_INFO, CMASK_ADDR_TYPE);
	r.alt_tile_mode          = KYTY_PM4_GET(buffer[0], CB_COLOR0_INFO, ALT_TILE_MODE) != 0;

	cp->GetCtx()->SetColorInfo(param, r);

	return 1;
}

KYTY_HW_CTX_PARSER(HwCtxSetDepthClear) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::DB_DEPTH_CLEAR);

	cp->GetCtx()->SetDepthClearValue(*reinterpret_cast<const float*>(buffer));

	return 1;
}

KYTY_HW_CTX_PARSER(HwCtxSetDepthControl) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::DB_DEPTH_CONTROL);

	HW::DepthControl r;

	r.stencil_enable      = KYTY_PM4_GET(buffer[0], DB_DEPTH_CONTROL, STENCIL_ENABLE) != 0;
	r.z_enable            = KYTY_PM4_GET(buffer[0], DB_DEPTH_CONTROL, Z_ENABLE) != 0;
	r.z_write_enable      = KYTY_PM4_GET(buffer[0], DB_DEPTH_CONTROL, Z_WRITE_ENABLE) != 0;
	r.depth_bounds_enable = KYTY_PM4_GET(buffer[0], DB_DEPTH_CONTROL, DEPTH_BOUNDS_ENABLE) != 0;
	r.zfunc               = KYTY_PM4_GET(buffer[0], DB_DEPTH_CONTROL, ZFUNC);
	r.backface_enable     = KYTY_PM4_GET(buffer[0], DB_DEPTH_CONTROL, BACKFACE_ENABLE) != 0;
	r.stencilfunc         = KYTY_PM4_GET(buffer[0], DB_DEPTH_CONTROL, STENCILFUNC);
	r.stencilfunc_bf      = KYTY_PM4_GET(buffer[0], DB_DEPTH_CONTROL, STENCILFUNC_BF);
	r.color_writes_on_depth_fail_enable =
	    KYTY_PM4_GET(buffer[0], DB_DEPTH_CONTROL, ENABLE_COLOR_WRITES_ON_DEPTH_FAIL) != 0;
	r.color_writes_on_depth_pass_disable =
	    KYTY_PM4_GET(buffer[0], DB_DEPTH_CONTROL, DISABLE_COLOR_WRITES_ON_DEPTH_PASS) != 0;

	cp->GetCtx()->SetDepthControl(r);

	return 1;
}

static HW::DepthRenderTargetHTileSurface ParseDepthHtileSurface(uint32_t value) {
	HW::DepthRenderTargetHTileSurface r;
	r.linear                  = KYTY_PM4_GET(value, DB_HTILE_SURFACE, LINEAR);
	r.full_cache              = KYTY_PM4_GET(value, DB_HTILE_SURFACE, FULL_CACHE);
	r.htile_uses_preload_win  = KYTY_PM4_GET(value, DB_HTILE_SURFACE, HTILE_USES_PRELOAD_WIN);
	r.preload                 = KYTY_PM4_GET(value, DB_HTILE_SURFACE, PRELOAD);
	r.prefetch_width          = KYTY_PM4_GET(value, DB_HTILE_SURFACE, PREFETCH_WIDTH);
	r.prefetch_height         = KYTY_PM4_GET(value, DB_HTILE_SURFACE, PREFETCH_HEIGHT);
	r.dst_outside_zero_to_one = KYTY_PM4_GET(value, DB_HTILE_SURFACE, DST_OUTSIDE_ZERO_TO_ONE);
	return r;
}

static void HwCtxSetDepthHtileSurfaceRegister(HW::Context* ctx, uint32_t value) {
	EXIT_IF(ctx == nullptr);

	auto target          = ctx->GetDepthRenderTarget();
	target.htile_surface = ParseDepthHtileSurface(value);
	ctx->SetDepthRenderTarget(target);
}

KYTY_HW_CTX_PARSER(HwCtxSetDepthHtileSurface) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	EXIT_NOT_IMPLEMENTED(num_values != 1);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::DB_HTILE_SURFACE);

	HwCtxSetDepthHtileSurfaceRegister(cp->GetCtx(), buffer[0]);

	return num_values;
}

KYTY_HW_CTX_PARSER(HwCtxSetDepthRenderTarget) {
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::DB_Z_INFO);

	uint32_t count = 1;

	if (cmd_id == 0xC0016900) {
		HW::DepthZInfo r;

		//	r.expclear_enabled = (buffer[0] & 0x08000000u) != 0;
		//	r.format              = (buffer[0] >> Pm4::DB_Z_INFO_FORMAT_SHIFT) &
		// Pm4::DB_Z_INFO_FORMAT_MASK; 	r.num_samples         = (buffer[0] >>
		// Pm4::DB_Z_INFO_NUM_SAMPLES_SHIFT) & Pm4::DB_Z_INFO_NUM_SAMPLES_MASK; r.tile_mode_index =
		//(buffer[0] >> Pm4::DB_Z_INFO_TILE_MODE_INDEX_SHIFT) & Pm4::DB_Z_INFO_TILE_MODE_INDEX_MASK;
		//	r.tile_surface_enable = ((buffer[0] >> Pm4::DB_Z_INFO_TILE_SURFACE_ENABLE_SHIFT) &
		// Pm4::DB_Z_INFO_TILE_SURFACE_ENABLE_MASK) !=0
		// 	r.zrange_precision    = (buffer[0] >> Pm4::DB_Z_INFO_ZRANGE_PRECISION_SHIFT) &
		// Pm4::DB_Z_INFO_ZRANGE_PRECISION_MASK;

		r.format                    = KYTY_PM4_GET(buffer[0], DB_Z_INFO, FORMAT);
		r.num_samples               = KYTY_PM4_GET(buffer[0], DB_Z_INFO, NUM_SAMPLES);
		r.embedded_sample_locations = KYTY_PM4_GET(buffer[0], DB_Z_INFO, ITERATE_FLUSH) != 0;
		r.partially_resident        = KYTY_PM4_GET(buffer[0], DB_Z_INFO, PARTIALLY_RESIDENT) != 0;
		r.num_mip_levels            = KYTY_PM4_GET(buffer[0], DB_Z_INFO, MAXMIP);
		r.tile_mode_index           = KYTY_PM4_GET(buffer[0], DB_Z_INFO, TILE_MODE_INDEX);
		r.plane_compression         = KYTY_PM4_GET(buffer[0], DB_Z_INFO, DECOMPRESS_ON_N_ZPLANES);
		r.expclear_enabled          = KYTY_PM4_GET(buffer[0], DB_Z_INFO, ALLOW_EXPCLEAR) != 0;
		r.tile_surface_enable       = KYTY_PM4_GET(buffer[0], DB_Z_INFO, TILE_SURFACE_ENABLE) != 0;
		r.zrange_precision          = KYTY_PM4_GET(buffer[0], DB_Z_INFO, ZRANGE_PRECISION);

		cp->GetCtx()->SetDepthZInfo(r);
	} else if (cmd_id == 0xC0086900) {
		if (dw >= 22 && buffer[8] == 0xC0016900 && buffer[9] == Pm4::DB_DEPTH_INFO &&
		    buffer[11] == 0xC0016900 && buffer[12] == Pm4::DB_DEPTH_VIEW &&
		    buffer[14] == 0xC0016900 && buffer[15] == Pm4::DB_HTILE_DATA_BASE &&
		    buffer[17] == 0xC0016900 && buffer[18] == Pm4::DB_HTILE_SURFACE &&
		    buffer[20] == 0xC0001000) {
			count = 22;

			HW::DepthRenderTarget z;

			//			z.z_info.expclear_enabled    = (buffer[0] & 0x08000000u) != 0;
			//			z.z_info.format              = (buffer[0] >> Pm4::DB_Z_INFO_FORMAT_SHIFT) &
			// Pm4::DB_Z_INFO_FORMAT_MASK; 			z.z_info.num_samples         = (buffer[0] >>
			// Pm4::DB_Z_INFO_NUM_SAMPLES_SHIFT) & Pm4::DB_Z_INFO_NUM_SAMPLES_MASK;
			//			z.z_info.tile_mode_index     = (buffer[0] >>
			// Pm4::DB_Z_INFO_TILE_MODE_INDEX_SHIFT) &
			// Pm4::DB_Z_INFO_TILE_MODE_INDEX_MASK; 			z.z_info.tile_surface_enable =
			// KYTY_PM4_GET(buffer[0], DB_Z_INFO, TILE_SURFACE_ENABLE) != 0;
			// z.z_info.zrange_precision    = (buffer[0] >> Pm4::DB_Z_INFO_ZRANGE_PRECISION_SHIFT) &
			// Pm4::DB_Z_INFO_ZRANGE_PRECISION_MASK;

			z.z_info.format      = KYTY_PM4_GET(buffer[0], DB_Z_INFO, FORMAT);
			z.z_info.num_samples = KYTY_PM4_GET(buffer[0], DB_Z_INFO, NUM_SAMPLES);
			z.z_info.embedded_sample_locations =
			    KYTY_PM4_GET(buffer[0], DB_Z_INFO, ITERATE_FLUSH) != 0;
			z.z_info.partially_resident =
			    KYTY_PM4_GET(buffer[0], DB_Z_INFO, PARTIALLY_RESIDENT) != 0;
			z.z_info.num_mip_levels  = KYTY_PM4_GET(buffer[0], DB_Z_INFO, MAXMIP);
			z.z_info.tile_mode_index = KYTY_PM4_GET(buffer[0], DB_Z_INFO, TILE_MODE_INDEX);
			z.z_info.plane_compression =
			    KYTY_PM4_GET(buffer[0], DB_Z_INFO, DECOMPRESS_ON_N_ZPLANES);
			z.z_info.expclear_enabled = KYTY_PM4_GET(buffer[0], DB_Z_INFO, ALLOW_EXPCLEAR) != 0;
			z.z_info.tile_surface_enable =
			    KYTY_PM4_GET(buffer[0], DB_Z_INFO, TILE_SURFACE_ENABLE) != 0;
			z.z_info.zrange_precision = KYTY_PM4_GET(buffer[0], DB_Z_INFO, ZRANGE_PRECISION);

			//			z.stencil_info.expclear_enabled     = (buffer[1] & 0x08000000u) != 0;
			//			z.stencil_info.tile_split           = (buffer[1] >> 13u) & 0x7u;
			//			z.stencil_info.format               = KYTY_PM4_GET(buffer[1],
			// DB_STENCIL_INFO, FORMAT); 			z.stencil_info.tile_mode_index      =
			// KYTY_PM4_GET(buffer[1], DB_STENCIL_INFO, TILE_MODE_INDEX);
			//			z.stencil_info.tile_stencil_disable = KYTY_PM4_GET(buffer[1],
			// DB_STENCIL_INFO, TILE_STENCIL_DISABLE);
			z.stencil_info.format = KYTY_PM4_GET(buffer[1], DB_STENCIL_INFO, FORMAT);
			z.stencil_info.texture_compatible_stencil =
			    KYTY_PM4_GET(buffer[1], DB_STENCIL_INFO, ITERATE_FLUSH) != 0;
			z.stencil_info.partially_resident =
			    KYTY_PM4_GET(buffer[1], DB_STENCIL_INFO, PARTIALLY_RESIDENT) != 0;
			z.stencil_info.tile_split = KYTY_PM4_GET(buffer[1], DB_STENCIL_INFO, RESERVED_FIELD_1);
			z.stencil_info.tile_mode_index =
			    KYTY_PM4_GET(buffer[1], DB_STENCIL_INFO, TILE_MODE_INDEX);
			z.stencil_info.expclear_enabled =
			    KYTY_PM4_GET(buffer[1], DB_STENCIL_INFO, ALLOW_EXPCLEAR) != 0;
			z.stencil_info.tile_stencil_disable =
			    KYTY_PM4_GET(buffer[1], DB_STENCIL_INFO, TILE_STENCIL_DISABLE) != 0;

			z.z_read_base_addr        = static_cast<uint64_t>(buffer[2]) << 8u;
			z.stencil_read_base_addr  = static_cast<uint64_t>(buffer[3]) << 8u;
			z.z_write_base_addr       = static_cast<uint64_t>(buffer[4]) << 8u;
			z.stencil_write_base_addr = static_cast<uint64_t>(buffer[5]) << 8u;

			// DB_DEPTH_SIZE
			z.pitch_div8_minus1  = (buffer[6] >> Pm4::DB_DEPTH_SIZE_PITCH_TILE_MAX_SHIFT) &
			                       Pm4::DB_DEPTH_SIZE_PITCH_TILE_MAX_MASK;
			z.height_div8_minus1 = (buffer[6] >> Pm4::DB_DEPTH_SIZE_HEIGHT_TILE_MAX_SHIFT) &
			                       Pm4::DB_DEPTH_SIZE_HEIGHT_TILE_MAX_MASK;
			z.pitch_height_valid = true;

			// DB_DEPTH_SLICE
			z.slice_div64_minus1 = (buffer[7] >> Pm4::DB_DEPTH_SLICE_SLICE_TILE_MAX_SHIFT) &
			                       Pm4::DB_DEPTH_SLICE_SLICE_TILE_MAX_MASK;

			z.depth_info.addr5_swizzle_mask =
			    KYTY_PM4_GET(buffer[10], DB_DEPTH_INFO, ADDR5_SWIZZLE_MASK);
			z.depth_info.array_mode  = (buffer[10] >> Pm4::DB_DEPTH_INFO_ARRAY_MODE_SHIFT) &
			                           Pm4::DB_DEPTH_INFO_ARRAY_MODE_MASK;
			z.depth_info.pipe_config = (buffer[10] >> Pm4::DB_DEPTH_INFO_PIPE_CONFIG_SHIFT) &
			                           Pm4::DB_DEPTH_INFO_PIPE_CONFIG_MASK;
			z.depth_info.bank_width  = (buffer[10] >> Pm4::DB_DEPTH_INFO_BANK_WIDTH_SHIFT) &
			                           Pm4::DB_DEPTH_INFO_BANK_WIDTH_MASK;
			z.depth_info.bank_height = (buffer[10] >> Pm4::DB_DEPTH_INFO_BANK_HEIGHT_SHIFT) &
			                           Pm4::DB_DEPTH_INFO_BANK_HEIGHT_MASK;
			z.depth_info.macro_tile_aspect =
			    KYTY_PM4_GET(buffer[10], DB_DEPTH_INFO, MACRO_TILE_ASPECT);
			z.depth_info.num_banks = (buffer[10] >> Pm4::DB_DEPTH_INFO_NUM_BANKS_SHIFT) &
			                         Pm4::DB_DEPTH_INFO_NUM_BANKS_MASK;

			// z.depth_view.slice_start = (buffer[13] >> Pm4::DB_DEPTH_VIEW_SLICE_START_SHIFT) &
			// Pm4::DB_DEPTH_VIEW_SLICE_START_MASK; z.depth_view.slice_max   = (buffer[13] >>
			// Pm4::DB_DEPTH_VIEW_SLICE_MAX_SHIFT) & Pm4::DB_DEPTH_VIEW_SLICE_MAX_MASK;

			z.depth_view.slice_start =
			    KYTY_PM4_GET(buffer[13], DB_DEPTH_VIEW, SLICE_START) +
			    (KYTY_PM4_GET(buffer[13], DB_DEPTH_VIEW, SLICE_START_HI) << 11u);
			z.depth_view.slice_max = KYTY_PM4_GET(buffer[13], DB_DEPTH_VIEW, SLICE_MAX) +
			                         (KYTY_PM4_GET(buffer[13], DB_DEPTH_VIEW, SLICE_MAX_HI) << 11u);
			z.depth_view.depth_write_disable =
			    KYTY_PM4_GET(buffer[13], DB_DEPTH_VIEW, Z_READ_ONLY) != 0;
			z.depth_view.stencil_write_disable =
			    KYTY_PM4_GET(buffer[13], DB_DEPTH_VIEW, STENCIL_READ_ONLY) != 0;
			z.depth_view.current_mip_level = KYTY_PM4_GET(buffer[13], DB_DEPTH_VIEW, MIPID);

			z.htile_data_base_addr = static_cast<uint64_t>(buffer[16]) << 8u;

			z.htile_surface = ParseDepthHtileSurface(buffer[19]);

			if (buffer[21] != 0) {
				// CxDepthRenderTarget stores width/height as value - 1 in register 13.
				z.width              = ((buffer[21] >> 0u) & 0x3fffu) + 1u;
				z.height             = ((buffer[21] >> 16u) & 0x3fffu) + 1u;
				z.width_height_valid = true;
			}

			cp->GetCtx()->SetDepthRenderTarget(z);
		} else {
			KYTY_NOT_IMPLEMENTED;
		}
	} else {
		KYTY_NOT_IMPLEMENTED;
	}

	return count;
}

static HW::EqaaControl ParseEqaaControl(uint32_t value) {
	HW::EqaaControl r;

	r.max_anchor_samples         = KYTY_PM4_GET(value, DB_EQAA, MAX_ANCHOR_SAMPLES);
	r.ps_iter_samples            = KYTY_PM4_GET(value, DB_EQAA, PS_ITER_SAMPLES);
	r.mask_export_num_samples    = KYTY_PM4_GET(value, DB_EQAA, MASK_EXPORT_NUM_SAMPLES);
	r.alpha_to_mask_num_samples  = KYTY_PM4_GET(value, DB_EQAA, ALPHA_TO_MASK_NUM_SAMPLES);
	r.high_quality_intersections = KYTY_PM4_GET(value, DB_EQAA, HIGH_QUALITY_INTERSECTIONS) != 0;
	r.incoherent_eqaa_reads      = KYTY_PM4_GET(value, DB_EQAA, INCOHERENT_EQAA_READS) != 0;
	r.interpolate_comp_z         = KYTY_PM4_GET(value, DB_EQAA, INTERPOLATE_COMP_Z) != 0;
	r.static_anchor_associations = KYTY_PM4_GET(value, DB_EQAA, STATIC_ANCHOR_ASSOCIATIONS) != 0;

	return r;
}

KYTY_HW_CTX_PARSER(HwCtxSetEqaaControl) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::DB_EQAA);

	cp->GetCtx()->SetEqaaControl(ParseEqaaControl(buffer[0]));

	return 1;
}

KYTY_HW_CTX_PARSER(HwCtxSetGenericScissor) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;
	EXIT_NOT_IMPLEMENTED(num_values == 0);
	EXIT_NOT_IMPLEMENTED(cmd_offset < Pm4::PA_SC_GENERIC_SCISSOR_TL);
	EXIT_NOT_IMPLEMENTED(cmd_offset + num_values - 1u > Pm4::PA_SC_GENERIC_SCISSOR_BR);

	for (uint32_t i = 0; i < num_values; i++) {
		g_hw_ctx_indirect_func[(cmd_offset + i) & (Pm4::CX_NUM - 1)](cp, cmd_offset + i, buffer[i]);
	}

	return num_values;
}

static int ParseScissorCoord15(uint32_t value) {
	return static_cast<int16_t>(static_cast<uint16_t>(value & 0x7fffu));
}

KYTY_HW_CTX_PARSER(HwCtxSetViewportScissor) {
	auto reg_num = (cmd_id >> 16u) & 0x3fffu;
	EXIT_NOT_IMPLEMENTED(reg_num == 0);
	EXIT_NOT_IMPLEMENTED(cmd_offset < Pm4::PA_SC_VPORT_SCISSOR_0_TL);
	EXIT_NOT_IMPLEMENTED(cmd_offset + reg_num - 1u > Pm4::PA_SC_VPORT_SCISSOR_15_BR);

	for (uint32_t i = 0; i < reg_num; i++) {
		const auto reg      = cmd_offset + i;
		const auto viewport = (reg - Pm4::PA_SC_VPORT_SCISSOR_0_TL) / 2u;
		const auto value    = buffer[i];

		EXIT_NOT_IMPLEMENTED(viewport >= 16);
		if (((reg - Pm4::PA_SC_VPORT_SCISSOR_0_TL) % 2u) == 0) {
			const int  left                  = ParseScissorCoord15(value);
			const int  top                   = ParseScissorCoord15(value >> 16u);
			const bool window_offset_disable = (value & 0x80000000u) != 0;
			cp->GetCtx()->SetViewportScissorTL(viewport, left, top, !window_offset_disable);
		} else {
			const int right  = ParseScissorCoord15(value);
			const int bottom = ParseScissorCoord15(value >> 16u);
			cp->GetCtx()->SetViewportScissorBR(viewport, right, bottom);
		}
	}

	return reg_num;
}

static void HwCtxSetClipRectRegister(HW::Context* ctx, uint32_t cmd_offset, uint32_t value) {
	EXIT_IF(ctx == nullptr);

	if (cmd_offset == Pm4::PA_SC_CLIPRECT_RULE) {
		ctx->SetClipRectRule(static_cast<uint16_t>(value & 0xffffu));
		return;
	}

	if (cmd_offset >= Pm4::PA_SC_CLIPRECT_0_TL && cmd_offset < Pm4::PA_SC_CLIPRECT_0_TL + 8u) {
		const uint32_t rect_id = (cmd_offset - Pm4::PA_SC_CLIPRECT_0_TL) / 2u;
		if (((cmd_offset - Pm4::PA_SC_CLIPRECT_0_TL) & 1u) == 0) {
			const int  left                  = ParseScissorCoord15(value);
			const int  top                   = ParseScissorCoord15(value >> 16u);
			const bool window_offset_disable = (value & 0x80000000u) != 0;
			ctx->SetClipRectTL(rect_id, left, top, !window_offset_disable);
		} else {
			const int right  = ParseScissorCoord15(value);
			const int bottom = ParseScissorCoord15(value >> 16u);
			ctx->SetClipRectBR(rect_id, right, bottom);
		}
		return;
	}

	EXIT("unknown clip-rect register: 0x%08" PRIx32 "\n", cmd_offset);
}

KYTY_HW_CTX_PARSER(HwCtxSetClipRect) {
	const auto value_count = (cmd_id >> 16u) & 0x3fffu;
	EXIT_NOT_IMPLEMENTED(value_count == 0);
	EXIT_NOT_IMPLEMENTED(cmd_offset < Pm4::PA_SC_CLIPRECT_RULE);
	EXIT_NOT_IMPLEMENTED(cmd_offset + value_count > Pm4::PA_SC_CLIPRECT_0_TL + 8u);

	for (uint32_t i = 0; i < value_count; i++) {
		HwCtxSetClipRectRegister(cp->GetCtx(), cmd_offset + i, buffer[i]);
	}

	return value_count;
}

KYTY_HW_CTX_PARSER(HwCtxSetGuardBands) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0046900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::PA_CL_GB_VERT_CLIP_ADJ);

	auto vert_clip    = *reinterpret_cast<const float*>(&buffer[0]); // PA_CL_GB_VERT_CLIP_ADJ
	auto vert_discard = *reinterpret_cast<const float*>(&buffer[1]); // PA_CL_GB_VERT_DISC_ADJ
	auto horz_clip    = *reinterpret_cast<const float*>(&buffer[2]); // PA_CL_GB_HORZ_CLIP_ADJ
	auto horz_discard = *reinterpret_cast<const float*>(&buffer[3]); // PA_CL_GB_HORZ_DISC_ADJ

	cp->GetCtx()->SetGuardBands(horz_clip, vert_clip, horz_discard, vert_discard);

	return 4;
}

KYTY_HW_CTX_PARSER(HwCtxSetHardwareScreenOffset) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::PA_SU_HARDWARE_SCREEN_OFFSET);

	// uint32_t x = static_cast<uint16_t>(buffer[0] & 0xffffu);
	// uint32_t y = static_cast<uint16_t>((buffer[0] >> 16u) & 0xffffu);

	uint32_t x = KYTY_PM4_GET(buffer[0], PA_SU_HARDWARE_SCREEN_OFFSET, HW_SCREEN_OFFSET_X);
	uint32_t y = KYTY_PM4_GET(buffer[0], PA_SU_HARDWARE_SCREEN_OFFSET, HW_SCREEN_OFFSET_Y);

	cp->GetCtx()->SetHardwareScreenOffset(x, y);

	return 1;
}

KYTY_HW_CTX_PARSER(HwCtxSetWindowOffset) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::PA_SC_WINDOW_OFFSET);

	int offset_x = static_cast<int16_t>(
	    static_cast<uint16_t>(KYTY_PM4_GET(buffer[0], PA_SC_WINDOW_OFFSET, WINDOW_X)));
	int offset_y = static_cast<int16_t>(
	    static_cast<uint16_t>(KYTY_PM4_GET(buffer[0], PA_SC_WINDOW_OFFSET, WINDOW_Y)));

	cp->GetCtx()->SetWindowOffset(offset_x, offset_y);

	return 1;
}

KYTY_HW_CTX_PARSER(HwCtxSetWindowScissor) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;
	EXIT_NOT_IMPLEMENTED(num_values == 0);
	EXIT_NOT_IMPLEMENTED(cmd_offset < Pm4::PA_SC_WINDOW_SCISSOR_TL);
	EXIT_NOT_IMPLEMENTED(cmd_offset + num_values - 1u > Pm4::PA_SC_WINDOW_SCISSOR_BR);

	for (uint32_t i = 0; i < num_values; i++) {
		g_hw_ctx_indirect_func[(cmd_offset + i) & (Pm4::CX_NUM - 1)](cp, cmd_offset + i, buffer[i]);
	}

	return num_values;
}

KYTY_HW_CTX_PARSER(HwCtxSetLineControl) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::PA_SU_LINE_CNTL);

	auto line_width = KYTY_PM4_GET(buffer[0], PA_SU_LINE_CNTL, WIDTH);

	if (line_width == 8) {
		cp->GetCtx()->SetLineWidth(1.0f);
	} else {
		cp->GetCtx()->SetLineWidth(static_cast<float>(line_width) / 8.0f);
	}

	return 1;
}

KYTY_HW_CTX_PARSER(HwCtxSetModeControl) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::PA_SU_SC_MODE_CNTL);

	HW::ModeControl r;

	r.cull_front           = KYTY_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, CULL_FRONT) != 0;
	r.cull_back            = KYTY_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, CULL_BACK) != 0;
	r.face                 = KYTY_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, FACE) != 0;
	r.poly_mode            = KYTY_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, POLY_MODE);
	r.polymode_front_ptype = KYTY_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, POLYMODE_FRONT_PTYPE);
	r.polymode_back_ptype  = KYTY_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, POLYMODE_BACK_PTYPE);
	r.poly_offset_front_enable =
	    KYTY_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, POLY_OFFSET_FRONT_ENABLE) != 0;
	r.poly_offset_back_enable =
	    KYTY_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, POLY_OFFSET_BACK_ENABLE) != 0;
	r.vtx_window_offset_enable =
	    KYTY_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, VTX_WINDOW_OFFSET_ENABLE) != 0;
	r.provoking_vtx_last = KYTY_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, PROVOKING_VTX_LAST) != 0;
	r.persp_corr_dis     = KYTY_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, PERSP_CORR_DIS) != 0;

	cp->GetCtx()->SetModeControl(r);

	return 1;
}

static void HwCtxIgnorePolyOffsetRegister(uint32_t cmd_offset, uint32_t value) {
	static std::atomic<uint32_t> log_count = 0;

	auto count = log_count.fetch_add(1);
	if (count < 8) {
		LOGF_COLOR(Log::Color::Red,
		           "\t temporary: ignoring polygon offset context register 0x%03" PRIx32
		           " = 0x%08" PRIx32 " (depth bias not implemented)\n",
		           cmd_offset, value);
	}
}

KYTY_HW_CTX_PARSER(HwCtxSetPolyOffsetRegisters) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwCtxIgnorePolyOffsetRegister(cmd_offset + i, buffer[i]);
	}

	return num_values;
}

KYTY_HW_CTX_PARSER(HwCtxSetPsInput) {
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::SPI_PS_INPUT_CNTL_0);

	uint32_t count = (cmd_id >> 16u) & 0x3fffu;

	EXIT_NOT_IMPLEMENTED(count == 0);
	EXIT_NOT_IMPLEMENTED(count > 32);

	for (uint32_t i = 0; i < count; i++) {
		cp->GetCtx()->SetPsInputSettings(i, buffer[i]);
	}

	return count;
}
KYTY_HW_CTX_PARSER(HwCtxSetRenderControl) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::DB_RENDER_CONTROL);

	HW::RenderControl r;

	r.depth_clear_enable   = KYTY_PM4_GET(buffer[0], DB_RENDER_CONTROL, DEPTH_CLEAR_ENABLE) != 0;
	r.stencil_clear_enable = KYTY_PM4_GET(buffer[0], DB_RENDER_CONTROL, STENCIL_CLEAR_ENABLE) != 0;
	r.resummarize_enable   = KYTY_PM4_GET(buffer[0], DB_RENDER_CONTROL, RESUMMARIZE_ENABLE) != 0;
	r.stencil_compress_disable =
	    KYTY_PM4_GET(buffer[0], DB_RENDER_CONTROL, STENCIL_COMPRESS_DISABLE) != 0;
	r.depth_compress_disable =
	    KYTY_PM4_GET(buffer[0], DB_RENDER_CONTROL, DEPTH_COMPRESS_DISABLE) != 0;
	r.copy_centroid = KYTY_PM4_GET(buffer[0], DB_RENDER_CONTROL, COPY_CENTROID) != 0;
	r.copy_sample   = KYTY_PM4_GET(buffer[0], DB_RENDER_CONTROL, COPY_SAMPLE);

	cp->GetCtx()->SetRenderControl(r);

	return 1;
}

KYTY_HW_CTX_PARSER(HwCtxSetRenderTarget) {
	uint32_t count = (cmd_id >> 16u) & 0x3fffu;
	if (count < 11 || count > 14) {
		bool handled = (count > 0);
		for (uint32_t i = 0; i < count; i++) {
			if (cmd_offset + i >= Pm4::CX_NUM ||
			    g_hw_ctx_indirect_func[(cmd_offset + i) & (Pm4::CX_NUM - 1)] == nullptr) {
				handled = false;
				break;
			}
		}
		if (handled) {
			for (uint32_t i = 0; i < count; i++) {
				g_hw_ctx_indirect_func[(cmd_offset + i) & (Pm4::CX_NUM - 1)](cp, cmd_offset + i,
				                                                             buffer[i]);
			}
			return count;
		}

		EXIT_NOT_IMPLEMENTED(count < 11 || count > 14);
	}

	uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_BASE) / 15;

	auto* ctx = cp->GetCtx();

	HW::ColorBase       base;
	HW::ColorPitch      pitch;
	HW::ColorSlice      slice;
	HW::ColorView       view;
	HW::ColorInfo       info;
	HW::ColorAttrib     attrib;
	HW::ColorDccControl dcc;
	HW::ColorCmask      cmask;
	HW::ColorCmaskSlice cmask_slice;
	HW::ColorFmask      fmask;
	HW::ColorFmaskSlice fmask_slice;

	base.addr                     = static_cast<uint64_t>(buffer[0]) << 8u;
	pitch.pitch_div8_minus1       = buffer[1] & 0x7ffu;
	pitch.fmask_pitch_div8_minus1 = (buffer[1] >> 20u) & 0x7ffu;
	slice.slice_div64_minus1      = buffer[2] & 0x3fffffu;

	// view.base_array_slice_index     = buffer[3] & 0x7ffu;
	// view.last_array_slice_index     = (buffer[3] >> 13u) & 0x7ffu;
	view.base_array_slice_index = KYTY_PM4_GET(buffer[3], CB_COLOR0_VIEW, SLICE_START);
	view.last_array_slice_index = KYTY_PM4_GET(buffer[3], CB_COLOR0_VIEW, SLICE_MAX);
	view.current_mip_level      = KYTY_PM4_GET(buffer[3], CB_COLOR0_VIEW, MIP_LEVEL);

	//	info.fmask_compression_enable = (buffer[4] & 0x4000u) != 0;
	//
	//	// info.fmask_compression_mode   = (buffer[4] >> 26u) & 0x3u;
	//	info.fmask_data_compression_disable = ((buffer[4] >> 26u) & 0x1u) != 0;
	//	info.fmask_one_frag_mode            = ((buffer[4] >> 27u) & 0x1u) != 0;
	//
	//	info.cmask_fast_clear_enable = (buffer[4] & 0x2000u) != 0;
	//	info.dcc_compression_enable  = (buffer[4] & 0x10000000u) != 0;
	//	info.cmask_is_linear         = (buffer[4] >> 19u) & 0x1u;
	//	info.cmask_addr_type         = (buffer[4] >> 29u) & 0x3u;
	//	info.alt_tile_mode           = ((buffer[4] >> 31u) & 0x1u) != 0;
	//	info.format                  = (buffer[4] >> 2u) & 0x1fu;
	//	info.channel_type            = (buffer[4] >> 8u) & 0x7u;
	//	info.channel_order           = (buffer[4] >> 11u) & 0x3u;
	info.format                   = KYTY_PM4_GET(buffer[4], CB_COLOR0_INFO, FORMAT);
	info.channel_type             = KYTY_PM4_GET(buffer[4], CB_COLOR0_INFO, NUMBER_TYPE);
	info.channel_order            = KYTY_PM4_GET(buffer[4], CB_COLOR0_INFO, COMP_SWAP);
	info.cmask_fast_clear_enable  = KYTY_PM4_GET(buffer[4], CB_COLOR0_INFO, FAST_CLEAR) != 0;
	info.fmask_compression_enable = KYTY_PM4_GET(buffer[4], CB_COLOR0_INFO, COMPRESSION) != 0;
	info.blend_clamp              = KYTY_PM4_GET(buffer[4], CB_COLOR0_INFO, BLEND_CLAMP) != 0;
	info.blend_bypass             = KYTY_PM4_GET(buffer[4], CB_COLOR0_INFO, BLEND_BYPASS) != 0;
	info.round_mode               = KYTY_PM4_GET(buffer[4], CB_COLOR0_INFO, ROUND_MODE) != 0;
	info.cmask_is_linear          = KYTY_PM4_GET(buffer[4], CB_COLOR0_INFO, CMASK_IS_LINEAR);
	info.fmask_data_compression_disable =
	    KYTY_PM4_GET(buffer[4], CB_COLOR0_INFO, FMASK_COMPRESSION_DISABLE) != 0;
	info.fmask_one_frag_mode =
	    KYTY_PM4_GET(buffer[4], CB_COLOR0_INFO, FMASK_COMPRESS_1FRAG_ONLY) != 0;
	info.dcc_compression_enable = KYTY_PM4_GET(buffer[4], CB_COLOR0_INFO, DCC_ENABLE) != 0;
	info.cmask_addr_type        = KYTY_PM4_GET(buffer[4], CB_COLOR0_INFO, CMASK_ADDR_TYPE);
	info.alt_tile_mode          = KYTY_PM4_GET(buffer[4], CB_COLOR0_INFO, ALT_TILE_MODE) != 0;

	//	attrib.force_dest_alpha_to_one  = (buffer[5] & 0x20000u) != 0;
	//	attrib.tile_mode                = buffer[5] & 0x1fu;
	//	attrib.fmask_tile_mode          = (buffer[5] >> 5u) & 0x1fu;
	//	attrib.num_samples              = (buffer[5] >> 12u) & 0x7u;
	//	attrib.num_fragments            = (buffer[5] >> 15u) & 0x3u;
	attrib.force_dest_alpha_to_one =
	    KYTY_PM4_GET(buffer[5], CB_COLOR0_ATTRIB, FORCE_DST_ALPHA_1) != 0;
	attrib.tile_mode       = KYTY_PM4_GET(buffer[5], CB_COLOR0_ATTRIB, TILE_MODE_INDEX);
	attrib.fmask_tile_mode = KYTY_PM4_GET(buffer[5], CB_COLOR0_ATTRIB, FMASK_TILE_MODE_INDEX);
	attrib.num_samples     = KYTY_PM4_GET(buffer[5], CB_COLOR0_ATTRIB, NUM_SAMPLES);
	attrib.num_fragments   = KYTY_PM4_GET(buffer[5], CB_COLOR0_ATTRIB, NUM_FRAGMENTS);

	//	dcc.max_uncompressed_block_size = (buffer[6] >> 2u) & 0x3u;
	//	dcc.max_compressed_block_size   = (buffer[6] >> 5u) & 0x3u;
	//	dcc.min_compressed_block_size   = (buffer[6] >> 4u) & 0x1u;
	//	dcc.color_transform             = (buffer[6] >> 7u) & 0x3u;
	//	dcc.overwrite_combiner_disable   = (buffer[6] & 0x1u) != 0;
	//	dcc.independent_64b_blocks    = (buffer[6] & 0x200u) != 0;
	dcc.overwrite_combiner_disable =
	    KYTY_PM4_GET(buffer[6], CB_COLOR0_DCC_CONTROL, OVERWRITE_COMBINER_DISABLE) != 0;
	dcc.dcc_clear_key_enable =
	    KYTY_PM4_GET(buffer[6], CB_COLOR0_DCC_CONTROL, KEY_CLEAR_ENABLE) != 0;
	dcc.max_uncompressed_block_size =
	    KYTY_PM4_GET(buffer[6], CB_COLOR0_DCC_CONTROL, MAX_UNCOMPRESSED_BLOCK_SIZE);
	dcc.min_compressed_block_size =
	    KYTY_PM4_GET(buffer[6], CB_COLOR0_DCC_CONTROL, MIN_COMPRESSED_BLOCK_SIZE);
	dcc.max_compressed_block_size =
	    KYTY_PM4_GET(buffer[6], CB_COLOR0_DCC_CONTROL, MAX_COMPRESSED_BLOCK_SIZE);
	dcc.color_transform = KYTY_PM4_GET(buffer[6], CB_COLOR0_DCC_CONTROL, COLOR_TRANSFORM);
	dcc.independent_64b_blocks =
	    KYTY_PM4_GET(buffer[6], CB_COLOR0_DCC_CONTROL, INDEPENDENT_64B_BLOCKS) != 0;
	dcc.data_write_on_dcc_clear_to_reg =
	    KYTY_PM4_GET(buffer[6], CB_COLOR0_DCC_CONTROL, ENABLE_CONSTANT_ENCODE_REG_WRITE) != 0;
	dcc.independent_128b_blocks =
	    KYTY_PM4_GET(buffer[6], CB_COLOR0_DCC_CONTROL, INDEPENDENT_128B_BLOCKS) != 0;

	cmask.addr               = static_cast<uint64_t>(buffer[7]) << 8u;
	cmask_slice.slice_minus1 = buffer[8] & 0x3fffu;

	fmask.addr               = static_cast<uint64_t>(buffer[9]) << 8u;
	fmask_slice.slice_minus1 = buffer[10] & 0x3fffffu;

	ctx->SetColorBase(slot, base);
	ctx->SetColorPitch(slot, pitch);
	ctx->SetColorSlice(slot, slice);
	ctx->SetColorView(slot, view);
	ctx->SetColorInfo(slot, info);
	ctx->SetColorAttrib(slot, attrib);
	ctx->SetColorDccControl(slot, dcc);
	ctx->SetColorCmask(slot, cmask);
	ctx->SetColorCmaskSlice(slot, cmask_slice);
	ctx->SetColorFmask(slot, fmask);
	ctx->SetColorFmaskSlice(slot, fmask_slice);

	if (count > 11) {
		HW::ColorClearWord0 clear_word0;

		clear_word0.word0 = buffer[11];

		ctx->SetColorClearWord0(slot, clear_word0);
	}
	if (count > 12) {
		HW::ColorClearWord1 clear_word1;

		clear_word1.word1 = buffer[12];

		ctx->SetColorClearWord1(slot, clear_word1);
	}
	if (count > 13) {
		HW::ColorDccAddr dcc_addr;

		dcc_addr.addr = static_cast<uint64_t>(buffer[13]) << 8u;

		ctx->SetColorDccAddr(slot, dcc_addr);
	}

	if (dw >= count + 2 && buffer[count] == 0xC0001000) {
		HW::ColorSize size;

		size.width  = (buffer[count + 1] >> 0u) & 0xffffu;
		size.height = (buffer[count + 1] >> 16u) & 0xffffu;

		ctx->SetColorSize(slot, size);

		count += 2;
	}

	return count;
}

KYTY_HW_CTX_PARSER(HwCtxSetRenderTargetMask) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::CB_TARGET_MASK);

	cp->GetCtx()->SetRenderTargetMask(*buffer);

	return 1;
}

KYTY_HW_CTX_PARSER(HwCtxSetScanModeControl) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::PA_SC_MODE_CNTL_0);

	HW::ScanModeControl r;

	r.msaa_enable          = KYTY_PM4_GET(buffer[0], PA_SC_MODE_CNTL_0, MSAA_ENABLE) != 0;
	r.vport_scissor_enable = KYTY_PM4_GET(buffer[0], PA_SC_MODE_CNTL_0, VPORT_SCISSOR_ENABLE) != 0;
	r.line_stipple_enable  = KYTY_PM4_GET(buffer[0], PA_SC_MODE_CNTL_0, LINE_STIPPLE_ENABLE) != 0;

	cp->GetCtx()->SetScanModeControl(r);

	return 1;
}

KYTY_HW_CTX_PARSER(HwCtxSetScreenScissor) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;
	EXIT_NOT_IMPLEMENTED(num_values == 0);
	EXIT_NOT_IMPLEMENTED(cmd_offset < Pm4::PA_SC_SCREEN_SCISSOR_TL);
	EXIT_NOT_IMPLEMENTED(cmd_offset + num_values - 1u > Pm4::PA_SC_SCREEN_SCISSOR_BR);

	for (uint32_t i = 0; i < num_values; i++) {
		g_hw_ctx_indirect_func[(cmd_offset + i) & (Pm4::CX_NUM - 1)](cp, cmd_offset + i, buffer[i]);
	}

	return num_values;
}

KYTY_HW_CTX_PARSER(HwCtxSetShaderStages) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::VGT_SHADER_STAGES_EN);

	cp->GetCtx()->SetShaderStages(buffer[0]);

	return 1;
}

KYTY_HW_CTX_PARSER(HwCtxSetStencilClear) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::DB_STENCIL_CLEAR);

	cp->GetCtx()->SetStencilClearValue(KYTY_PM4_GET(buffer[0], DB_STENCIL_CLEAR, CLEAR));

	return 1;
}

KYTY_HW_CTX_PARSER(HwCtxSetStencilControl) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::DB_STENCIL_CONTROL);

	HW::StencilControl r;

	r.stencil_fail     = KYTY_PM4_GET(buffer[0], DB_STENCIL_CONTROL, STENCILFAIL);
	r.stencil_zpass    = KYTY_PM4_GET(buffer[0], DB_STENCIL_CONTROL, STENCILZPASS);
	r.stencil_zfail    = KYTY_PM4_GET(buffer[0], DB_STENCIL_CONTROL, STENCILZFAIL);
	r.stencil_fail_bf  = KYTY_PM4_GET(buffer[0], DB_STENCIL_CONTROL, STENCILFAIL_BF);
	r.stencil_zpass_bf = KYTY_PM4_GET(buffer[0], DB_STENCIL_CONTROL, STENCILZPASS_BF);
	r.stencil_zfail_bf = KYTY_PM4_GET(buffer[0], DB_STENCIL_CONTROL, STENCILZFAIL_BF);

	cp->GetCtx()->SetStencilControl(r);

	return 1;
}

KYTY_HW_CTX_PARSER(HwCtxSetStencilInfo) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::DB_STENCIL_INFO);

	HW::DepthStencilInfo r;

	//	r.expclear_enabled = (buffer[0] & 0x08000000u) != 0;
	//	r.tile_split       = (buffer[0] >> 13u) & 0x7u;
	//	r.format          = (buffer[0] >> Pm4::DB_STENCIL_INFO_FORMAT_SHIFT) &
	// Pm4::DB_STENCIL_INFO_FORMAT_MASK; 	r.tile_mode_index = (buffer[0] >>
	// Pm4::DB_STENCIL_INFO_TILE_MODE_INDEX_SHIFT) & Pm4::DB_STENCIL_INFO_TILE_MODE_INDEX_MASK;
	//	r.tile_stencil_disable =
	//	    ((buffer[0] >> Pm4::DB_STENCIL_INFO_TILE_STENCIL_DISABLE_SHIFT) &
	// Pm4::DB_STENCIL_INFO_TILE_STENCIL_DISABLE_MASK) != 0;

	r.format                     = KYTY_PM4_GET(buffer[1], DB_STENCIL_INFO, FORMAT);
	r.texture_compatible_stencil = KYTY_PM4_GET(buffer[1], DB_STENCIL_INFO, ITERATE_FLUSH) != 0;
	r.partially_resident   = KYTY_PM4_GET(buffer[1], DB_STENCIL_INFO, PARTIALLY_RESIDENT) != 0;
	r.tile_split           = KYTY_PM4_GET(buffer[1], DB_STENCIL_INFO, RESERVED_FIELD_1);
	r.tile_mode_index      = KYTY_PM4_GET(buffer[1], DB_STENCIL_INFO, TILE_MODE_INDEX);
	r.expclear_enabled     = KYTY_PM4_GET(buffer[1], DB_STENCIL_INFO, ALLOW_EXPCLEAR) != 0;
	r.tile_stencil_disable = KYTY_PM4_GET(buffer[1], DB_STENCIL_INFO, TILE_STENCIL_DISABLE) != 0;

	cp->GetCtx()->SetDepthStencilInfo(r);

	return 1;
}

KYTY_HW_CTX_PARSER(HwCtxSetStencilMask) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0026900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::DB_STENCILREFMASK);

	HW::StencilMask r;

	r.stencil_testval      = KYTY_PM4_GET(buffer[0], DB_STENCILREFMASK, STENCILTESTVAL);
	r.stencil_mask         = KYTY_PM4_GET(buffer[0], DB_STENCILREFMASK, STENCILMASK);
	r.stencil_writemask    = KYTY_PM4_GET(buffer[0], DB_STENCILREFMASK, STENCILWRITEMASK);
	r.stencil_opval        = KYTY_PM4_GET(buffer[0], DB_STENCILREFMASK, STENCILOPVAL);
	r.stencil_testval_bf   = KYTY_PM4_GET(buffer[1], DB_STENCILREFMASK_BF, STENCILTESTVAL_BF);
	r.stencil_mask_bf      = KYTY_PM4_GET(buffer[1], DB_STENCILREFMASK_BF, STENCILMASK_BF);
	r.stencil_writemask_bf = KYTY_PM4_GET(buffer[1], DB_STENCILREFMASK_BF, STENCILWRITEMASK_BF);
	r.stencil_opval_bf     = KYTY_PM4_GET(buffer[1], DB_STENCILREFMASK_BF, STENCILOPVAL_BF);

	cp->GetCtx()->SetStencilMask(r);

	return 2;
}

KYTY_HW_CTX_PARSER(HwCtxSetViewportScaleOffset) {
	auto reg_num = (cmd_id >> 16u) & 0x3fffu;
	EXIT_NOT_IMPLEMENTED(reg_num == 0);
	EXIT_NOT_IMPLEMENTED(cmd_offset < Pm4::PA_CL_VPORT_XSCALE);
	EXIT_NOT_IMPLEMENTED(cmd_offset + reg_num - 1u > Pm4::PA_CL_VPORT_ZOFFSET_15);

	for (uint32_t i = 0; i < reg_num; i++) {
		const auto reg      = cmd_offset + i;
		const auto viewport = (reg - Pm4::PA_CL_VPORT_XSCALE) / 6u;
		const auto field    = (reg - Pm4::PA_CL_VPORT_XSCALE) % 6u;
		const auto value    = *reinterpret_cast<const float*>(buffer + i);

		EXIT_NOT_IMPLEMENTED(viewport >= 16);
		switch (field) {
			case 0: cp->GetCtx()->SetViewportXScale(viewport, value); break;
			case 1: cp->GetCtx()->SetViewportXOffset(viewport, value); break;
			case 2: cp->GetCtx()->SetViewportYScale(viewport, value); break;
			case 3: cp->GetCtx()->SetViewportYOffset(viewport, value); break;
			case 4: cp->GetCtx()->SetViewportZScale(viewport, value); break;
			case 5: cp->GetCtx()->SetViewportZOffset(viewport, value); break;
			default: EXIT("invalid viewport scale/offset field\n");
		}
	}

	return reg_num;
}

KYTY_HW_CTX_PARSER(HwCtxSetViewportTransformControl) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::PA_CL_VTE_CNTL);

	cp->GetCtx()->SetViewportTransformControl(*buffer);

	return 1;
}

KYTY_HW_CTX_PARSER(HwCtxSetViewportZ) {
	auto reg_num = (cmd_id >> 16u) & 0x3fffu;
	EXIT_NOT_IMPLEMENTED(reg_num == 0);
	EXIT_NOT_IMPLEMENTED(cmd_offset < Pm4::PA_SC_VPORT_ZMIN_0);
	EXIT_NOT_IMPLEMENTED(cmd_offset + reg_num - 1u > Pm4::PA_SC_VPORT_ZMAX_15);

	for (uint32_t i = 0; i < reg_num; i++) {
		const auto reg      = cmd_offset + i;
		const auto viewport = (reg - Pm4::PA_SC_VPORT_ZMIN_0) / 2u;
		const auto value    = *reinterpret_cast<const float*>(buffer + i);

		EXIT_NOT_IMPLEMENTED(viewport >= 16);
		if (((reg - Pm4::PA_SC_VPORT_ZMIN_0) % 2u) == 0) {
			cp->GetCtx()->SetViewportZMin(viewport, value);
		} else {
			cp->GetCtx()->SetViewportZMax(viewport, value);
		}
	}

	return reg_num;
}

static void HwShIgnoreComputeRegister([[maybe_unused]] uint32_t cmd_offset,
                                      [[maybe_unused]] uint32_t value) {}
static void HwShIgnoreShaderRegister([[maybe_unused]] uint32_t cmd_offset,
                                     [[maybe_unused]] uint32_t value) {}

KYTY_HW_SH_PARSER(HwShIgnoreRegisters) {
	return (cmd_id >> 16u) & 0x3fffu;
}

static void HwShSetCsRegister(CommandProcessor* cp, uint32_t cmd_offset, uint32_t value) {
	auto cs_regs = cp->GetShCtx()->GetCs().cs_regs;

	switch (cmd_offset) {
		case Pm4::COMPUTE_PGM_LO:
			cs_regs.data_addr &= 0xFFFFFF00000000FFull;
			cs_regs.data_addr |= static_cast<uint64_t>(value) << 8u;
			break;
		case Pm4::COMPUTE_PGM_HI:
			cs_regs.data_addr &= 0xFFFF00FFFFFFFFFFull;
			cs_regs.data_addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			break;
		case Pm4::COMPUTE_TBA_LO:
		case Pm4::COMPUTE_TBA_HI:
		case Pm4::COMPUTE_TMA_LO:
		case Pm4::COMPUTE_TMA_HI: HwShIgnoreComputeRegister(cmd_offset, value); break;
		case Pm4::COMPUTE_PGM_RSRC1:
			cs_regs.vgprs =
			    (value >> Pm4::COMPUTE_PGM_RSRC1_VGPRS_SHIFT) & Pm4::COMPUTE_PGM_RSRC1_VGPRS_MASK;
			cs_regs.sgprs =
			    (value >> Pm4::COMPUTE_PGM_RSRC1_SGPRS_SHIFT) & Pm4::COMPUTE_PGM_RSRC1_SGPRS_MASK;
			cs_regs.bulky =
			    (value >> Pm4::COMPUTE_PGM_RSRC1_BULKY_SHIFT) & Pm4::COMPUTE_PGM_RSRC1_BULKY_MASK;
			cs_regs.wave_size = (((value >> Pm4::COMPUTE_PGM_RSRC1_W32_EN_SHIFT) &
			                      Pm4::COMPUTE_PGM_RSRC1_W32_EN_MASK) != 0u)
			                        ? 32u
			                        : 64u;
			break;
		case Pm4::COMPUTE_PGM_RSRC2:
			cs_regs.scratch_en     = (value >> Pm4::COMPUTE_PGM_RSRC2_SCRATCH_EN_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC2_SCRATCH_EN_MASK;
			cs_regs.user_sgpr      = (value >> Pm4::COMPUTE_PGM_RSRC2_USER_SGPR_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC2_USER_SGPR_MASK;
			cs_regs.tgid_x_en      = (value >> Pm4::COMPUTE_PGM_RSRC2_TGID_X_EN_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC2_TGID_X_EN_MASK;
			cs_regs.tgid_y_en      = (value >> Pm4::COMPUTE_PGM_RSRC2_TGID_Y_EN_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC2_TGID_Y_EN_MASK;
			cs_regs.tgid_z_en      = (value >> Pm4::COMPUTE_PGM_RSRC2_TGID_Z_EN_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC2_TGID_Z_EN_MASK;
			cs_regs.tg_size_en     = (value >> Pm4::COMPUTE_PGM_RSRC2_TG_SIZE_EN_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC2_TG_SIZE_EN_MASK;
			cs_regs.tidig_comp_cnt = (value >> Pm4::COMPUTE_PGM_RSRC2_TIDIG_COMP_CNT_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC2_TIDIG_COMP_CNT_MASK;
			cs_regs.lds_size       = (value >> Pm4::COMPUTE_PGM_RSRC2_LDS_SIZE_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC2_LDS_SIZE_MASK;
			break;
		case Pm4::COMPUTE_NUM_THREAD_X: cs_regs.num_thread_x = value; break;
		case Pm4::COMPUTE_NUM_THREAD_Y: cs_regs.num_thread_y = value; break;
		case Pm4::COMPUTE_NUM_THREAD_Z: cs_regs.num_thread_z = value; break;
		case Pm4::COMPUTE_START_X:
		case Pm4::COMPUTE_START_Y:
		case Pm4::COMPUTE_START_Z:
		case Pm4::COMPUTE_RESOURCE_LIMITS:
		case Pm4::COMPUTE_DESTINATION_EN_SE0:
		case Pm4::COMPUTE_DESTINATION_EN_SE1:
		case Pm4::COMPUTE_TMPRING_SIZE:
		case Pm4::COMPUTE_DESTINATION_EN_SE2:
		case Pm4::COMPUTE_DESTINATION_EN_SE3:
		case Pm4::COMPUTE_PGM_RSRC3:
		case Pm4::COMPUTE_SHADER_CHKSUM:
		case Pm4::COMPUTE_DISPATCH_TUNNEL: HwShIgnoreComputeRegister(cmd_offset, value); break;
		default:
			EXIT("unsupported compute SH register 0x%08" PRIx32 " = 0x%08" PRIx32 "\n", cmd_offset,
			     value);
	}

	cp->GetShCtx()->SetCsShader(cs_regs);
}

KYTY_HW_SH_PARSER(HwShSetCsRegisters) {
	auto reg_num = (cmd_id >> 16u) & 0x3fffu;

	for (uint32_t i = 0; i < reg_num; i++) {
		HwShSetCsRegister(cp, cmd_offset + i, buffer[i]);
	}

	return reg_num;
}

static uint32_t UserSgprWriteNum(uint32_t slot, uint32_t reg_num, const char* stage) {
	EXIT_IF(stage == nullptr);

	if (slot >= HW::UserSgprInfo::SGPRS_MAX) {
		LOGF("\t %s user sgpr write starts out of range: slot = %" PRIu32 ", reg_num = %" PRIu32
		     "\n",
		     stage, slot, reg_num);
		return 0;
	}

	const auto available = static_cast<uint32_t>(HW::UserSgprInfo::SGPRS_MAX) - slot;
	if (reg_num > available) {
		LOGF("\t %s user sgpr write truncated: slot = %" PRIu32 ", reg_num = %" PRIu32
		     ", available = %" PRIu32 "\n",
		     stage, slot, reg_num, available);
		return available;
	}

	return reg_num;
}

KYTY_HW_SH_PARSER(HwShSetCsUserSgpr) {
	EXIT_NOT_IMPLEMENTED(
	    !(cmd_offset >= Pm4::COMPUTE_USER_DATA_0 && cmd_offset <= Pm4::COMPUTE_USER_DATA_15));

	uint32_t slot = (cmd_offset - Pm4::COMPUTE_USER_DATA_0) / 1;

	auto reg_num   = (cmd_id >> 16u) & 0x3fffu;
	auto write_num = UserSgprWriteNum(slot, reg_num, "cs");

	for (uint32_t i = 0; i < write_num; i++) {
		cp->GetShCtx()->SetCsUserSgpr(slot + i, buffer[i], cp->GetUserDataMarker());
	}
	cp->SetUserDataMarker(HW::UserSgprType::Unknown);

	return reg_num;
}

KYTY_HW_SH_PARSER(HwShSetCsUserAccumSgpr) {
	EXIT_NOT_IMPLEMENTED(cmd_offset < Pm4::COMPUTE_USER_ACCUM_0);

	uint32_t slot = 16u + (cmd_offset - Pm4::COMPUTE_USER_ACCUM_0);

	auto reg_num   = (cmd_id >> 16u) & 0x3fffu;
	auto write_num = UserSgprWriteNum(slot, reg_num, "cs accum");

	for (uint32_t i = 0; i < write_num; i++) {
		cp->GetShCtx()->SetCsUserSgpr(slot + i, buffer[i], cp->GetUserDataMarker());
	}
	cp->SetUserDataMarker(HW::UserSgprType::Unknown);

	return reg_num;
}

KYTY_HW_SH_PARSER(HwShSetPsUserSgpr) {
	EXIT_NOT_IMPLEMENTED(!(cmd_offset >= Pm4::SPI_SHADER_USER_DATA_PS_0 &&
	                       cmd_offset <= Pm4::SPI_SHADER_USER_DATA_PS_31));

	uint32_t slot = (cmd_offset - Pm4::SPI_SHADER_USER_DATA_PS_0) / 1;

	auto reg_num   = (cmd_id >> 16u) & 0x3fffu;
	auto write_num = UserSgprWriteNum(slot, reg_num, "ps");

	for (uint32_t i = 0; i < write_num; i++) {
		cp->GetShCtx()->SetPsUserSgpr(slot + i, buffer[i], cp->GetUserDataMarker());
	}
	cp->SetUserDataMarker(HW::UserSgprType::Unknown);

	return reg_num;
}

KYTY_HW_SH_PARSER(HwShSetPsUserAccumSgpr) {
	EXIT_NOT_IMPLEMENTED(!(cmd_offset >= Pm4::SPI_SHADER_USER_ACCUM_PS_0 &&
	                       cmd_offset <= Pm4::SPI_SHADER_USER_ACCUM_PS_3));

	uint32_t slot = 16u + (cmd_offset - Pm4::SPI_SHADER_USER_ACCUM_PS_0);

	auto reg_num   = (cmd_id >> 16u) & 0x3fffu;
	auto write_num = UserSgprWriteNum(slot, reg_num, "ps accum");

	for (uint32_t i = 0; i < write_num; i++) {
		cp->GetShCtx()->SetPsUserSgpr(slot + i, buffer[i], cp->GetUserDataMarker());
	}
	cp->SetUserDataMarker(HW::UserSgprType::Unknown);

	return reg_num;
}

KYTY_HW_SH_PARSER(HwShSetGsUserSgpr) {
	EXIT_NOT_IMPLEMENTED(!(cmd_offset >= Pm4::SPI_SHADER_USER_DATA_GS_0 &&
	                       cmd_offset <= Pm4::SPI_SHADER_USER_DATA_GS_31));

	uint32_t slot = (cmd_offset - Pm4::SPI_SHADER_USER_DATA_GS_0) / 1;

	auto reg_num   = (cmd_id >> 16u) & 0x3fffu;
	auto write_num = UserSgprWriteNum(slot, reg_num, "gs");

	for (uint32_t i = 0; i < write_num; i++) {
		cp->GetShCtx()->SetGsUserSgpr(slot + i, buffer[i], cp->GetUserDataMarker());
	}
	cp->SetUserDataMarker(HW::UserSgprType::Unknown);

	return reg_num;
}

KYTY_HW_SH_PARSER(HwShSetGsUserAccumSgpr) {
	EXIT_NOT_IMPLEMENTED(!(cmd_offset >= Pm4::SPI_SHADER_USER_ACCUM_ESGS_0 &&
	                       cmd_offset <= Pm4::SPI_SHADER_USER_ACCUM_ESGS_3));

	uint32_t slot = 16u + (cmd_offset - Pm4::SPI_SHADER_USER_ACCUM_ESGS_0);

	auto reg_num   = (cmd_id >> 16u) & 0x3fffu;
	auto write_num = UserSgprWriteNum(slot, reg_num, "gs accum");

	for (uint32_t i = 0; i < write_num; i++) {
		cp->GetShCtx()->SetGsUserSgpr(slot + i, buffer[i], cp->GetUserDataMarker());
	}
	cp->SetUserDataMarker(HW::UserSgprType::Unknown);

	return reg_num;
}

KYTY_HW_SH_PARSER(HwShSetHsUserSgpr) {
	EXIT_NOT_IMPLEMENTED(!(cmd_offset >= Pm4::SPI_SHADER_USER_DATA_HS_0 &&
	                       cmd_offset <= Pm4::SPI_SHADER_USER_DATA_HS_31));

	uint32_t slot = (cmd_offset - Pm4::SPI_SHADER_USER_DATA_HS_0) / 1;

	auto reg_num   = (cmd_id >> 16u) & 0x3fffu;
	auto write_num = UserSgprWriteNum(slot, reg_num, "hs");

	for (uint32_t i = 0; i < write_num; i++) {
		cp->GetShCtx()->SetHsUserSgpr(slot + i, buffer[i], cp->GetUserDataMarker());
	}
	cp->SetUserDataMarker(HW::UserSgprType::Unknown);

	return reg_num;
}

KYTY_HW_SH_PARSER(HwShSetHsUserAccumSgpr) {
	EXIT_NOT_IMPLEMENTED(!(cmd_offset >= Pm4::SPI_SHADER_USER_ACCUM_LSHS_0 &&
	                       cmd_offset <= Pm4::SPI_SHADER_USER_ACCUM_LSHS_3));
	uint32_t slot = 16u + (cmd_offset - Pm4::SPI_SHADER_USER_ACCUM_LSHS_0);

	auto reg_num   = (cmd_id >> 16u) & 0x3fffu;
	auto write_num = UserSgprWriteNum(slot, reg_num, "hs accum");

	for (uint32_t i = 0; i < write_num; i++) {
		cp->GetShCtx()->SetHsUserSgpr(slot + i, buffer[i], cp->GetUserDataMarker());
	}
	cp->SetUserDataMarker(HW::UserSgprType::Unknown);

	return reg_num;
}

KYTY_HW_UC_PARSER(HwUcSetPrimitiveType) {
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0017900 && cmd_id != 0xC0017A00);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::VGT_PRIMITIVE_TYPE);

	uint32_t prim_type = KYTY_PM4_GET(buffer[0], VGT_PRIMITIVE_TYPE, PRIM_TYPE);

	cp->GetUcfg()->SetPrimitiveType(prim_type);

	return 1;
}

KYTY_HW_UC_PARSER(HwUcSetIndexType) {
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::VGT_INDEX_TYPE);
	cp->SetIndexType(buffer[0] & 0x3u);

	return 1;
}

KYTY_HW_UC_PARSER(HwUcSetObjectId) {
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::VGT_OBJECT_ID);
	cp->GetUcfg()->SetObjectId(buffer[0]);

	return 1;
}

static void HwUcIgnoreTextureGradientFactors([[maybe_unused]] uint32_t value) {}

KYTY_HW_UC_PARSER(HwUcSetTextureGradientFactors) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwUcIgnoreTextureGradientFactors(buffer[i]);
	}

	return num_values;
}

static void HwUcIgnoreIaMultiVgtParam([[maybe_unused]] uint32_t value) {}

KYTY_HW_UC_PARSER(HwUcSetIaMultiVgtParam) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwUcIgnoreIaMultiVgtParam(buffer[i]);
	}

	return num_values;
}

static void HwIgnoreMultiPrimIbReset([[maybe_unused]] uint32_t value) {}

KYTY_HW_UC_PARSER(HwUcSetMultiPrimIbReset) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwIgnoreMultiPrimIbReset(buffer[i]);
	}

	return num_values;
}

static void HwUcIgnoreBorderColorTableAddr([[maybe_unused]] uint32_t cmd_offset,
                                           [[maybe_unused]] uint32_t value) {}

KYTY_HW_UC_PARSER(HwUcSetBorderColorTableAddr) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwUcIgnoreBorderColorTableAddr(cmd_offset + i, buffer[i]);
	}

	return num_values;
}

KYTY_HW_UC_PARSER(HwUcSetGeIndexOffset) {
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::GE_INDX_OFFSET);
	cp->GetUcfg()->SetIndexOffset(buffer[0]);

	return 1;
}

static void HwUcSetGdsOaRegister(CommandProcessor* cp, uint32_t cmd_offset, uint32_t value) {
	EXIT_IF(cp == nullptr);

	auto* ucfg = cp->GetUcfg();

	switch (cmd_offset) {
		case Pm4::GDS_OA_CNTL: ucfg->SetGdsOaCntl(value); break;
		case Pm4::GDS_OA_COUNTER: ucfg->SetGdsOaCounter(value); break;
		case Pm4::GDS_OA_ADDRESS: ucfg->SetGdsOaAddress(value); break;
		default: EXIT("unknown GDS OA register: 0x%08" PRIx32 "\n", cmd_offset);
	}

	const auto                   index = ucfg->GetGdsOaState().GetIndex();
	const auto&                  oa    = ucfg->GetGdsOaCounter(index);
	static std::atomic<uint32_t> log_count {0};
	if (oa.IsCounterEnabled() && log_count.fetch_add(1, std::memory_order_relaxed) < 128) {
		LOGF("GDS_OA: index=%u address_bytes=0x%04" PRIx32 " space=0x%08" PRIx32
		     " crawler=%u crawler_id=%u "
		     "alloc_crawler=%s raw_cntl=0x%08" PRIx32 " raw_counter=0x%08" PRIx32
		     " raw_address=0x%08" PRIx32 "\n",
		     index, oa.GetAddressBytes(), oa.GetSpaceAvailable(), oa.GetCrawlerType(),
		     oa.GetCrawlerId(), oa.IsAllocationCrawlerDisabled() ? "disabled" : "enabled",
		     ucfg->GetGdsOaState().cntl, oa.counter, oa.address);
	}
}

KYTY_HW_UC_PARSER(HwUcSetGdsOaRegisters) {
	auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;

	for (uint32_t i = 0; i < num_values; i++) {
		HwUcSetGdsOaRegister(cp, cmd_offset + i, buffer[i]);
	}

	return num_values;
}

static bool HwUcIsFakeRegister(uint32_t cmd_offset) {
	switch (cmd_offset) {
		case Pm4::FSR_EXTEND_SUBPIXEL_ROUNDING:
		case Pm4::FSR_ALPHA_VALUE0:
		case Pm4::FSR_ALPHA_VALUE1:
		case Pm4::FSR_CONTROL_POINT0:
		case Pm4::FSR_CONTROL_POINT1:
		case Pm4::FSR_CONTROL_POINT2:
		case Pm4::FSR_CONTROL_POINT3:
		case Pm4::FSR_WINDOW0:
		case Pm4::FSR_WINDOW1:
		case Pm4::TEXTURE_GRADIENT_CONTROL:
		case Pm4::MEMORY_MAPPING_MASK:
		case Pm4::UC_NOP: return true;
		default: return false;
	}
}

static void HwUcIgnoreFakeRegister([[maybe_unused]] uint32_t cmd_offset,
                                   [[maybe_unused]] uint32_t value) {}

static bool HwUcTrySetFakeRegister(uint32_t cmd_offset, uint32_t value) {
	if (!HwUcIsFakeRegister(cmd_offset)) {
		return false;
	}

	if (cmd_offset != Pm4::UC_NOP) {
		HwUcIgnoreFakeRegister(cmd_offset, value);
	}

	return true;
}

static bool HwUcTrySetFakeRegisterRange(uint32_t cmd_offset, const uint32_t* buffer,
                                        uint32_t num_values) {
	if (!HwUcIsFakeRegister(cmd_offset)) {
		return false;
	}

	for (uint32_t i = 0; i < num_values; i++) {
		auto reg = cmd_offset + i;
		if (!HwUcTrySetFakeRegister(reg, buffer[i])) {
			EXIT("unsupported fake UC register range at 0x%08" PRIx32 " + %" PRIu32
			     ", value = 0x%08" PRIx32 "\n",
			     cmd_offset, i, buffer[i]);
		}
	}

	return true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
KYTY_CP_OP_PARSER(CpOpAcquireMem) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0055800 && cmd_id != 0xc0061050);

	bool custom = (cmd_id == 0xc0061050);

	uint32_t                  engine       = buffer[0] >> 31u;
	uint32_t                  stall_mode   = (custom ? 1u : engine);
	uint32_t                  cache_action = buffer[0] & 0x7fffffffu;
	uint64_t                  size_lo      = buffer[1];
	uint32_t                  size_hi      = buffer[2];
	uint64_t                  base_lo      = buffer[3];
	uint32_t                  base_hi      = buffer[4];
	uint32_t                  poll         = buffer[5];
	[[maybe_unused]] uint32_t gcr_cntl     = (custom ? buffer[6] : 0);

	uint32_t target_mask     = cache_action & 0x00007FC0u;
	uint32_t extended_action = cache_action & 0x2E000000u;
	uint32_t action =
	    ((cache_action & 0x00C00000u) >> 0x12u) | ((cache_action & 0x00058000u) >> 0xfu);

	if (custom && engine > 1) {
		LOGF("\t warning: custom acquire_mem unsupported engine: %" PRIu32 "\n", engine);
	}

	// EXIT_NOT_IMPLEMENTED(stall_mode != 1);
	EXIT_NOT_IMPLEMENTED(size_hi != 0);
	EXIT_NOT_IMPLEMENTED(base_hi != 0);
	if (poll != 10) {
		LOGF("\t warning: acquire_mem unexpected poll interval: %" PRIu32 "\n", poll);
	}

	switch (cache_action) {
		case 0x00000000: {
			if (custom && gcr_cntl != 0) {
				LOGF("\t custom acquire_mem GCR-only barrier, gcr_cntl = 0x%08" PRIx32
				     ", base = 0x%016" PRIx64 ", size = 0x%016" PRIx64 "\n",
				     gcr_cntl, base_lo << 8u, size_lo << 8u);

				cp->MemoryBarrier();
				if ((gcr_cntl & AcquireGcrGl2Writeback) != 0) {
					cp->SynchronizeGpu();
				}
			}
		} break;
		case 0x00000040:
		case 0x00003fc0:
		case 0x00004000:
		case 0x00007fc0: {
			// target_mask set, no CB/DB action bits. Treat as an ordering barrier.
			EXIT_IF(target_mask != cache_action);
			EXIT_IF(extended_action != 0x00000000);
			EXIT_IF(action != 0x00);

			LOGF("\t temporary: acquire_mem target-mask-only barrier, target_mask = 0x%08" PRIx32
			     ", gcr_cntl = 0x%08" PRIx32 ", base = 0x%016" PRIx64 ", size = 0x%016" PRIx64 "\n",
			     target_mask, gcr_cntl, base_lo << 8u, size_lo << 8u);

			cp->MemoryBarrier();
		} break;
		case 0x02000000: {
			// target_mask:     0x00000000 (none)
			// extended_action: 0x02000000 (FlushAndInvalidateCbCache)
			// action:          0x00 (none)
			EXIT_IF(target_mask != 0x00000000);
			EXIT_IF(extended_action != 0x02000000);
			EXIT_IF(action != 0x00);

			LOGF("\t temporary: acquire_mem CB-cache-only barrier, gcr_cntl = 0x%08" PRIx32
			     ", base = 0x%016" PRIx64 ", size = 0x%016" PRIx64 "\n",
			     gcr_cntl, base_lo << 8u, size_lo << 8u);

			cp->MemoryBarrier();
		} break;
		case 0x04000000: {
			// target_mask:     0x00000000 (none)
			// extended_action: 0x04000000 (FlushAndInvalidateDbCache)
			// action:          0x00 (none)
			EXIT_IF(target_mask != 0x00000000);
			EXIT_IF(extended_action != 0x04000000);
			EXIT_IF(action != 0x00);

			LOGF("\t temporary: acquire_mem DB-cache-only barrier, gcr_cntl = 0x%08" PRIx32
			     ", base = 0x%016" PRIx64 ", size = 0x%016" PRIx64 "\n",
			     gcr_cntl, base_lo << 8u, size_lo << 8u);

			cp->MemoryBarrier();
		} break;
		case 0x04004000:
		case 0x04007fc0: {
			// target_mask:     0x00004000 (Depth Target), 0x00007fc0 (all rt and depth)
			// extended_action: 0x04000000 (FlushAndInvalidateDbCache)
			// action:          0x00 (none)
			EXIT_IF(target_mask != 0x00004000 && target_mask != 0x00007FC0);
			EXIT_IF(extended_action != 0x04000000);
			EXIT_IF(action != 0x00);

			LOGF("\t temporary: acquire_mem DB target barrier, target_mask = 0x%08" PRIx32
			     ", gcr_cntl = 0x%08" PRIx32 ", base = 0x%016" PRIx64 ", size = 0x%016" PRIx64 "\n",
			     target_mask, gcr_cntl, base_lo << 8u, size_lo << 8u);

			if (size_lo != 0) {
				cp->DepthStencilBarrier(base_lo << 8u, size_lo << 8u);
			} else {
				cp->MemoryBarrier();
			}
		} break;
		case 0x02c40040:
		case 0x02c43fc0:
		case 0x02c47fc0: {
			// target_mask:     0x00000040 (rt0), 0x00003fc0 (all rt), 0x00007fc0 (all rt and depth)
			// extended_action: 0x02000000 (FlushAndInvalidateCbCache)
			// action:          0x38 (WriteBackAndInvalidateL1andL2)
			EXIT_IF(target_mask != 0x00000040 && target_mask != 0x00003FC0 &&
			        target_mask != 0x00007FC0);
			EXIT_IF(extended_action != 0x02000000);
			EXIT_IF(action != 0x38);
			EXIT_NOT_IMPLEMENTED(size_lo == 0);
			EXIT_NOT_IMPLEMENTED(base_lo == 0);

			cp->RenderTextureBarrier(base_lo << 8u, size_lo << 8u);
			cp->SynchronizeGpu();
		} break;
		case 0x02003fc0:
		case 0x02007fc0: {
			// target_mask:     0x00003FC0 (all rt), 0x00007fc0 (all rt and depth)
			// extended_action: 0x02000000 (FlushAndInvalidateCbCache)
			// action:          0x00 (none)
			EXIT_IF(target_mask != 0x00003FC0 && target_mask != 0x00007fc0);
			EXIT_IF(extended_action != 0x02000000);
			EXIT_IF(action != 0x00);

			if (size_lo == 0) {
				if (base_lo != 0) {
					LOGF("\t warning: acquire_mem CB-cache barrier with non-zero base");
				}

				cp->MemoryBarrier();
			} else {
				EXIT_NOT_IMPLEMENTED(base_lo == 0);

				cp->RenderTextureBarrier(base_lo << 8u, size_lo << 8u);
			}
		} break;
		case 0x00C40000: {
			// target_mask:     0x00000000 (none)
			// extended_action: 0x00000000 (none)
			// action:          0x38 (WriteBackAndInvalidateL1andL2)
			EXIT_IF(target_mask != 0x00000000);
			EXIT_IF(extended_action != 0x00000000);
			EXIT_IF(action != 0x38);
			EXIT_NOT_IMPLEMENTED(size_lo != 1);
			EXIT_NOT_IMPLEMENTED(base_lo != 0);

			cp->MemoryBarrier();
			cp->SynchronizeGpu();
		} break;
		case 0x00400000: {
			// target_mask:     0x00000000 (none)
			// extended_action: 0x00000000 (none)
			// action:          0x10 (InvalidateL1)
			EXIT_IF(target_mask != 0x00000000);
			EXIT_IF(extended_action != 0x00000000);
			EXIT_IF(action != 0x10);
			EXIT_NOT_IMPLEMENTED(size_lo != 1);
			EXIT_NOT_IMPLEMENTED(base_lo != 0);

			cp->MemoryBarrier();
		} break;
		case 0x04c44000: {
			// target_mask:     0x00004000 (Depth Target)
			// extended_action: 0x04000000 (FlushAndInvalidateDbCache)
			// action:          0x38 (WriteBackAndInvalidateL1andL2)
			EXIT_IF(target_mask != 0x00004000);
			EXIT_IF(extended_action != 0x04000000);
			EXIT_IF(action != 0x38);

			cp->DepthStencilBarrier(base_lo << 8u, size_lo << 8u);
			cp->SynchronizeGpu();
		} break;

		case 0x06000040:
		case 0x06000080:
		case 0x06003fc0:
		case 0x06007fc0: {
			// target_mask:     0x00000040 (rt0), 0x00000080 (rt1), 0x00003fc0 (all rt), 0x00007fc0
			// (all rt and depth) extended_action: 0x06000000 (Flush Cb & Db) action:          0x00
			// (none)
			if (gcr_cntl != 0 && gcr_cntl != 0x280 && gcr_cntl != 0x300) {
				LOGF("\t temporary: acquire_mem CB+DB barrier with unhandled GCR control "
				     "0x%08" PRIx32 "\n",
				     gcr_cntl);
			}

			EXIT_IF(target_mask != 0x00000040 && target_mask != 0x00000080 &&
			        target_mask != 0x00003fc0 && target_mask != 0x00007fc0);
			EXIT_IF(extended_action != 0x06000000);
			EXIT_IF(action != 0x00);

			if (size_lo != 0) {
				if ((target_mask & 0x00003fc0) != 0) {
					cp->RenderTextureBarrier(base_lo << 8u, size_lo << 8u);
				}
				if ((target_mask & 0x00004000) != 0) {
					cp->DepthStencilBarrier(base_lo << 8u, size_lo << 8u);
				}
			} else {
				cp->MemoryBarrier();
			}
		} break;

		default:
			EXIT("unknown barrier: 0x%08" PRIx32 ", 0x%08" PRIx32 ", 0x%08" PRIx32 ", 0x%08" PRIx32
			     "\n",
			     cache_action, target_mask, extended_action, action);
	}

	if (stall_mode == 0) {
		cp->BufferFlush();
		cp->BufferWait();
	}

	return (custom ? 7 : 6);
}

KYTY_CP_OP_PARSER(CpOpDispatchDirect) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(((cmd_id >> 8u) & 0xffu) != Pm4::IT_DISPATCH_DIRECT);
	EXIT_NOT_IMPLEMENTED(KYTY_PM4_LEN(cmd_id) != 5u);

	uint32_t thread_group_x = buffer[0];
	uint32_t thread_group_y = buffer[1];
	uint32_t thread_group_z = buffer[2];
	uint32_t mode           = buffer[3];

	cp->DispatchDirect(thread_group_x, thread_group_y, thread_group_z, mode);

	return KYTY_PM4_LEN(cmd_id) - 1;
}

KYTY_CP_OP_PARSER(CpOpDispatchIndirect) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0011600 && cmd_id != 0xc0021600);

	if (cmd_id == 0xc0021600) {
		struct DispatchIndirectArgs {
			uint32_t thread_group_x;
			uint32_t thread_group_y;
			uint32_t thread_group_z;
		};

		auto* args = reinterpret_cast<const DispatchIndirectArgs*>(
		    buffer[0] | (static_cast<uint64_t>(buffer[1]) << 32u));
		uint32_t mode = buffer[2];

		EXIT_NOT_IMPLEMENTED(args == nullptr);
		cp->DispatchDirect(args->thread_group_x, args->thread_group_y, args->thread_group_z, mode);

		return 3;
	}

	uint32_t data_offset = buffer[0];
	uint32_t mode        = buffer[1];

	cp->DispatchIndirect(data_offset, mode);

	return 2;
}

KYTY_CP_OP_PARSER(CpOpGetLodStats) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0038e00);

	const auto buffer_size = buffer[0];
	auto*      dst         = reinterpret_cast<void*>((buffer[1] & 0xffffffc0u) |
	                                                 (static_cast<uint64_t>(buffer[2]) << 32u));

	if (dst != nullptr && buffer_size != 0) {
		memset(dst, 0, buffer_size);
		// Hack?
		if (buffer_size >= sizeof(uint32_t)) {
			auto* label = static_cast<uint32_t*>(dst);
			*label      = 1;
		}
	}

	return 4;
}

KYTY_CP_OP_PARSER(CpOpDispatchReset) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0001024);

	cp->Reset();

	return 1;
}

KYTY_CP_OP_PARSER(CpOpPfpSyncMe) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0004200);

	return 1;
}

KYTY_CP_OP_PARSER(CpOpSetPredication) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(((cmd_id >> 8u) & 0xffu) != Pm4::IT_SET_PREDICATION);

	auto payload_dw = KYTY_PM4_LEN(cmd_id) - 1u;

	EXIT_NOT_IMPLEMENTED(payload_dw < 2);

	uint32_t           flags                  = 0;
	uint64_t           address_value          = 0;
	constexpr uint32_t predication_flags_mask = 0x00071100u;
	if (payload_dw >= 3 && (buffer[0] & ~predication_flags_mask) == 0 && buffer[2] <= 0x0000ffffu) {
		flags         = buffer[0];
		address_value = (static_cast<uint64_t>(buffer[2]) << 32u) | (buffer[1] & 0xfffffff0u);
	} else {
		flags = buffer[1];
		address_value =
		    (buffer[0] & 0xfffffff0u) | (static_cast<uint64_t>(buffer[1] & 0xffu) << 32u);
	}

	auto  condition       = (flags >> 8u) & 0x1u;
	auto  wait_op         = (flags >> 12u) & 0x1u;
	auto  op              = (flags >> 16u) & 0x7u;
	auto* address         = reinterpret_cast<const volatile void*>(address_value);
	auto  count_in_dwords = 0u;

	cp->SetPredication(condition, op, wait_op, address, count_in_dwords);

	return payload_dw;
}

KYTY_CP_OP_PARSER(CpOpCondExec) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(((cmd_id >> 8u) & 0xffu) != Pm4::IT_COND_EXEC);

	auto payload_dw = KYTY_PM4_LEN(cmd_id) - 1u;

	EXIT_NOT_IMPLEMENTED(payload_dw < 4);

	auto addr       = (static_cast<uint64_t>(buffer[1]) << 32u) | (buffer[0] & 0xfffffffcu);
	auto control    = buffer[0] & 0x3u;
	auto exec_count = buffer[3] & 0x3fffu;

	EXIT_NOT_IMPLEMENTED(control != 0);
	EXIT_NOT_IMPLEMENTED(buffer[2] != 0);
	EXIT_NOT_IMPLEMENTED(addr == 0);
	EXIT_NOT_IMPLEMENTED(payload_dw + exec_count >= dw);

	if (*reinterpret_cast<const volatile uint32_t*>(addr) == 0) {
		return payload_dw + exec_count;
	}

	return payload_dw;
}

KYTY_CP_OP_PARSER(CpOpBranch) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(((cmd_id >> 8u) & 0xffu) != Pm4::IT_INDIRECT_BUFFER);

	auto payload_dw = KYTY_PM4_LEN(cmd_id) - 1u;

	EXIT_NOT_IMPLEMENTED(payload_dw != 13);

	auto* compare_addr = reinterpret_cast<const volatile uint64_t*>(
	    (buffer[1] & 0xfffffff8u) | (static_cast<uint64_t>(buffer[2]) << 32u));
	uint64_t mask        = buffer[3] | (static_cast<uint64_t>(buffer[4]) << 32u);
	uint64_t reference   = buffer[5] | (static_cast<uint64_t>(buffer[6]) << 32u);
	uint32_t mode        = buffer[0] & 0x3u;
	uint32_t function    = (buffer[0] >> 8u) & 0x7u;
	auto*    then_buffer = reinterpret_cast<uint32_t*>((buffer[7] & 0xfffffffcu) |
	                                                   (static_cast<uint64_t>(buffer[8]) << 32u));
	uint32_t then_num_dw = buffer[9] & 0xfffffu;
	auto*    else_buffer = reinterpret_cast<uint32_t*>((buffer[10] & 0xfffffffcu) |
	                                                   (static_cast<uint64_t>(buffer[11]) << 32u));
	uint32_t else_num_dw = buffer[12] & 0xfffffu;

	EXIT_NOT_IMPLEMENTED(compare_addr == nullptr);
	EXIT_NOT_IMPLEMENTED(mode != 1 && mode != 2);
	EXIT_NOT_IMPLEMENTED(function > 6);
	EXIT_NOT_IMPLEMENTED(then_buffer == nullptr || then_num_dw == 0);

	const bool take_then = TestWaitRegMemValue(*compare_addr, reference, mask, function);
	LOGF("\t branch: take=%u then=0x%016" PRIx64 "/%" PRIu32 " else=0x%016" PRIx64 "/%" PRIu32 "\n",
	     take_then ? 1u : 0u, reinterpret_cast<uint64_t>(then_buffer), then_num_dw,
	     reinterpret_cast<uint64_t>(else_buffer), else_num_dw);

	if (take_then) {
		cp->Run(then_buffer, then_num_dw);
	} else if (mode == 2 && else_num_dw != 0) {
		EXIT_NOT_IMPLEMENTED(else_buffer == nullptr);
		cp->Run(else_buffer, else_num_dw);
	}

	return payload_dw;
}

static uint8_t CopyDataDstToDma(uint32_t dst) {
	switch (dst) {
		case 2:
		case 4:
		case 5: return 3;
		case 3:
		case 6:
		case 7: return 1;
		default: break;
	}

	EXIT("unsupported copyData destination selector 0x%02" PRIx32 "\n", dst);
}

static uint8_t CopyDataSrcToDma(uint32_t src) {
	switch (src) {
		case 2:
		case 4:
		case 5: return 3;
		case 3:
		case 6:
		case 7: return 1;
		case 5 << 1:
		case (5 << 1) | 1: return 2;
		default: break;
	}

	EXIT("unsupported copyData source selector 0x%02" PRIx32 "\n", src);
}

KYTY_CP_OP_PARSER(CpOpCopyData) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != KYTY_PM4(6, Pm4::IT_COPY_DATA, 0u));

	const uint32_t control       = buffer[0];
	const uint32_t src_sel       = ((control & 0xfu) << 1u) | ((control >> 30u) & 0x1u);
	const uint32_t dst_sel       = ((control >> 8u) & 0xfu) << 1u;
	const uint8_t  src_cache     = static_cast<uint8_t>((control >> 13u) & 0x3u);
	const uint8_t  dst_cache     = static_cast<uint8_t>((control >> 25u) & 0x3u);
	const uint8_t  write_confirm = static_cast<uint8_t>((control >> 20u) & 0x1u);
	const uint32_t num_bytes     = ((control >> 16u) & 0x1u) != 0 ? 8u : 4u;
	const uint64_t src           = buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u);
	const uint64_t dst           = buffer[3] | (static_cast<uint64_t>(buffer[4]) << 32u);
	if (src_sel == (9u << 1u)) {
		if (dst_sel != (2u << 1u) || dst == 0 || (dst & (num_bytes - 1u)) != 0) {
			EXIT("unsupported reference-clock copyData, src_sel=0x%02" PRIx32
			     " dst_sel=0x%02" PRIx32 " dst=0x%016" PRIx64 " size=%u\n",
			     src_sel, dst_sel, dst, num_bytes);
		}
		cp->WriteReferenceClock(dst, num_bytes);
		return 5;
	}
	const auto dma_src = CopyDataSrcToDma(src_sel);
	if (dma_src == 2 && num_bytes == 8) {
		EXIT("unsupported 64-bit immediate copyData\n");
	}

	cp->DmaData(0, CopyDataDstToDma(dst_sel), dst_cache, dst, dma_src, src_cache, src, num_bytes, 1,
	            write_confirm, 1);

	return 5;
}

KYTY_CP_OP_PARSER(CpOpDmaData) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != KYTY_PM4(7, Pm4::IT_DMA_DATA, 0u));

	const uint32_t control  = buffer[0];
	const uint32_t control2 = buffer[5];
	const uint64_t src      = buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u);
	const uint64_t dst      = buffer[3] | (static_cast<uint64_t>(buffer[4]) << 32u);

	if (control == 0x60000000 && dst == 0x0003022c && (control2 >> 21u) == 0x141u) {
		auto* addr = reinterpret_cast<void*>(src);

		cp->PrefetchL2(addr, control2 & 0x1fffffu);
		return 6;
	}

	const uint8_t engine           = static_cast<uint8_t>(control & 0x1u);
	const uint8_t src_cache_policy = static_cast<uint8_t>((control >> 13u) & 0x3u);
	const uint8_t dst_sel          = static_cast<uint8_t>(
	    ((control >> 20u) & 0x3u) | ((control2 >> 25u) & 0x4u) | ((control2 >> 26u) & 0x8u));
	const uint8_t dst_cache_policy = static_cast<uint8_t>((control >> 25u) & 0x3u);
	const uint8_t src_sel          = static_cast<uint8_t>(
	    ((control >> 29u) & 0x3u) | ((control2 >> 24u) & 0x4u) | ((control2 >> 25u) & 0x8u));
	const uint8_t  block_engine  = static_cast<uint8_t>((control >> 31u) & 0x1u);
	const uint8_t  wait_previous = static_cast<uint8_t>((control2 >> 30u) & 0x1u);
	const uint8_t  write_confirm = static_cast<uint8_t>((control2 >> 31u) & 0x1u);
	const uint32_t num_bytes     = control2 & 0x03ffffffu;

	cp->DmaData(engine, dst_sel, dst_cache_policy, dst, src_sel, src_cache_policy, src, num_bytes,
	            wait_previous, write_confirm, block_engine);

	return 6;
}

KYTY_CP_OP_PARSER(CpOpDrawIndex) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0073a00 && cmd_id != 0xc0042700);

	if (cmd_id == 0xc0073a00) {
		uint32_t index_count = buffer[0];
		auto*    index_addr =
		    reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
		uint32_t max_instance_count = buffer[3];
		auto*    object_ids =
		    reinterpret_cast<void*>(buffer[4] | (static_cast<uint64_t>(buffer[5]) << 32u));
		uint32_t instance_count = buffer[6];
		uint32_t flags          = buffer[7];

		EXIT_NOT_IMPLEMENTED(instance_count > max_instance_count);
		EXIT_NOT_IMPLEMENTED((flags & ~0xa0u) != 0);

		cp->DrawIndex(index_count, index_addr, 0, 1, instance_count, object_ids);

		return 8;
	}

	if (cmd_id == 0xc0042700) {
		uint32_t max_index_count = buffer[0];
		auto*    index_addr =
		    reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
		uint32_t index_count = buffer[3];
		uint32_t flags       = buffer[4];

		EXIT_NOT_IMPLEMENTED(index_count > max_index_count);
		EXIT_NOT_IMPLEMENTED((flags & ~0x20u) != 0);

		cp->DrawIndex(index_count, index_addr, 0, 1);

		return 5;
	}

	return 1;
}

KYTY_CP_OP_PARSER(CpOpDrawIndirect) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0032400 && cmd_id != 0xc0032500);

	const auto data_offset    = buffer[0];
	const auto draw_initiator = buffer[3];
	const bool indexed        = (cmd_id == 0xc0032500);

	cp->DrawIndirect(data_offset, draw_initiator, indexed);

	return 4;
}

KYTY_CP_OP_PARSER(CpOpDrawIndirectMulti) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0082c00 && cmd_id != 0xc0083800);

	const auto data_offset        = buffer[0];
	const auto count_indirect     = (buffer[3] >> 30u) & 0x1u;
	const auto max_count_or_count = buffer[4];
	auto*      count_addr         = reinterpret_cast<const volatile uint32_t*>(
	    buffer[5] | (static_cast<uint64_t>(buffer[6]) << 32u));
	const auto stride_in_bytes = buffer[7];
	const auto draw_initiator  = buffer[8];
	const bool indexed         = (cmd_id == 0xc0083800);

	if (count_indirect == 0) {
		count_addr = nullptr;
	}

	cp->DrawIndirectMulti(data_offset, max_count_or_count, count_addr, stride_in_bytes,
	                      draw_initiator, indexed);

	return 9;
}

KYTY_CP_OP_PARSER(CpOpDrawIndexOffset) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0033500);

	uint32_t max_index_size = buffer[0];
	uint32_t index_offset   = buffer[1];
	uint32_t index_count    = buffer[2];
	uint32_t flags          = buffer[3];

	EXIT_NOT_IMPLEMENTED(index_count > max_index_size);
	EXIT_NOT_IMPLEMENTED((flags & ~0x20u) != 0);

	cp->DrawIndexOffset(index_offset, index_count, 0);

	return 4;
}

KYTY_CP_OP_PARSER(CpOpDrawIndexAuto) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0012d00);

	uint32_t index_count = buffer[0];
	uint32_t flags       = buffer[1];

	EXIT_NOT_IMPLEMENTED((flags & ~0x22u) != 0);

	cp->DrawIndexAuto(index_count, 0);

	return 2;
}

KYTY_CP_OP_PARSER(CpOpClearState) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0001200);
	EXIT_NOT_IMPLEMENTED((buffer[0] & ~0xfu) != 0);

	cp->Reset();

	return 1;
}

KYTY_CP_OP_PARSER(CpOpDumpConstRam) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0038300);

	auto  offset = buffer[0];
	auto  dw_num = buffer[1];
	auto* dst = reinterpret_cast<uint32_t*>(buffer[2] | (static_cast<uint64_t>(buffer[3]) << 32u));

	EXIT_NOT_IMPLEMENTED(dw_num >= 0x3000);
	EXIT_NOT_IMPLEMENTED(offset > 0xbffc);
	EXIT_NOT_IMPLEMENTED((offset & 0x3u) != 0);

	cp->DumpConstRam(dst, offset, dw_num);

	return 4;
}

KYTY_CP_OP_PARSER(CpOpEventWrite) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(((cmd_id >> 8u) & 0xffu) != Pm4::IT_EVENT_WRITE);

	uint32_t event_index = (buffer[0] >> 8u) & 0x7u;
	uint32_t event_type  = (buffer[0]) & 0x3fu;

	cp->TriggerEvent(event_type, event_index);

	return KYTY_PM4_LEN(cmd_id) - 1u;
}

KYTY_CP_OP_PARSER(CpOpEventWriteEop) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0044700);

	uint32_t cache_policy       = (buffer[0] >> 25u) & 0x3u;
	uint32_t event_write_dest   = ((buffer[0] >> 23u) & 0x10u) | ((buffer[2] >> 16u) & 0x01u);
	uint32_t eop_event_type     = (buffer[0]) & 0x3fu;
	uint32_t cache_action       = (buffer[0] >> 12u) & 0x3fu;
	uint32_t event_index        = (buffer[0] >> 8u) & 0x7u;
	uint32_t event_write_source = ((buffer[2] >> 29u) & 0x7u);
	uint32_t interrupt_selector = (buffer[2] >> 24u) & 0x7u;
	auto*    dst_gpu_addr =
	    reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2] & 0xffffu) << 32u));
	uint64_t value = (buffer[3] | (static_cast<uint64_t>(buffer[4]) << 32u));

	cp->WriteAtEndOfPipe64(cache_policy, event_write_dest, eop_event_type, cache_action,
	                       event_index, event_write_source, dst_gpu_addr, value,
	                       interrupt_selector);

	return 5;
}

KYTY_CP_OP_PARSER(CpOpEventWriteEos) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0034802);

	uint32_t cache_policy       = (buffer[0] >> 25u) & 0x3u;
	uint32_t event_write_dest   = 0;
	uint32_t eop_event_type     = (buffer[0]) & 0x3fu;
	uint32_t cache_action       = (buffer[0] >> 12u) & 0x3fu;
	uint32_t event_index        = (buffer[0] >> 8u) & 0x7u;
	uint32_t event_write_source = ((buffer[2] >> 29u) & 0x7u);
	uint32_t interrupt_selector = (buffer[2] >> 24u) & 0x7u;

	auto* dst_gpu_addr =
	    reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2] & 0xffffu) << 32u));
	uint32_t value = buffer[3];

	cp->WriteAtEndOfPipe32(cache_policy, event_write_dest, eop_event_type, cache_action,
	                       event_index, event_write_source, dst_gpu_addr, value,
	                       interrupt_selector);

	return 4;
}

KYTY_CP_OP_PARSER(CpOpFlip) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc004105c);

	CommandProcessor::FlipInfo f;

	f.handle    = static_cast<int>(buffer[0]);
	f.index     = static_cast<int>(buffer[1]);
	f.flip_mode = static_cast<int>(buffer[2]);
	f.flip_arg  = static_cast<int64_t>(buffer[3] | (static_cast<uint64_t>(buffer[4]) << 32u));

	cp->SetFlip(f);
	cp->Flip();
	return 5;
}

KYTY_CP_OP_PARSER(CpOpIncrementCeCounter) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0008400);
	EXIT_NOT_IMPLEMENTED(buffer[0] != 1);

	cp->IncremenetCe();

	return 1;
}

KYTY_CP_OP_PARSER(CpOpIncrementDeCounter) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0008500);
	EXIT_NOT_IMPLEMENTED(buffer[0] != 0);

	cp->IncremenetDe();

	return 1;
}

KYTY_CP_OP_PARSER(CpOpIndexType) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0002A00);

	cp->SetIndexType(buffer[0]);

	return 1;
}

KYTY_CP_OP_PARSER(CpOpIndexBufferSize) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0001300);

	cp->SetIndexBufferSize(buffer[0]);

	return 1;
}

KYTY_CP_OP_PARSER(CpOpIndexBase) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0012600);

	auto index_base_addr = buffer[0] | (static_cast<uint64_t>(buffer[1]) << 32u);

	cp->SetIndexBaseAddress(index_base_addr);

	return 2;
}

KYTY_CP_OP_PARSER(CpOpSetBase) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(((cmd_id >> 8u) & 0xffu) != Pm4::IT_SET_BASE);
	EXIT_NOT_IMPLEMENTED(KYTY_PM4_LEN(cmd_id) != 4u);

	auto base_index = buffer[0] & 0xfu;
	auto base_addr  = (buffer[1] & ~0x7ull) | (static_cast<uint64_t>(buffer[2] & 0xffffu) << 32u);
	// libSceAgc::setBaseIndirectArgs encodes Gnmp::ShaderType in PM4 header bit 1.
	auto shader_type = (cmd_id >> 1u) & 0x3u;

	EXIT_NOT_IMPLEMENTED(base_index != 1u);
	EXIT_NOT_IMPLEMENTED(shader_type > 1u);

	if (shader_type == 0u) {
		cp->SetDrawIndirectArgsBaseAddress(base_addr);
	} else {
		cp->SetDispatchIndirectArgsBaseAddress(base_addr);
	}

	return 3;
}

KYTY_CP_OP_PARSER(CpOpIndirectBuffer) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(((cmd_id >> 8u) & 0xffu) != Pm4::IT_INDIRECT_BUFFER);

	if (KYTY_PM4_LEN(cmd_id) == 14u) {
		return CpOpBranch(cp, cmd_id, buffer, dw, num_dw);
	}

	EXIT_NOT_IMPLEMENTED(KYTY_PM4_LEN(cmd_id) != 4u);

	const uint32_t control = buffer[2];

	const uint32_t control_flags = control & 0x0fe00000u;
	if (control_flags != 0x0f200000u) {
		LOGF("\t temporary: accepting PS5 indirect buffer control flags = 0x%08" PRIx32 "\n",
		     control_flags);
	}

	auto* indirect_buffer =
	    reinterpret_cast<uint32_t*>(buffer[0] | (static_cast<uint64_t>(buffer[1]) << 32u));
	uint32_t                     indirect_num_dw = control & 0xfffffu;
	static std::atomic<uint32_t> indirect_log_count {0};
	if (indirect_log_count.fetch_add(1) < 128) {
		LOGF("\t indirect buffer: addr=0x%016" PRIx64 ", num_dw=%" PRIu32 ", control=0x%08" PRIx32
		     "\n",
		     reinterpret_cast<uint64_t>(indirect_buffer), indirect_num_dw, control);
	}

	if (indirect_num_dw == 0) {
		return 3;
	}
	if (indirect_buffer == nullptr) {
		EXIT("indirect command buffer has null address, num_dw = %" PRIu32
		     ", control = 0x%08" PRIx32 "\n",
		     indirect_num_dw, buffer[2]);
	}

	GraphicsDbgDumpDcb("ci", indirect_num_dw, indirect_buffer);

	cp->Run(indirect_buffer, indirect_num_dw);

	return 3;
}

KYTY_CP_OP_PARSER(CpOpIndirectCxRegs) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(((cmd_id >> 8u) & 0xffu) != Pm4::IT_SET_CONTEXT_REG_INDIRECT);
	EXIT_NOT_IMPLEMENTED(KYTY_PM4_LEN(cmd_id) != 5u);

	auto* indirect_buffer =
	    reinterpret_cast<uint32_t*>((static_cast<uint64_t>(buffer[0]) & 0xfffffffcu) |
	                                (static_cast<uint64_t>(buffer[1]) << 32u));
	uint32_t indirect_num_dw = buffer[3] & 0x3fffu;

	if (indirect_num_dw == 0) {
		return KYTY_PM4_LEN(cmd_id) - 1u;
	}
	if (indirect_buffer == nullptr) {
		EXIT("indirect CX registers have null address, num_regs = %" PRIu32 "\n", indirect_num_dw);
	}
	for (uint32_t i = 0; i < indirect_num_dw; i++, indirect_buffer += 2) {
		auto cmd_offset = indirect_buffer[0];
		auto value      = indirect_buffer[1];

		if (HwCtxTrySetFakeRegister(cmd_offset, value)) {
			continue;
		}

		if (cmd_offset == 0xffffffffu) {
			static bool logged = false;
			if (!logged) {
				LOGF("\t temporary: skipping indirect CX sentinel pair offset = 0xffffffff, value "
				     "= 0x%08" PRIx32 "\n",
				     value);
				logged = true;
			}
			continue;
		}

		if (cmd_offset >= Pm4::CX_NUM) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
				LOGF("\t temporary: skipping unknown indirect CX extended offset = 0x%08" PRIx32
				     ", value = 0x%08" PRIx32 "\n",
				     cmd_offset, value);
			}
			continue;
		}

		auto pfunc = g_hw_ctx_indirect_func[cmd_offset & (Pm4::CX_NUM - 1)];

		if (pfunc == nullptr) {
			EXIT("unknown cx reg at %05" PRIx32 ": 0x%" PRIx32 "\n", num_dw - dw, cmd_offset);
		}

		pfunc(cp, cmd_offset, value);
	}

	return KYTY_PM4_LEN(cmd_id) - 1u;
}

KYTY_CP_OP_PARSER(CpOpIndirectShRegs) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(((cmd_id >> 8u) & 0xffu) != Pm4::IT_SET_SH_REG_INDIRECT);
	EXIT_NOT_IMPLEMENTED(KYTY_PM4_LEN(cmd_id) != 5u);

	auto* indirect_buffer =
	    reinterpret_cast<uint32_t*>((static_cast<uint64_t>(buffer[0]) & 0xfffffffcu) |
	                                (static_cast<uint64_t>(buffer[1]) << 32u));
	uint32_t indirect_num_dw = buffer[3] & 0x3fffu;

	if (indirect_num_dw == 0) {
		return KYTY_PM4_LEN(cmd_id) - 1u;
	}
	if (indirect_buffer == nullptr) {
		EXIT("indirect SH registers have null address, num_regs = %" PRIu32 "\n", indirect_num_dw);
	}
	const auto indirect_address = reinterpret_cast<uint64_t>(indirect_buffer);

	for (uint32_t i = 0; i < indirect_num_dw; i++, indirect_buffer += 2) {
		auto raw_cmd_offset = indirect_buffer[0];
		auto cmd_offset     = NormalizeRegisterOffset(raw_cmd_offset);
		auto value          = indirect_buffer[1];

		// Not sure if this is correct
		if (raw_cmd_offset != cmd_offset) {
			LOGF_COLOR(Log::Color::Red,
			           "\t temporary: normalized indirect SH register offset 0x%08" PRIx32
			           " -> 0x%08" PRIx32 "\n",
			           raw_cmd_offset, cmd_offset);
		}

		// Not sure if this is correct
		if (cmd_offset == Pm4::SH_NOP || raw_cmd_offset == 0xffffffffu) {
			continue;
		}

		if (cmd_offset >= Pm4::SH_NUM) {
			EXIT("unsupported indirect SH register offset 0x%08" PRIx32 " (raw 0x%08" PRIx32
			     "), value = 0x%08" PRIx32 "\n",
			     cmd_offset, raw_cmd_offset, value);
		}

		auto pfunc = g_hw_sh_indirect_func[cmd_offset];

		if (pfunc == nullptr) {
			LOGF("unknown indirect SH register: index=%" PRIu32 "/%" PRIu32 ", regs=0x%016" PRIx64
			     ", offset=0x%08" PRIx32 ", value=0x%08" PRIx32 "\n",
			     i, indirect_num_dw, indirect_address, cmd_offset, value);
			auto* dump_regs = indirect_buffer - i * 2;
			for (uint32_t j = 0; j < indirect_num_dw && j < 16; j++) {
				LOGF("\t sh_indirect[%" PRIu32 "] offset=0x%08" PRIx32 ", value=0x%08" PRIx32 "\n",
				     j, dump_regs[j * 2], dump_regs[j * 2 + 1]);
			}
			EXIT("unknown sh reg at %05" PRIx32 ": 0x%" PRIx32 "\n", num_dw - dw, cmd_offset);
		}

		pfunc(cp, cmd_offset, value);
	}

	return KYTY_PM4_LEN(cmd_id) - 1u;
}

KYTY_CP_OP_PARSER(CpOpIndirectUcRegs) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(((cmd_id >> 8u) & 0xffu) != Pm4::IT_SET_UCONFIG_REG_INDIRECT);
	EXIT_NOT_IMPLEMENTED(KYTY_PM4_LEN(cmd_id) != 5u);

	auto* indirect_buffer =
	    reinterpret_cast<uint32_t*>((static_cast<uint64_t>(buffer[0]) & 0xfffffffcu) |
	                                (static_cast<uint64_t>(buffer[1]) << 32u));
	uint32_t indirect_num_dw = buffer[3] & 0x3fffu;

	if (indirect_num_dw == 0) {
		return KYTY_PM4_LEN(cmd_id) - 1u;
	}
	if (indirect_buffer == nullptr) {
		EXIT("indirect UC registers have null address, num_regs = %" PRIu32 "\n", indirect_num_dw);
	}
	for (uint32_t i = 0; i < indirect_num_dw; i++, indirect_buffer += 2) {
		auto raw_cmd_offset = indirect_buffer[0];
		auto cmd_offset     = NormalizeRegisterOffset(raw_cmd_offset);
		auto value          = indirect_buffer[1];

		// Not sure if this is correct
		if (raw_cmd_offset != cmd_offset) {
			LOGF_COLOR(Log::Color::Red,
			           "\t temporary: normalized indirect UC register offset 0x%08" PRIx32
			           " -> 0x%08" PRIx32 "\n",
			           raw_cmd_offset, cmd_offset);
		}
		if (HwUcTrySetFakeRegister(cmd_offset, value)) {
			continue;
		}
		if (cmd_offset < Pm4::CX_NUM) {
			auto ctx_func = g_hw_ctx_indirect_func[cmd_offset & (Pm4::CX_NUM - 1)];
			if (ctx_func != nullptr) {
				ctx_func(cp, cmd_offset, value);
				continue;
			}
		}
		if (cmd_offset >= Pm4::UC_NUM) {
			EXIT("unsupported indirect UC register offset 0x%08" PRIx32 " (raw 0x%08" PRIx32
			     "), value = 0x%08" PRIx32 "\n",
			     cmd_offset, raw_cmd_offset, value);
		}

		auto pfunc = g_hw_uc_indirect_func[cmd_offset & (Pm4::UC_NUM - 1)];

		if (pfunc == nullptr) {
			auto* dump_regs = indirect_buffer - i * 2;
			for (uint32_t j = 0; j < indirect_num_dw && j < 16; j++) {
				LOGF("\t uc_indirect[%" PRIu32 "] offset=0x%08" PRIx32 ", value=0x%08" PRIx32 "\n",
				     j, dump_regs[j * 2], dump_regs[j * 2 + 1]);
			}
			EXIT("unknown uc reg at %05" PRIx32 ": 0x%" PRIx32 "\n", num_dw - dw, cmd_offset);
		}
		pfunc(cp, cmd_offset, value);
	}

	return KYTY_PM4_LEN(cmd_id) - 1u;
}

KYTY_CP_OP_PARSER(CpOpMarker) {
	KYTY_PROFILER_FUNCTION();

	// EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0001000);

	uint32_t id     = buffer[0] & 0xfffu;
	uint32_t align  = (buffer[0] >> 12u) & 0xfu;
	uint32_t len_dw = ((cmd_id >> 16u) & 0x3fffu);

	switch (id) {
		case 0x0: cp->SetEmbeddedDataMarker(buffer + 1, len_dw, align); break;
		case 0x4: cp->SetUserDataMarker(HW::UserSgprType::Vsharp); break;
		case 0xd: cp->SetUserDataMarker(HW::UserSgprType::Region); break;
		case 0x777: {
			cp->Flip();
			break;
		}
		case 0x778: {
			auto* addr =
			    reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
			uint32_t value = buffer[3];
			cp->Flip(addr, value);
			break;
		}
		case 0x781: {
			auto* addr =
			    reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
			uint32_t value          = buffer[3];
			uint32_t eop_event_type = buffer[4];
			uint32_t cache_action   = buffer[5];
			cp->FlipWithInterrupt(eop_event_type, cache_action, addr, value);
			break;
		}
		default: EXIT("unknown marker at %05" PRIx32 ": 0x%" PRIx32 "\n", num_dw - dw, id);
	}

	return len_dw + 1;
}

KYTY_CP_OP_PARSER(CpOpNop) {
	KYTY_PROFILER_FUNCTION();

	auto r = KYTY_PM4_R(cmd_id);

	if (r == Pm4::R_ZERO) {
		if ((buffer[0] & 0xffff0000u) == 0x68750000u) {
			return CpOpMarker(cp, cmd_id, buffer, dw, num_dw);
		}
		return KYTY_PM4_LEN(cmd_id) - 1;
	}

	auto cp_op = g_cp_op_custom_func[r];

	if (cp_op != nullptr) {
		return cp_op(cp, cmd_id, buffer, dw, num_dw);
	}

	EXIT("unknown custom code at 0x%05" PRIx32 ": 0x%02" PRIx32 "\n", num_dw - dw, r);

	return 0;
}

KYTY_CP_OP_PARSER(CpOpNumInstances) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0002f00);

	cp->SetNumInstances(buffer[0]);

	return 1;
}

KYTY_CP_OP_PARSER(CpOpPopMarker) {
	KYTY_PROFILER_FUNCTION();

	auto dw_num = (cmd_id >> 16u) & 0x3fffu;

	static std::atomic<uint32_t> pop_marker_log_count {0};
	if (pop_marker_log_count.fetch_add(1) < 64) {
		LOGF("Pop marker\n");
	}

	cp->PopMarker();

	return dw_num + 1;
}

KYTY_CP_OP_PARSER(CpOpPushMarker) {
	KYTY_PROFILER_FUNCTION();

	auto dw_num = (cmd_id >> 16u) & 0x3fffu;

	const char* str = reinterpret_cast<const char*>(buffer);

	static std::atomic<uint32_t> push_marker_log_count {0};
	if (push_marker_log_count.fetch_add(1) < 128) {
		LOGF("Push marker: %s\n", str);
	}

	cp->PushMarker(str);

	return dw_num + 1;
}

KYTY_CP_OP_PARSER(CpOpReleaseMem) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0061060);

	uint32_t cache_policy       = (buffer[0] >> 25u) & 0x3u;
	uint32_t eop_event_type     = buffer[0] & 0x3fu;
	uint32_t event_index        = (buffer[0] >> 8u) & 0x7u;
	uint32_t gcr_cntl           = (buffer[0] >> 12u) & 0xfffu;
	uint32_t release_dst        = (buffer[1] >> 16u) & 0x3u;
	uint32_t event_write_dest   = 0;
	uint32_t data_sel           = (buffer[1] >> 29u) & 0x7u;
	uint32_t interrupt_selector = (buffer[1] >> 24u) & 0x7u;
	auto*    dst_gpu_addr =
	    reinterpret_cast<void*>(buffer[2] | (static_cast<uint64_t>(buffer[3]) << 32u));
	uint64_t value                = buffer[4] | (static_cast<uint64_t>(buffer[5]) << 32u);
	uint32_t interrupt_context_id = buffer[6] & 0x07ffffffu;

	constexpr uint32_t ReleaseMemDstMemory = 0;
	constexpr uint32_t ReleaseMemDstTcL2   = 1;
	EXIT_NOT_IMPLEMENTED(release_dst != ReleaseMemDstMemory && release_dst != ReleaseMemDstTcL2);
	EXIT_NOT_IMPLEMENTED(data_sel != 0 && data_sel != 1 && data_sel != 2 && data_sel != 3 &&
	                     data_sel != 5);

	LogUnknownReleaseMemGcr(gcr_cntl);

	const bool gl2_writeback = ((gcr_cntl & GcrGl2Writeback) != 0);

	auto trigger_interrupt = [&]() {
		switch (interrupt_selector) {
			case 0x00:
			case 0x03: break;
			case 0x01:
			case 0x02:
			case 0x04: cp->TriggerEopEventAtEndOfPipe(interrupt_context_id); break;
			default: EXIT("unknown release_mem interrupt selector\n");
		}
	};

	if (data_sel == 0 || interrupt_selector == 4) {
		if (eop_event_type != 0x28 || gcr_cntl != 0) {
			cp->MemoryBarrier();
		}

		if (gl2_writeback) {
			cp->SynchronizeGpu();
		}

		trigger_interrupt();

		return 7;
	}

	if (release_dst == ReleaseMemDstMemory && dst_gpu_addr == nullptr) {
		if (eop_event_type != 0x28 || gcr_cntl != 0) {
			cp->MemoryBarrier();
		}

		if (gl2_writeback) {
			cp->SynchronizeGpu();
		}

		trigger_interrupt();

		return 7;
	}

	if (ReleaseMemGcrNeedsBarrier(eop_event_type, gcr_cntl)) {
		cp->MemoryBarrier();
	}

	auto cache_action = ReleaseMemCacheActionFromGcr(gcr_cntl);
	cache_policy      = 0;

	if (data_sel == 1) {
		static std::atomic<uint32_t> log_count {0};
		const auto                   dst_addr = reinterpret_cast<uint64_t>(dst_gpu_addr);
		if ((dst_addr & 0xffffffffff000000ull) == 0x0000001021000000ull || log_count.load() < 64) {
			if (log_count.fetch_add(1) < 256) {
				LOGF(
				    "\t runtime release_mem32: dst=0x%016" PRIx64 ", value=0x%08" PRIx32
				    ", action=0x%02" PRIx32 ", gcr=0x%04" PRIx32 ", cache_action=0x%08" PRIx32 "\n",
				    dst_addr, static_cast<uint32_t>(value), eop_event_type, gcr_cntl, cache_action);
			}
		}

		eop_event_type    = 0x2f;
		event_index       = 6;
		auto event_source = 2u;

		cp->WriteAtEndOfPipe32(
		    cache_policy, event_write_dest, eop_event_type, cache_action, event_index, event_source,
		    dst_gpu_addr, static_cast<uint32_t>(value), interrupt_selector, interrupt_context_id);
		cp->BufferFlush();

		return 7;
	}

	if (data_sel == 5) {
		if (gl2_writeback) {
			cp->SynchronizeGpu();
			cache_action = 0;
		}

		eop_event_type    = 0x2f;
		event_index       = 6;
		auto event_source = 1u;

		cp->WriteAtEndOfPipe32(
		    cache_policy, event_write_dest, eop_event_type, cache_action, event_index, event_source,
		    dst_gpu_addr, static_cast<uint32_t>(value), interrupt_selector, interrupt_context_id);
		cp->BufferFlush();

		return 7;
	}

	const bool keep_native_event_index =
	    (eop_event_type == 0x04 && cache_action == 0x00) ||
	    (eop_event_type == 0x2f && event_index == 0x06 && cache_action == 0x38);

	if (data_sel == 2) {
		if (!keep_native_event_index) {
			event_index = 0;
		}
	} else {
		EXIT_IF(data_sel != 3);
		if (!keep_native_event_index) {
			event_index = 0;
		}
		data_sel = 4;
	}

	cp->WriteAtEndOfPipe64(cache_policy, event_write_dest, eop_event_type, cache_action,
	                       event_index, data_sel, dst_gpu_addr, value, interrupt_selector,
	                       interrupt_context_id);

	return 7;
}

KYTY_CP_OP_PARSER(CpOpSetContextReg) {
	KYTY_PROFILER_FUNCTION();

	auto cmd_offset = NormalizeRegisterOffset(buffer[0]);

	if (HwCtxTrySetFakeRegister(cmd_offset, buffer[1])) {
		return 2;
	}

	if (cmd_offset >= Pm4::CX_NUM) {
		EXIT("unknown extended context register\n\t%05" PRIx32 ":\n\tcmd_id = %08" PRIx32
		     "\n\tcmd_offset = %08" PRIx32 "\n\tvalue = %08" PRIx32 "\n",
		     num_dw - dw, cmd_id, cmd_offset, buffer[1]);
	}

	auto pfunc = g_hw_ctx_func[cmd_offset & (Pm4::CX_NUM - 1)];

	if (pfunc == nullptr) {
		auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;
		bool handled    = true;
		for (uint32_t i = 0; i < num_values; i++) {
			if (cmd_offset + i >= Pm4::CX_NUM ||
			    g_hw_ctx_indirect_func[(cmd_offset + i) & (Pm4::CX_NUM - 1)] == nullptr) {
				handled = false;
				break;
			}
		}
		if (handled) {
			for (uint32_t i = 0; i < num_values; i++) {
				g_hw_ctx_indirect_func[(cmd_offset + i) & (Pm4::CX_NUM - 1)](cp, cmd_offset + i,
				                                                             buffer[1 + i]);
			}
			return num_values + 1u;
		}
		EXIT("unknown context register\n\t%05" PRIx32 ":\n\tcmd_id = %08" PRIx32
		     "\n\tcmd_offset = %08" PRIx32 "\n",
		     num_dw - dw, cmd_id, cmd_offset);
	}

	auto s = pfunc(cp, cmd_id, cmd_offset, buffer + 1, dw);

	EXIT_IF(s == 0);

	return s + 1;
}

KYTY_CP_OP_PARSER(CpOpSetShaderReg) {
	KYTY_PROFILER_FUNCTION();

	auto cmd_offset = buffer[0];
	if (cmd_offset == Pm4::SH_NOP) {
		return 2;
	}

	EXIT_NOT_IMPLEMENTED(cmd_offset >= Pm4::SH_NUM);

	auto pfunc = g_hw_sh_func[cmd_offset];

	if (pfunc == nullptr) {
		EXIT("unknown shader register\n\t%05" PRIx32 ":\n\tcmd_id = %08" PRIx32
		     "\n\tcmd_offset = %08" PRIx32 "\n",
		     num_dw - dw, cmd_id, cmd_offset);
	}

	auto s = pfunc(cp, cmd_id, cmd_offset, buffer + 1, dw);
	EXIT_IF(s == 0);
	return s + 1;
}

KYTY_CP_OP_PARSER(CpOpSetUconfigReg) {
	KYTY_PROFILER_FUNCTION();

	auto raw_cmd_offset = buffer[0];
	auto cmd_offset     = NormalizeRegisterOffset(raw_cmd_offset);

	if (((cmd_id >> 8u) & 0xffu) == Pm4::IT_SET_UCONFIG_REG_INDEX) {
		cmd_offset = raw_cmd_offset & 0x0fffffffu;
	}

	if (raw_cmd_offset != cmd_offset) {
		LOGF_COLOR(Log::Color::Red,
		           "\t temporary: normalized UC register offset 0x%08" PRIx32 " -> 0x%08" PRIx32
		           "\n",
		           raw_cmd_offset, cmd_offset);
	}
	if (HwUcTrySetFakeRegisterRange(cmd_offset, buffer + 1, KYTY_PM4_LEN(cmd_id) - 2u)) {
		return KYTY_PM4_LEN(cmd_id) - 1u;
	}
	if (cmd_offset >= Pm4::UC_NUM) {
		EXIT("unsupported UC register offset 0x%08" PRIx32 " (raw 0x%08" PRIx32
		     "), cmd_id = 0x%08" PRIx32 "\n",
		     cmd_offset, raw_cmd_offset, cmd_id);
	}

	auto pfunc = g_hw_uc_func[cmd_offset & (Pm4::UC_NUM - 1)];

	if (pfunc == nullptr) {
		auto num_values = KYTY_PM4_LEN(cmd_id) - 2u;
		bool handled    = true;
		for (uint32_t i = 0; i < num_values; i++) {
			if (cmd_offset + i >= Pm4::UC_NUM ||
			    g_hw_uc_indirect_func[(cmd_offset + i) & (Pm4::UC_NUM - 1)] == nullptr) {
				handled = false;
				break;
			}
		}
		if (handled) {
			for (uint32_t i = 0; i < num_values; i++) {
				g_hw_uc_indirect_func[(cmd_offset + i) & (Pm4::UC_NUM - 1)](cp, cmd_offset + i,
				                                                            buffer[1 + i]);
			}
			return num_values + 1u;
		}
		EXIT("unknown user config register\n\t%05" PRIx32 ":\n\tcmd_id = %08" PRIx32
		     "\n\tcmd_offset = %08" PRIx32 "\n",
		     num_dw - dw, cmd_id, cmd_offset);
	}

	auto s = pfunc(cp, cmd_id, cmd_offset, buffer + 1, dw);
	EXIT_IF(s == 0);
	return s + 1;
}

KYTY_CP_OP_PARSER(CpOpWaitFlipDone) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0051018);

	auto video_out_handle     = buffer[0];
	auto display_buffer_index = buffer[1];

	cp->WaitFlipDone(video_out_handle, display_buffer_index);

	return 6;
}

template <typename T>
static T CpOpWaitRegMemReadValue(const uint32_t* buffer) {
	static_assert(sizeof(T) == sizeof(uint32_t) || sizeof(T) == sizeof(uint64_t));

	if constexpr (sizeof(T) == sizeof(uint32_t)) {
		return buffer[0];
	} else {
		return buffer[0] | (static_cast<uint64_t>(buffer[1]) << 32u);
	}
}

template <typename T>
static uint32_t CpOpWaitRegMemWaitOp(uint32_t control) {
	static_assert(sizeof(T) == sizeof(uint32_t) || sizeof(T) == sizeof(uint64_t));

	if constexpr (sizeof(T) == sizeof(uint32_t)) {
		return ((control >> 8u) & 0x3u) | ((control >> 4u) & 0xcu);
	} else {
		return ((control >> 8u) & 0x1u) | ((control >> 5u) & 0x6u);
	}
}

template <typename T>
static uint32_t CpOpWaitRegMemSized(CommandProcessor* cp, uint32_t cmd_id, const uint32_t* buffer) {
	static_assert(sizeof(T) == sizeof(uint32_t) || sizeof(T) == sizeof(uint64_t));

	constexpr auto value_dw   = static_cast<uint32_t>(sizeof(T) / sizeof(uint32_t));
	constexpr auto payload_dw = 4u + value_dw * 2u;
	constexpr auto packet_dw  = payload_dw + 1u;
	constexpr auto register_id =
	    (sizeof(T) == sizeof(uint32_t) ? Pm4::R_WAIT_MEM_32 : Pm4::R_WAIT_MEM_64);

	EXIT_NOT_IMPLEMENTED(KYTY_PM4_R(cmd_id) != register_id);
	EXIT_NOT_IMPLEMENTED(KYTY_PM4_LEN(cmd_id) != packet_dw);

	auto* addr = reinterpret_cast<const T*>(buffer[0] | (static_cast<uint64_t>(buffer[1]) << 32u));
	auto  mask = CpOpWaitRegMemReadValue<T>(buffer + 2u);
	auto  ref  = CpOpWaitRegMemReadValue<T>(buffer + 2u + value_dw);
	auto  ctrl = buffer[2u + value_dw * 2u];
	auto  poll = buffer[3u + value_dw * 2u];

	EXIT_NOT_IMPLEMENTED((ctrl & 0x10u) == 0);

	cp->WaitRegMem(ctrl & 0x7u, addr, ref, mask, poll, CpOpWaitRegMemWaitOp<T>(ctrl));

	return payload_dw;
}

KYTY_CP_OP_PARSER(CpOpWaitRegMem32) {
	KYTY_PROFILER_FUNCTION();

	return CpOpWaitRegMemSized<uint32_t>(cp, cmd_id, buffer);
}

KYTY_CP_OP_PARSER(CpOpWaitRegMem64) {
	KYTY_PROFILER_FUNCTION();

	return CpOpWaitRegMemSized<uint64_t>(cp, cmd_id, buffer);
}

KYTY_CP_OP_PARSER(CpOpWaitOnCeCounter) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0008600);
	EXIT_NOT_IMPLEMENTED(buffer[0] != 1);

	cp->WaitCe();

	return 1;
}

KYTY_CP_OP_PARSER(CpOpWaitOnDeCounterDiff) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0008800);

	cp->WaitDeDiff(buffer[0]);

	return 1;
}

KYTY_CP_OP_PARSER(CpOpWriteConstRam) {
	KYTY_PROFILER_FUNCTION();

	auto dw_num = (cmd_id >> 16u) & 0x3fffu;
	auto offset = buffer[0];

	EXIT_NOT_IMPLEMENTED(dw_num >= 0x3000);
	EXIT_NOT_IMPLEMENTED(offset > 0xbffc);
	EXIT_NOT_IMPLEMENTED((offset & 0x3u) != 0);

	cp->WriteConstRam(offset, buffer + 1, dw_num);

	return 1 + dw_num;
}

KYTY_CP_OP_PARSER(CpOpWriteData) {
	KYTY_PROFILER_FUNCTION();

	auto op = (cmd_id >> 8u) & 0xffu;

	EXIT_NOT_IMPLEMENTED(op != Pm4::IT_WRITE_DATA);

	auto dw_num = (cmd_id >> 16u) & 0x3fffu;

	auto  write_control = buffer[0];
	auto* dst = reinterpret_cast<uint32_t*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));

	cp->WriteData(dst, buffer + 3, dw_num - 2, write_control);

	return 1 + dw_num;
}

void GraphicsInitJmpTablesCxIndirect() {
	for (auto& func: g_hw_ctx_indirect_func) {
		func = nullptr;
	}

	for (auto cmd_offset = Pm4::SPI_PS_INPUT_CNTL_0; cmd_offset <= Pm4::SPI_PS_INPUT_CNTL_31;
	     cmd_offset++) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t slot = cmd_offset - Pm4::SPI_PS_INPUT_CNTL_0;
			cp->GetCtx()->SetPsInputSettings(slot, value);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_BASE; cmd_offset <= Pm4::CB_COLOR7_BASE;
	     cmd_offset += 15) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_BASE) / 15;
			auto     base = cp->GetCtx()->GetRenderTarget(slot).base;
			base.addr &= 0xFFFFFF00000000FFull;
			base.addr |= static_cast<uint64_t>(value) << 8u;
			cp->GetCtx()->SetColorBase(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_BASE + 1; cmd_offset <= Pm4::CB_COLOR7_BASE + 1;
	     cmd_offset += 15) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t       slot = (cmd_offset - (Pm4::CB_COLOR0_BASE + 1)) / 15;
			HW::ColorPitch pitch;
			pitch.pitch_div8_minus1       = value & 0x7ffu;
			pitch.fmask_pitch_div8_minus1 = (value >> 20u) & 0x7ffu;
			cp->GetCtx()->SetColorPitch(slot, pitch);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_BASE + 2; cmd_offset <= Pm4::CB_COLOR7_BASE + 2;
	     cmd_offset += 15) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t       slot = (cmd_offset - (Pm4::CB_COLOR0_BASE + 2)) / 15;
			HW::ColorSlice slice;
			slice.slice_div64_minus1 = value & 0x3fffffu;
			cp->GetCtx()->SetColorSlice(slot, slice);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_BASE_EXT; cmd_offset <= Pm4::CB_COLOR7_BASE_EXT;
	     cmd_offset++) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_BASE_EXT);
			auto     base = cp->GetCtx()->GetRenderTarget(slot).base;
			base.addr &= 0xFFFF00FFFFFFFFFFull;
			base.addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			cp->GetCtx()->SetColorBase(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_VIEW; cmd_offset <= Pm4::CB_COLOR7_VIEW;
	     cmd_offset += 15) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t      slot = (cmd_offset - Pm4::CB_COLOR0_VIEW) / 15;
			HW::ColorView view;
			view.base_array_slice_index = KYTY_PM4_GET(value, CB_COLOR0_VIEW, SLICE_START);
			view.last_array_slice_index = KYTY_PM4_GET(value, CB_COLOR0_VIEW, SLICE_MAX);
			view.current_mip_level      = KYTY_PM4_GET(value, CB_COLOR0_VIEW, MIP_LEVEL);
			cp->GetCtx()->SetColorView(slot, view);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_INFO; cmd_offset <= Pm4::CB_COLOR7_INFO;
	     cmd_offset += 15) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t      slot = (cmd_offset - Pm4::CB_COLOR0_INFO) / 15;
			HW::ColorInfo info;
			info.format                   = KYTY_PM4_GET(value, CB_COLOR0_INFO, FORMAT);
			info.channel_type             = KYTY_PM4_GET(value, CB_COLOR0_INFO, NUMBER_TYPE);
			info.channel_order            = KYTY_PM4_GET(value, CB_COLOR0_INFO, COMP_SWAP);
			info.cmask_fast_clear_enable  = KYTY_PM4_GET(value, CB_COLOR0_INFO, FAST_CLEAR) != 0;
			info.fmask_compression_enable = KYTY_PM4_GET(value, CB_COLOR0_INFO, COMPRESSION) != 0;
			info.blend_clamp              = KYTY_PM4_GET(value, CB_COLOR0_INFO, BLEND_CLAMP) != 0;
			info.blend_bypass             = KYTY_PM4_GET(value, CB_COLOR0_INFO, BLEND_BYPASS) != 0;
			info.round_mode               = KYTY_PM4_GET(value, CB_COLOR0_INFO, ROUND_MODE) != 0;
			info.cmask_is_linear          = KYTY_PM4_GET(value, CB_COLOR0_INFO, CMASK_IS_LINEAR);
			info.fmask_data_compression_disable =
			    KYTY_PM4_GET(value, CB_COLOR0_INFO, FMASK_COMPRESSION_DISABLE) != 0;
			info.fmask_one_frag_mode =
			    KYTY_PM4_GET(value, CB_COLOR0_INFO, FMASK_COMPRESS_1FRAG_ONLY) != 0;
			info.dcc_compression_enable = KYTY_PM4_GET(value, CB_COLOR0_INFO, DCC_ENABLE) != 0;
			info.cmask_addr_type        = KYTY_PM4_GET(value, CB_COLOR0_INFO, CMASK_ADDR_TYPE);
			info.alt_tile_mode          = KYTY_PM4_GET(value, CB_COLOR0_INFO, ALT_TILE_MODE) != 0;
			cp->GetCtx()->SetColorInfo(slot, info);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_ATTRIB; cmd_offset <= Pm4::CB_COLOR7_ATTRIB;
	     cmd_offset += 15) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t        slot = (cmd_offset - Pm4::CB_COLOR0_ATTRIB) / 15;
			HW::ColorAttrib attrib;
			attrib.force_dest_alpha_to_one =
			    KYTY_PM4_GET(value, CB_COLOR0_ATTRIB, FORCE_DST_ALPHA_1) != 0;
			attrib.tile_mode       = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB, TILE_MODE_INDEX);
			attrib.fmask_tile_mode = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB, FMASK_TILE_MODE_INDEX);
			attrib.num_samples     = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB, NUM_SAMPLES);
			attrib.num_fragments   = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB, NUM_FRAGMENTS);
			cp->GetCtx()->SetColorAttrib(slot, attrib);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_DCC_CONTROL; cmd_offset <= Pm4::CB_COLOR7_DCC_CONTROL;
	     cmd_offset += 15) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t            slot = (cmd_offset - Pm4::CB_COLOR0_DCC_CONTROL) / 15;
			HW::ColorDccControl dcc;
			dcc.overwrite_combiner_disable =
			    KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, OVERWRITE_COMBINER_DISABLE) != 0;
			dcc.dcc_clear_key_enable =
			    KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, KEY_CLEAR_ENABLE) != 0;
			dcc.max_uncompressed_block_size =
			    KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, MAX_UNCOMPRESSED_BLOCK_SIZE);
			dcc.min_compressed_block_size =
			    KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, MIN_COMPRESSED_BLOCK_SIZE);
			dcc.max_compressed_block_size =
			    KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, MAX_COMPRESSED_BLOCK_SIZE);
			dcc.color_transform = KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, COLOR_TRANSFORM);
			dcc.independent_64b_blocks =
			    KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, INDEPENDENT_64B_BLOCKS) != 0;
			dcc.data_write_on_dcc_clear_to_reg =
			    KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, ENABLE_CONSTANT_ENCODE_REG_WRITE) != 0;
			dcc.independent_128b_blocks =
			    KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, INDEPENDENT_128B_BLOCKS) != 0;
			cp->GetCtx()->SetColorDccControl(slot, dcc);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_CMASK; cmd_offset <= Pm4::CB_COLOR7_CMASK;
	     cmd_offset += 15) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_CMASK) / 15;
			auto     base = cp->GetCtx()->GetRenderTarget(slot).cmask;
			base.addr &= 0xFFFFFF00000000FFull;
			base.addr |= static_cast<uint64_t>(value) << 8u;
			cp->GetCtx()->SetColorCmask(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_CMASK + 1; cmd_offset <= Pm4::CB_COLOR7_BASE + 8;
	     cmd_offset += 15) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t            slot = (cmd_offset - (Pm4::CB_COLOR0_CMASK + 1)) / 15;
			HW::ColorCmaskSlice cmask_slice;
			cmask_slice.slice_minus1 = value & 0x3fffu;
			cp->GetCtx()->SetColorCmaskSlice(slot, cmask_slice);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_CMASK_BASE_EXT;
	     cmd_offset <= Pm4::CB_COLOR7_CMASK_BASE_EXT; cmd_offset++) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_CMASK_BASE_EXT);
			auto     base = cp->GetCtx()->GetRenderTarget(slot).cmask;
			base.addr &= 0xFFFF00FFFFFFFFFFull;
			base.addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			cp->GetCtx()->SetColorCmask(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_FMASK; cmd_offset <= Pm4::CB_COLOR7_FMASK;
	     cmd_offset += 15) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_FMASK) / 15;
			auto     base = cp->GetCtx()->GetRenderTarget(slot).fmask;
			base.addr &= 0xFFFFFF00000000FFull;
			base.addr |= static_cast<uint64_t>(value) << 8u;
			cp->GetCtx()->SetColorFmask(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_FMASK + 1; cmd_offset <= Pm4::CB_COLOR7_BASE + 10;
	     cmd_offset += 15) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t            slot = (cmd_offset - (Pm4::CB_COLOR0_FMASK + 1)) / 15;
			HW::ColorFmaskSlice fmask_slice;
			fmask_slice.slice_minus1 = value & 0x3fffffu;
			cp->GetCtx()->SetColorFmaskSlice(slot, fmask_slice);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_FMASK_BASE_EXT;
	     cmd_offset <= Pm4::CB_COLOR7_FMASK_BASE_EXT; cmd_offset++) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_FMASK_BASE_EXT);
			auto     base = cp->GetCtx()->GetRenderTarget(slot).fmask;
			base.addr &= 0xFFFF00FFFFFFFFFFull;
			base.addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			cp->GetCtx()->SetColorFmask(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_CLEAR_WORD0; cmd_offset <= Pm4::CB_COLOR7_CLEAR_WORD0;
	     cmd_offset += 15) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			HW::ColorClearWord0 clear_word0;
			clear_word0.word0 = value;
			uint32_t slot     = (cmd_offset - Pm4::CB_COLOR0_CLEAR_WORD0) / 15;
			cp->GetCtx()->SetColorClearWord0(slot, clear_word0);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_CLEAR_WORD1; cmd_offset <= Pm4::CB_COLOR7_CLEAR_WORD1;
	     cmd_offset += 15) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			HW::ColorClearWord1 clear_word1;
			clear_word1.word1 = value;
			uint32_t slot     = (cmd_offset - Pm4::CB_COLOR0_CLEAR_WORD1) / 15;
			cp->GetCtx()->SetColorClearWord1(slot, clear_word1);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_DCC_BASE; cmd_offset <= Pm4::CB_COLOR7_DCC_BASE;
	     cmd_offset += 15) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_DCC_BASE) / 15;
			auto     base = cp->GetCtx()->GetRenderTarget(slot).dcc_addr;
			base.addr &= 0xFFFFFF00000000FFull;
			base.addr |= static_cast<uint64_t>(value) << 8u;
			cp->GetCtx()->SetColorDccAddr(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_DCC_BASE_EXT; cmd_offset <= Pm4::CB_COLOR7_DCC_BASE_EXT;
	     cmd_offset++) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_DCC_BASE_EXT);
			auto     base = cp->GetCtx()->GetRenderTarget(slot).dcc_addr;
			base.addr &= 0xFFFF00FFFFFFFFFFull;
			base.addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			cp->GetCtx()->SetColorDccAddr(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_ATTRIB2; cmd_offset <= Pm4::CB_COLOR7_ATTRIB2;
	     cmd_offset++) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t         slot = (cmd_offset - Pm4::CB_COLOR0_ATTRIB2);
			HW::ColorAttrib2 attrib2;
			attrib2.height         = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB2, MIP0_HEIGHT);
			attrib2.width          = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB2, MIP0_WIDTH);
			attrib2.num_mip_levels = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB2, MAX_MIP);
			cp->GetCtx()->SetColorAttrib2(slot, attrib2);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_ATTRIB3; cmd_offset <= Pm4::CB_COLOR7_ATTRIB3;
	     cmd_offset++) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t         slot = (cmd_offset - Pm4::CB_COLOR0_ATTRIB3);
			HW::ColorAttrib3 attrib3;
			attrib3.depth              = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB3, MIP0_DEPTH);
			attrib3.tile_mode          = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB3, COLOR_SW_MODE);
			attrib3.dimension          = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB3, RESOURCE_TYPE);
			attrib3.cmask_pipe_aligned = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB3, CMASK_PIPE_ALIGNED);
			attrib3.dcc_pipe_aligned   = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB3, DCC_PIPE_ALIGNED);
			cp->GetCtx()->SetColorAttrib3(slot, attrib3);
		};
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_XSCALE; cmd_offset <= Pm4::PA_CL_VPORT_XSCALE_15;
	     cmd_offset += 6) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			cp->GetCtx()->SetViewportXScale((cmd_offset - Pm4::PA_CL_VPORT_XSCALE) / 6,
			                                *reinterpret_cast<const float*>(&value));
		};
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_XOFFSET; cmd_offset <= Pm4::PA_CL_VPORT_XOFFSET_15;
	     cmd_offset += 6) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			cp->GetCtx()->SetViewportXOffset((cmd_offset - Pm4::PA_CL_VPORT_XOFFSET) / 6,
			                                 *reinterpret_cast<const float*>(&value));
		};
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_YSCALE; cmd_offset <= Pm4::PA_CL_VPORT_YSCALE_15;
	     cmd_offset += 6) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			cp->GetCtx()->SetViewportYScale((cmd_offset - Pm4::PA_CL_VPORT_YSCALE) / 6,
			                                *reinterpret_cast<const float*>(&value));
		};
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_YOFFSET; cmd_offset <= Pm4::PA_CL_VPORT_YOFFSET_15;
	     cmd_offset += 6) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			cp->GetCtx()->SetViewportYOffset((cmd_offset - Pm4::PA_CL_VPORT_YOFFSET) / 6,
			                                 *reinterpret_cast<const float*>(&value));
		};
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_ZSCALE; cmd_offset <= Pm4::PA_CL_VPORT_ZSCALE_15;
	     cmd_offset += 6) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			cp->GetCtx()->SetViewportZScale((cmd_offset - Pm4::PA_CL_VPORT_ZSCALE) / 6,
			                                *reinterpret_cast<const float*>(&value));
		};
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_ZOFFSET; cmd_offset <= Pm4::PA_CL_VPORT_ZOFFSET_15;
	     cmd_offset += 6) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			cp->GetCtx()->SetViewportZOffset((cmd_offset - Pm4::PA_CL_VPORT_ZOFFSET) / 6,
			                                 *reinterpret_cast<const float*>(&value));
		};
	}

	for (auto cmd_offset = Pm4::PA_SC_VPORT_SCISSOR_0_TL;
	     cmd_offset <= Pm4::PA_SC_VPORT_SCISSOR_15_TL; cmd_offset += 2) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			int left = static_cast<int16_t>(
			    static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_TL, TL_X)));
			int top = static_cast<int16_t>(
			    static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_TL, TL_Y)));
			bool window_offset_disable =
			    KYTY_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_TL, WINDOW_OFFSET_DISABLE) != 0;
			cp->GetCtx()->SetViewportScissorTL((cmd_offset - Pm4::PA_SC_VPORT_SCISSOR_0_TL) / 2,
			                                   left, top, !window_offset_disable);
		};
	}

	for (auto cmd_offset = Pm4::PA_SC_VPORT_SCISSOR_0_BR;
	     cmd_offset <= Pm4::PA_SC_VPORT_SCISSOR_15_BR; cmd_offset += 2) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			int right = static_cast<int16_t>(
			    static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_BR, BR_X)));
			int bottom = static_cast<int16_t>(
			    static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_BR, BR_Y)));
			cp->GetCtx()->SetViewportScissorBR((cmd_offset - Pm4::PA_SC_VPORT_SCISSOR_0_BR) / 2,
			                                   right, bottom);
		};
	}

	for (auto cmd_offset = Pm4::PA_SC_VPORT_ZMIN_0; cmd_offset <= Pm4::PA_SC_VPORT_ZMIN_15;
	     cmd_offset += 2) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			cp->GetCtx()->SetViewportZMin((cmd_offset - Pm4::PA_SC_VPORT_ZMIN_0) / 2,
			                              *reinterpret_cast<const float*>(&value));
		};
	}

	for (auto cmd_offset = Pm4::PA_SC_VPORT_ZMAX_0; cmd_offset <= Pm4::PA_SC_VPORT_ZMAX_15;
	     cmd_offset += 2) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			cp->GetCtx()->SetViewportZMax((cmd_offset - Pm4::PA_SC_VPORT_ZMAX_0) / 2,
			                              *reinterpret_cast<const float*>(&value));
		};
	}

	g_hw_ctx_indirect_func[Pm4::PA_CL_GB_VERT_CLIP_ADJ] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto vp = cp->GetCtx()->GetScreenViewport();
		cp->GetCtx()->SetGuardBands(vp.guard_band_horz_clip,
		                            *reinterpret_cast<const float*>(&value),
		                            vp.guard_band_horz_discard, vp.guard_band_vert_discard);
	};

	g_hw_ctx_indirect_func[Pm4::PA_CL_GB_VERT_DISC_ADJ] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto vp = cp->GetCtx()->GetScreenViewport();
		cp->GetCtx()->SetGuardBands(vp.guard_band_horz_clip, vp.guard_band_vert_clip,
		                            vp.guard_band_horz_discard,
		                            *reinterpret_cast<const float*>(&value));
	};

	g_hw_ctx_indirect_func[Pm4::PA_CL_GB_HORZ_CLIP_ADJ] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto vp = cp->GetCtx()->GetScreenViewport();
		cp->GetCtx()->SetGuardBands(*reinterpret_cast<const float*>(&value),
		                            vp.guard_band_vert_clip, vp.guard_band_horz_discard,
		                            vp.guard_band_vert_discard);
	};

	g_hw_ctx_indirect_func[Pm4::PA_CL_GB_HORZ_DISC_ADJ] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto vp = cp->GetCtx()->GetScreenViewport();
		cp->GetCtx()->SetGuardBands(vp.guard_band_horz_clip, vp.guard_band_vert_clip,
		                            *reinterpret_cast<const float*>(&value),
		                            vp.guard_band_vert_discard);
	};

	for (auto cmd_offset = Pm4::CB_BLEND_RED; cmd_offset <= Pm4::CB_BLEND_ALPHA; cmd_offset++) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			auto color = cp->GetCtx()->GetBlendColor();
			auto f     = *reinterpret_cast<const float*>(&value);
			switch (cmd_offset) {
				case Pm4::CB_BLEND_RED: color.red = f; break;
				case Pm4::CB_BLEND_GREEN: color.green = f; break;
				case Pm4::CB_BLEND_BLUE: color.blue = f; break;
				case Pm4::CB_BLEND_ALPHA: color.alpha = f; break;
				default: break;
			}
			cp->GetCtx()->SetBlendColor(color);
		};
	}
	g_hw_ctx_indirect_func[Pm4::CB_DCC_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreCbDccControl(value);
	};
	g_hw_ctx_indirect_func[Pm4::DB_COUNT_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreDepthMetadataRegister(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::DB_RENDER_OVERRIDE] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreDepthMetadataRegister(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::DB_RENDER_OVERRIDE2] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreDepthMetadataRegister(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::DB_DFSM_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreDepthMetadataRegister(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::DB_RMI_L2_CACHE_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreDepthMetadataRegister(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::CB_RMI_GL2_CACHE_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreDepthMetadataRegister(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::TA_BC_BASE_ADDR] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreBorderColorTableAddr(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::TA_BC_BASE_ADDR_HI] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreBorderColorTableAddr(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SU_POINT_SIZE] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnorePointState(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SU_POINT_MINMAX] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnorePointState(cmd_offset, value);
	};

	g_hw_ctx_indirect_func[Pm4::SPI_VS_OUT_CONFIG] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetVsOutConfig(value);
	};

	g_hw_ctx_indirect_func[Pm4::SPI_SHADER_POS_FORMAT] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetShaderPosFormat(value);
	};
	g_hw_ctx_indirect_func[Pm4::SPI_SHADER_IDX_FORMAT] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetShaderIdxFormat(value);
	};
	g_hw_ctx_indirect_func[Pm4::SPI_TMPRING_SIZE] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreSpiTmpringSize(value);
	};
	g_hw_ctx_indirect_func[Pm4::PA_CL_VS_OUT_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetClVsOutCntl(value);
	};
	g_hw_ctx_indirect_func[Pm4::GE_NGG_SUBGRP_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetNggSubgrpCntl(value);
	};
	g_hw_ctx_indirect_func[Pm4::VGT_GS_INSTANCE_CNT] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetGsInstanceCnt(value);
	};
	g_hw_ctx_indirect_func[Pm4::VGT_GS_ONCHIP_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetGsOnchipCntl(value);
	};
	g_hw_ctx_indirect_func[Pm4::VGT_HOS_MAX_TESS_LEVEL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetHosMaxTessLevel(value);
	};
	g_hw_ctx_indirect_func[Pm4::VGT_HOS_MIN_TESS_LEVEL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetHosMinTessLevel(value);
	};

	g_hw_ctx_indirect_func[Pm4::GE_MAX_OUTPUT_PER_SUBGROUP] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetMaxOutputPerSubgroup(value);
	};

	g_hw_ctx_indirect_func[Pm4::VGT_ESGS_RING_ITEMSIZE] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetEsgsRingItemsize(value);
	};
	g_hw_ctx_indirect_func[Pm4::VGT_GS_MAX_VERT_OUT] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetGsMaxVertOut(value);
	};
	g_hw_ctx_indirect_func[Pm4::VGT_PRIMITIVEID_EN] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetPrimitiveIdEn(value);
	};
	g_hw_ctx_indirect_func[Pm4::VGT_REUSE_OFF] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetReuseOff(value);
	};
	g_hw_ctx_indirect_func[Pm4::VGT_MULTI_PRIM_IB_RESET_INDX] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwIgnoreMultiPrimIbReset(value);
	};
	g_hw_ctx_indirect_func[Pm4::VGT_TESS_DISTRIBUTION] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetTessDistribution(value);
	};
	g_hw_ctx_indirect_func[Pm4::VGT_SHADER_STAGES_EN] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetShaderStages(value);
	};
	g_hw_ctx_indirect_func[Pm4::VGT_LS_HS_CONFIG] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetLsHsConfig(value);
	};
	g_hw_ctx_indirect_func[Pm4::VGT_GS_OUT_PRIM_TYPE] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetGsOutPrimType(value);
	};
	g_hw_ctx_indirect_func[Pm4::VGT_TF_PARAM] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetTfParam(value);
	};
	g_hw_ctx_indirect_func[Pm4::VGT_DRAW_PAYLOAD_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreDrawPayloadControl(value);
	};
	g_hw_ctx_indirect_func[Pm4::VGT_PRIMITIVEID_RESET] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnorePrimitiveIdReset(value);
	};
	g_hw_ctx_indirect_func[Pm4::SPI_SHADER_Z_FORMAT] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetShaderZFormat(value);
	};
	g_hw_ctx_indirect_func[Pm4::PA_CL_OBJPRIM_ID_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreObjprimIdControl(value);
	};

	for (auto cmd_offset = Pm4::PA_SC_CLIPRECT_RULE; cmd_offset < Pm4::PA_SC_CLIPRECT_0_TL + 8u;
	     cmd_offset++) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			HwCtxSetClipRectRegister(cp->GetCtx(), cmd_offset, value);
		};
	}

	g_hw_ctx_indirect_func[Pm4::SPI_SHADER_COL_FORMAT] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		for (uint32_t i = 0; i < 8; i++) {
			cp->GetCtx()->SetTargetOutputMode(i, (value >> (i * 4)) & 0xFu);
		}
	};

	g_hw_ctx_indirect_func[Pm4::SPI_PS_INPUT_ENA] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetPsInputEna(value);
	};
	g_hw_ctx_indirect_func[Pm4::SPI_PS_INPUT_ADDR] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetPsInputAddr(value);
	};
	g_hw_ctx_indirect_func[Pm4::SPI_PS_IN_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetPsInControl(value);
	};
	g_hw_ctx_indirect_func[Pm4::SPI_BARYC_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetBarycCntl(value);
	};

	g_hw_ctx_indirect_func[Pm4::DB_SHADER_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HW::DepthShaderControl db_shader_control {};
		db_shader_control.other_bits = value & 0xFFFF908Eu;
		db_shader_control.conservative_z_export_value =
		    KYTY_PM4_GET(value, DB_SHADER_CONTROL, CONSERVATIVE_Z_EXPORT);
		db_shader_control.shader_z_behavior = KYTY_PM4_GET(value, DB_SHADER_CONTROL, Z_ORDER);
		db_shader_control.shader_kill_enable =
		    KYTY_PM4_GET(value, DB_SHADER_CONTROL, KILL_ENABLE) != 0;
		db_shader_control.shader_z_export_enable =
		    KYTY_PM4_GET(value, DB_SHADER_CONTROL, Z_EXPORT_ENABLE) != 0;
		db_shader_control.shader_mask_export_enable =
		    KYTY_PM4_GET(value, DB_SHADER_CONTROL, MASK_EXPORT_ENABLE) != 0;
		db_shader_control.shader_dual_export_enable =
		    KYTY_PM4_GET(value, DB_SHADER_CONTROL, DUAL_EXPORT_ENABLE) != 0;
		db_shader_control.shader_execute_on_noop =
		    KYTY_PM4_GET(value, DB_SHADER_CONTROL, EXEC_ON_NOOP) != 0;
		db_shader_control.alpha_to_mask_disable =
		    KYTY_PM4_GET(value, DB_SHADER_CONTROL, ALPHA_TO_MASK_DISABLE) != 0;
		cp->GetCtx()->SetDepthShaderControl(db_shader_control);
	};

	g_hw_ctx_indirect_func[Pm4::CB_SHADER_MASK] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetShaderMask(value);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SC_SHADER_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetScShaderControl(value);
	};
	g_hw_ctx_indirect_func[Pm4::CB_TARGET_MASK] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetRenderTargetMask(value);
	};

	g_hw_ctx_indirect_func[Pm4::PA_SC_SCREEN_SCISSOR_TL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		const auto& vp   = cp->GetCtx()->GetScreenViewport();
		int         left = static_cast<int16_t>(
		    static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_SCREEN_SCISSOR_TL, TL_X)));
		int top = static_cast<int16_t>(
		    static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_SCREEN_SCISSOR_TL, TL_Y)));
		cp->GetCtx()->SetScreenScissor(left, top, vp.screen_scissor_right,
		                               vp.screen_scissor_bottom);
	};

	g_hw_ctx_indirect_func[Pm4::PA_SC_SCREEN_SCISSOR_BR] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		const auto& vp    = cp->GetCtx()->GetScreenViewport();
		int         right = static_cast<int16_t>(
		    static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_SCREEN_SCISSOR_BR, BR_X)));
		int bottom = static_cast<int16_t>(
		    static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_SCREEN_SCISSOR_BR, BR_Y)));
		cp->GetCtx()->SetScreenScissor(vp.screen_scissor_left, vp.screen_scissor_top, right,
		                               bottom);
	};

	g_hw_ctx_indirect_func[Pm4::PA_SC_GENERIC_SCISSOR_TL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		const auto& vp   = cp->GetCtx()->GetScreenViewport();
		int         left = static_cast<int16_t>(
		    static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_TL, TL_X)));
		int top = static_cast<int16_t>(
		    static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_TL, TL_Y)));
		bool window_offset_disable =
		    KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_TL, WINDOW_OFFSET_DISABLE) != 0;
		cp->GetCtx()->SetGenericScissor(left, top, vp.generic_scissor_right,
		                                vp.generic_scissor_bottom, !window_offset_disable);
	};

	g_hw_ctx_indirect_func[Pm4::PA_SC_GENERIC_SCISSOR_BR] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		const auto& vp    = cp->GetCtx()->GetScreenViewport();
		int         right = static_cast<int16_t>(
		    static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_BR, BR_X)));
		int bottom = static_cast<int16_t>(
		    static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_BR, BR_Y)));
		cp->GetCtx()->SetGenericScissor(vp.generic_scissor_left, vp.generic_scissor_top, right,
		                                bottom, vp.generic_scissor_window_offset_enable);
	};

	g_hw_ctx_indirect_func[Pm4::PA_SU_HARDWARE_SCREEN_OFFSET] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		uint32_t x = KYTY_PM4_GET(value, PA_SU_HARDWARE_SCREEN_OFFSET, HW_SCREEN_OFFSET_X);
		uint32_t y = KYTY_PM4_GET(value, PA_SU_HARDWARE_SCREEN_OFFSET, HW_SCREEN_OFFSET_Y);
		cp->GetCtx()->SetHardwareScreenOffset(x, y);
	};

	g_hw_ctx_indirect_func[Pm4::PA_SC_WINDOW_OFFSET] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		int offset_x = static_cast<int16_t>(
		    static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_WINDOW_OFFSET, WINDOW_X)));
		int offset_y = static_cast<int16_t>(
		    static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_WINDOW_OFFSET, WINDOW_Y)));

		cp->GetCtx()->SetWindowOffset(offset_x, offset_y);
	};

	g_hw_ctx_indirect_func[Pm4::PA_SC_WINDOW_SCISSOR_TL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		const auto& vp   = cp->GetCtx()->GetScreenViewport();
		int         left = static_cast<int16_t>(
		    static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_WINDOW_SCISSOR_TL, TL_X)));
		int top = static_cast<int16_t>(
		    static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_WINDOW_SCISSOR_TL, TL_Y)));
		bool window_offset_disable =
		    KYTY_PM4_GET(value, PA_SC_WINDOW_SCISSOR_TL, WINDOW_OFFSET_DISABLE) != 0;
		cp->GetCtx()->SetWindowScissor(left, top, vp.window_scissor_right, vp.window_scissor_bottom,
		                               !window_offset_disable);
	};

	g_hw_ctx_indirect_func[Pm4::PA_SC_WINDOW_SCISSOR_BR] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		const auto& vp    = cp->GetCtx()->GetScreenViewport();
		int         right = static_cast<int16_t>(
		    static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_WINDOW_SCISSOR_BR, BR_X)));
		int bottom = static_cast<int16_t>(
		    static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_WINDOW_SCISSOR_BR, BR_Y)));
		cp->GetCtx()->SetWindowScissor(vp.window_scissor_left, vp.window_scissor_top, right, bottom,
		                               vp.window_scissor_window_offset_enable);
	};

	for (uint32_t slot = 0; slot < 8; slot++) {
		g_hw_ctx_indirect_func[Pm4::CB_BLEND0_CONTROL + slot] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			uint32_t param = cmd_offset - Pm4::CB_BLEND0_CONTROL;

			HW::BlendControl r;
			r.color_srcblend  = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, COLOR_SRCBLEND);
			r.color_comb_fcn  = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, COLOR_COMB_FCN);
			r.color_destblend = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, COLOR_DESTBLEND);
			r.alpha_srcblend  = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, ALPHA_SRCBLEND);
			r.alpha_comb_fcn  = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, ALPHA_COMB_FCN);
			r.alpha_destblend = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, ALPHA_DESTBLEND);
			r.separate_alpha_blend =
			    KYTY_PM4_GET(value, CB_BLEND0_CONTROL, SEPARATE_ALPHA_BLEND) != 0;
			r.enable = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, ENABLE) != 0;

			cp->GetCtx()->SetBlendControl(param, r);
		};
	}

	g_hw_ctx_indirect_func[Pm4::PA_SU_SC_MODE_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HW::ModeControl r;
		r.cull_front           = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, CULL_FRONT) != 0;
		r.cull_back            = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, CULL_BACK) != 0;
		r.face                 = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, FACE) != 0;
		r.poly_mode            = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, POLY_MODE);
		r.polymode_front_ptype = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, POLYMODE_FRONT_PTYPE);
		r.polymode_back_ptype  = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, POLYMODE_BACK_PTYPE);
		r.poly_offset_front_enable =
		    KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, POLY_OFFSET_FRONT_ENABLE) != 0;
		r.poly_offset_back_enable =
		    KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, POLY_OFFSET_BACK_ENABLE) != 0;
		r.vtx_window_offset_enable =
		    KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, VTX_WINDOW_OFFSET_ENABLE) != 0;
		r.provoking_vtx_last = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, PROVOKING_VTX_LAST) != 0;
		r.persp_corr_dis     = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, PERSP_CORR_DIS) != 0;
		cp->GetCtx()->SetModeControl(r);
	};

	g_hw_ctx_indirect_func[Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnorePolyOffsetRegister(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SU_POLY_OFFSET_CLAMP] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnorePolyOffsetRegister(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SU_POLY_OFFSET_FRONT_SCALE] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnorePolyOffsetRegister(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SU_POLY_OFFSET_FRONT_OFFSET] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnorePolyOffsetRegister(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SU_POLY_OFFSET_BACK_SCALE] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnorePolyOffsetRegister(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SU_POLY_OFFSET_BACK_OFFSET] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnorePolyOffsetRegister(cmd_offset, value);
	};

	g_hw_ctx_indirect_func[Pm4::DB_Z_INFO] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HW::DepthZInfo r;
		r.format                    = KYTY_PM4_GET(value, DB_Z_INFO, FORMAT);
		r.num_samples               = KYTY_PM4_GET(value, DB_Z_INFO, NUM_SAMPLES);
		r.embedded_sample_locations = KYTY_PM4_GET(value, DB_Z_INFO, ITERATE_FLUSH) != 0;
		r.partially_resident        = KYTY_PM4_GET(value, DB_Z_INFO, PARTIALLY_RESIDENT) != 0;
		r.num_mip_levels            = KYTY_PM4_GET(value, DB_Z_INFO, MAXMIP);
		r.tile_mode_index           = KYTY_PM4_GET(value, DB_Z_INFO, TILE_MODE_INDEX);
		r.plane_compression         = KYTY_PM4_GET(value, DB_Z_INFO, DECOMPRESS_ON_N_ZPLANES);
		r.expclear_enabled          = KYTY_PM4_GET(value, DB_Z_INFO, ALLOW_EXPCLEAR) != 0;
		r.tile_surface_enable       = KYTY_PM4_GET(value, DB_Z_INFO, TILE_SURFACE_ENABLE) != 0;
		r.zrange_precision          = KYTY_PM4_GET(value, DB_Z_INFO, ZRANGE_PRECISION);
		cp->GetCtx()->SetDepthZInfo(r);
	};

	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_INFO] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto z                          = cp->GetCtx()->GetDepthRenderTarget();
		z.depth_info.addr5_swizzle_mask = KYTY_PM4_GET(value, DB_DEPTH_INFO, ADDR5_SWIZZLE_MASK);
		z.depth_info.array_mode =
		    (value >> Pm4::DB_DEPTH_INFO_ARRAY_MODE_SHIFT) & Pm4::DB_DEPTH_INFO_ARRAY_MODE_MASK;
		z.depth_info.pipe_config =
		    (value >> Pm4::DB_DEPTH_INFO_PIPE_CONFIG_SHIFT) & Pm4::DB_DEPTH_INFO_PIPE_CONFIG_MASK;
		z.depth_info.bank_width =
		    (value >> Pm4::DB_DEPTH_INFO_BANK_WIDTH_SHIFT) & Pm4::DB_DEPTH_INFO_BANK_WIDTH_MASK;
		z.depth_info.bank_height =
		    (value >> Pm4::DB_DEPTH_INFO_BANK_HEIGHT_SHIFT) & Pm4::DB_DEPTH_INFO_BANK_HEIGHT_MASK;
		z.depth_info.macro_tile_aspect = KYTY_PM4_GET(value, DB_DEPTH_INFO, MACRO_TILE_ASPECT);
		z.depth_info.num_banks =
		    (value >> Pm4::DB_DEPTH_INFO_NUM_BANKS_SHIFT) & Pm4::DB_DEPTH_INFO_NUM_BANKS_MASK;
		cp->GetCtx()->SetDepthRenderTarget(z);
	};

	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_BOUNDS_MIN] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxSetDepthBoundsRegister(cp->GetCtx(), cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_BOUNDS_MAX] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxSetDepthBoundsRegister(cp->GetCtx(), cmd_offset, value);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_INFO] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HW::DepthStencilInfo r;
		r.format                     = KYTY_PM4_GET(value, DB_STENCIL_INFO, FORMAT);
		r.texture_compatible_stencil = KYTY_PM4_GET(value, DB_STENCIL_INFO, ITERATE_FLUSH) != 0;
		r.partially_resident   = KYTY_PM4_GET(value, DB_STENCIL_INFO, PARTIALLY_RESIDENT) != 0;
		r.tile_split           = KYTY_PM4_GET(value, DB_STENCIL_INFO, RESERVED_FIELD_1);
		r.tile_mode_index      = KYTY_PM4_GET(value, DB_STENCIL_INFO, TILE_MODE_INDEX);
		r.expclear_enabled     = KYTY_PM4_GET(value, DB_STENCIL_INFO, ALLOW_EXPCLEAR) != 0;
		r.tile_stencil_disable = KYTY_PM4_GET(value, DB_STENCIL_INFO, TILE_STENCIL_DISABLE) != 0;
		cp->GetCtx()->SetDepthStencilInfo(r);
	};

	g_hw_ctx_indirect_func[Pm4::DB_Z_READ_BASE] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto base = cp->GetCtx()->GetDepthRenderTarget().z_read_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthZReadBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_Z_READ_BASE_HI] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto base = cp->GetCtx()->GetDepthRenderTarget().z_read_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthZReadBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_READ_BASE] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto base = cp->GetCtx()->GetDepthRenderTarget().stencil_read_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthStencilReadBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_READ_BASE_HI] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto base = cp->GetCtx()->GetDepthRenderTarget().stencil_read_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthStencilReadBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_Z_WRITE_BASE] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto base = cp->GetCtx()->GetDepthRenderTarget().z_write_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthZWriteBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_Z_WRITE_BASE_HI] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto base = cp->GetCtx()->GetDepthRenderTarget().z_write_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthZWriteBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_WRITE_BASE] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto base = cp->GetCtx()->GetDepthRenderTarget().stencil_write_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthStencilWriteBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_WRITE_BASE_HI] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto base = cp->GetCtx()->GetDepthRenderTarget().stencil_write_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthStencilWriteBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_HTILE_DATA_BASE] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto base = cp->GetCtx()->GetDepthRenderTarget().htile_data_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthHTileDataBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_HTILE_DATA_BASE_HI] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto base = cp->GetCtx()->GetDepthRenderTarget().htile_data_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthHTileDataBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_HTILE_SURFACE] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxSetDepthHtileSurfaceRegister(cp->GetCtx(), value);
	};

	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_VIEW] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HW::DepthDepthView r;
		r.slice_start           = KYTY_PM4_GET(value, DB_DEPTH_VIEW, SLICE_START) +
		                          (KYTY_PM4_GET(value, DB_DEPTH_VIEW, SLICE_START_HI) << 11u);
		r.slice_max             = KYTY_PM4_GET(value, DB_DEPTH_VIEW, SLICE_MAX) +
		                          (KYTY_PM4_GET(value, DB_DEPTH_VIEW, SLICE_MAX_HI) << 11u);
		r.depth_write_disable   = KYTY_PM4_GET(value, DB_DEPTH_VIEW, Z_READ_ONLY) != 0;
		r.stencil_write_disable = KYTY_PM4_GET(value, DB_DEPTH_VIEW, STENCIL_READ_ONLY) != 0;
		r.current_mip_level     = KYTY_PM4_GET(value, DB_DEPTH_VIEW, MIPID);
		cp->GetCtx()->SetDepthDepthView(r);
	};

	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_SIZE_XY] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HW::DepthDepthSizeXY r;
		r.x_max = KYTY_PM4_GET(value, DB_DEPTH_SIZE_XY, X_MAX);
		r.y_max = KYTY_PM4_GET(value, DB_DEPTH_SIZE_XY, Y_MAX);
		r.valid = true;
		cp->GetCtx()->SetDepthDepthSizeXY(r);
	};
	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_SIZE] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto target               = cp->GetCtx()->GetDepthRenderTarget();
		target.pitch_div8_minus1  = KYTY_PM4_GET(value, DB_DEPTH_SIZE, PITCH_TILE_MAX);
		target.height_div8_minus1 = KYTY_PM4_GET(value, DB_DEPTH_SIZE, HEIGHT_TILE_MAX);
		target.pitch_height_valid = true;
		cp->GetCtx()->SetDepthRenderTarget(target);
	};
	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_SLICE] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto target               = cp->GetCtx()->GetDepthRenderTarget();
		target.slice_div64_minus1 = KYTY_PM4_GET(value, DB_DEPTH_SLICE, SLICE_TILE_MAX);
		cp->GetCtx()->SetDepthRenderTarget(target);
	};

	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_CLEAR] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetDepthClearValue(*reinterpret_cast<const float*>(&value));
	};
	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_CLEAR] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetStencilClearValue(KYTY_PM4_GET(value, DB_STENCIL_CLEAR, CLEAR));
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HW::StencilControl r;
		r.stencil_fail     = KYTY_PM4_GET(value, DB_STENCIL_CONTROL, STENCILFAIL);
		r.stencil_zpass    = KYTY_PM4_GET(value, DB_STENCIL_CONTROL, STENCILZPASS);
		r.stencil_zfail    = KYTY_PM4_GET(value, DB_STENCIL_CONTROL, STENCILZFAIL);
		r.stencil_fail_bf  = KYTY_PM4_GET(value, DB_STENCIL_CONTROL, STENCILFAIL_BF);
		r.stencil_zpass_bf = KYTY_PM4_GET(value, DB_STENCIL_CONTROL, STENCILZPASS_BF);
		r.stencil_zfail_bf = KYTY_PM4_GET(value, DB_STENCIL_CONTROL, STENCILZFAIL_BF);
		cp->GetCtx()->SetStencilControl(r);
	};

	g_hw_ctx_indirect_func[Pm4::DB_RENDER_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HW::RenderControl r;
		r.depth_clear_enable   = KYTY_PM4_GET(value, DB_RENDER_CONTROL, DEPTH_CLEAR_ENABLE) != 0;
		r.stencil_clear_enable = KYTY_PM4_GET(value, DB_RENDER_CONTROL, STENCIL_CLEAR_ENABLE) != 0;
		r.resummarize_enable   = KYTY_PM4_GET(value, DB_RENDER_CONTROL, RESUMMARIZE_ENABLE) != 0;
		r.stencil_compress_disable =
		    KYTY_PM4_GET(value, DB_RENDER_CONTROL, STENCIL_COMPRESS_DISABLE) != 0;
		r.depth_compress_disable =
		    KYTY_PM4_GET(value, DB_RENDER_CONTROL, DEPTH_COMPRESS_DISABLE) != 0;
		r.copy_centroid = KYTY_PM4_GET(value, DB_RENDER_CONTROL, COPY_CENTROID) != 0;
		r.copy_sample   = KYTY_PM4_GET(value, DB_RENDER_CONTROL, COPY_SAMPLE);
		cp->GetCtx()->SetRenderControl(r);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCILREFMASK] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto r              = cp->GetCtx()->GetStencilMask();
		r.stencil_testval   = KYTY_PM4_GET(value, DB_STENCILREFMASK, STENCILTESTVAL);
		r.stencil_mask      = KYTY_PM4_GET(value, DB_STENCILREFMASK, STENCILMASK);
		r.stencil_writemask = KYTY_PM4_GET(value, DB_STENCILREFMASK, STENCILWRITEMASK);
		r.stencil_opval     = KYTY_PM4_GET(value, DB_STENCILREFMASK, STENCILOPVAL);
		cp->GetCtx()->SetStencilMask(r);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCILREFMASK_BF] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto r                 = cp->GetCtx()->GetStencilMask();
		r.stencil_testval_bf   = KYTY_PM4_GET(value, DB_STENCILREFMASK_BF, STENCILTESTVAL_BF);
		r.stencil_mask_bf      = KYTY_PM4_GET(value, DB_STENCILREFMASK_BF, STENCILMASK_BF);
		r.stencil_writemask_bf = KYTY_PM4_GET(value, DB_STENCILREFMASK_BF, STENCILWRITEMASK_BF);
		r.stencil_opval_bf     = KYTY_PM4_GET(value, DB_STENCILREFMASK_BF, STENCILOPVAL_BF);
		cp->GetCtx()->SetStencilMask(r);
	};

	g_hw_ctx_indirect_func[Pm4::PA_CL_CLIP_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HW::ClipControl r;
		r.user_clip_planes          = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, UCP_ENA);
		r.user_clip_plane_mode      = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, PS_UCP_MODE);
		r.dx_clip_space             = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, DX_CLIP_SPACE_DEF) != 0;
		r.vertex_kill_any           = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, VTX_KILL_OR) != 0;
		r.min_z_clip_disable        = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, ZCLIP_NEAR_DISABLE) != 0;
		r.max_z_clip_disable        = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, ZCLIP_FAR_DISABLE) != 0;
		r.user_clip_plane_negate_y  = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, PS_UCP_Y_SCALE_NEG) != 0;
		r.clip_disable              = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, CLIP_DISABLE) != 0;
		r.user_clip_plane_cull_only = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, UCP_CULL_ONLY_ENA) != 0;
		r.cull_on_clipping_error_disable =
		    KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, DIS_CLIP_ERR_DETECT) != 0;
		r.linear_attribute_clip_enable =
		    KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, DX_LINEAR_ATTR_CLIP_ENA) != 0;
		r.force_viewport_index_from_vs_enable =
		    KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, VTE_VPORT_PROVOKE_DISABLE) != 0;
		cp->GetCtx()->SetClipControl(r);
	};

	g_hw_ctx_indirect_func[Pm4::PA_SU_LINE_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto line_width = KYTY_PM4_GET(value, PA_SU_LINE_CNTL, WIDTH);
		cp->GetCtx()->SetLineWidth(line_width == 8 ? 1.0f : static_cast<float>(line_width) / 8.0f);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SC_FOV_WINDOW_LR] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreFovWindow(value);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SC_FOV_WINDOW_TB] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreFovWindow(value);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SC_FSR_ENABLE] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreFsrRegister(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::FSR_RECURSIONS0] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreFsrRegister(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::FSR_RECURSIONS1] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreFsrRegister(cmd_offset, value);
	};

	g_hw_ctx_indirect_func[Pm4::PA_CL_VTE_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetViewportTransformControl(value);
	};

	g_hw_ctx_indirect_func[Pm4::PA_SC_MODE_CNTL_0] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HW::ScanModeControl r;
		r.msaa_enable          = KYTY_PM4_GET(value, PA_SC_MODE_CNTL_0, MSAA_ENABLE) != 0;
		r.vport_scissor_enable = KYTY_PM4_GET(value, PA_SC_MODE_CNTL_0, VPORT_SCISSOR_ENABLE) != 0;
		r.line_stipple_enable  = KYTY_PM4_GET(value, PA_SC_MODE_CNTL_0, LINE_STIPPLE_ENABLE) != 0;
		cp->GetCtx()->SetScanModeControl(r);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SC_MODE_CNTL_1] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreScanModeControl1(value);
	};

	g_hw_ctx_indirect_func[Pm4::PA_SC_AA_CONFIG] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetAaConfig(ParseAaConfig(value));
	};

	for (auto cmd_offset = Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0;
	     cmd_offset < Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0 + 16; cmd_offset++) {
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
			auto r = cp->GetCtx()->GetAaSampleControl();
			r.locations[cmd_offset - Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0] = value;
			cp->GetCtx()->SetAaSampleControl(r);
		};
	}

	g_hw_ctx_indirect_func[Pm4::PA_SC_CENTROID_PRIORITY_0] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto r              = cp->GetCtx()->GetAaSampleControl();
		r.centroid_priority = (r.centroid_priority & 0xffffffff00000000ull) | value;
		cp->GetCtx()->SetAaSampleControl(r);
	};

	g_hw_ctx_indirect_func[Pm4::PA_SC_CENTROID_PRIORITY_1] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		auto r = cp->GetCtx()->GetAaSampleControl();
		r.centroid_priority =
		    (r.centroid_priority & 0x00000000ffffffffull) | (static_cast<uint64_t>(value) << 32u);
		cp->GetCtx()->SetAaSampleControl(r);
	};

	g_hw_ctx_indirect_func[Pm4::PA_SC_AA_MASK_X0Y0_X1Y0] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreAaMaskRegister(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SC_AA_MASK_X0Y1_X1Y1] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreAaMaskRegister(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SU_VTX_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreVtxControl(value);
	};
	g_hw_ctx_indirect_func[Pm4::PS_SHADER_SAMPLE_EXCLUSION_MASK] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreAaMaskRegister(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SC_BINNER_CNTL_0] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnorePaScExtendedControl(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SC_BINNER_CNTL_1] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnorePaScExtendedControl(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SC_CONSERVATIVE_RASTERIZATION_CNTL] =
	    [](KYTY_HW_CTX_INDIRECT_ARGS) { HwCtxIgnorePaScExtendedControl(cmd_offset, value); };
	g_hw_ctx_indirect_func[Pm4::PA_SC_NGG_MODE_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnorePaScExtendedControl(cmd_offset, value);
	};
	g_hw_ctx_indirect_func[Pm4::DB_ALPHA_TO_MASK] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HwCtxIgnoreAlphaToMaskRegister(value);
	};
	g_hw_ctx_indirect_func[Pm4::CB_COLOR_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HW::ColorControl r;
		r.mode = KYTY_PM4_GET(value, CB_COLOR_CONTROL, MODE);
		r.op   = KYTY_PM4_GET(value, CB_COLOR_CONTROL, ROP3);
		cp->GetCtx()->SetColorControl(r);
	};

	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		HW::DepthControl r;
		r.stencil_enable      = KYTY_PM4_GET(value, DB_DEPTH_CONTROL, STENCIL_ENABLE) != 0;
		r.z_enable            = KYTY_PM4_GET(value, DB_DEPTH_CONTROL, Z_ENABLE) != 0;
		r.z_write_enable      = KYTY_PM4_GET(value, DB_DEPTH_CONTROL, Z_WRITE_ENABLE) != 0;
		r.depth_bounds_enable = KYTY_PM4_GET(value, DB_DEPTH_CONTROL, DEPTH_BOUNDS_ENABLE) != 0;
		r.zfunc               = KYTY_PM4_GET(value, DB_DEPTH_CONTROL, ZFUNC);
		r.backface_enable     = KYTY_PM4_GET(value, DB_DEPTH_CONTROL, BACKFACE_ENABLE) != 0;
		r.stencilfunc         = KYTY_PM4_GET(value, DB_DEPTH_CONTROL, STENCILFUNC);
		r.stencilfunc_bf      = KYTY_PM4_GET(value, DB_DEPTH_CONTROL, STENCILFUNC_BF);
		r.color_writes_on_depth_fail_enable =
		    KYTY_PM4_GET(value, DB_DEPTH_CONTROL, ENABLE_COLOR_WRITES_ON_DEPTH_FAIL) != 0;
		r.color_writes_on_depth_pass_disable =
		    KYTY_PM4_GET(value, DB_DEPTH_CONTROL, DISABLE_COLOR_WRITES_ON_DEPTH_PASS) != 0;
		cp->GetCtx()->SetDepthControl(r);
	};

	g_hw_ctx_indirect_func[Pm4::DB_EQAA] = [](KYTY_HW_CTX_INDIRECT_ARGS) {
		cp->GetCtx()->SetEqaaControl(ParseEqaaControl(value));
	};
}

void GraphicsInitJmpTablesShIndirect() {
	for (auto& func: g_hw_sh_indirect_func) {
		func = nullptr;
	}

	g_hw_sh_indirect_func[Pm4::COMPUTE_PGM_LO] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_PGM_HI] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_TBA_LO] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_TBA_HI] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_TMA_LO] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_TMA_HI] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_PGM_RSRC1] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_PGM_RSRC2] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_START_X] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_START_Y] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_START_Z] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_NUM_THREAD_X] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_NUM_THREAD_Y] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_NUM_THREAD_Z] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_RESOURCE_LIMITS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_DESTINATION_EN_SE0] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_DESTINATION_EN_SE1] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_TMPRING_SIZE] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_DESTINATION_EN_SE2] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_DESTINATION_EN_SE3] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_PGM_RSRC3] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_SHADER_CHKSUM] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_DISPATCH_TUNNEL] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShSetCsRegister(cp, cmd_offset, value);
	};

	for (uint32_t slot = 0; slot < 16; slot++) {
		g_hw_sh_indirect_func[Pm4::COMPUTE_USER_DATA_0 + slot] = [](KYTY_HW_SH_INDIRECT_ARGS) {
			auto sgpr = cmd_offset - Pm4::COMPUTE_USER_DATA_0;
			cp->GetShCtx()->SetCsUserSgpr(sgpr, value, cp->GetUserDataMarker());
			cp->SetUserDataMarker(HW::UserSgprType::Unknown);
		};
	}
	for (uint32_t slot = 0; Pm4::COMPUTE_USER_ACCUM_0 + slot < Pm4::COMPUTE_USER_DATA_0; slot++) {
		g_hw_sh_indirect_func[Pm4::COMPUTE_USER_ACCUM_0 + slot] = [](KYTY_HW_SH_INDIRECT_ARGS) {
			auto sgpr = 16u + (cmd_offset - Pm4::COMPUTE_USER_ACCUM_0);
			cp->GetShCtx()->SetCsUserSgpr(sgpr, value, cp->GetUserDataMarker());
			cp->SetUserDataMarker(HW::UserSgprType::Unknown);
		};
	}
	for (uint32_t slot = 0; slot < 32; slot++) {
		g_hw_sh_indirect_func[Pm4::SPI_SHADER_USER_DATA_PS_0 + slot] =
		    [](KYTY_HW_SH_INDIRECT_ARGS) {
			    auto sgpr = cmd_offset - Pm4::SPI_SHADER_USER_DATA_PS_0;
			    cp->GetShCtx()->SetPsUserSgpr(sgpr, value, cp->GetUserDataMarker());
			    cp->SetUserDataMarker(HW::UserSgprType::Unknown);
		    };
		g_hw_sh_indirect_func[Pm4::SPI_SHADER_USER_DATA_GS_0 + slot] =
		    [](KYTY_HW_SH_INDIRECT_ARGS) {
			    auto sgpr = cmd_offset - Pm4::SPI_SHADER_USER_DATA_GS_0;
			    cp->GetShCtx()->SetGsUserSgpr(sgpr, value, cp->GetUserDataMarker());
			    cp->SetUserDataMarker(HW::UserSgprType::Unknown);
		    };
		g_hw_sh_indirect_func[Pm4::SPI_SHADER_USER_DATA_HS_0 + slot] =
		    [](KYTY_HW_SH_INDIRECT_ARGS) {
			    auto sgpr = cmd_offset - Pm4::SPI_SHADER_USER_DATA_HS_0;
			    cp->GetShCtx()->SetHsUserSgpr(sgpr, value, cp->GetUserDataMarker());
			    cp->SetUserDataMarker(HW::UserSgprType::Unknown);
		    };
	}
	for (uint32_t slot = 0; slot < 4; slot++) {
		g_hw_sh_indirect_func[Pm4::SPI_SHADER_USER_ACCUM_PS_0 + slot] =
		    [](KYTY_HW_SH_INDIRECT_ARGS) {
			    auto sgpr = 16u + (cmd_offset - Pm4::SPI_SHADER_USER_ACCUM_PS_0);
			    cp->GetShCtx()->SetPsUserSgpr(sgpr, value, cp->GetUserDataMarker());
			    cp->SetUserDataMarker(HW::UserSgprType::Unknown);
		    };
		g_hw_sh_indirect_func[Pm4::SPI_SHADER_USER_ACCUM_ESGS_0 + slot] =
		    [](KYTY_HW_SH_INDIRECT_ARGS) {
			    auto sgpr = 16u + (cmd_offset - Pm4::SPI_SHADER_USER_ACCUM_ESGS_0);
			    cp->GetShCtx()->SetGsUserSgpr(sgpr, value, cp->GetUserDataMarker());
			    cp->SetUserDataMarker(HW::UserSgprType::Unknown);
		    };
		g_hw_sh_indirect_func[Pm4::SPI_SHADER_USER_ACCUM_LSHS_0 + slot] =
		    [](KYTY_HW_SH_INDIRECT_ARGS) {
			    auto sgpr = 16u + (cmd_offset - Pm4::SPI_SHADER_USER_ACCUM_LSHS_0);
			    cp->GetShCtx()->SetHsUserSgpr(sgpr, value, cp->GetUserDataMarker());
			    cp->SetUserDataMarker(HW::UserSgprType::Unknown);
		    };
	}
	g_hw_sh_indirect_func[Pm4::SPI_SHADER_TBA_LO_PS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShIgnoreShaderRegister(cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC3_PS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShIgnoreShaderRegister(cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC4_PS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShIgnoreShaderRegister(cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::SPI_SHADER_TMA_LO_PS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShIgnoreShaderRegister(cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::SPI_SHADER_TMA_HI_PS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShIgnoreShaderRegister(cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::SPI_SHADER_REQ_CTRL_PS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShIgnoreShaderRegister(cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC4_GS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShIgnoreShaderRegister(cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC3_GS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShIgnoreShaderRegister(cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::SPI_SHADER_USER_DATA_ADDR_LO_GS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShIgnoreShaderRegister(cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::SPI_SHADER_USER_DATA_ADDR_HI_GS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShIgnoreShaderRegister(cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::SPI_SHADER_REQ_CTRL_ESGS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShIgnoreShaderRegister(cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC4_HS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShIgnoreShaderRegister(cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC3_HS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShIgnoreShaderRegister(cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::SPI_SHADER_USER_DATA_ADDR_LO_HS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShIgnoreShaderRegister(cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::SPI_SHADER_USER_DATA_ADDR_HI_HS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShIgnoreShaderRegister(cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::SPI_SHADER_REQ_CTRL_LSHS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HwShIgnoreShaderRegister(cmd_offset, value);
	};
	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_LO_HS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		auto base = cp->GetShCtx()->GetVs().hs_regs.data_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetShCtx()->SetHsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_HI_HS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		auto base = cp->GetShCtx()->GetVs().hs_regs.data_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetShCtx()->SetHsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_CHKSUM_HS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		cp->GetShCtx()->SetHsShaderChksum(value);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC1_HS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HW::VsShaderResource1 r1;
		r1.vgprs                = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, VGPRS);
		r1.sgprs                = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, SGPRS);
		r1.priority             = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, PRIORITY);
		r1.float_mode           = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, FLOAT_MODE);
		r1.dx10_clamp           = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, DX10_CLAMP) != 0;
		r1.ieee_mode            = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, IEEE_MODE) != 0;
		r1.vgpr_component_count = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT);
		r1.cu_group_enable = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, CU_GROUP_ENABLE) != 0;
		r1.require_forward_progress =
		    KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, FWD_PROGRESS) != 0;
		r1.fp16_overflow = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, FP16_OVFL) != 0;
		cp->GetShCtx()->SetHsShaderResource1(r1);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC2_HS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HW::VsShaderResource2 r2;
		r2.scratch_en        = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_VS, SCRATCH_EN) != 0;
		r2.user_sgpr         = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR) +
		                       (KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR_MSB) << 5u);
		r2.offchip_lds       = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_VS, OC_LDS_EN) != 0;
		r2.streamout_enabled = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_VS, SO_EN) != 0;
		r2.shared_vgprs      = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_VS, SHARED_VGPR_CNT);
		cp->GetShCtx()->SetHsShaderResource2(r2);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_LO_LS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		auto base = cp->GetShCtx()->GetVs().ls_regs.data_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetShCtx()->SetLsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_HI_LS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		auto base = cp->GetShCtx()->GetVs().ls_regs.data_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetShCtx()->SetLsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC1_LS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HW::VsShaderResource1 r1;
		r1.vgprs                = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, VGPRS);
		r1.sgprs                = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, SGPRS);
		r1.priority             = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, PRIORITY);
		r1.float_mode           = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, FLOAT_MODE);
		r1.dx10_clamp           = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, DX10_CLAMP) != 0;
		r1.ieee_mode            = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, IEEE_MODE) != 0;
		r1.vgpr_component_count = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT);
		r1.cu_group_enable = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, CU_GROUP_ENABLE) != 0;
		r1.require_forward_progress =
		    KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, FWD_PROGRESS) != 0;
		r1.fp16_overflow = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_VS, FP16_OVFL) != 0;
		cp->GetShCtx()->SetLsShaderResource1(r1);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC2_LS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HW::VsShaderResource2 r2;
		r2.scratch_en        = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_VS, SCRATCH_EN) != 0;
		r2.user_sgpr         = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR) +
		                       (KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR_MSB) << 5u);
		r2.offchip_lds       = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_VS, OC_LDS_EN) != 0;
		r2.streamout_enabled = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_VS, SO_EN) != 0;
		r2.shared_vgprs      = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_VS, SHARED_VGPR_CNT);
		cp->GetShCtx()->SetLsShaderResource2(r2);
	};
	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_LO_ES] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		auto base = cp->GetShCtx()->GetVs().es_regs.data_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetShCtx()->SetEsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_HI_ES] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		auto base = cp->GetShCtx()->GetVs().es_regs.data_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetShCtx()->SetEsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_LO_GS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		auto base = cp->GetShCtx()->GetVs().gs_regs.data_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetShCtx()->SetGsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_HI_GS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		auto base = cp->GetShCtx()->GetVs().gs_regs.data_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetShCtx()->SetGsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_CHKSUM_GS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		cp->GetShCtx()->SetGsShaderChksum(value);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC1_GS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HW::GsShaderResource1 r1;
		r1.vgprs           = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, VGPRS);
		r1.sgprs           = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, SGPRS);
		r1.priority        = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, PRIORITY);
		r1.float_mode      = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, FLOAT_MODE);
		r1.dx10_clamp      = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, DX10_CLAMP) != 0;
		r1.debug_mode      = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, DEBUG_MODE) != 0;
		r1.ieee_mode       = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, IEEE_MODE) != 0;
		r1.cu_group_enable = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, CU_GROUP_ENABLE) != 0;
		r1.require_forward_progress =
		    KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, FWD_PROGRESS) != 0;
		r1.lds_configuration       = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, WGP_MODE) != 0;
		r1.gs_vgpr_component_count = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, GS_VGPR_COMP_CNT);
		r1.fp16_overflow           = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, FP16_OVFL) != 0;
		cp->GetShCtx()->SetGsShaderResource1(r1);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC2_GS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HW::GsShaderResource2 r2;
		r2.scratch_en = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, SCRATCH_EN) != 0;
		r2.user_sgpr  = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR) +
		                (KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB) << 5u);
		r2.es_vgpr_component_count = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, ES_VGPR_COMP_CNT);
		r2.offchip_lds             = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, OC_LDS_EN) != 0;
		r2.lds_size                = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, LDS_SIZE);
		r2.shared_vgprs            = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, SHARED_VGPR_CNT);
		cp->GetShCtx()->SetGsShaderResource2(r2);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_LO_PS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		auto base = cp->GetShCtx()->GetPs().ps_regs.data_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetShCtx()->SetPsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_HI_PS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		auto base = cp->GetShCtx()->GetPs().ps_regs.data_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetShCtx()->SetPsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_CHKSUM_PS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		cp->GetShCtx()->SetPsShaderChksum(value);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC1_PS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HW::PsShaderResource1 r1;
		r1.vgprs            = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, VGPRS);
		r1.sgprs            = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, SGPRS);
		r1.priority         = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, PRIORITY);
		r1.float_mode       = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, FLOAT_MODE);
		r1.dx10_clamp       = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, DX10_CLAMP) != 0;
		r1.debug_mode       = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, DEBUG_MODE) != 0;
		r1.ieee_mode        = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, IEEE_MODE) != 0;
		r1.cu_group_disable = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, CU_GROUP_DISABLE) != 0;
		r1.require_forward_progress =
		    KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, FWD_PROGRESS) != 0;
		r1.fp16_overflow = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, FP16_OVFL) != 0;
		cp->GetShCtx()->SetPsShaderResource1(r1);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC2_PS] = [](KYTY_HW_SH_INDIRECT_ARGS) {
		HW::PsShaderResource2 r2;
		r2.scratch_en     = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, SCRATCH_EN);
		r2.user_sgpr      = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR) +
		                    (KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR_MSB) << 5u);
		r2.wave_cnt_en    = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, WAVE_CNT_EN);
		r2.extra_lds_size = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, EXTRA_LDS_SIZE);
		r2.raster_ordered_shading =
		    KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, LOAD_INTRAWAVE_COLLISION);
		r2.shared_vgprs = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, SHARED_VGPR_CNT);
		cp->GetShCtx()->SetPsShaderResource2(r2);
	};
}

void GraphicsInitJmpTablesUcIndirect() {
	for (auto& func: g_hw_uc_indirect_func) {
		func = nullptr;
	}

	g_hw_uc_indirect_func[Pm4::GE_CNTL] = [](KYTY_HW_UC_INDIRECT_ARGS) {
		HW::GeControl r;
		r.primitive_group_size = KYTY_PM4_GET(value, GE_CNTL, PRIM_GRP_SIZE);
		r.vertex_group_size    = KYTY_PM4_GET(value, GE_CNTL, VERT_GRP_SIZE);
		cp->GetUcfg()->SetGeControl(r);
	};

	g_hw_uc_indirect_func[Pm4::GE_USER_VGPR_EN] = [](KYTY_HW_UC_INDIRECT_ARGS) {
		HW::GeUserVgprEn r;
		r.vgpr1 = KYTY_PM4_GET(value, GE_USER_VGPR_EN, EN_USER_VGPR1) != 0;
		r.vgpr2 = KYTY_PM4_GET(value, GE_USER_VGPR_EN, EN_USER_VGPR2) != 0;
		r.vgpr3 = KYTY_PM4_GET(value, GE_USER_VGPR_EN, EN_USER_VGPR3) != 0;
		cp->GetUcfg()->SetGeUserVgprEn(r);
	};

	g_hw_uc_indirect_func[Pm4::VGT_PRIMITIVE_TYPE] = [](KYTY_HW_UC_INDIRECT_ARGS) {
		uint32_t prim_type = KYTY_PM4_GET(value, VGT_PRIMITIVE_TYPE, PRIM_TYPE);
		cp->GetUcfg()->SetPrimitiveType(prim_type);
	};
	g_hw_uc_indirect_func[Pm4::VGT_INDEX_TYPE] = [](KYTY_HW_UC_INDIRECT_ARGS) {
		cp->SetIndexType(value & 0x3u);
	};
	g_hw_uc_indirect_func[Pm4::VGT_OBJECT_ID] = [](KYTY_HW_UC_INDIRECT_ARGS) {
		cp->GetUcfg()->SetObjectId(value);
	};
	g_hw_uc_indirect_func[Pm4::TEXTURE_GRADIENT_FACTORS] = [](KYTY_HW_UC_INDIRECT_ARGS) {
		HwUcIgnoreTextureGradientFactors(value);
	};
	g_hw_uc_indirect_func[Pm4::IA_MULTI_VGT_PARAM] = [](KYTY_HW_UC_INDIRECT_ARGS) {
		HwUcIgnoreIaMultiVgtParam(value);
	};
	g_hw_uc_indirect_func[Pm4::GE_MULTI_PRIM_IB_RESET_EN] = [](KYTY_HW_UC_INDIRECT_ARGS) {
		HwIgnoreMultiPrimIbReset(value);
	};
	g_hw_uc_indirect_func[Pm4::TA_CS_BC_BASE_ADDR] = [](KYTY_HW_UC_INDIRECT_ARGS) {
		HwUcIgnoreBorderColorTableAddr(cmd_offset, value);
	};
	g_hw_uc_indirect_func[Pm4::TA_CS_BC_BASE_ADDR_HI] = [](KYTY_HW_UC_INDIRECT_ARGS) {
		HwUcIgnoreBorderColorTableAddr(cmd_offset, value);
	};

	g_hw_uc_indirect_func[Pm4::GE_INDX_OFFSET] = [](KYTY_HW_UC_INDIRECT_ARGS) {
		cp->GetUcfg()->SetIndexOffset(value);
	};
	g_hw_uc_indirect_func[Pm4::GDS_OA_CNTL] = [](KYTY_HW_UC_INDIRECT_ARGS) {
		HwUcSetGdsOaRegister(cp, cmd_offset, value);
	};
	g_hw_uc_indirect_func[Pm4::GDS_OA_COUNTER] = [](KYTY_HW_UC_INDIRECT_ARGS) {
		HwUcSetGdsOaRegister(cp, cmd_offset, value);
	};
	g_hw_uc_indirect_func[Pm4::GDS_OA_ADDRESS] = [](KYTY_HW_UC_INDIRECT_ARGS) {
		HwUcSetGdsOaRegister(cp, cmd_offset, value);
	};

	g_hw_uc_indirect_func[Pm4::GE_STEREO_CNTL] = [](KYTY_HW_UC_INDIRECT_ARGS) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 16) {
			LOGF("warning: ignoring indirect uc reg GE_STEREO_CNTL at 0x%" PRIx32
			     ", value = 0x%08" PRIx32 "\n",
			     cmd_offset, value);
		}
	};
}

} // namespace Libs::Graphics
