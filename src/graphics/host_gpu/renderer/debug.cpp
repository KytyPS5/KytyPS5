#include "graphics/host_gpu/renderer/debug.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/render.h"

#include <atomic>
#include <cmath>
#include <fmt/format.h>

namespace Libs::Graphics {

static std::atomic<uint32_t> g_scissor_default_log_count = 0;

uint32_t render_target_mask_slot(uint32_t mask, uint32_t slot) {
	return (mask >> (slot * 4u)) & 0x0fu;
}

static bool RenderTargetMaskHasMrt(uint32_t mask) {
	return (mask & ~0x0fu) != 0;
}

static bool RenderTargetMaskHasBoundMrt(const HW::Context& hw) {
	const auto mask = hw.GetRenderTargetMask();

	if (!RenderTargetMaskHasMrt(mask)) {
		return false;
	}

	uint32_t bound_targets = 0;
	for (uint32_t i = 0; i < 8; i++) {
		if (render_target_mask_slot(mask, i) != 0 && hw.GetRenderTarget(i).base.addr != 0) {
			bound_targets++;
		}
	}

	return bound_targets > 1;
}

uint32_t render_target_first_bound_slot(const HW::Context& hw) {
	const auto mask = hw.GetRenderTargetMask();
	for (uint32_t i = 0; i < 8; i++) {
		if (render_target_mask_slot(mask, i) != 0 && hw.GetRenderTarget(i).base.addr != 0) {
			return i;
		}
	}

	return 0;
}

bool graphics_debug_dump_enabled() {
	return Config::GraphicsDebugDumpEnabled() &&
	       Config::GetPrintfDirection() != Config::OutputDirection::Silent;
}

void uc_print(const char* func, const HW::UserConfig& uc) {
	LOGF("%s\n", func);

	const auto& ge_cntl = uc.GetGeControl();
	const auto& user_en = uc.GetGeUserVgprEn();

	LOGF("\t GetPrimType()         = 0x%08" PRIx32 "\n"
	     "\t GetIndexOffset()      = 0x%08" PRIx32 "\n"
	     "\t GetObjectId()         = 0x%08" PRIx32 "\n"
	     "\t primitive_group_size  = 0x%04" PRIx16 "\n"
	     "\t vertex_group_size     = 0x%04" PRIx16 "\n"
	     "\t en_user_vgpr1         = %s\n"
	     "\t en_user_vgpr2         = %s\n"
	     "\t en_user_vgpr3         = %s\n",
	     uc.GetPrimType(), uc.GetIndexOffset(), uc.GetObjectId(), ge_cntl.primitive_group_size,
	     ge_cntl.vertex_group_size, user_en.vgpr1 ? "true" : "false",
	     user_en.vgpr2 ? "true" : "false", user_en.vgpr3 ? "true" : "false");
}

void uc_check(const HW::UserConfig& uc) {
	const auto& ge_cntl = uc.GetGeControl();
	const auto& user_en = uc.GetGeUserVgprEn();

	if (ge_cntl.primitive_group_size > 0x0040) {
		LOGF("\t warning: unsupported GE_CNTL primitive_group_size = 0x%04" PRIx16 "\n",
		     ge_cntl.primitive_group_size);
	}
	if (ge_cntl.vertex_group_size > 0x0040) {
		LOGF("\t warning: unsupported GE_CNTL vertex_group_size = 0x%04" PRIx16 "\n",
		     ge_cntl.vertex_group_size);
	}
	EXIT_NOT_IMPLEMENTED(user_en.vgpr1 != false);
	EXIT_NOT_IMPLEMENTED(user_en.vgpr2 != false);
	EXIT_NOT_IMPLEMENTED(user_en.vgpr3 != false);
}

void sh_print(const char* func, const HW::Shader& /*uc*/) {
	LOGF("%s\n", func);
}

void sh_check(const HW::Shader& /*uc*/) {}

std::vector<std::string> rt_print(const char* func, const HW::RenderTarget& rt) {
	std::vector<std::string> dst;
	dst.reserve(53);

	dst.push_back(fmt::format("{}\n", func));

	dst.push_back(fmt::format("\t base.addr                       = 0x{:016x}\n", rt.base.addr));
	dst.push_back(
	    fmt::format("\t pitch.pitch_div8_minus1         = 0x{:08x}\n", rt.pitch.pitch_div8_minus1));
	dst.push_back(fmt::format("\t pitch.fmask_pitch_div8_minus1   = 0x{:08x}\n",
	                          rt.pitch.fmask_pitch_div8_minus1));
	dst.push_back(fmt::format("\t slice.slice_div64_minus1        = 0x{:08x}\n",
	                          rt.slice.slice_div64_minus1));
	dst.push_back(fmt::format("\t view.base_array_slice_index     = 0x{:08x}\n",
	                          rt.view.base_array_slice_index));
	dst.push_back(fmt::format("\t view.last_array_slice_index     = 0x{:08x}\n",
	                          rt.view.last_array_slice_index));
	dst.push_back(
	    fmt::format("\t view.current_mip_level          = 0x{:08x}\n", rt.view.current_mip_level));
	dst.push_back(fmt::format("\t info.fmask_compression_enable   = {}\n",
	                          rt.info.fmask_compression_enable ? "true" : "false"));

	// dst.push_back(fmt::format("\t info.fmask_compression_mode     = 0x{:08x}\n", //
	// rt.info.fmask_compression_mode));
	dst.push_back(fmt::format("\t info.fmask_data_compression_disable = {}\n",
	                          rt.info.fmask_data_compression_disable ? "true" : "false"));
	dst.push_back(fmt::format("\t info.fmask_one_frag_mode        = {}\n",
	                          rt.info.fmask_one_frag_mode ? "true" : "false"));

	dst.push_back(fmt::format("\t info.cmask_fast_clear_enable    = {}\n",
	                          rt.info.cmask_fast_clear_enable ? "true" : "false"));
	dst.push_back(fmt::format("\t info.dcc_compression_enable     = {}\n",
	                          rt.info.dcc_compression_enable ? "true" : "false"));
	dst.push_back(
	    fmt::format("\t info.cmask_is_linear            = 0x{:08x}\n", rt.info.cmask_is_linear));
	dst.push_back(
	    fmt::format("\t info.cmask_addr_type            = 0x{:08x}\n", rt.info.cmask_addr_type));
	dst.push_back(fmt::format("\t info.alt_tile_mode              = {}\n",
	                          rt.info.alt_tile_mode ? "true" : "false"));
	dst.push_back(fmt::format("\t info.format                     = 0x{:08x}\n", rt.info.format));
	dst.push_back(
	    fmt::format("\t info.channel_type               = 0x{:08x}\n", rt.info.channel_type));
	dst.push_back(
	    fmt::format("\t info.channel_order              = 0x{:08x}\n", rt.info.channel_order));
	dst.push_back(fmt::format("\t info.blend_bypa                 = {}\n",
	                          rt.info.blend_bypass ? "true" : "false"));
	dst.push_back(fmt::format("\t info.blend_clamp                = {}\n",
	                          rt.info.blend_clamp ? "true" : "false"));
	dst.push_back(fmt::format("\t info.round_mode                 = {}\n",
	                          rt.info.round_mode ? "true" : "false"));
	dst.push_back(fmt::format("\t attrib.force_dest_alpha_to_one  = {}\n",
	                          rt.attrib.force_dest_alpha_to_one ? "true" : "false"));
	dst.push_back(
	    fmt::format("\t attrib.tile_mode                = 0x{:08x}\n", rt.attrib.tile_mode));
	dst.push_back(
	    fmt::format("\t attrib.fmask_tile_mode          = 0x{:08x}\n", rt.attrib.fmask_tile_mode));
	dst.push_back(
	    fmt::format("\t attrib.num_samples              = 0x{:08x}\n", rt.attrib.num_samples));
	dst.push_back(
	    fmt::format("\t attrib.num_fragments            = 0x{:08x}\n", rt.attrib.num_fragments));
	dst.push_back(fmt::format("\t attrib2.width                   = 0x{:08x}\n", rt.attrib2.width));
	dst.push_back(
	    fmt::format("\t attrib2.height                  = 0x{:08x}\n", rt.attrib2.height));
	dst.push_back(
	    fmt::format("\t attrib2.num_mip_levels          = 0x{:08x}\n", rt.attrib2.num_mip_levels));
	dst.push_back(fmt::format("\t attrib3.depth                   = 0x{:08x}\n", rt.attrib3.depth));
	dst.push_back(
	    fmt::format("\t attrib3.tile_mode               = 0x{:08x}\n", rt.attrib3.tile_mode));
	dst.push_back(
	    fmt::format("\t attrib3.dimension               = 0x{:08x}\n", rt.attrib3.dimension));
	dst.push_back(fmt::format("\t attrib3.cmask_pipe_aligned      = {}\n",
	                          rt.attrib3.cmask_pipe_aligned ? "true" : "false"));
	dst.push_back(fmt::format("\t attrib3.dcc_pipe_aligned        = {}\n",
	                          rt.attrib3.dcc_pipe_aligned ? "true" : "false"));
	dst.push_back(fmt::format("\t dcc.max_uncompressed_block_size = 0x{:08x}\n",
	                          rt.dcc.max_uncompressed_block_size));
	dst.push_back(fmt::format("\t dcc.max_compressed_block_size   = 0x{:08x}\n",
	                          rt.dcc.max_compressed_block_size));
	dst.push_back(fmt::format("\t dcc.min_compressed_block_size   = 0x{:08x}\n",
	                          rt.dcc.min_compressed_block_size));
	dst.push_back(
	    fmt::format("\t dcc.color_transform             = 0x{:08x}\n", rt.dcc.color_transform));
	dst.push_back(fmt::format("\t dcc.overwrite_combiner_disable  = {}\n",
	                          rt.dcc.overwrite_combiner_disable ? "true" : "false"));
	dst.push_back(fmt::format("\t dcc.independent_64b_blocks      = {}\n",
	                          rt.dcc.independent_64b_blocks ? "true" : "false"));
	dst.push_back(fmt::format("\t dcc.independent_128b_blocks     = {}\n",
	                          rt.dcc.independent_128b_blocks ? "true" : "false"));
	dst.push_back(fmt::format("\t data_write_on_dcc_clear_to_reg  = {}\n",
	                          rt.dcc.data_write_on_dcc_clear_to_reg ? "true" : "false"));
	dst.push_back(fmt::format("\t dcc.dcc_clear_key_enable        = {}\n",
	                          rt.dcc.dcc_clear_key_enable ? "true" : "false"));
	dst.push_back(fmt::format("\t cmask.addr                      = 0x{:016x}\n", rt.cmask.addr));
	dst.push_back(fmt::format("\t cmask_slice.slice_minus1        = 0x{:08x}\n",
	                          rt.cmask_slice.slice_minus1));
	dst.push_back(fmt::format("\t fmask.addr                      = 0x{:016x}\n", rt.fmask.addr));
	dst.push_back(fmt::format("\t fmask_slice.slice_minus1        = 0x{:08x}\n",
	                          rt.fmask_slice.slice_minus1));
	dst.push_back(
	    fmt::format("\t clear_word0.word0               = 0x{:08x}\n", rt.clear_word0.word0));
	dst.push_back(
	    fmt::format("\t clear_word1.word1               = 0x{:08x}\n", rt.clear_word1.word1));
	dst.push_back(
	    fmt::format("\t dcc_addr.addr                   = 0x{:016x}\n", rt.dcc_addr.addr));
	dst.push_back(fmt::format("\t size.width                      = 0x{:08x}\n", rt.size.width));
	dst.push_back(fmt::format("\t size.height                     = 0x{:08x}\n", rt.size.height));

	return dst;
}

static bool RenderIsColorTileMode(uint32_t tile_mode) {
	// AGC CxRenderTarget::TileMode shifted by CB_COLOR*_ATTRIB3.COLOR_SW_MODE.
	switch (tile_mode) {
		case Prospero::GpuEnumValue(Prospero::TileMode::kLinear):
		case Prospero::GpuEnumValue(Prospero::TileMode::kStandard256B):
		case Prospero::GpuEnumValue(Prospero::TileMode::kStandard4KB):
		case Prospero::GpuEnumValue(Prospero::TileMode::kStandard64KB):
		case Prospero::GpuEnumValue(Prospero::TileMode::kPrt):
		case Prospero::GpuEnumValue(Prospero::TileMode::kDepth):
		case Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget): return true;
		default: return false;
	}
}

bool RenderIsColorTileModeLinear(uint32_t tile_mode) {
	return tile_mode == Prospero::GpuEnumValue(Prospero::TileMode::kLinear);
}

static bool RenderIsColorDimension(uint32_t dimension) {
	return dimension <= 0x02;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void RtCheck(const HW::RenderTarget& rt) {
	if (rt.base.addr != 0) {
		// bool render_to_texture = (rt.attrib.tile_mode == 0x0d);
		//  EXIT_NOT_IMPLEMENTED(rt.base_addr == 0);

		EXIT_NOT_IMPLEMENTED(rt.pitch.pitch_div8_minus1 != 0);
		EXIT_NOT_IMPLEMENTED(rt.pitch.fmask_pitch_div8_minus1 != 0);
		EXIT_NOT_IMPLEMENTED(rt.slice.slice_div64_minus1 != 0);

		EXIT_NOT_IMPLEMENTED(rt.view.base_array_slice_index > rt.view.last_array_slice_index);
		if (rt.view.base_array_slice_index != 0x00000000 ||
		    rt.view.last_array_slice_index != 0x00000000) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: using color target array slice range %" PRIu32 "..%" PRIu32
				     "\n",
				     rt.view.base_array_slice_index, rt.view.last_array_slice_index);
				logged = true;
			}
		}
		if (rt.view.current_mip_level != 0x00000000) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: using PS5 color target mip level %" PRIu32 "\n",
				     rt.view.current_mip_level);
				logged = true;
			}
		}
		if (rt.info.fmask_compression_enable) {
			EXIT_NOT_IMPLEMENTED(rt.attrib.num_samples == 0 && rt.attrib.num_fragments == 0);
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: ignoring PS5 FMASK metadata for single-sample MSAA "
				     "fallback, fmask=0x%016" PRIx64 "\n",
				     rt.fmask.addr);
				logged = true;
			}
		}

		// EXIT_NOT_IMPLEMENTED(rt.info.fmask_compression_mode != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.info.fmask_data_compression_disable != false);
		EXIT_NOT_IMPLEMENTED(rt.info.fmask_one_frag_mode != false);

		if (rt.info.cmask_fast_clear_enable || rt.info.dcc_compression_enable) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: ignoring PS5 color metadata fast_clear=%s dcc=%s "
				     "cmask=0x%016" PRIx64 " dcc_addr=0x%016" PRIx64 "\n",
				     rt.info.cmask_fast_clear_enable ? "true" : "false",
				     rt.info.dcc_compression_enable ? "true" : "false", rt.cmask.addr,
				     rt.dcc_addr.addr);
				logged = true;
			}
		}
		if (rt.info.cmask_is_linear != 0x00000000 || rt.info.cmask_addr_type != 0x00000000) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: ignoring PS5 CMASK metadata fields "
				     "is_linear=%u addr_type=%u\n",
				     rt.info.cmask_is_linear, rt.info.cmask_addr_type);
				logged = true;
			}
		}
		if (rt.info.alt_tile_mode) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: ignoring alternate tile mode flag\n");
				logged = true;
			}
		}
		if (rt.info.blend_bypass) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: using PS5 blend bypass as disabled Vulkan "
				     "blending\n");
				logged = true;
			}
		}
		// EXIT_NOT_IMPLEMENTED(rt.info.blend_clamp != false);
		if (rt.info.round_mode) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: ignoring PS5 color round mode\n");
				logged = true;
			}
		}
		//		 EXIT_NOT_IMPLEMENTED(rt.format != 0x0000000a);
		// EXIT_NOT_IMPLEMENTED(rt.channel_type != 0x00000006);
		// EXIT_NOT_IMPLEMENTED(rt.channel_order != 0x00000001);
		if (rt.attrib.force_dest_alpha_to_one) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: accepting PS5 force destination alpha-to-one\n");
				logged = true;
			}
		}
		// EXIT_NOT_IMPLEMENTED(rt.tile_mode != 0x0000000a);
		// EXIT_NOT_IMPLEMENTED(rt.fmask_tile_mode != 0x0000000a);
		if (rt.attrib.num_samples != 0x00000000 || rt.attrib.num_fragments != 0x00000000) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: rendering PS5 MSAA color target as single-sample, "
				     "samples=0x%08" PRIx32 " fragments=0x%08" PRIx32 "\n",
				     rt.attrib.num_samples, rt.attrib.num_fragments);
				logged = true;
			}
		}

		if (rt.attrib2.width == 0x00000000 || rt.attrib2.height == 0x00000000) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: accepting PS5 raw 1-pixel color target extent "
				     "fields width_minus1=0x%08" PRIx32 " height_minus1=0x%08" PRIx32 "\n",
				     rt.attrib2.width, rt.attrib2.height);
				logged = true;
			}
		}
		if (rt.attrib2.num_mip_levels != 0x00000000) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: using PS5 color target mip count field 0x%08" PRIx32 "\n",
				     rt.attrib2.num_mip_levels);
				logged = true;
			}
		}
		if (rt.attrib3.depth != 0x00000000) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: ignoring PS5 color target depth_minus1=0x%08" PRIx32
				     "\n",
				     rt.attrib3.depth);
				logged = true;
			}
		}
		if (!RenderIsColorTileMode(rt.attrib3.tile_mode)) {
			EXIT("unknown PS5 render-target tile mode: 0x%08" PRIx32 "\n", rt.attrib3.tile_mode);
		}
		if (!RenderIsColorDimension(rt.attrib3.dimension)) {
			EXIT("unknown PS5 render-target dimension: 0x%08" PRIx32 "\n", rt.attrib3.dimension);
		}
		if (rt.attrib3.dimension != 0x00000001) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: using 2D fallback for PS5 color "
				     "dimension=0x%08" PRIx32 "\n",
				     rt.attrib3.dimension);
				logged = true;
			}
		}
		if (!rt.attrib3.cmask_pipe_aligned) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: accepting unaligned PS5 CMASK pipe flag\n");
				logged = true;
			}
		}
		if (!rt.attrib3.dcc_pipe_aligned) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: accepting unaligned PS5 DCC pipe flag\n");
				logged = true;
			}
		}

		// EXIT_NOT_IMPLEMENTED(rt.dcc_max_uncompressed_block_size != 0x00000002);
		// EXIT_NOT_IMPLEMENTED(rt.dcc.max_compressed_block_size != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.dcc.min_compressed_block_size != 0x00000000);
		// EXIT_NOT_IMPLEMENTED(rt.dcc.color_transform != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.dcc.overwrite_combiner_disable != false);
		// EXIT_NOT_IMPLEMENTED(rt.dcc.force_independent_blocks != false);
		// EXIT_NOT_IMPLEMENTED(rt.dcc.independent_128b_blocks != false);
		// EXIT_NOT_IMPLEMENTED(rt.dcc.data_write_on_dcc_clear_to_reg != false);
		EXIT_NOT_IMPLEMENTED(rt.dcc.dcc_clear_key_enable != false);
		if (rt.cmask.addr != 0x0000000000000000 || rt.cmask_slice.slice_minus1 != 0x00000000 ||
		    rt.fmask.addr != 0x0000000000000000 ||
		    (rt.fmask_slice.slice_minus1 != 0x00000000 &&
		     rt.fmask_slice.slice_minus1 != rt.slice.slice_div64_minus1) ||
		    rt.dcc_addr.addr != 0x0000000000000000) {
			static bool logged = false;
			if (!logged) {
				LOGF("RenderTarget: temporary: ignoring PS5 metadata addresses cmask=0x%016" PRIx64
				     " fmask=0x%016" PRIx64 " dcc=0x%016" PRIx64 "\n",
				     rt.cmask.addr, rt.fmask.addr, rt.dcc_addr.addr);
				logged = true;
			}
		}

		EXIT_NOT_IMPLEMENTED(rt.size.width != 0);
		EXIT_NOT_IMPLEMENTED(rt.size.height != 0);
	}
}

static void ZPrint(const char* func, const HW::DepthRenderTarget& z) {
	LOGF("%s\n", func);

	LOGF("\t z_info.format                         = 0x%08" PRIx32 "\n"
	     "\t z_info.tile_mode_index                = 0x%08" PRIx32 "\n"
	     "\t z_info.num_samples                    = 0x%08" PRIx32 "\n"
	     "\t z_info.tile_surface_enable            = %s\n"
	     "\t z_info.expclear_enabled               = %s\n"
	     "\t z_info.zrange_precision               = 0x%08" PRIx32 "\n"
	     "\t z_info.embedded_sample_locations      = %s\n"
	     "\t z_info.partially_resident             = %s\n"
	     "\t z_info.num_mip_levels                 = 0x%02" PRIx8 "\n"
	     "\t z_info.plane_compression              = 0x%02" PRIx8 "\n"
	     "\t stencil_info.format                   = 0x%08" PRIx32 "\n"
	     "\t stencil_info.tile_stencil_disable     = %s\n"
	     "\t stencil_info.expclear_enabled         = %s\n"
	     "\t stencil_info.tile_mode_index          = 0x%08" PRIx32 "\n"
	     "\t stencil_info.tile_split               = 0x%08" PRIx32 "\n"
	     "\t stencil_info.texture_compatible_stencil = %s\n"
	     "\t stencil_info.partially_resident       = %s\n"
	     "\t depth_info.addr5_swizzle_mask         = 0x%08" PRIx32 "\n"
	     "\t depth_info.array_mode                 = 0x%08" PRIx32 "\n"
	     "\t depth_info.pipe_config                = 0x%08" PRIx32 "\n"
	     "\t depth_info.bank_width                 = 0x%08" PRIx32 "\n"
	     "\t depth_info.bank_height                = 0x%08" PRIx32 "\n"
	     "\t depth_info.macro_tile_aspect          = 0x%08" PRIx32 "\n"
	     "\t depth_info.num_banks                  = 0x%08" PRIx32 "\n"
	     "\t depth_view.slice_start                = 0x%08" PRIx32 "\n"
	     "\t depth_view.slice_max                  = 0x%08" PRIx32 "\n"
	     "\t depth_view.current_mip_level          = 0x%02" PRIx8 "\n"
	     "\t depth_view.depth_write_disable        = %s\n"
	     "\t depth_view.stencil_write_disable      = %s\n"
	     "\t htile_surface.linear                  = 0x%08" PRIx32 "\n"
	     "\t htile_surface.full_cache              = 0x%08" PRIx32 "\n"
	     "\t htile_surface.htile_uses_preload_win  = 0x%08" PRIx32 "\n"
	     "\t htile_surface.preload                 = 0x%08" PRIx32 "\n"
	     "\t htile_surface.prefetch_width          = 0x%08" PRIx32 "\n"
	     "\t htile_surface.prefetch_height         = 0x%08" PRIx32 "\n"
	     "\t htile_surface.dst_outside_zero_to_one = 0x%08" PRIx32 "\n"
	     "\t z_read_base_addr                      = 0x%016" PRIx64 "\n"
	     "\t stencil_read_base_addr                = 0x%016" PRIx64 "\n"
	     "\t z_write_base_addr                     = 0x%016" PRIx64 "\n"
	     "\t stencil_write_base_addr               = 0x%016" PRIx64 "\n"
	     "\t pitch_div8_minus1                     = 0x%08" PRIx32 "\n"
	     "\t height_div8_minus1                    = 0x%08" PRIx32 "\n"
	     "\t slice_div64_minus1                    = 0x%08" PRIx32 "\n"
	     "\t htile_data_base_addr                  = 0x%016" PRIx64 "\n"
	     "\t width                                 = 0x%08" PRIx32 "\n"
	     "\t height                                = 0x%08" PRIx32 "\n"
	     "\t size.x_max                            = 0x%04" PRIx16 "\n"
	     "\t size.y_max                            = 0x%04" PRIx16 "\n",
	     z.z_info.format, z.z_info.tile_mode_index, z.z_info.num_samples,
	     z.z_info.tile_surface_enable ? "true" : "false",
	     z.z_info.expclear_enabled ? "true" : "false", z.z_info.zrange_precision,
	     z.z_info.embedded_sample_locations ? "true" : "false",
	     z.z_info.partially_resident ? "true" : "false", z.z_info.num_mip_levels,
	     z.z_info.plane_compression, z.stencil_info.format,
	     z.stencil_info.tile_stencil_disable ? "true" : "false",
	     z.stencil_info.expclear_enabled ? "true" : "false", z.stencil_info.tile_mode_index,
	     z.stencil_info.tile_split, z.stencil_info.texture_compatible_stencil ? "true" : "false",
	     z.stencil_info.partially_resident ? "true" : "false", z.depth_info.addr5_swizzle_mask,
	     z.depth_info.array_mode, z.depth_info.pipe_config, z.depth_info.bank_width,
	     z.depth_info.bank_height, z.depth_info.macro_tile_aspect, z.depth_info.num_banks,
	     z.depth_view.slice_start, z.depth_view.slice_max, z.depth_view.current_mip_level,
	     z.depth_view.depth_write_disable ? "true" : "false",
	     z.depth_view.stencil_write_disable ? "true" : "false", z.htile_surface.linear,
	     z.htile_surface.full_cache, z.htile_surface.htile_uses_preload_win,
	     z.htile_surface.preload, z.htile_surface.prefetch_width, z.htile_surface.prefetch_height,
	     z.htile_surface.dst_outside_zero_to_one, z.z_read_base_addr, z.stencil_read_base_addr,
	     z.z_write_base_addr, z.stencil_write_base_addr, z.pitch_div8_minus1, z.height_div8_minus1,
	     z.slice_div64_minus1, z.htile_data_base_addr, z.width, z.height, z.size.x_max,
	     z.size.y_max);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void ZCheck(const HW::DepthRenderTarget& z) {
	if (z.z_info.format == 0) {
		EXIT_NOT_IMPLEMENTED(z.z_info.format != 0);
		EXIT_NOT_IMPLEMENTED(z.z_info.tile_mode_index != 0);
		EXIT_NOT_IMPLEMENTED(z.z_info.num_samples != 0);
		EXIT_NOT_IMPLEMENTED(z.z_info.tile_surface_enable != false);
		EXIT_NOT_IMPLEMENTED(z.z_info.expclear_enabled != false);
		if (z.z_info.zrange_precision != 0) {
			LOGF("Warning: zrange_precision != 0\n");
			// z.z_info.zrange_precision = 0;
		}
		if (z.z_info.embedded_sample_locations) {
			static bool logged = false;
			if (!logged) {
				LOGF("DepthTarget: temporary: ignoring embedded sample locations\n");
				logged = true;
			}
		}
		EXIT_NOT_IMPLEMENTED(z.z_info.partially_resident != false);
		EXIT_NOT_IMPLEMENTED(z.z_info.num_mip_levels != 0);
		if (z.z_info.plane_compression != 0) {
			static bool logged = false;
			if (!logged) {
				LOGF("DepthTarget: temporary: ignoring PS5 plane_compression=0x%02" PRIx8 "\n",
				     z.z_info.plane_compression);
				logged = true;
			}
		}
	} else {
		EXIT_NOT_IMPLEMENTED(z.z_info.format != 0x00000001 && z.z_info.format != 0x00000003);
		// EXIT_NOT_IMPLEMENTED(z.z_info.tile_mode_index != 0x00000002);
		if (z.z_info.num_samples != 0x00000000) {
			static bool logged = false;
			if (!logged) {
				LOGF("DepthTarget: temporary: ignoring num_samples=0x%08" PRIx32 "\n",
				     z.z_info.num_samples);
				logged = true;
			}
		}
		// EXIT_NOT_IMPLEMENTED(z.z_info.tile_surface_enable != true);
		EXIT_NOT_IMPLEMENTED(z.z_info.expclear_enabled != false);
		if (z.z_info.zrange_precision != 0x00000001) {
			static bool logged = false;
			if (!logged) {
				LOGF("DepthTarget: temporary: ignoring zrange_precision=0x%08" PRIx32 "\n",
				     z.z_info.zrange_precision);
				logged = true;
			}
		}
		if (z.z_info.embedded_sample_locations) {
			static bool logged = false;
			if (!logged) {
				LOGF("DepthTarget: temporary: ignoring embedded sample locations\n");
				logged = true;
			}
		}
		EXIT_NOT_IMPLEMENTED(z.z_info.partially_resident != false);
		EXIT_NOT_IMPLEMENTED(z.z_info.num_mip_levels != 0);
		if (z.z_info.plane_compression != 0) {
			static bool logged = false;
			if (!logged) {
				LOGF("DepthTarget: temporary: ignoring PS5 plane_compression=0x%02" PRIx8 "\n",
				     z.z_info.plane_compression);
				logged = true;
			}
		}
	}

	if (z.stencil_info.format == 0) {
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.format != 0);
		//  EXIT_NOT_IMPLEMENTED(z.stencil_info.tile_stencil_disable != false);
		EXIT_NOT_IMPLEMENTED(z.stencil_info.expclear_enabled != false);
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.tile_mode_index != 0);
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.tile_split != 0);
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.texture_compatible_stencil != true);
		EXIT_NOT_IMPLEMENTED(z.stencil_info.partially_resident != false);
	} else {
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.format != 0x00000001);
		if (z.stencil_info.tile_stencil_disable != true) {

			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1) < 16) {
				LOGF("DepthTarget: temporary: ignoring PS5 HTILE stencil acceleration\n");
			}
		}
		EXIT_NOT_IMPLEMENTED(z.stencil_info.expclear_enabled != false);
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.tile_mode_index != 0x00000002);
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.tile_split != 0x00000002);
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.texture_compatible_stencil != true);
		EXIT_NOT_IMPLEMENTED(z.stencil_info.partially_resident != false);
	}

	if (z.z_info.format != 0 || z.stencil_info.format != 0) {

		EXIT_NOT_IMPLEMENTED(z.depth_info.addr5_swizzle_mask != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.depth_info.array_mode != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.depth_info.pipe_config != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.depth_info.bank_width != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.depth_info.bank_height != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.depth_info.macro_tile_aspect != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.depth_info.num_banks != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.linear != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.full_cache != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.htile_uses_preload_win != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.preload != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.prefetch_width != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.prefetch_height != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.dst_outside_zero_to_one != 0x00000000);

		if (z.depth_view.slice_start != 0x00000000 || z.depth_view.slice_max != 0x00000000) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
				LOGF("DepthTarget: temporary: ignoring PS5 array slice view start=0x%08" PRIx32
				     ", max=0x%08" PRIx32 "\n",
				     z.depth_view.slice_start, z.depth_view.slice_max);
			}
		}
		if (z.depth_view.current_mip_level != 0x00000000) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
				LOGF("DepthTarget: temporary: ignoring PS5 current mip level=0x%08" PRIx32 "\n",
				     z.depth_view.current_mip_level);
			}
		}
		if (z.depth_view.depth_write_disable || z.depth_view.stencil_write_disable) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
				LOGF("DepthTarget: honoring write disable depth=%s, stencil=%s\n",
				     z.depth_view.depth_write_disable ? "true" : "false",
				     z.depth_view.stencil_write_disable ? "true" : "false");
			}
		}
		EXIT_NOT_IMPLEMENTED(z.z_read_base_addr != z.z_write_base_addr);
		EXIT_NOT_IMPLEMENTED(z.stencil_read_base_addr != z.stencil_write_base_addr);
		EXIT_NOT_IMPLEMENTED(z.z_write_base_addr == 0);
		if (z.stencil_info.format != 0 && z.stencil_write_base_addr == 0) {
			LOGF("\t warning: stencil format is set without a stencil base address, continuing "
			     "without stencil attachment\n");
		}
		// EXIT_NOT_IMPLEMENTED(z.pitch_div8_minus1 != 0x000000ff);
		// EXIT_NOT_IMPLEMENTED(z.height_div8_minus1 != 0x0000008f);
		// EXIT_NOT_IMPLEMENTED(z.slice_div64_minus1 != 0x00008fff);
		// EXIT_NOT_IMPLEMENTED(z.htile_data_base_addr == 0);
		// EXIT_NOT_IMPLEMENTED(z.width != 0x00000780);
		// EXIT_NOT_IMPLEMENTED(z.height != 0x00000438);

		EXIT_NOT_IMPLEMENTED(z.width != 0);
		EXIT_NOT_IMPLEMENTED(z.height != 0);
		if (z.size.x_max == 0 || z.size.y_max == 0) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1) < 16) {
				LOGF("DepthTarget: temporary: inferring PS5 default depth extent x_max=0x%04" PRIx16
				     ", y_max=0x%04" PRIx16 "\n",
				     z.size.x_max, z.size.y_max);
			}
		}
	}
}

static void ClipPrint(const char* func, const HW::ClipControl& c) {
	LOGF("%s\n", func);

	LOGF("\t user_clip_planes                    = 0x%02" PRIx8 "\n"
	     "\t user_clip_plane_mode                = 0x%02" PRIx8 "\n"
	     "\t dx_clip_space                       = %s\n"
	     "\t vertex_kill_any                     = %s\n"
	     "\t min_z_clip_disable                  = %s\n"
	     "\t max_z_clip_disable                  = %s\n"
	     "\t user_clip_plane_negate_y            = %s\n"
	     "\t clip_disable                        = %s\n"
	     "\t user_clip_plane_cull_only           = %s\n"
	     "\t cull_on_clipping_error_disable      = %s\n"
	     "\t linear_attribute_clip_enable        = %s\n"
	     "\t force_viewport_index_from_vs_enable = %s\n",
	     c.user_clip_planes, c.user_clip_plane_mode, c.dx_clip_space ? "true" : "false",
	     c.vertex_kill_any ? "true" : "false", c.min_z_clip_disable ? "true" : "false",
	     c.max_z_clip_disable ? "true" : "false", c.user_clip_plane_negate_y ? "true" : "false",
	     c.clip_disable ? "true" : "false", c.user_clip_plane_cull_only ? "true" : "false",
	     c.cull_on_clipping_error_disable ? "true" : "false",
	     c.linear_attribute_clip_enable ? "true" : "false",
	     c.force_viewport_index_from_vs_enable ? "true" : "false");
}

static void ClipCheck(const HW::ClipControl& c) {
	// dx_linear_attr_clip_enable preserves linear (noperspective) attributes at clip-generated
	// vertices, which Vulkan provides as part of clipping and interpolation.
	EXIT_NOT_IMPLEMENTED(c.user_clip_planes != 0 || c.user_clip_plane_mode != 0 ||
	                     c.vertex_kill_any || c.min_z_clip_disable || c.max_z_clip_disable ||
	                     c.user_clip_plane_negate_y || c.clip_disable ||
	                     c.user_clip_plane_cull_only || c.cull_on_clipping_error_disable ||
	                     c.force_viewport_index_from_vs_enable);
}

static void RcPrint(const char* func, const HW::RenderControl& c) {
	LOGF("%s\n", func);

	LOGF("\t depth_clear_enable       = %s\n"
	     "\t stencil_clear_enable     = %s\n"
	     "\t resummarize_enable       = %s\n"
	     "\t stencil_compress_disable = %s\n"
	     "\t depth_compress_disable   = %s\n"
	     "\t copy_centroid            = %s\n"
	     "\t copy_sample              = %" PRIu8 "\n",
	     c.depth_clear_enable ? "true" : "false", c.stencil_clear_enable ? "true" : "false",
	     c.resummarize_enable ? "true" : "false", c.stencil_compress_disable ? "true" : "false",
	     c.depth_compress_disable ? "true" : "false", c.copy_centroid ? "true" : "false",
	     c.copy_sample);
}

static void RcCheck(const HW::RenderControl& c) {
	// EXIT_NOT_IMPLEMENTED(c.depth_clear_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.stencil_clear_enable != false);
	if ((c.resummarize_enable || c.copy_centroid || c.copy_sample != 0)) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 16) {
			LOGF("\t warning: unsupported PS5 render-control metadata state, continuing: "
			     "resummarize=%s copy_centroid=%s copy_sample=%" PRIu8 "\n",
			     c.resummarize_enable ? "true" : "false", c.copy_centroid ? "true" : "false",
			     c.copy_sample);
		}
		return;
	}
	EXIT_NOT_IMPLEMENTED(c.resummarize_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.stencil_compress_disable != false);
	// EXIT_NOT_IMPLEMENTED(c.depth_compress_disable != false);
	EXIT_NOT_IMPLEMENTED(c.copy_centroid != false);
	EXIT_NOT_IMPLEMENTED(c.copy_sample != 0);
}

static void McPrint(const char* func, const HW::ModeControl& c) {
	LOGF("%s\n", func);

	LOGF("\t cull_front               = %s\n"
	     "\t cull_back                = %s\n"
	     "\t face                     = %s\n"
	     "\t poly_mode                = %" PRIu8 "\n"
	     "\t polymode_front_ptype     = %" PRIu8 "\n"
	     "\t polymode_back_ptype      = %" PRIu8 "\n"
	     "\t poly_offset_front_enable = %s\n"
	     "\t poly_offset_back_enable  = %s\n"
	     "\t vtx_window_offset_enable = %s\n"
	     "\t provoking_vtx_last       = %s\n"
	     "\t persp_corr_dis           = %s\n",
	     c.cull_front ? "true" : "false", c.cull_back ? "true" : "false", c.face ? "true" : "false",
	     c.poly_mode, c.polymode_front_ptype, c.polymode_back_ptype,
	     c.poly_offset_front_enable ? "true" : "false",
	     c.poly_offset_back_enable ? "true" : "false",
	     c.vtx_window_offset_enable ? "true" : "false", c.provoking_vtx_last ? "true" : "false",
	     c.persp_corr_dis ? "true" : "false");
}

static void McCheck(const HW::ModeControl& c) {
	// EXIT_NOT_IMPLEMENTED(c.cull_front != false);
	// EXIT_NOT_IMPLEMENTED(c.cull_back != false);
	// EXIT_NOT_IMPLEMENTED(c.face != false);
	if (c.poly_mode != 0) {
		static bool logged = false;
		if (!logged) {
			LOGF("\t temporary: PA_SU_SC_MODE_CNTL.POLY_MODE is not fully implemented; continuing "
			     "with filled polygons\n");
			logged = true;
		}
	}
	EXIT_NOT_IMPLEMENTED(c.polymode_front_ptype != 0 && c.polymode_front_ptype != 2);
	EXIT_NOT_IMPLEMENTED(c.polymode_back_ptype != 0 && c.polymode_back_ptype != 2);
	if (c.poly_offset_front_enable || c.poly_offset_back_enable) {
		static bool logged = false;
		if (!logged) {
			LOGF("\t temporary: PA_SU_SC_MODE_CNTL.POLY_OFFSET_*_ENABLE is not implemented; "
			     "continuing without depth bias\n");
			logged = true;
		}
	}
	if (c.vtx_window_offset_enable) {
		static bool logged = false;
		if (!logged) {
			LOGF("\t temporary: PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE is not fully "
			     "implemented; continuing without vertex window "
			     "offset adjustment\n");
			logged = true;
		}
	}
	EXIT_NOT_IMPLEMENTED(c.provoking_vtx_last != false);
	EXIT_NOT_IMPLEMENTED(c.persp_corr_dis != false);
}

static void BcPrint(const char* func, const HW::BlendControl& c, const HW::BlendColor& color,
                    const HW::ColorControl& cc) {
	LOGF("%s\n", func);

	LOGF("\t color_srcblend       = %" PRIu8 "\n"
	     "\t color_comb_fcn       = %" PRIu8 "\n"
	     "\t color_destblend      = %" PRIu8 "\n"
	     "\t alpha_srcblend       = %" PRIu8 "\n"
	     "\t alpha_comb_fcn       = %" PRIu8 "\n"
	     "\t alpha_destblend      = %" PRIu8 "\n"
	     "\t separate_alpha_blend = %s\n"
	     "\t enable               = %s\n"
	     "\t red                  = %f\n"
	     "\t green                = %f\n"
	     "\t blue                 = %f\n"
	     "\t alpha                = %f\n"
	     "\t cc.mode              = %" PRIu8 "\n"
	     "\t cc.op                = %" PRIu8 "\n",
	     c.color_srcblend, c.color_comb_fcn, c.color_destblend, c.alpha_srcblend, c.alpha_comb_fcn,
	     c.alpha_destblend, c.separate_alpha_blend ? "true" : "false", c.enable ? "true" : "false",
	     color.red, color.green, color.blue, color.alpha, cc.mode, cc.op);
}

static void BcCheck(const HW::BlendControl& /*c*/, const HW::BlendColor& color,
                    const HW::ColorControl& cc) {
	// EXIT_NOT_IMPLEMENTED(c.color_srcblend != 0);
	// EXIT_NOT_IMPLEMENTED(c.color_comb_fcn != 0);
	// EXIT_NOT_IMPLEMENTED(c.color_destblend != 0);
	// EXIT_NOT_IMPLEMENTED(c.alpha_srcblend != 0);
	// EXIT_NOT_IMPLEMENTED(c.alpha_comb_fcn != 0);
	// EXIT_NOT_IMPLEMENTED(c.alpha_destblend != 0);
	// EXIT_NOT_IMPLEMENTED(c.separate_alpha_blend != false);
	// EXIT_NOT_IMPLEMENTED(c.enable != false);
	if (color.red != 0.0f || color.green != 0.0f || color.blue != 0.0f || color.alpha != 0.0f) {
		static bool logged = false;
		if (!logged) {
			LOGF("BlendControl: temporary: accepting nonzero blend constants (%f, %f, %f, %f)\n",
			     color.red, color.green, color.blue, color.alpha);
			logged = true;
		}
	}
	if (cc.mode != 1 && cc.mode != 0 && cc.mode != 2 && cc.mode != 3 && cc.mode != 5 &&
	    cc.mode != 6) {
		static bool logged = false;
		if (!logged) {
			LOGF("BlendControl: temporary: accepting unsupported color-control mode %" PRIu8 "\n",
			     cc.mode);
			logged = true;
		}
	}
	if (cc.op != 0xCC) {
		static bool logged = false;
		if (!logged) {
			LOGF("BlendControl: temporary: accepting unsupported raster op 0x%02" PRIx8 "\n",
			     cc.op);
			logged = true;
		}
	}
}

static void DPrint(const char* func, const HW::DepthControl& c, const HW::StencilControl& s,
                   const HW::StencilMask& sm) {
	LOGF("%s\n", func);

	LOGF("\t stencil_enable       = %s\n"
	     "\t z_enable             = %s\n"
	     "\t z_write_enable       = %s\n"
	     "\t depth_bounds_enable  = %s\n"
	     "\t zfunc                = %" PRIu8 "\n"
	     "\t backface_enable      = %s\n"
	     "\t stencilfunc          = %" PRIu8 "\n"
	     "\t stencilfunc_bf       = %" PRIu8 "\n"
	     "\t color_writes_on_depth_fail_enable  = %s\n"
	     "\t color_writes_on_depth_pass_disable = %s\n"
	     "\t stencil_fail         = %" PRIu8 "\n"
	     "\t stencil_zpass        = %" PRIu8 "\n"
	     "\t stencil_zfail        = %" PRIu8 "\n"
	     "\t stencil_fail_bf      = %" PRIu8 "\n"
	     "\t stencil_zpass_bf     = %" PRIu8 "\n"
	     "\t stencil_zfail_bf     = %" PRIu8 "\n"
	     "\t stencil_testval      = %" PRIu8 "\n"
	     "\t stencil_mask         = %" PRIu8 "\n"
	     "\t stencil_writemask    = %" PRIu8 "\n"
	     "\t stencil_opval        = %" PRIu8 "\n"
	     "\t stencil_testval_bf   = %" PRIu8 "\n"
	     "\t stencil_mask_bf      = %" PRIu8 "\n"
	     "\t stencil_writemask_bf = %" PRIu8 "\n"
	     "\t stencil_opval_bf     = %" PRIu8 "\n",
	     c.stencil_enable ? "true" : "false", c.z_enable ? "true" : "false",
	     c.z_write_enable ? "true" : "false", c.depth_bounds_enable ? "true" : "false", c.zfunc,
	     c.backface_enable ? "true" : "false", c.stencilfunc, c.stencilfunc_bf,
	     c.color_writes_on_depth_fail_enable ? "true" : "false",
	     c.color_writes_on_depth_pass_disable ? "true" : "false", s.stencil_fail, s.stencil_zpass,
	     s.stencil_zfail, s.stencil_fail_bf, s.stencil_zpass_bf, s.stencil_zfail_bf,
	     sm.stencil_testval, sm.stencil_mask, sm.stencil_writemask, sm.stencil_opval,
	     sm.stencil_testval_bf, sm.stencil_mask_bf, sm.stencil_writemask_bf, sm.stencil_opval_bf);
}

static void DCheck(const HW::DepthControl& c, const HW::StencilControl& s,
                   const HW::StencilMask& /*sm*/) {
	// EXIT_NOT_IMPLEMENTED(c.stencil_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.z_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.z_write_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.zfunc != 0);
	// Back-face stencil state is handled separately when enabled.
	// EXIT_NOT_IMPLEMENTED(c.stencilfunc != 0);
	// EXIT_NOT_IMPLEMENTED(c.stencilfunc_bf != 0);
	EXIT_NOT_IMPLEMENTED(c.color_writes_on_depth_fail_enable != false);
	EXIT_NOT_IMPLEMENTED(c.color_writes_on_depth_pass_disable != false);
	// EXIT_NOT_IMPLEMENTED(s.stencil_fail != 0);
	// EXIT_NOT_IMPLEMENTED(s.stencil_zpass != 0);
	// EXIT_NOT_IMPLEMENTED(s.stencil_zfail != 0);
	// Back-face stencil ops may legitimately differ from the front-face ops.
	// EXIT_NOT_IMPLEMENTED(sm.stencil_testval != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_mask != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_writemask != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_opval != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_testval_bf != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_mask_bf != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_writemask_bf != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_opval_bf != 0);
}

static void EqaaPrint(const char* func, const HW::EqaaControl& c) {
	LOGF("%s\n", func);

	LOGF("\t max_anchor_samples         = %" PRIu8 "\n"
	     "\t ps_iter_samples            = %" PRIu8 "\n"
	     "\t mask_export_num_samples    = %" PRIu8 "\n"
	     "\t alpha_to_mask_num_samples  = %" PRIu8 "\n"
	     "\t high_quality_intersections = %s\n"
	     "\t incoherent_eqaa_reads      = %s\n"
	     "\t interpolate_comp_z         = %s\n"
	     "\t static_anchor_associations = %s\n",
	     c.max_anchor_samples, c.ps_iter_samples, c.mask_export_num_samples,
	     c.alpha_to_mask_num_samples, c.high_quality_intersections ? "true" : "false",
	     c.incoherent_eqaa_reads ? "true" : "false", c.interpolate_comp_z ? "true" : "false",
	     c.static_anchor_associations ? "true" : "false");
}

static void EqaaCheck(const HW::EqaaControl& c) {
	if (c.max_anchor_samples != 0 || c.ps_iter_samples != 0 || c.mask_export_num_samples != 0 ||
	    c.alpha_to_mask_num_samples != 0 || c.high_quality_intersections ||
	    c.incoherent_eqaa_reads || c.interpolate_comp_z || c.static_anchor_associations) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 16) {
			LOGF("\t warning: unsupported PS5 EQAA state, rendering with single-sample fallback\n");
		}
	}
}

static void AaPrint(const char* func, const HW::AaSampleControl& c, const HW::AaConfig& cf) {
	LOGF("%s\n", func);

	LOGF("\t centroid_priority = %016" PRIx64 "\n", c.centroid_priority);
	for (int i = 0; i < 16; i++) {
		LOGF("\t locations[%d] = %08" PRIx32 "\n", i, c.locations[i]);
	}
	LOGF("\t msaa_num_samples      = %" PRIu8 "\n"
	     "\t aa_mask_centroid_dtmn = %s\n"
	     "\t max_sample_dist       = %" PRIu8 "\n"
	     "\t msaa_exposed_samples  = %" PRIu8 "\n",
	     cf.msaa_num_samples, cf.aa_mask_centroid_dtmn ? "true" : "false", cf.max_sample_dist,
	     cf.msaa_exposed_samples);
}

static void AaCheck(const HW::AaSampleControl& c, const HW::AaConfig& cf) {
	bool non_default_locations = (c.centroid_priority != 0);
	for (uint32_t l: c.locations) {
		non_default_locations |= (l != 0);
	}

	if (non_default_locations || cf.msaa_num_samples != 0 || cf.aa_mask_centroid_dtmn ||
	    cf.max_sample_dist != 0 || cf.msaa_exposed_samples != 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 16) {
			LOGF("\t warning: unsupported PS5 AA/MSAA state, rendering with single-sample "
			     "fallback: samples=%" PRIu8 ", exposed=%" PRIu8 ", max_dist=%" PRIu8 "\n",
			     cf.msaa_num_samples, cf.msaa_exposed_samples, cf.max_sample_dist);
		}
	}
}

void LogDrawPhase(const char* draw_name, const char* phase) {
	if (graphics_debug_dump_enabled()) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 1024) {
			LOGF("DrawPhase: %s %s\n", draw_name, phase);
		}
	}
}

void LogPipelineTrace(const char* phase, uint32_t vs_hash0, uint32_t vs_crc32, uint32_t ps_hash0,
                      uint32_t ps_crc32) {
	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: %s VS=0x%08" PRIx32 "/0x%08" PRIx32 " PS=0x%08" PRIx32 "/0x%08" PRIx32
		     "\n",
		     phase, vs_hash0, vs_crc32, ps_hash0, ps_crc32);
	}
}

static void VpPrint(const char* func, const HW::ScreenViewport& vp,
                    const HW::ScanModeControl& smc) {
	LOGF("%s\n", func);

	LOGF("\t msaa_enable                    = %s\n"
	     "\t vport_scissor_enable           = %s\n"
	     "\t line_stipple_enable            = %s\n"
	     "\t vp[0].zmin                     = %f\n"
	     "\t vp[0].zmax                     = %f\n"
	     "\t vp[0].xscale                   = %f\n"
	     "\t vp[0].xoffset                  = %f\n"
	     "\t vp[0].yscale                   = %f\n"
	     "\t vp[0].yoffset                  = %f\n"
	     "\t vp[0].zscale                   = %f\n"
	     "\t vp[0].zoffset                  = %f\n"
	     "\t vp[0].viewport_scissor_left    = %d\n"
	     "\t vp[0].viewport_scissor_top     = %d\n"
	     "\t vp[0].viewport_scissor_right   = %d\n"
	     "\t vp[0].viewport_scissor_bottom  = %d\n"
	     "\t transform_control              = 0x%08" PRIx32 "\n"
	     "\t screen_scissor_left            = %d\n"
	     "\t screen_scissor_top             = %d\n"
	     "\t screen_scissor_right           = %d\n"
	     "\t screen_scissor_bottom          = %d\n"
	     "\t window_scissor_left            = %d\n"
	     "\t window_scissor_top             = %d\n"
	     "\t window_scissor_right           = %d\n"
	     "\t window_scissor_bottom          = %d\n"
	     "\t generic_scissor_left           = %d\n"
	     "\t generic_scissor_top            = %d\n"
	     "\t generic_scissor_right          = %d\n"
	     "\t generic_scissor_bottom         = %d\n"
	     "\t window_offset_x                = %d\n"
	     "\t window_offset_y                = %d\n"
	     "\t hw_offset_x                    = %u\n"
	     "\t hw_offset_y                    = %u\n"
	     "\t guard_band_horz_clip           = %f\n"
	     "\t guard_band_vert_clip           = %f\n"
	     "\t guard_band_horz_discard        = %f\n"
	     "\t guard_band_vert_discard        = %f\n"
	     "\t clip_rect_rule                 = 0x%04" PRIx16 "\n"
	     "\t clip_rect_0                    = (%d,%d)-(%d,%d), window_offset = %s\n"
	     "\t window_scissor_window_offset_enable                = %s\n"
	     "\t generic_scissor_window_offset_enable               = %s\n",
	     smc.msaa_enable ? "true" : "false", smc.vport_scissor_enable ? "true" : "false",
	     smc.line_stipple_enable ? "true" : "false", vp.viewports[0].zmin, vp.viewports[0].zmax,
	     vp.viewports[0].xscale, vp.viewports[0].xoffset, vp.viewports[0].yscale,
	     vp.viewports[0].yoffset, vp.viewports[0].zscale, vp.viewports[0].zoffset,
	     vp.viewports[0].viewport_scissor_left, vp.viewports[0].viewport_scissor_top,
	     vp.viewports[0].viewport_scissor_right, vp.viewports[0].viewport_scissor_bottom,
	     vp.transform_control, vp.screen_scissor_left, vp.screen_scissor_top,
	     vp.screen_scissor_right, vp.screen_scissor_bottom, vp.window_scissor_left,
	     vp.window_scissor_top, vp.window_scissor_right, vp.window_scissor_bottom,
	     vp.generic_scissor_left, vp.generic_scissor_top, vp.generic_scissor_right,
	     vp.generic_scissor_bottom, vp.window_offset_x, vp.window_offset_y, vp.hw_offset_x,
	     vp.hw_offset_y, vp.guard_band_horz_clip, vp.guard_band_vert_clip,
	     vp.guard_band_horz_discard, vp.guard_band_vert_discard, vp.clip_rect_rule,
	     vp.clip_rect_left[0], vp.clip_rect_top[0], vp.clip_rect_right[0], vp.clip_rect_bottom[0],
	     vp.clip_rect_window_offset_enable[0] ? "true" : "false",
	     vp.window_scissor_window_offset_enable ? "true" : "false",
	     vp.generic_scissor_window_offset_enable ? "true" : "false");
	LOGF("\t viewports[0].viewport_scissor_window_offset_enable = %s\n",
	     vp.viewports[0].viewport_scissor_window_offset_enable ? "true" : "false");
}

static void VpCheck(const HW::ScreenViewport& vp, const HW::ScanModeControl& smc) {

	if (smc.msaa_enable) {

		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 16) {
			LOGF("\t warning: unsupported PS5 MSAA raster state, rendering with single-sample "
			     "fallback\n");
		}
	}
	// EXIT_NOT_IMPLEMENTED(smc.vport_scissor_enable);
	EXIT_NOT_IMPLEMENTED(smc.line_stipple_enable);

	if (vp.viewports[0].zmin > 0.000000 || vp.viewports[0].zmax != 1.000000) {
		static bool logged = false;
		if (!logged) {
			LOGF("\t warning: non-default viewport depth clamp zmin = %f, zmax = %f; using "
			     "viewport scale/offset for Vulkan depth range\n",
			     vp.viewports[0].zmin, vp.viewports[0].zmax);
			logged = true;
		}
	}
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].xscale != 960.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].xoffset != 960.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].yscale != -540.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].yoffset != 540.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].zscale != 0.500000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].zoffset != 0.500000);
	if (vp.transform_control != 1087) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("\t warning: non-default viewport transform control = 0x%08" PRIx32
			     ", applying enabled scale/offset bits\n",
			     vp.transform_control);
		}
	}
	// EXIT_NOT_IMPLEMENTED(vp.hw_offset_x != 60);
	// EXIT_NOT_IMPLEMENTED(vp.hw_offset_y != 32);
	// EXIT_NOT_IMPLEMENTED(fabsf(vp.guard_band_horz_clip - 33.133327f) > 0.001f);
	// EXIT_NOT_IMPLEMENTED(fabsf(vp.guard_band_vert_clip - 59.629623f) > 0.001f);

	if (vp.guard_band_horz_discard != 0.0f || vp.guard_band_vert_discard != 0.0f) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 16) {
			LOGF("\t warning: unsupported PS5 guard band discard = %f, %f, continuing\n",
			     vp.guard_band_horz_discard, vp.guard_band_vert_discard);
		}
	}

	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].viewport_scissor_left != 0);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].viewport_scissor_top != 0);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].viewport_scissor_right != 0);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].viewport_scissor_bottom != 0);
	// EXIT_NOT_IMPLEMENTED(viewport_scissor &&
	// vp.viewports[0].viewport_scissor_window_offset_enable != true);
}

static bool ScissorRectValid(const ScissorRect& r) {
	return r.right > r.left && r.bottom > r.top;
}

static bool ScissorRectSet(const ScissorRect& r) {
	return r.left != 0 || r.top != 0 || r.right != 0 || r.bottom != 0;
}

static ScissorRect ScissorRectOffset(ScissorRect r, int x, int y) {
	r.left += x;
	r.right += x;
	r.top += y;
	r.bottom += y;
	return r;
}

static ScissorRect ScissorRectIntersect(const ScissorRect& a, const ScissorRect& b) {
	return {a.left > b.left ? a.left : b.left, a.top > b.top ? a.top : b.top,
	        a.right < b.right ? a.right : b.right, a.bottom < b.bottom ? a.bottom : b.bottom};
}

static ScissorRect ScissorRectClamp(ScissorRect r, uint32_t width, uint32_t height) {
	int max_right  = static_cast<int>(width);
	int max_bottom = static_cast<int>(height);

	r.left   = (r.left < 0 ? 0 : (r.left > max_right ? max_right : r.left));
	r.right  = (r.right < 0 ? 0 : (r.right > max_right ? max_right : r.right));
	r.top    = (r.top < 0 ? 0 : (r.top > max_bottom ? max_bottom : r.top));
	r.bottom = (r.bottom < 0 ? 0 : (r.bottom > max_bottom ? max_bottom : r.bottom));

	if (!ScissorRectValid(r)) {
		r.right  = r.left;
		r.bottom = r.top;
	}

	return r;
}

static bool ScissorClipRuleToIntersectionMask(uint16_t rule, uint8_t* mask) {
	EXIT_IF(mask == nullptr);

	for (uint32_t candidate = 0; candidate < 16; candidate++) {
		uint16_t expected = 0;
		for (uint32_t combination = 0; combination < 16; combination++) {
			if ((combination & candidate) == candidate) {
				expected |= static_cast<uint16_t>(1u << combination);
			}
		}

		if (expected == rule) {
			*mask = static_cast<uint8_t>(candidate);
			return true;
		}
	}

	return false;
}

ScissorRect calc_final_scissor(const HW::ScreenViewport& vp, const HW::ScanModeControl& smc,
                               vk::Extent2D extent) {
	ScissorRect screen {vp.screen_scissor_left, vp.screen_scissor_top, vp.screen_scissor_right,
	                    vp.screen_scissor_bottom};
	ScissorRect final = screen;

	if (!ScissorRectSet(screen)) {
		final = {0, 0, static_cast<int>(extent.width), static_cast<int>(extent.height)};

		auto log_id = g_scissor_default_log_count.fetch_add(1);
		if (log_id < 32) {
			LOGF("temporary: default unset screen scissor to framebuffer extent %ux%u\n",
			     extent.width, extent.height);
		}
	}

	ScissorRect window {vp.window_scissor_left, vp.window_scissor_top, vp.window_scissor_right,
	                    vp.window_scissor_bottom};
	if (ScissorRectSet(window)) {
		if (vp.window_scissor_window_offset_enable) {
			window = ScissorRectOffset(window, vp.window_offset_x, vp.window_offset_y);
		}
		final = ScissorRectIntersect(final, window);
	}

	ScissorRect generic {vp.generic_scissor_left, vp.generic_scissor_top, vp.generic_scissor_right,
	                     vp.generic_scissor_bottom};
	if (ScissorRectSet(generic)) {
		if (vp.generic_scissor_window_offset_enable) {
			generic = ScissorRectOffset(generic, vp.window_offset_x, vp.window_offset_y);
		}
		final = ScissorRectIntersect(final, generic);
	}

	const auto& viewport = vp.viewports[0];
	ScissorRect viewport_scissor {viewport.viewport_scissor_left, viewport.viewport_scissor_top,
	                              viewport.viewport_scissor_right,
	                              viewport.viewport_scissor_bottom};
	if (smc.vport_scissor_enable && ScissorRectSet(viewport_scissor)) {
		if (viewport.viewport_scissor_window_offset_enable) {
			viewport_scissor =
			    ScissorRectOffset(viewport_scissor, vp.window_offset_x, vp.window_offset_y);
		}
		final = ScissorRectIntersect(final, viewport_scissor);
	}

	if (vp.clip_rect_rule == 0) {
		final = {0, 0, 0, 0};
	} else if (vp.clip_rect_rule != 0xffffu) {
		uint8_t clip_rect_mask = 0;
		if (ScissorClipRuleToIntersectionMask(vp.clip_rect_rule, &clip_rect_mask)) {
			for (uint32_t i = 0; i < 4; i++) {
				if ((clip_rect_mask & (1u << i)) == 0) {
					continue;
				}

				ScissorRect clip {vp.clip_rect_left[i], vp.clip_rect_top[i], vp.clip_rect_right[i],
				                  vp.clip_rect_bottom[i]};
				if (vp.clip_rect_window_offset_enable[i]) {
					clip = ScissorRectOffset(clip, vp.window_offset_x, vp.window_offset_y);
				}
				final = ScissorRectIntersect(final, clip);
			}
		} else {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
				LOGF("unsupported clip-rect rule 0x%04" PRIx16 ", leaving scissor unchanged\n",
				     vp.clip_rect_rule);
			}
		}
	}

	return ScissorRectClamp(final, extent.width, extent.height);
}

void hw_check(const HW::Context& hw) {
	const auto  rt_slot = render_target_first_bound_slot(hw);
	const auto& rt      = hw.GetRenderTarget(rt_slot);
	const auto& bc      = hw.GetBlendControl(rt_slot);
	const auto& bclr    = hw.GetBlendColor();
	const auto& vp      = hw.GetScreenViewport();
	const auto& z       = hw.GetDepthRenderTarget();
	const auto& c       = hw.GetClipControl();
	const auto& rc      = hw.GetRenderControl();
	const auto& d       = hw.GetDepthControl();
	const auto& s       = hw.GetStencilControl();
	const auto& sm      = hw.GetStencilMask();
	const auto& mc      = hw.GetModeControl();
	const auto& eqaa    = hw.GetEqaaControl();
	const auto& cc      = hw.GetColorControl();
	const auto& smc     = hw.GetScanModeControl();
	const auto& aa      = hw.GetAaSampleControl();
	const auto& ac      = hw.GetAaConfig();

	auto log_phase = [](const char* phase) {
		if (graphics_debug_dump_enabled()) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 512) {
				LOGF("HwCheckPhase: %s\n", phase);
			}
		}
	};

	log_phase("rt");
	RtCheck(rt);
	log_phase("vp");
	VpCheck(vp, smc);
	log_phase("z");
	ZCheck(z);
	log_phase("clip");
	ClipCheck(c);
	log_phase("rc");
	RcCheck(rc);
	log_phase("depth");
	DCheck(d, s, sm);
	log_phase("mode");
	McCheck(mc);
	log_phase("blend");
	BcCheck(bc, bclr, cc);
	log_phase("eqaa");
	EqaaCheck(eqaa);
	log_phase("aa");
	AaCheck(aa, ac);
	log_phase("done");

	if (RenderTargetMaskHasBoundMrt(hw)) {
		LOGF("MRT render target mask: 0x%08" PRIx32 "\n", hw.GetRenderTargetMask());
		for (uint32_t i = 0; i < 8; i++) {
			const auto& mrt = hw.GetRenderTarget(i);
			LOGF("\t RT%u addr=0x%010" PRIx64 " mask=0x%x fmt=0x%08" PRIx32
			     " width=%u height=%u tile=%u\n",
			     i, mrt.base.addr, (hw.GetRenderTargetMask() >> (i * 4u)) & 0x0fu, mrt.info.format,
			     mrt.attrib2.width + 1, mrt.attrib2.height + 1, mrt.attrib3.tile_mode);
		}
	}
	if (rc.depth_clear_enable && hw.GetDepthClearValue() != 0.0f &&
	    hw.GetDepthClearValue() != 1.0f) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 16) {
			LOGF("\t temporary: accepting non-default depth clear value %f\n",
			     hw.GetDepthClearValue());
		}
	}
	// EXIT_NOT_IMPLEMENTED(hw.GetStencilClearValue() != 0);
}

void hw_print(const HW::Context& hw) {
	const auto  rt_slot = render_target_first_bound_slot(hw);
	const auto& rt      = hw.GetRenderTarget(rt_slot);
	const auto& bc      = hw.GetBlendControl(rt_slot);
	const auto& bclr    = hw.GetBlendColor();
	const auto& vp      = hw.GetScreenViewport();
	const auto& z       = hw.GetDepthRenderTarget();
	const auto& c       = hw.GetClipControl();
	const auto& rc      = hw.GetRenderControl();
	const auto& d       = hw.GetDepthControl();
	const auto& s       = hw.GetStencilControl();
	const auto& sm      = hw.GetStencilMask();
	const auto& mc      = hw.GetModeControl();
	const auto& eqaa    = hw.GetEqaaControl();
	const auto& cc      = hw.GetColorControl();
	const auto& smc     = hw.GetScanModeControl();
	const auto& aa      = hw.GetAaSampleControl();
	const auto& ac      = hw.GetAaConfig();

	if (graphics_debug_dump_enabled()) {
		LOGF("Context\n"
		     "\t GetRenderTargetMask()   = 0x%08" PRIx32 "\n"
		     "\t GetDepthClearValue()    = %f\n"
		     "\t GetStencilClearValue()  = %" PRIu8 "\n"
		     "\t GetLineWidth()          = %f\n",
		     hw.GetRenderTargetMask(), hw.GetDepthClearValue(), hw.GetStencilClearValue(),
		     hw.GetLineWidth());

		LOGF("%s", Common::Concat(rt_print("RenderTraget:", rt), "").c_str());

		ZPrint("DepthRenderTraget:", z);
		VpPrint("ScreenViewport:", vp, smc);
		ClipPrint("ClipControl:", c);
		RcPrint("RenderControl:", rc);
		DPrint("DepthStencilControlMask:", d, s, sm);
		McPrint("ModeControl:", mc);
		BcPrint("BlendColorControl:", bc, bclr, cc);
		EqaaPrint("EqaaControl:", eqaa);
		AaPrint("AaSampleControl:", aa, ac);
	}
}

} // namespace Libs::Graphics
