#include "graphics/host_gpu/renderer/renderDraw.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics {

int32_t ResolveVertexOffset(uint32_t index_offset, const ShaderVertexInputInfo& vs_input_info) {
	if (index_offset != 0 || !vs_input_info.fetch_embedded) {
		return static_cast<int32_t>(index_offset);
	}

	EXIT_IF(!vs_input_info.stage);
	const auto& program   = *vs_input_info.stage.program;
	const auto& resources = vs_input_info.stage.resources;
	if (program.info.vertex_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.vertex_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			return static_cast<int32_t>(resources.user_data[index]);
		}
	}

	return 0;
}

uint32_t ResolveInstanceOffset(const ShaderVertexInputInfo& vs_input_info) {
	if (!vs_input_info.fetch_embedded) {
		return 0;
	}

	EXIT_IF(!vs_input_info.stage);
	const auto& program   = *vs_input_info.stage.program;
	const auto& resources = vs_input_info.stage.resources;
	if (program.info.instance_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.instance_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			return resources.user_data[index];
		}
	}

	return 0;
}

// See Log::RateLimit (common/logging/log.h) for why these are named limiters rather than the
// bare `static std::atomic<uint32_t> ..._log_count` idiom this file used to use.
static Log::RateLimit g_draw_state_log_limit {"DrawTargetState", 192};
static Log::RateLimit g_draw_input_log_limit {"DrawInputState", 64};
static Log::RateLimit g_mrt_state_log_limit {"MrtState", 32};

static Log::RateLimit g_framebuffer_skip_log_limit {"DrawFramebufferSkip", 128};

// Found 2026-09-10 while chasing ASTRO's Playroom's rendering defect: a real, heavy scene can
// issue far more than 192 draws before the one actually worth inspecting even runs, so the flat
// draw-count cap above exhausts on whichever shader happens to draw first (repeated thousands
// of times) and every OTHER distinct shader in the same frame is invisible to
// LogDrawTargetState -- there is no way to tell "which shaders exist and what state do they use"
// from the log, only "the first few hundred draws in submission order". This is a SEPARATE,
// unconditional coverage log keyed by (vs_hash, ps_hash): it fires once per distinct shader pair
// no matter how exhausted the flat cap above is, so a scene with tens of thousands of shaders
// still gets one line per shader that actually drew something. Capped generously (not
// unbounded) so a pathological case (a shader hash that's actually unstable/changing per-draw)
// can't grow this set without limit.
static std::mutex                   g_seen_shader_pairs_mutex;
static std::unordered_set<uint64_t> g_seen_shader_pairs;
static Log::RateLimit g_shader_pair_coverage_log_limit {"DrawTargetStateByShaderPair", 20000};

static bool FirstTimeShaderPairSeen(uint64_t vs_hash, uint64_t ps_hash) {
	// Not a real hash function requirement here (birthday-bound collisions are fine, this
	// only decides "log again or not"), just a cheap combine of two independently-good hashes.
	const uint64_t key = vs_hash ^ (ps_hash * 0x9E3779B97F4A7C15ull + 0x517CC1B727220A95ull);
	std::lock_guard<std::mutex> lock(g_seen_shader_pairs_mutex);
	if (g_seen_shader_pairs.size() >= 20000) {
		return false;
	}
	return g_seen_shader_pairs.insert(key).second;
}

// True when a per-draw diagnostic at the given guest frame number should spend its
// (independently capped, see Log::RateLimit) log budget, per --draw-log-frame-first/-last.
// Both bounds default to -1 (unbounded) so a run with neither flag behaves exactly as before.
static bool DrawLogFrameInWindow(int frame) {
	const auto first = Config::GetDrawLogFrameFirst();
	const auto last  = Config::GetDrawLogFrameLast();
	if (first >= 0 && frame < first) {
		return false;
	}
	if (last >= 0 && frame > last) {
		return false;
	}
	return true;
}

static float ConvertPolygonOffsetConstantFactor(float guest_factor, const HW::PolyOffset& offset,
                                                vk::Format host_depth_format) {
	if (offset.db_is_float_fmt) {
		return guest_factor;
	}

	int host_depth_bits = 0;
	switch (host_depth_format) {
		case vk::Format::eD16Unorm:
		case vk::Format::eD16UnormS8Uint: host_depth_bits = 16; break;
		case vk::Format::eD24UnormS8Uint: host_depth_bits = 24; break;
		default:
			// A fixed-point guest bias cannot be represented exactly by a floating-point host
			// attachment without VK_EXT_depth_bias_control.
			return guest_factor;
	}
	return std::ldexp(guest_factor, host_depth_bits + offset.neg_num_db_bits);
}

static const char* RenderColorTypeName(const RenderColorInfo& color) {
	return color.image_id ? "RenderTexture" : "NoColorOutput";
}

static void LogFramebufferSkip(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               uint32_t index_count, uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	const auto hit = g_framebuffer_skip_log_limit.Hit();
	if (!hit) {
		return;
	}
	const auto log_id = static_cast<unsigned long long>(*hit);

	LOGF(
	    "DrawFramebufferSkip[%llu]: %s color=%s color_addr=0x%010" PRIx64 " color_size=0x%016" PRIx64
	    " color_image=%s depth_format=%s depth_image=%s depth_vaddr_num=%d target_mask=0x%08" PRIx32
	    " prim=%u index_count=%u flags=0x%08" PRIx32 "\n",
	    log_id, draw_name, RenderColorTypeName(color), color.desc.info.data.address,
	    color.desc.info.data.size, color.image_id ? "yes" : "no",
	    vk::to_string(depth.desc.view_info.format).c_str(), depth.image_id ? "yes" : "no",
	    static_cast<int>(!depth.desc.info.data.Empty()) +
	        static_cast<int>(depth.desc.info.HasStencil()),
	    ctx.GetRenderTargetMask(), static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags);
}

static void LogMrtState(const char* draw_name, const CommandBuffer& buffer,
                        const ShaderPixelInputInfo& ps_input_info) {
	const auto& ctx            = buffer.GetRegisters();
	const auto& sh_regs        = ctx.GetShaderRegisters();
	const auto  rt_mask        = ctx.GetRenderTargetMask();
	const auto  cb_shader_mask = sh_regs.m_cbShaderMask;
	const auto& bc0            = ctx.GetBlendControl(0);

	const auto hit = g_mrt_state_log_limit.Hit();
	if (!hit) {
		return;
	}
	const auto log_id = static_cast<unsigned long long>(*hit);

	LOGF("MrtState[%llu]: %s rt_mask=0x%08" PRIx32 " cb_shader_mask=0x%08" PRIx32
	     " blend0=%s src=%u dst=%u alpha_src=%u alpha_dst=%u sep_alpha=%s\n",
	     log_id, draw_name, rt_mask, cb_shader_mask, bc0.enable ? "true" : "false",
	     bc0.color_srcblend, bc0.color_destblend, bc0.alpha_srcblend, bc0.alpha_destblend,
	     bc0.separate_alpha_blend ? "true" : "false");

	for (uint32_t i = 0; i < 8; i++) {
		const auto& rt  = ctx.GetRenderTarget(i);
		const auto& bc  = ctx.GetBlendControl(i);
		const auto  ctm = (rt_mask >> (i * 4u)) & 0x0fu;
		const auto  csm = (cb_shader_mask >> (i * 4u)) & 0x0fu;

		if (rt.base.addr == 0 && ps_input_info.target_output_mode[i] == 0 && ctm == 0 && csm == 0 &&
		    !bc.enable) {
			continue;
		}

		LOGF("MrtState[%llu]: slot=%u addr=0x%010" PRIx64
		     " target_mask=0x%x shader_mask=0x%x out_mode=%u"
		     " fmt=0x%08" PRIx32 " nfmt=0x%08" PRIx32 " order=0x%08" PRIx32
		     " width=%u height=%u tile=%u"
		     " blend=%s src=%u dst=%u alpha_src=%u alpha_dst=%u\n",
		     log_id, i, rt.base.addr, ctm, csm, ps_input_info.target_output_mode[i],
		     static_cast<uint32_t>(rt.info.format), static_cast<uint32_t>(rt.info.channel_type),
		     static_cast<uint32_t>(rt.info.channel_order), rt.attrib2.width + 1,
		     rt.attrib2.height + 1, static_cast<uint32_t>(rt.attrib3.tile_mode),
		     bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.alpha_srcblend,
		     bc.alpha_destblend);
	}
}

static void LogDrawTargetState(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               const ShaderPixelInputInfo& ps_input_info, uint32_t index_count,
                               uint32_t flags, uint64_t vs_hash, uint64_t ps_hash) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!color.image_id) {
		return;
	}

	const auto frame = buffer.GetContext().GetGpu().GetFrameNum();
	if (!DrawLogFrameInWindow(frame)) {
		return;
	}

	// Two independent triggers: the flat draw-count budget (chronological sample of the first
	// N draws), OR first time this exact (vs_hash, ps_hash) pair has drawn anything -- see
	// g_seen_shader_pairs' comment for why the flat cap alone leaves a heavy scene's shaders
	// almost entirely uncovered. Whichever fires supplies the log_id.
	const auto hit             = g_draw_state_log_limit.Hit();
	const bool first_for_pair  = FirstTimeShaderPairSeen(vs_hash, ps_hash);
	std::optional<uint64_t> coverage_hit;
	if (!hit && first_for_pair) {
		coverage_hit = g_shader_pair_coverage_log_limit.Hit();
	}
	if (!hit && !coverage_hit) {
		return;
	}
	const auto log_id = static_cast<unsigned long long>(hit ? *hit : *coverage_hit);

	const auto& cc             = ctx.GetColorControl();
	const auto& bc             = ctx.GetBlendControl(color.target_slot);
	const auto& dc             = ctx.GetDepthControl();
	const auto& mc             = ctx.GetModeControl();
	const auto& sh             = ctx.GetShaderRegisters();
	const auto& vp             = ctx.GetScreenViewport();
	const auto& vp0            = vp.viewports[0];
	const auto& ps_resources   = ps_input_info.stage.program->info;
	const auto  sampled_images = std::count_if(
	    ps_resources.images.begin(), ps_resources.images.end(), [](const auto& image) {
		    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
	    });

	const auto extent = color.Extent();
	const auto sc     = calc_final_scissor(vp, ctx.GetScanModeControl(), extent, 0);

	LOGF(
	    "DrawTargetState[%llu]%s: frame=%d %s target=%s addr=0x%010" PRIx64
	    " extent=%ux%u prim=%u vs=0x%016" PRIx64 " ps=0x%016" PRIx64
	    " index_count=%u flags=0x%08" PRIx32 " color_mask=0x%08" PRIx32
	    " cull_front=%s cull_back=%s face=%s poly_mode=%" PRIu8
	    " cc_mode=%u cc_op=0x%02x"
	    " blend=%s src=%u dst=%u comb=%u ps_tex=%d sampled=%d storage=%d ps_kill=%s target_mode0=%u"
	    " depth_test=%s depth_write=%s depth_func=%u depth_clear=%s viewport=(%.1f,%.1f %.1fx%.1f) "
	    "scissor=(%d,%d)-(%d,%d)"
	    " provoking_vtx_last=%s persp_corr_dis=%s ps_input_ena=0x%08" PRIx32 "\n",
	    log_id, (hit ? "" : "[coverage]"), frame, draw_name, RenderColorTypeName(color),
	    color.desc.info.data.address, extent.width, extent.height,
	    static_cast<uint32_t>(ucfg.GetPrimType()), vs_hash, ps_hash, index_count, flags,
	    ctx.GetRenderTargetMask(),
	    mc.cull_front ? "true" : "false", mc.cull_back ? "true" : "false",
	    mc.face ? "true" : "false", mc.poly_mode, cc.mode, cc.op,
	    bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.color_comb_fcn,
	    static_cast<int>(ps_resources.images.size()), static_cast<int>(sampled_images),
	    static_cast<int>(ps_resources.images.size() - sampled_images),
	    ps_input_info.ps_pixel_kill_enable ? "true" : "false", ps_input_info.target_output_mode[0],
	    dc.z_enable ? "true" : "false", dc.z_write_enable ? "true" : "false", dc.zfunc,
	    depth.depth_clear_enable ? "true" : "false", vp0.xoffset - vp0.xscale,
	    vp0.yoffset - vp0.yscale, vp0.xscale * 2.0f, vp0.yscale * 2.0f, sc.left, sc.top, sc.right,
	    sc.bottom, mc.provoking_vtx_last ? "true" : "false", mc.persp_corr_dis ? "true" : "false",
	    sh.ps_input_ena);

	LogMrtState(draw_name, buffer, ps_input_info);
}

static void LogDrawInputState(const CommandBuffer& buffer, const RenderColorInfo& color,
                              const ShaderVertexInputInfo& vs_input_info,
                              uint32_t index_type_and_size, uint32_t index_count,
                              const void* index_addr, uint32_t prim_type, uint64_t vs_hash,
                              uint64_t ps_hash) {
	const auto frame = buffer.GetContext().GetGpu().GetFrameNum();
	if (!DrawLogFrameInWindow(frame)) {
		return;
	}

	const auto hit = g_draw_input_log_limit.Hit();
	if (!hit) {
		return;
	}
	const auto log_id = static_cast<unsigned long long>(*hit);

	LOGF("DrawInputState[%llu]: frame=%d target=%s addr=0x%010" PRIx64
	     " prim=%u vs=0x%016" PRIx64 " ps=0x%016" PRIx64
	     " index_type=%u index_count=%u index_addr=0x%016" PRIx64
	     " vs_resources=%d vs_buffers=%d\n",
	     log_id, frame, RenderColorTypeName(color),
	     color.desc.info.data.address, prim_type, vs_hash, ps_hash, index_type_and_size,
	     index_count, reinterpret_cast<uint64_t>(index_addr), vs_input_info.resources_num,
	     vs_input_info.buffers_num);

	for (int bi = 0; bi < vs_input_info.buffers_num; bi++) {
		const auto& b = vs_input_info.buffers[bi];
		LOGF("DrawInputState[%llu]: vb[%d] addr=0x%010" PRIx64
		     " stride=%u records=%u fetch_index=%u attr_num=%d\n",
		     log_id, bi, b.addr, b.stride, b.num_records, b.fetch_index, b.attr_num);

		const auto* bytes = reinterpret_cast<const uint8_t*>(b.addr);
		if (bytes != nullptr && b.stride != 0) {
			const uint32_t records = std::min<uint32_t>(b.num_records, 4u);
			for (uint32_t rec = 0; rec < records; rec++) {
				const auto* rec_bytes = bytes + static_cast<uint64_t>(rec) * b.stride;
				const auto  dword_num = std::min<uint32_t>(b.stride / 4u, 12u);
				uint32_t    raw[12]   = {};
				float       flt[12]   = {};
				for (uint32_t i = 0; i < dword_num; i++) {
					std::memcpy(&raw[i], rec_bytes + i * 4u, sizeof(raw[i]));
					std::memcpy(&flt[i], rec_bytes + i * 4u, sizeof(flt[i]));
				}
				LOGF("DrawInputState[%llu]: vb[%d].rec[%u] stride=%u dwords=%u raw=%08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " f=(%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f)\n",
				     log_id, bi, rec, b.stride, dword_num, raw[0], raw[1], raw[2], raw[3], raw[4],
				     raw[5], raw[6], raw[7], raw[8], flt[0], flt[1], flt[2], flt[3], flt[4], flt[5],
				     flt[6], flt[7], flt[8]);

				for (int ai = 0; ai < b.attr_num; ai++) {
					const auto  res_index = b.attr_indices[ai];
					const auto& r         = vs_input_info.resources[res_index];
					const auto& rd        = vs_input_info.resources_dst[res_index];
					const auto  offset    = b.attr_offsets[ai];
					if (offset + 4u <= b.stride &&
					    r.Format() == Prospero::BufferFormat::k8_8_8_8UNorm) {
						uint32_t packed = 0;
						std::memcpy(&packed, rec_bytes + offset, sizeof(packed));
						const auto r8 = (packed >> 0u) & 0xffu;
						const auto g8 = (packed >> 8u) & 0xffu;
						const auto b8 = (packed >> 16u) & 0xffu;
						const auto a8 = (packed >> 24u) & 0xffu;
						LOGF("DrawInputState[%llu]: vb[%d].rec[%u].attr[%d] dst=v%d fmt=56 "
						     "rgba8=%02" PRIx32 "%02" PRIx32 "%02" PRIx32 "%02" PRIx32
						     " rgba=(%.3f,%.3f,%.3f,%.3f)\n",
						     log_id, bi, rec, ai, rd.register_start, r8, g8, b8, a8,
						     static_cast<double>(r8) / 255.0, static_cast<double>(g8) / 255.0,
						     static_cast<double>(b8) / 255.0, static_cast<double>(a8) / 255.0);
					}
				}
			}
		}

		for (int ai = 0; ai < b.attr_num; ai++) {
			const auto  res_index = b.attr_indices[ai];
			const auto& r         = vs_input_info.resources[res_index];
			const auto& rd        = vs_input_info.resources_dst[res_index];
			LOGF("DrawInputState[%llu]: attr[%d] res=%d offset=%u dst=v%d regs=%d fetch_index=%u "
			     "sharp=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
			     log_id, ai, res_index, b.attr_offsets[ai], rd.register_start, rd.registers_num,
			     rd.fetch_index, r.fields[0], r.fields[1], r.fields[2], r.fields[3]);
		}
	}
}

static void SetGraphicsDynamicParams(const CommandBuffer& buffer, vk::CommandBuffer vk_buffer,
                                     const ShaderVertexInputInfo& vs_input_info,
                                     const RenderColorInfo* colors, uint32_t color_count,
                                     const RenderDepthInfo& depth) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(colors == nullptr);
	const auto& ctx = buffer.GetRegisters();

	const auto&  vp = ctx.GetScreenViewport();
	vk::Extent2D framebuffer_extent {};
	if (color_count > 0 && colors[0].image_id) {
		framebuffer_extent = colors[0].Extent();
	} else if (depth.image_id) {
		framebuffer_extent = {depth.desc.info.extent.width, depth.desc.info.extent.height};
	} else {
		const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
		framebuffer_extent = {limits.maxFramebufferWidth, limits.maxFramebufferHeight};
	}
	const auto& outputs = vs_input_info.stage.program->info.outputs;
	const bool  indexed_viewports =
	    std::any_of(outputs.begin(), outputs.end(), [](const auto& output) {
		    return output.kind == ShaderRecompiler::IR::StageOutputKind::ViewportIndex;
	    });
	constexpr uint32_t viewport_slots = std::size(HW::ScreenViewport {}.viewports);
	std::array<vk::Viewport, viewport_slots> viewports {};
	std::array<vk::Rect2D, viewport_slots>   scissors {};
	const uint32_t viewport_count = indexed_viewports ? viewport_slots : 1;
	for (uint32_t i = 0; i < viewport_count; i++) {
		const auto& guest    = vp.viewports[i];
		auto&       viewport = viewports[i];
		if (ctx.GetClipControl().clip_disable) {
			const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
			viewport.width  = static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u));
			viewport.height = static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u));
		} else {
			viewport.x      = guest.xoffset - guest.xscale;
			viewport.y      = guest.yoffset - guest.yscale;
			viewport.width  = guest.xscale * 2.0f;
			viewport.height = guest.yscale * 2.0f;
		}
		viewport.minDepth =
		    guest.zoffset - (ctx.GetClipControl().dx_clip_space ? 0.0f : guest.zscale);
		viewport.maxDepth = guest.zscale + guest.zoffset;

		const auto final_scissor =
		    calc_final_scissor(vp, ctx.GetScanModeControl(), framebuffer_extent, i);
		auto& scissor  = scissors[i];
		scissor.offset = {final_scissor.left, final_scissor.top};
		scissor.extent = {static_cast<uint32_t>(final_scissor.right - final_scissor.left),
		                  static_cast<uint32_t>(final_scissor.bottom - final_scissor.top)};
		if (viewport.width == 0.0f) {
			// Keep empty slots at their guest index; Vulkan requires a positive viewport width.
			viewport.width = 1.0f;
			scissor.extent = {0, 0};
		}
	}
	vk_buffer.setViewportWithCount(viewport_count, viewports.data());
	vk_buffer.setScissorWithCount(viewport_count, scissors.data());

	float line_width = ctx.GetLineWidth();
	if (line_width != 1.0f) {
		static bool logged = false;
		if (!logged) {
			LOGF("Render: temporary: clamping Vulkan line width %f to 1.0 because wideLines is "
			     "not enabled\n",
			     line_width);
			logged = true;
		}
		line_width = 1.0f;
	}
	vk_buffer.setLineWidth(line_width);
	const auto&      blend = ctx.GetBlendColor();
	const std::array blend_constants {blend.red, blend.green, blend.blue, blend.alpha};
	vk_buffer.setBlendConstants(blend_constants.data());
	vk_buffer.setDepthTestEnable(depth.depth_test_enable ? VK_TRUE : VK_FALSE);
	vk_buffer.setDepthWriteEnable(depth.depth_write_enable ? VK_TRUE : VK_FALSE);
	// TEMPORARY diagnostic, 2026-09-10: KYTY_DEBUG_DEPTH_ALWAYS_PASS=1 forces every draw's depth
	// compare to Always, to test whether the always-black-output draws (e.g.
	// vs=0x98d8e293b8ff2e48/ps=0x113acd3e87a31cf0, which carries OpExecutionMode
	// EarlyFragmentTests + depth_test=true + depth_write=false + depth_clear=false, i.e. its
	// visibility depends entirely on a depth buffer some earlier pass must have written
	// correctly) are being early-Z rejected against a stale/wrong depth buffer. Not a real fix,
	// remove after the experiment.
	static const bool debug_depth_always_pass = std::getenv("KYTY_DEBUG_DEPTH_ALWAYS_PASS") != nullptr;
	vk_buffer.setDepthCompareOp(debug_depth_always_pass ? vk::CompareOp::eAlways
	                                                    : depth.depth_compare_op);

	const auto& mode              = ctx.GetModeControl();
	const auto& poly_offset       = ctx.GetPolyOffset();
	const bool  use_front         = mode.poly_offset_front_enable && !mode.cull_front;
	const bool  use_back          = mode.poly_offset_back_enable && !mode.cull_back;
	const bool  depth_bias_enable = use_front || use_back;
	vk_buffer.setDepthBiasEnable(depth_bias_enable ? VK_TRUE : VK_FALSE);
	if (depth_bias_enable) {
		// Vulkan has one bias for both faces. Prefer a visible front face when both are enabled.
		const float guest_constant_factor =
		    use_front ? poly_offset.front_offset : poly_offset.back_offset;
		const float constant_factor = ConvertPolygonOffsetConstantFactor(
		    guest_constant_factor, poly_offset, depth.desc.view_info.format);
		const float slope_factor =
		    (use_front ? poly_offset.front_scale : poly_offset.back_scale) / 16.0f;
		vk_buffer.setDepthBias(constant_factor, poly_offset.clamp, slope_factor);
	}

	if (depth.stencil_test_enable) {
		vk_buffer.setStencilCompareMask(vk::StencilFaceFlagBits::eFront,
		                                depth.stencil_dynamic_front.compareMask);
		vk_buffer.setStencilCompareMask(vk::StencilFaceFlagBits::eBack,
		                                depth.stencil_dynamic_back.compareMask);
		vk_buffer.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,
		                              depth.stencil_dynamic_front.writeMask);
		vk_buffer.setStencilWriteMask(vk::StencilFaceFlagBits::eBack,
		                              depth.stencil_dynamic_back.writeMask);
		vk_buffer.setStencilReference(vk::StencilFaceFlagBits::eFront,
		                              depth.stencil_dynamic_front.reference);
		vk_buffer.setStencilReference(vk::StencilFaceFlagBits::eBack,
		                              depth.stencil_dynamic_back.reference);
	}

#if defined(__APPLE__)
	// MoltenVK has no VK_EXT_color_write_enable; the pipeline is created without the
	// eColorWriteEnableEXT dynamic state and relies on the static colorWriteMask instead.
#else
	vk::Bool32 enable[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	// Color-control operation selects special color-buffer paths, not the normal component write
	// mask. Attachment availability therefore follows the target write mask.
	for (uint32_t i = 0; i < color_count; i++) {
		enable[i] = render_target_mask_slot(ctx.GetRenderTargetMask(), colors[i].target_slot) != 0
		                ? VK_TRUE
		                : VK_FALSE;
	}
	if (color_count != 0) {
		vk_buffer.setColorWriteEnableEXT(color_count, enable);
	}
#endif
}

static bool DrawHasValidVertexShader(const HW::Shader& sh_ctx) {

	const auto& vs = sh_ctx.GetVs();
	return ShaderAddressValid(vs.es_regs.data_addr);
}

static bool PixelShaderHasDepthOrCoverageSideEffects(const HW::ShaderRegisters& sh_regs) {
	const auto& db = sh_regs.db_shader_control;
	return db.shader_kill_enable || db.shader_z_export_enable || db.shader_mask_export_enable ||
	       db.shader_dual_export_enable || db.shader_execute_on_noop;
}

struct DrawRenderState {
	RenderDepthInfo       depth_info;
	RenderColorInfo       color_info[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	uint32_t              color_count                              = 0;
	bool                  ps_active                                = true;
	ShaderVertexInputInfo vs_input_info;
	ShaderPixelInputInfo  ps_input_info;
	PipelineCache::GraphicsPrograms programs;
};

struct DrawCallInfo {
	const char*          name           = nullptr;
	CommandBufferDebugOp debug_op       = CommandBufferDebugOp::DrawIndex;
	uint32_t             index_count    = 0;
	uint32_t             instance_count = 0;
	uint32_t             first_instance = 0;
};

RenderState RenderExecutor::AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
                                                 uint32_t color_count, RenderDepthInfo& depth,
                                                 const std::optional<PreparedBindings>& pixel) {
	EXIT_IF(colors == nullptr || color_count > RENDER_COLOR_ATTACHMENTS_MAX);
	auto&       cache = m_context.GetTextureCache();
	RenderState state {};
	state.width                 = std::numeric_limits<uint32_t>::max();
	state.height                = std::numeric_limits<uint32_t>::max();
	state.num_layers            = std::numeric_limits<uint32_t>::max();
	state.num_color_attachments = color_count;
	uint32_t attachment_samples = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		auto& target = colors[i];
		EXIT_IF(!target.image_id);
		const auto old_image = cache.m_slot_images.try_get(target.image_id);
		if (old_image == nullptr || (!old_image->registered && !old_image->info.data.Empty()) ||
		    old_image->binding.needs_rebind) {
			if (old_image != nullptr) {
				old_image->binding = {};
			}
			target.image_id = cache.FindImage(target.desc);
			BindRenderTarget(target.image_id);
		}
		const auto image_view = cache.FindRenderTarget(target.image_id, target.desc);
		auto&      image      = cache.GetImage(target.image_id);
		SetVulkanObjectNameF(m_context.GetGraphics().device, image.backing.image,
		                     "Kyty.MRT{}.Image[guest=0x{:016x} size=0x{:x} format={}]",
		                     target.target_slot, image.info.data.address, image.info.data.size,
		                     static_cast<uint32_t>(image.info.pixel_format));
		SetVulkanObjectNameF(m_context.GetGraphics().device, image_view,
		                     "Kyty.MRT{}.View[guest=0x{:016x} mip={} layer={}+{}]",
		                     target.target_slot, image.info.data.address,
		                     target.desc.view_info.base_level, target.desc.view_info.base_layer,
		                     target.desc.view_info.layer_count);
		EXIT_IF(image.backing.samples != target.desc.info.samples || image_view == nullptr);
		if (attachment_samples == 0) {
			attachment_samples = target.desc.info.samples;
		} else if (attachment_samples != target.desc.info.samples) {
			EXIT("mixed color attachment sample counts are unsupported: %u and %u\n",
			     attachment_samples, target.desc.info.samples);
		}
		const auto& view   = target.desc.view_info;
		const auto  layout = image.binding.is_bound ? vk::ImageLayout::eGeneral
		                                            : vk::ImageLayout::eColorAttachmentOptimal;
		image.binding.attachment_layout = layout;
		image.binding.attachment_access =
		    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
		image.Transit(layout, image.binding.attachment_access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count},
		              buffer.Handle());
		const auto extent       = target.Extent();
		state.width             = std::min(state.width, extent.width);
		state.height            = std::min(state.height, extent.height);
		state.num_layers        = std::min(state.num_layers, view.layer_count);
		auto& attachment        = state.color_attachments[i];
		attachment.image_view   = image_view;
		attachment.image_layout = layout;
		attachment.image_id     = target.image_id;
		// Session-30 fix: consume a pending clear armed by ConsumeMetadataColorOperation's
		// EliminateFastClear handling (renderDraw.cpp). Consume-once (TakePendingColorClear
		// erases on hit) so this render pass clears exactly once, matching real hardware's own
		// fast-clear-eliminate semantics -- context.cpp already reads is_clear/clear_value to
		// pick loadOp eClear vs eLoad, previously always eLoad since nothing ever set is_clear.
		vk::ClearColorValue pending_clear {};
		if (cache.TakePendingColorClear(image.info.data.address, &pending_clear)) {
			attachment.is_clear   = true;
			attachment.clear_value = pending_clear.uint32;
		}
	}
	// A stale depth target from an earlier, smaller pass must not shrink the render area for a
	// larger colour pass. Astro Bot's title screen keeps a 1920x1080 depth attachment bound on
	// a 3840x2160 composite pass; the render area would clamp to the 1080p corner and leave the
	// rest of the frame holding whatever was there before ("only a square is cleared"). The
	// guest cannot actually pair mismatched attachment sizes, so treat the depth as unbound.
	// The same applies when the depth target is the larger one: hardware clips each attachment
	// to its own bounds, but a Vulkan pass has a single render area, so pairing a 1024x1024
	// colour target with a 1920x1080 depth attachment writes depth only in the 1024x1024 corner
	// and leaves the rest of it stale. Astro Bot does exactly that -- two colour slots covering
	// one 1024x1024 surface with complementary channel masks, over a full-size depth buffer.
	if (depth.image_id && color_count > 0 &&
	    (depth.desc.info.extent.width != state.width ||
	     depth.desc.info.extent.height != state.height)) {
		static std::atomic_bool logged = false;
		if (!logged.exchange(true, std::memory_order_relaxed)) {
			LOGF("RenderState: depth target %ux%u does not match colour %ux%u -- unbinding it "
			     "for the draw\n",
			     depth.desc.info.extent.width, depth.desc.info.extent.height, state.width,
			     state.height);
		}
		depth.image_id = {};
	}
	if (depth.image_id) {
		const auto owner = cache.m_slot_images.try_get(depth.image_id);
		if (owner == nullptr || !owner->registered || owner->binding.needs_rebind) {
			EXIT("depth target changed after render-state discovery\n");
		}
		const auto  image_view = cache.FindDepthTarget(depth.image_id, depth.desc);
		const auto& metadata   = depth.desc.info.metadata;
		if (metadata.kind == ImageMetadataKind::Htile && depth.depth_clear_enable &&
		    !cache.ClearMeta(metadata.range.address)) {
			EXIT("failed to acquire HTile metadata for a depth clear\n");
		}
		const bool meta_clear =
		    metadata.kind == ImageMetadataKind::Htile &&
		    cache.IsMetaCleared(metadata.range.address, depth.desc.view_info.base_layer);
		depth.depth_load_clear_enable = depth.depth_clear_enable || meta_clear;
		if (meta_clear &&
		    !cache.TouchMeta(metadata.range.address, depth.desc.view_info.base_layer, false)) {
			EXIT("failed to consume HTile clear state\n");
		}
		auto& image = cache.GetImage(depth.image_id);
		SetVulkanObjectNameF(m_context.GetGraphics().device, image.backing.image,
		                     "Kyty.DepthTarget.Image[guest=0x{:016x} size=0x{:x} format={}]",
		                     image.info.data.address, image.info.data.size,
		                     static_cast<uint32_t>(image.info.pixel_format));
		SetVulkanObjectNameF(m_context.GetGraphics().device, image_view,
		                     "Kyty.DepthTarget.View[guest=0x{:016x} layer={}+{}]",
		                     image.info.data.address, depth.desc.view_info.base_layer,
		                     depth.desc.view_info.layer_count);
		EXIT_IF(image_view == nullptr || image.backing.samples != depth.desc.info.samples);
		if (attachment_samples == 0) {
			attachment_samples = depth.desc.info.samples;
		} else if (attachment_samples != depth.desc.info.samples) {
			EXIT("mixed color/depth sample counts are unsupported: %u and %u\n", attachment_samples,
			     depth.desc.info.samples);
		}
		const bool feedback = depth.depth_write_enable && pixel &&
		    std::ranges::any_of(pixel->images, [&](const TextureBinding& binding) {
			    if (binding.image_id != depth.image_id ||
			        binding.desc.type != TextureCache::BindingType::Texture) {
				    return false;
			    }
			    const auto native =
			        std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
			    EXIT_IF(native == image.views.end());
			    const auto& sampled = native->info;
			    const auto& target = depth.desc.view_info;
			    return (sampled.aspect & vk::ImageAspectFlagBits::eDepth) &&
			           ImageRangeOverlaps(sampled.base_level, sampled.level_count,
			                              target.base_level, target.level_count) &&
			           ImageRangeOverlaps(sampled.base_layer, sampled.layer_count,
			                              target.base_layer, target.layer_count);
		    });
		if (feedback && !m_context.GetGraphics().attachment_feedback_loop_enabled) {
			EXIT("depth attachment feedback loop is not supported by the host\n");
		}
		const auto layout = feedback ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
		                             : depth_attachment_layout(depth);
		// The attachment store writes even when guest depth/stencil tests do not.
		const auto access = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
		                    vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
		image.binding.attachment_layout = layout;
		image.binding.attachment_access = access;
		const auto& view                = depth.desc.view_info;
		image.Transit(layout, access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count},
		              buffer.Handle());
		state.width               = std::min(state.width, depth.desc.info.extent.width);
		state.height              = std::min(state.height, depth.desc.info.extent.height);
		state.num_layers          = std::min(state.num_layers, view.layer_count);
		const auto aspects        = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		auto&      attachment     = state.depth_stencil_attachment;
		attachment.image_view     = image_view;
		attachment.image_layout   = layout;
		attachment.clear_value[0] = std::bit_cast<uint32_t>(depth.depth_clear_value);
		attachment.clear_value[1] = depth.stencil_clear_value;
		attachment.has_depth      = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eDepth);
		attachment.depth_clear    = depth.depth_load_clear_enable;
		attachment.has_stencil    = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eStencil);
		attachment.stencil_clear  = depth.stencil_clear_enable;
	}
	if (color_count == 0 && !depth.image_id) {
		const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
		state.width        = limits.maxFramebufferWidth;
		state.height       = limits.maxFramebufferHeight;
	} else if (attachment_samples == 0 ||
	           vulkan_sample_count(attachment_samples) == vk::SampleCountFlagBits {}) {
		EXIT("render state has no valid attachments\n");
	}
	if (state.num_layers == std::numeric_limits<uint32_t>::max()) {
		state.num_layers = 1;
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.width == std::numeric_limits<uint32_t>::max() ||
	        state.height == std::numeric_limits<uint32_t>::max());
	return state;
}

static bool DrawHasActivePixelShader(const CommandBuffer& buffer) {
	const auto& ctx              = buffer.GetRegisters();
	const auto& sh_regs          = ctx.GetShaderRegisters();
	const bool  has_color_output = (ctx.GetRenderTargetMask() & sh_regs.m_cbShaderMask) != 0;
	return ShaderAddressValid(buffer.GetShaders().GetPs().ps_regs.data_addr) &&
	       (has_color_output || PixelShaderHasDepthOrCoverageSideEffects(sh_regs));
}

enum class CbColorMode : uint8_t {
	Disable            = 0,
	Normal             = 1,
	EliminateFastClear = 2,
	Resolve            = 3,
	FmaskDecompress    = 5,
	DccDecompress      = 6,
};

bool RenderExecutor::ConsumeMetadataColorOperation(CommandBuffer& buffer) {
	const auto& ctx  = buffer.GetRegisters();
	const auto  mode = ctx.GetColorControl().mode;
	// These special modes run color-buffer metadata or decompression operations. The shader is a
	// vehicle for that operation, and its exported color must not be applied as a normal draw.
	// Kyty stores expanded Vulkan images rather than compressed guest surfaces, so no equivalent
	// hardware pass is emitted. Tracked DCC clear state is materialized on attachment bind;
	// future CMask/FMask support can consume its state through the same TextureCache path.
	const bool consumed = mode == static_cast<uint8_t>(CbColorMode::EliminateFastClear) ||
	                      mode == static_cast<uint8_t>(CbColorMode::FmaskDecompress) ||
	                      mode == static_cast<uint8_t>(CbColorMode::DccDecompress);
	// This draw is otherwise dropped with zero trace (ResetBindings(); return;) -- per this
	// project's logging rule, a silently-consumed draw is a defect in itself until it says so.
	// Labelled so investigation doesn't have to guess which of the three modes fired, and which
	// render-target address(es) it targeted (cross-references RenderColorTargetInspect).
	// Deliberately NOT gated by graphics_debug_dump_enabled(): that flag also turns on
	// unconditional per-draw sh_print/uc_print/hw_print dumps, whose volume is what makes deep
	// runs slow (workflow/study_tooling_perf.md); this is a single rate-limited line that a cheap
	// run should still be able to surface.
	if (consumed) {
		static Log::RateLimit limiter {"ConsumeMetadataColorOperation", 256};
		if (limiter.Hit()) {
			const auto  mask = ctx.GetRenderTargetMask();
			const auto& rt0  = ctx.GetRenderTarget(0);
			LOGF("ConsumeMetadataColorOperation: dropping draw, mode=%u (%s) target_mask=0x%08" PRIx32
			     " rt0_addr=0x%010" PRIx64 " rt0_dcc_enable=%s rt1_addr=0x%010" PRIx64 "\n", mode,
			     mode == static_cast<uint8_t>(CbColorMode::EliminateFastClear)  ? "EliminateFastClear"
			     : mode == static_cast<uint8_t>(CbColorMode::FmaskDecompress) ? "FmaskDecompress"
			                                                                  : "DccDecompress",
			     mask, rt0.base.addr, rt0.info.dcc_compression_enable ? "true" : "false",
			     ctx.GetRenderTarget(1).base.addr);
		}
		// Real fix (sessions 30-31, astro_playroom_issues.md): EliminateFastClear is real evidence
		// the guest wants this target cleared to its currently-set clear-word register value --
		// confirmed against real hardware (mesa RADV's radv_fast_clear_eliminate renders a real
		// full-screen triangle) and a real peer emulator (shadPS4's Rasterizer::EliminateFastClear
		// calls image.Clear() with a decoded clear value), neither of which drop the draw with
		// zero effect the way this function previously did unconditionally. FmaskDecompress/
		// DccDecompress stay dropped -- those are genuine no-ops for Kyty's expanded (non-DCC-
		// compressed) image storage, per the comment above; only EliminateFastClear carries a
		// real clear-color intent.
		//
		// Applied immediately, right here, on this draw's own CommandBuffer, when the target is
		// already a registered image (TryImmediateColorClear) -- this is the common case for
		// ASTRO's UI targets and is what a session-31 live diagnostic showed is required: they
		// get read straight back as a sampled texture by a later compute pass rather than
		// rebound as a color attachment, and Vulkan's descriptor-binding cache means the
		// render-attachment/CommitBindings consume points this session tried first are visited
		// far too rarely (7-8 times vs. 256 arms in a real run) to catch it reliably. Falls back
		// to the address-keyed arm/consume pair (TextureCache::ArmColorClear /
		// TakePendingColorClear, consumed by the render-attachment path in AcquireRenderTargets)
		// only for the rare case where the image isn't registered yet.
		if (mode == static_cast<uint8_t>(CbColorMode::EliminateFastClear)) {
			auto&      cache = buffer.GetContext().GetTextureCache();
			const auto mask  = ctx.GetRenderTargetMask();
			for (uint32_t slot = 0; slot < 8; slot++) {
				if (render_target_mask_slot(mask, slot) == 0) {
					continue;
				}
				const auto& rt = ctx.GetRenderTarget(slot);
				if (rt.base.addr == 0) {
					continue;
				}
				const auto format =
				    TextureGetRenderTargetFormat(rt.info.format, rt.info.channel_type,
				                                 rt.info.channel_order);
				vk::ClearColorValue value {};
				const bool decoded = DecodePackedColorClear(format.format, rt.clear_word0.word0,
				                                            value, rt.clear_word1.word1);
				if (!decoded) {
					continue;
				}
				if (!cache.TryImmediateColorClear(buffer, rt.base.addr, value)) {
					// Session-32 fix (astro_playroom_issues.md, Bug B): live-confirmed root cause
					// for the one target in ASTRO's 4-target rotation that never clears --
					// ResolveRenderColorTarget is the ONLY path that creates/registers a
					// TextureCache::Image for a color target, and it is never reached for a
					// target that this frame touches solely through this metadata-consume path
					// (no real draw or dispatch binds/samples/writes it). Force registration here
					// from the same register state already in hand, then retry the immediate
					// clear once, before falling back to the address-keyed arm/consume pair below.
					// ignore_target_mask=true mirrors ResolveMirroredColorCopy's existing use of
					// the same override for a different metadata scenario (renderDraw.cpp).
					RenderColorInfo forced_target {};
					ResolveRenderColorTarget(buffer, forced_target, 0, slot,
					                         /*ignore_target_mask=*/true);
					if (!cache.TryImmediateColorClear(buffer, rt.base.addr, value)) {
						cache.ArmColorClear(rt.base.addr, value);
					}
				}
			}
		}
	}
	return consumed;
}

struct DrawEmitInfo {
	bool     indexed       = false;
	int32_t  vertex_offset = 0;
	uint32_t first_vertex  = 0;
	uint32_t first_instance = 0;
};

struct DrawIndexBufferSource {
	uint64_t      address   = 0;
	const void*   host_data = nullptr;
	uint64_t      size      = 0;
	vk::IndexType type      = vk::IndexType::eUint16;
	uint32_t      guest_element_size = 0;
};

struct PreparedIndexBuffer {
	vk::Buffer     buffer = nullptr;
	vk::DeviceSize offset = 0;
	vk::IndexType  type   = vk::IndexType::eUint16;
};

static uint64_t VertexBufferDescriptorSize(const ShaderVertexInputBuffer& buffer,
                                           const ShaderVertexInputInfo& info) {
	if (buffer.stride != 0 || buffer.num_records == 0) {
		return static_cast<uint64_t>(buffer.stride) * buffer.num_records;
	}

	uint64_t size = 0;
	for (int i = 0; i < buffer.attr_num; i++) {
		const auto& resource = info.resources[buffer.attr_indices[i]];
		// RDNA2 OOB_SELECT=2 only checks NumRecords != 0. A constant attribute still
		// fetches its entire format; NumRecords is not a byte count in this mode.
		const uint64_t extent = resource.OutOfBounds() == 2
		                            ? static_cast<uint64_t>(buffer.attr_offsets[i]) +
		                                  ShaderRecompiler::Format::GetFormatInfo(resource.Format()).byte_size
		                            : buffer.num_records;
		size = std::max(size, extent);
	}
	return size;
}

struct VertexBufferRange {
	uint64_t                     base_address  = 0;
	uint64_t                     requested_end = 0;
	uint64_t                     acquired_end  = 0;
	std::pair<Buffer*, uint64_t> binding;

	[[nodiscard]] uint64_t RequestedSize() const { return requested_end - base_address; }
};

struct PreparedVertexBuffers {
	static constexpr uint32_t MaxBuffers = ShaderVertexInputInfo::RES_MAX;

	std::array<vk::Buffer, MaxBuffers>     buffers {};
	std::array<vk::DeviceSize, MaxBuffers> offsets {};
	uint32_t                               count = 0;
};

static PreparedVertexBuffers AcquireVertexBuffers(CommandBuffer&               buffer,
                                                  const ShaderVertexInputInfo& vs_input_info) {
	EXIT_IF(vs_input_info.buffers_num < 0 ||
	        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX);

	// Collect the non-empty guest vertex ranges.
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> ranges {};
	uint32_t                                                      range_count = 0;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = VertexBufferDescriptorSize(vertex, vs_input_info);
		if (size == 0) {
			continue;
		}
		if (vertex.addr == 0 || size > UINT64_MAX - vertex.addr) {
			EXIT("invalid vertex buffer range: addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     vertex.addr, size);
		}
		ranges[range_count++] = {vertex.addr, vertex.addr + size};
	}

	std::sort(ranges.begin(), ranges.begin() + range_count,
	          [](const VertexBufferRange& left, const VertexBufferRange& right) {
		          return left.base_address < right.base_address;
	          });

	// Merge overlapping or touching ranges before acquiring host buffers.
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> merged_ranges {};
	uint32_t                                                      merged_count = 0;
	for (uint32_t i = 0; i < range_count; i++) {
		const auto& range = ranges[i];
		if (merged_count != 0 &&
		    merged_ranges[merged_count - 1].requested_end >= range.base_address) {
			merged_ranges[merged_count - 1].requested_end =
			    std::max(merged_ranges[merged_count - 1].requested_end, range.requested_end);
			continue;
		}
		merged_ranges[merged_count++] = {range.base_address, range.requested_end};
	}

	auto& cache = buffer.GetContext().GetBufferCache();
	for (uint32_t i = 0; i < merged_count; i++) {
		auto& range = merged_ranges[i];
		// PPSA20298
		const auto size =
		    Libs::LibKernel::Memory::ClampRangeSize(range.base_address, range.RequestedSize());
		range.acquired_end = range.base_address + size;
		range.binding      = cache.ObtainBuffer(range.base_address, size, false);
		SetVulkanObjectNameF(
		    buffer.GetContext().GetGraphics().device, range.binding.first->Handle(),
		    "Kyty.VertexBufferRange[guest=0x{:016x} size=0x{:x}]", range.base_address, size);
	}

	// Rebuild slot bindings, offsetting non-empty slots into their acquired merged range.
	PreparedVertexBuffers prepared;
	prepared.count         = static_cast<uint32_t>(vs_input_info.buffers_num);
	vk::Buffer null_buffer = nullptr;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = VertexBufferDescriptorSize(vertex, vs_input_info);
		if (size == 0) {
			if (null_buffer == nullptr) {
				null_buffer = cache.GetBuffer(NULL_BUFFER_ID).Handle();
			}
			prepared.buffers[i] = null_buffer;
			prepared.offsets[i] = 0;
			continue;
		}

		const auto range = std::find_if(merged_ranges.begin(), merged_ranges.begin() + merged_count,
		                                [&](const VertexBufferRange& value) {
			                                return vertex.addr >= value.base_address &&
			                                       vertex.addr < value.acquired_end;
		                                });
		if (range == merged_ranges.begin() + merged_count) {
			EXIT("vertex buffer address is outside the acquired range: addr=0x%016" PRIx64 "\n",
			     vertex.addr);
		}

		prepared.buffers[i] = range->binding.first->Handle();
		prepared.offsets[i] = range->binding.second + vertex.addr - range->base_address;
		SetVulkanObjectNameF(
		    buffer.GetContext().GetGraphics().device, prepared.buffers[i],
		    "Kyty.VertexBuffer[slot={} guest=0x{:016x} size=0x{:x} stride={} records={}]", i,
		    vertex.addr, size, vertex.stride, vertex.num_records);
	}

	return prepared;
}

static void SetDrawDebugPhase(CommandBuffer& buffer, uint64_t submit_id, const DrawCallInfo& draw,
                              uint32_t phase) {
	buffer.SetDebugInfo(static_cast<uint32_t>(draw.debug_op), submit_id, phase, draw.index_count, 0,
	                    draw.instance_count, draw.first_instance);
}

static bool GetDrawTopology(const HW::UserConfig& ucfg, bool auto_draw,
                            vk::PrimitiveTopology& topology) {

	topology = vk::PrimitiveTopology::ePointList;

	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kNone: return false;
		case Prospero::PrimitiveType::kPointList:
			topology = vk::PrimitiveTopology::ePointList;
			break;
		case Prospero::PrimitiveType::kLineList: topology = vk::PrimitiveTopology::eLineList; break;
		case Prospero::PrimitiveType::kLineStrip:
			topology = vk::PrimitiveTopology::eLineStrip;
			break;
		case Prospero::PrimitiveType::kTriList:
			topology = vk::PrimitiveTopology::eTriangleList;
			break;
		case Prospero::PrimitiveType::kTriFan:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		case Prospero::PrimitiveType::kTriStrip:
			topology = vk::PrimitiveTopology::eTriangleStrip;
			break;
		case Prospero::PrimitiveType::kRectList:
			topology = vk::PrimitiveTopology::ePatchList;
			break;
		case Prospero::PrimitiveType::kRectListLegacy:
			// kRectListLegacy is AMD's real hardware DI_PT_RECTLIST (confirmed against Mesa's
			// RADV primitive table, which lists it as {3 vertices consumed, 3 per primitive}):
			// the guest submits exactly 3 vertices per rectangle and the hardware derives the
			// 4th corner itself. It must expand exactly like kRectList above -- through the
			// tessellation control/evaluation shaders in rectListShader.cpp, which interpolate
			// the 4th corner from the 3 real vertices -- and NOT as a 4-vertex triangle strip,
			// which used to invoke the guest vertex shader a 4th time on an index the guest never
			// submitted: a real out-of-bounds guest storage-buffer read at gl_VertexIndex==3 for
			// a 3-vertex draw, confirmed against ASTRO's Playroom's own shader bytecode.
			if (!auto_draw) {
				EXIT("unknown primitive type: %u (kRectListLegacy is only expected from "
				     "DrawIndexAuto)\n",
				     static_cast<uint32_t>(ucfg.GetPrimType()));
			}
			topology = vk::PrimitiveTopology::ePatchList;
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		default: {
			static std::atomic_bool logged = false;
			if (!logged.exchange(true, std::memory_order_relaxed)) {
				std::printf("Skipping draw with unknown primitive type: %u\n",
				            static_cast<uint32_t>(ucfg.GetPrimType()));
			}
			return false;
		}
	}

	return true;
}

static bool ResolvePrimitiveRestart(const CommandBuffer& buffer,
                                    const DrawIndexBufferSource& source) {
	const auto control = buffer.GetUserConfig().GetPrimitiveResetControl();
	EXIT_NOT_IMPLEMENTED((control & ~0x3u) != 0);
	if ((control & 0x1u) == 0) {
		return false;
	}
	switch (buffer.GetUserConfig().GetPrimType()) {
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip: break;
		default: return false;
	}

	const auto element_size = source.guest_element_size;
	const auto index_mask   = UINT32_MAX >> ((4 - element_size) * 8);
	const auto reset_index  = buffer.GetRegisters().GetPrimitiveResetIndex();
	if ((control & 0x2u) != 0 && (reset_index & ~index_mask) != 0) {
		return false;
	}
	const auto restart_index = reset_index & index_mask;
	if (restart_index == index_mask) {
		// Use native restart; the 8-bit path widens its marker to 0xffff.
		return true;
	}

	// A game can set a custom reset value without using it in the index buffer.
	// Keep restart off in that case; fail if we actually find the value.
	// Scan before preparing draw resources: readback can restart the command buffer.
	EXIT_NOT_IMPLEMENTED(source.address == 0);
	const auto* indices = reinterpret_cast<const uint8_t*>(source.address);
	for (uint64_t offset = 0; offset < source.size; offset += element_size) {
		uint32_t index = 0;
		std::memcpy(&index, indices + offset, element_size);
		EXIT_NOT_IMPLEMENTED(index == restart_index);
	}
	return false;
}

bool RenderExecutor::PrepareDrawRenderState(CommandBuffer& buffer, const DrawCallInfo& draw,
                                            uint32_t            render_target_slice_offset,
                                            bool log_setup_phases, DrawRenderState& state) {
	auto& ctx = buffer.GetRegisters();

	if (ResolveColorTargets(buffer, render_target_slice_offset)) {
		return false;
	}
	if (log_setup_phases) {
		LogDrawPhase(draw.name, "ResolveRenderColorTarget");
	}
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		if (slot == 0 || (render_target_mask_slot(ctx.GetRenderTargetMask(), slot) != 0 &&
		                  ctx.GetRenderTarget(slot).base.addr != 0)) {
			ResolveRenderColorTarget(buffer, state.color_info[state.color_count],
			                         render_target_slice_offset, slot);
			if (state.color_info[state.color_count].image_id) {
				state.color_count++;
			}
		}
	}
	if (log_setup_phases) {
		LogDrawPhase(draw.name, "ResolveRenderDepthTarget");
	}
	ResolveRenderDepthTarget(buffer, state.depth_info);

	state.ps_active       = DrawHasActivePixelShader(buffer);
	if (state.color_count == 0 && !state.depth_info.image_id && !state.ps_active) {
		LogFramebufferSkip(draw.name, state.color_info[0], state.depth_info, buffer,
		                   draw.index_count, 0);
		return false;
	}

	return true;
}

static void RefreshShaders(CommandBuffer& buffer, const DrawCallInfo& draw, bool log_phases,
                           DrawRenderState& state) {
	auto& ctx    = buffer.GetRegisters();
	auto& sh_ctx = buffer.GetShaders();

	const auto& vertex_shader_info = sh_ctx.GetVs();
	const auto& pixel_shader_info  = sh_ctx.GetPs();
	const auto& shader_regs        = ctx.GetShaderRegisters();

	state.programs      = {};
	state.ps_input_info = {};
	std::array<Prospero::ColorComponentMapping, RENDER_COLOR_ATTACHMENTS_MAX>
	    target_export_mapping {};
	for (uint32_t i = 0; i < state.color_count; i++) {
		target_export_mapping[state.color_info[i].target_slot] = state.color_info[i].export_mapping;
	}
	auto& pipeline_cache = buffer.GetContext().GetPipelineCache();
	if (log_phases) {
		LogDrawPhase(draw.name, "GetGraphicsPrograms");
	}
	state.programs = pipeline_cache.GetGraphicsPrograms(
	    vertex_shader_info, pixel_shader_info, shader_regs, ctx, buffer.GetUserConfig(),
	    target_export_mapping, state.ps_active, state.vs_input_info, state.ps_input_info);
}

static PreparedIndexBuffer PrepareIndexBuffer(CommandBuffer&               buffer,
                                              const DrawIndexBufferSource& source) {
	PreparedIndexBuffer prepared;
	if (source.size == 0) {
		return prepared;
	}
	prepared.type = source.type;
	if (source.host_data != nullptr) {
		auto& stream = buffer.GetContext().GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
		prepared.offset = stream.Copy(source.host_data, source.size, 16);
		prepared.buffer = stream.Handle();
	} else {
		auto [buffer_ptr, offset] =
		    buffer.GetContext().GetBufferCache().ObtainBuffer(source.address, source.size, false);
		prepared.buffer = buffer_ptr->Handle();
		prepared.offset = offset;
	}
	if (source.host_data != nullptr) {
		SetVulkanObjectNameF(buffer.GetContext().GetGraphics().device, prepared.buffer,
		                     "Kyty.IndexBuffer[guest=transient size=0x{:x} type={}]", source.size,
		                     static_cast<uint32_t>(source.type));
	} else {
		SetVulkanObjectNameF(buffer.GetContext().GetGraphics().device, prepared.buffer,
		                     "Kyty.IndexBuffer[guest=0x{:016x} size=0x{:x} type={}]",
		                     source.address, source.size, static_cast<uint32_t>(source.type));
	}
	return prepared;
}

static void CommitVertexBuffers(vk::CommandBuffer            vk_buffer,
                                const PreparedVertexBuffers& prepared) {
	for (uint32_t i = 0; i < prepared.count; i++) {
		EXIT_IF(prepared.buffers[i] == nullptr);
	}
	if (prepared.count != 0) {
		vk_buffer.bindVertexBuffers(0, prepared.count, prepared.buffers.data(),
		                            prepared.offsets.data());
	}
}

static void CommitIndexBuffer(vk::CommandBuffer vk_buffer, const PreparedIndexBuffer& prepared) {
	if (prepared.buffer == nullptr) {
		return;
	}
	vk_buffer.bindIndexBuffer(prepared.buffer, prepared.offset, prepared.type);
}

// Shared by every diagnostic below that wants to name a draw's shaders in a log line or a
// --draw-dump filename (drawDump.cpp reads the same fields off RenderAttachment). A draw with no
// active pixel shader, or whose MaterializeResources rejected its program, reports 0 rather than
// dereferencing a null program.
static uint64_t DrawVsShaderHash(const DrawRenderState& state) {
	return state.vs_input_info.stage.program != nullptr
	           ? state.vs_input_info.stage.program->shader_hash
	           : 0u;
}

static uint64_t DrawPsShaderHash(const DrawRenderState& state) {
	return state.ps_active && state.ps_input_info.stage.program != nullptr
	           ? state.ps_input_info.stage.program->shader_hash
	           : 0u;
}

static Log::RateLimit g_rect_list_flat_log_limit {"RectListFlatHazard", 128};

// Diagnostic for the "rect-list tessellation expansion may mis-handle flat PS inputs" hypothesis:
// RectListEmitter::EmitControl (rectListShader.cpp) feeds every
// synthesized quad corner the value from vertex 0 for a flat-shaded PS input, regardless of which of
// the 3 real vertices is the anchor for that specific rectangle. Flag every rect-list draw whose
// pixel shader has an active flat-shaded input, so a specific --draw-dump sequence can be matched
// against this hazard before any fix is applied.
static void LogRectListFlatHazardIfNeeded(const CommandBuffer& buffer, const char* draw_name,
                                          vk::PrimitiveTopology topology,
                                          const DrawRenderState& state) {
	// See LogDrawStateIfNeeded's comment (2026-09-10): --graphics-debug-dump's blanket gate
	// used to sit here too, defeating this diagnostic's own frame window for exactly the same
	// reason. DrawLogFrameInWindow + g_rect_list_flat_log_limit below already control cost.
	if (topology != vk::PrimitiveTopology::ePatchList || !state.ps_active ||
	    state.ps_input_info.stage.program == nullptr ||
	    state.vs_input_info.stage.program == nullptr) {
		return;
	}

	const auto frame = buffer.GetContext().GetGpu().GetFrameNum();
	if (!DrawLogFrameInWindow(frame)) {
		return;
	}

	const auto& ps_info = state.ps_input_info;
	for (const auto& input: ps_info.stage.program->info.inputs) {
		if (input.kind != ShaderRecompiler::IR::StageInputKind::Parameter ||
		    !ShaderPixelParameterIsFlat(ps_info, input.location)) {
			continue;
		}
		const auto hit = g_rect_list_flat_log_limit.Hit();
		if (!hit) {
			return;
		}
		LOGF("RectListFlatHazard[%llu]: frame=%d %s flat_input_location=%u vs_hash=0x%016" PRIx64
		     " ps_hash=0x%016" PRIx64 "\n",
		     static_cast<unsigned long long>(*hit), frame, draw_name, input.location,
		     state.vs_input_info.stage.program->shader_hash, ps_info.stage.program->shader_hash);
	}
}

static Log::RateLimit g_legacy_rect_draw_log_limit {"LegacyRectDraw", 128};

// Announces every draw that goes through the GNM rectangle-list expansion (kRectList or
// kRectListLegacy -- see GetDrawTopology/EmitDrawPrimitives above), so a --draw-dump sequence can
// always be matched to the exact draw that produced it without needing to re-derive it from
// DrawTargetState/DrawInputState. This is the diagnostic that would have made the
// kRectListLegacy defect visible in the log from the first run, instead of requiring several
// rounds of instrumentation to narrow down to this primitive.
static void LogLegacyRectDrawIfNeeded(const CommandBuffer& buffer, const char* draw_name,
                                      const HW::UserConfig& ucfg, const DrawRenderState& state,
                                      const DrawCallInfo& draw, const DrawEmitInfo& emit) {
	// See LogDrawStateIfNeeded's comment (2026-09-10) for why --graphics-debug-dump's blanket
	// gate doesn't belong here either: DrawLogFrameInWindow + g_legacy_rect_draw_log_limit
	// below already control cost.
	const auto prim_type = ucfg.GetPrimType();
	if (prim_type != Prospero::PrimitiveType::kRectList &&
	    prim_type != Prospero::PrimitiveType::kRectListLegacy) {
		return;
	}

	const auto frame = buffer.GetContext().GetGpu().GetFrameNum();
	if (!DrawLogFrameInWindow(frame)) {
		return;
	}

	const auto hit = g_legacy_rect_draw_log_limit.Hit();
	if (!hit) {
		return;
	}

	LOGF("LegacyRectDraw[%llu]: frame=%d %s prim=%u index_count=%u instance_count=%u "
	     "first_vertex=%u vs_buffers_num=%d param_export_mask=0x%08" PRIx32
	     " vs_hash=0x%016" PRIx64 " ps_hash=0x%016" PRIx64 "\n",
	     static_cast<unsigned long long>(*hit), frame, draw_name,
	     static_cast<uint32_t>(prim_type), draw.index_count, draw.instance_count,
	     emit.first_vertex, state.vs_input_info.buffers_num,
	     state.vs_input_info.stage.program != nullptr
	         ? state.vs_input_info.stage.program->param_export_mask
	         : 0u,
	     DrawVsShaderHash(state), DrawPsShaderHash(state));
}

static void LogDrawStateIfNeeded(const CommandBuffer& buffer, const DrawCallInfo& draw,
                                 const DrawRenderState& state, bool always_log,
                                 bool force_legacy_rect_log, uint32_t index_type_and_size,
                                 const void* index_addr) {
	// Found 2026-09-10 while chasing a live rendering defect: this used to also require
	// --graphics-debug-dump, which ALSO unconditionally enables hw_print/sh_print/uc_print (a
	// full, per-draw, NOT frame-windowed register dump -- see those calls in DrawIndex/DrawAuto)
	// that turned a run with a heavy real scene into an effective hang, hours of it mistaken for
	// a genuine emulator stall before being traced to logging cost, not a bug. LogDrawTargetState
	// and LogDrawInputState below are already both frame-windowed (--draw-log-frame-first/-last)
	// and independently rate-limited (Log::RateLimit); they don't need --graphics-debug-dump's
	// blanket gate on top, and tying them to it made it impossible to get per-draw target/input
	// state without ALSO paying for the expensive firehose. Cost control is
	// DrawLogFrameInWindow + the named limiters, same as every other diagnostic in this file.
	if (!always_log && !force_legacy_rect_log) {
		return;
	}

	const auto vs_hash = DrawVsShaderHash(state);
	const auto ps_hash = DrawPsShaderHash(state);

	if (state.ps_active) {
		LogDrawTargetState(draw.name, state.color_info[0], state.depth_info, buffer,
		                   state.ps_input_info, draw.index_count, 0, vs_hash, ps_hash);
	}
	LogDrawInputState(buffer, state.color_info[0], state.vs_input_info, index_type_and_size,
	                  draw.index_count, index_addr,
	                  static_cast<uint32_t>(buffer.GetUserConfig().GetPrimType()), vs_hash,
	                  ps_hash);
}

static void EmitDrawPrimitives(const HW::UserConfig& ucfg, vk::CommandBuffer vk_buffer,
                               const ShaderVertexInputInfo& vs_input_info, const DrawCallInfo& draw,
                               const DrawEmitInfo& emit) {
	// Invariant: this function must never ask Vulkan to run more vertex-shader invocations for a
	// single EmitDrawPrimitives call than the guest actually submitted indices/vertices for
	// (draw.index_count). A step here is not a place to invent an extra vertex and hope the
	// guest's vertex shader produces something sane for an index it was never meant to see -- an
	// out-of-bounds guest read is what real games do with that invented index (this is exactly
	// how the kRectListLegacy defect happened, via a fabricated 4th vertex when the guest only
	// submitted 3). Centralizing every draw call
	// through this makes that impossible to reintroduce by accident in a future primitive type.
	const auto emit_draw = [&](uint32_t vertex_count, uint32_t first_index) {
		if (vertex_count > draw.index_count) {
			EXIT("EmitDrawPrimitives: refusing to emit %u vertices for a %u-index draw "
			     "(prim=%u indexed=%u) -- a primitive-expansion step would run the guest vertex "
			     "shader on an index it never submitted\n",
			     vertex_count, draw.index_count, static_cast<uint32_t>(ucfg.GetPrimType()),
			     emit.indexed);
		}
		if (emit.indexed) {
			vk_buffer.drawIndexed(vertex_count, draw.instance_count, first_index,
			                      emit.vertex_offset, emit.first_instance);
		} else {
			vk_buffer.draw(vertex_count, draw.instance_count, first_index + emit.first_vertex,
			               emit.first_instance);
		}
	};

	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kPointList:
		case Prospero::PrimitiveType::kLineList:
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriList:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip:
			emit_draw(draw.index_count, 0);
			break;
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kRectListLegacy:
			// Both are GNM rectangle-list encodings expanded by the tessellation control/
			// evaluation shaders in rectListShader.cpp, which consume 3 real guest vertices per
			// rectangle (patchControlPoints == 3, see pipeline/shaders.cpp) and interpolate the
			// 4th corner themselves. A count that is not a multiple of 3 means the guest issued a
			// malformed rect-list draw -- surface that with the actual numbers rather than
			// silently truncating or over-reading.
			if (draw.index_count % 3 != 0) {
				EXIT("rect-list draw has index_count not a multiple of 3: prim=%u index_count=%u "
				     "vs_buffers_num=%d indexed=%u\n",
				     static_cast<uint32_t>(ucfg.GetPrimType()), draw.index_count,
				     vs_input_info.buffers_num, emit.indexed);
			}
			emit_draw(draw.index_count, 0);
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			EXIT_NOT_IMPLEMENTED((draw.index_count & 0x3u) != 0);
			for (uint32_t i = 0; i < draw.index_count; i += 4) {
				emit_draw(4, i);
			}
			break;
		default: EXIT("unknown primitive type: %u\n", static_cast<uint32_t>(ucfg.GetPrimType()));
	}
}

void RenderExecutor::ExecutePreparedDraw(uint64_t submit_id, CommandBuffer& buffer,
                                         const DrawCallInfo& draw, DrawRenderState& state,
                                         vk::PrimitiveTopology topology, const DrawEmitInfo& emit,
                                         const DrawIndexBufferSource& index_source,
                                         bool primitive_restart_enable, bool log_pipeline_phase,
                                         bool set_bind_debug, bool set_auto_debug) {
	auto& ucfg = buffer.GetUserConfig();
	const bool mesh_active = state.vs_input_info.stage.program->stage == ShaderType::Mesh;
	uint32_t   mesh_groups = 0;
	if (mesh_active) {
		const auto& mesh = state.vs_input_info.mesh;
		if (primitive_restart_enable || mesh.primitives_per_group == 0) {
			EXIT("unsupported mesh draw: primitive=%u indexed=%u restart=%u\n",
			     static_cast<uint32_t>(ucfg.GetPrimType()), emit.indexed, primitive_restart_enable);
		}
		const auto primitives = mesh.InputPrimitiveCount(draw.index_count);
		if (primitives == 0 || draw.instance_count == 0) {
			return;
		}
		mesh_groups        = (primitives - 1u) / mesh.primitives_per_group + 1u;
		const auto& limits = m_context.GetGraphics().mesh_shader_properties;
		if (mesh_groups > limits.maxMeshWorkGroupCount[0] ||
		    draw.instance_count > limits.maxMeshWorkGroupCount[1] ||
		    static_cast<uint64_t>(mesh_groups) * draw.instance_count >
		        limits.maxMeshWorkGroupTotalCount) {
			EXIT("mesh draw exceeds host workgroup limits: %ux%u\n", mesh_groups,
			     draw.instance_count);
		}
	}

	if (mesh_active && emit.indexed) {
		// Register the original guest indices for shader reads; PrepareGraphicsBindings
		// synchronizes registered BDA ranges before any draw commands are committed.
		(void)m_context.GetBufferCache().FindBuffer(
		    index_source.address, static_cast<uint64_t>(draw.index_count) *
		                              index_source.guest_element_size);
	}
	LogDrawPhase(draw.name, "PrepareBindings");
	auto bindings = PrepareGraphicsBindings(state.vs_input_info.stage, state.ps_input_info.stage,
	                                        state.ps_active);
	PreparedVertexBuffers vertex_bindings;
	PreparedIndexBuffer   index_binding;
	if (!mesh_active) {
		LogDrawPhase(draw.name, "PrepareVertexBuffers");
		vertex_bindings = AcquireVertexBuffers(buffer, state.vs_input_info);
		index_binding   = PrepareIndexBuffer(buffer, index_source);
	}
	auto rendering =
	    AcquireRenderTargets(buffer, state.color_info, state.color_count, state.depth_info,
	                         bindings.pixel);
	// Debug-only (see RenderAttachment::vs_shader_hash in renderTarget.h): stamp the shader
	// identity and guest primitive type onto every color attachment this draw targets, so
	// --draw-dump can name a dumped PNG after the exact shader/primitive that produced it.
	// Deliberately excluded from RenderAttachment::operator== -- see that struct's comment.
	{
		const auto vs_hash    = DrawVsShaderHash(state);
		const auto ps_hash    = DrawPsShaderHash(state);
		const auto prim_type = static_cast<uint32_t>(ucfg.GetPrimType());
		uint64_t   tex_address[RenderAttachment::TEX_DEBUG_MAX] {};
		uint32_t   tex_format[RenderAttachment::TEX_DEBUG_MAX] {};
		uint32_t   tex_tile_mode[RenderAttachment::TEX_DEBUG_MAX] {};
		if (bindings.pixel.has_value()) {
			for (uint32_t t = 0;
			     t < RenderAttachment::TEX_DEBUG_MAX && t < bindings.pixel->images.size(); t++) {
				const auto& info  = bindings.pixel->images[t].desc.info;
				tex_address[t]    = info.data.address;
				tex_format[t]     = static_cast<uint32_t>(info.guest_format);
				tex_tile_mode[t]  = static_cast<uint32_t>(info.tile_mode);
			}
		}
		for (uint32_t i = 0; i < state.color_count; i++) {
			auto& attachment          = rendering.color_attachments[i];
			attachment.vs_shader_hash = vs_hash;
			attachment.ps_shader_hash = ps_hash;
			attachment.prim_type      = prim_type;
			for (uint32_t t = 0; t < RenderAttachment::TEX_DEBUG_MAX; t++) {
				attachment.tex_address[t]   = tex_address[t];
				attachment.tex_format[t]    = tex_format[t];
				attachment.tex_tile_mode[t] = tex_tile_mode[t];
			}
		}
	}

	if (log_pipeline_phase) {
		LogDrawPhase(draw.name, "CreatePipeline");
	}
	auto& pipeline = m_context.GetPipelineCache().CreateGraphicsPipeline(
	    std::span {state.color_info, state.color_count}, state.depth_info, state.vs_input_info, buffer,
	    state.ps_active ? &state.ps_input_info : nullptr, topology, primitive_restart_enable,
	    state.programs.vertex, state.programs.pixel);

	// Resource preparation above may synchronously finish and restart the scheduler. From this
	// point onward, every operation targets the current command buffer and cannot touch guest
	// memory.
	auto vk_buffer = buffer.Handle();
	if (set_bind_debug) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x100u);
	}
	if (set_auto_debug) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x200u);
	}
	if (!mesh_active) {
		CommitVertexBuffers(vk_buffer, vertex_bindings);
	}
	if (bindings.pixel.has_value()) {
		if (set_auto_debug) {
			SetDrawDebugPhase(buffer, submit_id, draw, 0x300u);
		}
	}
	std::array<PreparedBindings*, 2> descriptor_stages {&bindings.vertex, nullptr};
	const size_t                     descriptor_stage_count = bindings.pixel.has_value() ? 2u : 1u;
	if (bindings.pixel) {
		descriptor_stages[1] = &*bindings.pixel;
	}
	CommitBindings(buffer, vk::PipelineBindPoint::eGraphics, pipeline,
	               std::span {descriptor_stages.data(), descriptor_stage_count});
	if (mesh_active) {
		const uint32_t draw_data[] {
		    draw.index_count,
		    emit.indexed ? static_cast<uint32_t>(emit.vertex_offset) : emit.first_vertex,
		    emit.first_instance, index_source.guest_element_size,
		    static_cast<uint32_t>(index_source.address),
		    static_cast<uint32_t>(index_source.address >> 32u)};
		static_assert(std::size(draw_data) == ShaderRecompiler::IR::PushData::MeshDrawDwordCount);
		vk_buffer.pushConstants(pipeline.pipeline_layout,
		                        vk::ShaderStageFlagBits::eMeshEXT |
		                            vk::ShaderStageFlagBits::eFragment,
		                        0, sizeof(draw_data), draw_data);
	} else {
		CommitIndexBuffer(vk_buffer, index_binding);
	}

	SetGraphicsDynamicParams(buffer, vk_buffer, state.vs_input_info, state.color_info,
	                         state.color_count, state.depth_info);
	if (m_context.GetGraphics().attachment_feedback_loop_enabled) {
		vk_buffer.setAttachmentFeedbackLoopEnableEXT(
		    rendering.depth_stencil_attachment.image_layout ==
		            vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
		        ? vk::ImageAspectFlags {vk::ImageAspectFlagBits::eDepth}
		        : vk::ImageAspectFlags {});
	}

	LogDrawPhase(draw.name, "BeginRendering");
	if (set_auto_debug) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x400u);
	}
	m_context.GetCommandScheduler().BeginRendering(rendering);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
	if (set_auto_debug) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x500u);
	}
	if (mesh_active) {
		vk_buffer.drawMeshTasksEXT(mesh_groups, draw.instance_count, 1);
	} else {
		EmitDrawPrimitives(ucfg, vk_buffer, state.vs_input_info, draw, emit);
	}

	if (set_auto_debug) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x600u);
	}
	vk::PipelineStageFlags shader_write_stages = {};
	if (HasShaderBufferWrites(state.vs_input_info.stage)) {
		shader_write_stages |= mesh_active ? vk::PipelineStageFlagBits::eMeshShaderEXT
		                                   : vk::PipelineStageFlagBits::eVertexShader;
	}
	if (state.ps_active && HasShaderBufferWrites(state.ps_input_info.stage)) {
		shader_write_stages |= vk::PipelineStageFlagBits::eFragmentShader;
	}
	if (shader_write_stages) {
		m_context.GetCommandScheduler().EndRendering();
		ShaderWriteBarrier(vk_buffer, shader_write_stages);
	}
	LogDrawPhase(draw.name, "DrawComplete");
	if (set_auto_debug) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x700u);
	}
}

void RenderExecutor::DrawIndex(uint64_t submit_id, CommandBuffer& buffer,
                               const DrawIndexArgs& args) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(buffer.IsInvalid());
	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	m_context.GetCommandScheduler().PopPendingOperations();
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndex), submit_id,
	                    args.index_count, 0, 1, args.instance_count,
	                    reinterpret_cast<uint64_t>(args.index_addr));

	Common::LockGuard lock(m_context.GetMutex());
	if (args.index_count == 0 || args.instance_count == 0) {
		return;
	}

	if (ConsumeMetadataColorOperation(buffer) || DepthStencilCopy(buffer)) {
		ResetBindings();
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return;
	}

	if (graphics_debug_dump_enabled()) {
		sh_print("GraphicsRenderDrawIndex():Shader:", sh_ctx, buffer.GetRegisters());
		uc_print("GraphicsRenderDrawIndex():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndex():Parameters:\n"
		     "\t index_type_and_size = 0x%08" PRIx32 "\n"
		     "\t index_count         = 0x%08" PRIx32 "\n"
		     "\t index_addr          = 0x%016" PRIx64 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t base_vertex         = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.index_type_and_size, args.index_count,
		     reinterpret_cast<uint64_t>(args.index_addr), args.instance_count,
		     static_cast<uint32_t>(args.base_vertex), args.first_instance);
	}

	uc_check(ucfg);

	hw_check(buffer);

	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(ucfg, false, topology)) {
		return;
	}

	DrawIndexBufferSource index_source {};
	index_source.address = reinterpret_cast<uint64_t>(args.index_addr);
	switch (static_cast<Prospero::IndexType>(args.index_type_and_size)) {
		case Prospero::IndexType::kIndex16:
			index_source.type               = vk::IndexType::eUint16;
			index_source.guest_element_size = 2;
			break;
		case Prospero::IndexType::kIndex32:
			index_source.type               = vk::IndexType::eUint32;
			index_source.guest_element_size = 4;
			break;
		case Prospero::IndexType::kIndex8:
			index_source.guest_element_size = 1;
			break;
		default: EXIT("unknown index_type_and_size: %u\n", args.index_type_and_size);
	}
	index_source.size = static_cast<uint64_t>(args.index_count) * index_source.guest_element_size;
	const bool primitive_restart = ResolvePrimitiveRestart(buffer, index_source);

	std::vector<uint16_t> expanded_indices;
	if (index_source.guest_element_size == 1) {
		EXIT_NOT_IMPLEMENTED(args.index_addr == nullptr);
		const auto* src = static_cast<const uint8_t*>(args.index_addr);
		expanded_indices.resize(args.index_count);
		for (uint32_t i = 0; i < args.index_count; i++) {
			expanded_indices[i] = primitive_restart && src[i] == 0xffu ? 0xffffu : src[i];
		}
		index_source.host_data = expanded_indices.data();
		index_source.size      = expanded_indices.size() * sizeof(uint16_t);
	}

	const DrawCallInfo draw {"DrawIndex", CommandBufferDebugOp::DrawIndex, args.index_count,
	                        args.instance_count, args.first_instance};
	DrawRenderState state {};
	if (!PrepareDrawRenderState(buffer, draw, args.render_target_slice_offset, true,
	                            state)) {
		ResetBindings();
		return;
	}

	RefreshShaders(buffer, draw, true, state);
	if (!state.programs.vertex || (state.ps_active && !state.programs.pixel)) {
		// MaterializeResources rejected a descriptor set for this draw's VS or PS
		// (ProgramCache::Get already logged the shader hash, stage and reason once) -- skip the
		// draw instead of dereferencing a null state.*_input_info.stage.program below.
		ResetBindings();
		return;
	}

	LogDrawStateIfNeeded(buffer, draw, state, true, false, args.index_type_and_size,
	                     args.index_addr);
	LogRectListFlatHazardIfNeeded(buffer, draw.name, topology, state);

	const bool indirect = args.offset_source == DrawOffsetSource::IndirectArgs;
	const auto vertex_offset =
	    indirect
	        ? args.base_vertex
	        : ResolveVertexOffset(ucfg.GetIndexOffset(), state.vs_input_info) + args.base_vertex;

	DrawEmitInfo emit {};
	emit.indexed       = true;
	emit.vertex_offset = vertex_offset;
	emit.first_instance =
	    indirect ? args.first_instance : ResolveInstanceOffset(state.vs_input_info);

	LogLegacyRectDrawIfNeeded(buffer, draw.name, ucfg, state, draw, emit);

	ExecutePreparedDraw(submit_id, buffer, draw, state, topology, emit, index_source,
	                    primitive_restart, true, true, false);
	ResetBindings();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RenderExecutor::DrawAuto(uint64_t submit_id, CommandBuffer& buffer, const DrawAutoArgs& args) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(buffer.IsInvalid());
	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	m_context.GetCommandScheduler().PopPendingOperations();
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndexAuto), submit_id,
	                    args.vertex_count, 0, args.first_vertex, args.instance_count,
	                    args.first_instance);

	Common::LockGuard lock(m_context.GetMutex());
	if (args.vertex_count == 0 || args.instance_count == 0) {
		return;
	}

	if (ConsumeMetadataColorOperation(buffer) || DepthStencilCopy(buffer)) {
		ResetBindings();
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return;
	}

	if (graphics_debug_dump_enabled()) {
		sh_print("GraphicsRenderDrawIndexAuto():Shader:", sh_ctx, buffer.GetRegisters());
		uc_print("GraphicsRenderDrawIndexAuto():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndexAuto():Parameters:\n"
		     "\t vertex_count        = 0x%08" PRIx32 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t first_vertex        = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.vertex_count, args.instance_count, args.first_vertex, args.first_instance);
	}

	uc_check(ucfg);

	hw_check(buffer);

	const DrawCallInfo draw {"DrawIndexAuto", CommandBufferDebugOp::DrawIndexAuto,
	                         args.vertex_count, args.instance_count, args.first_instance};

	DrawRenderState state {};
	if (!PrepareDrawRenderState(buffer, draw, args.render_target_slice_offset, false,
	                            state)) {
		ResetBindings();
		return;
	}

	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(ucfg, true, topology)) {
		ResetBindings();
		return;
	}
	RefreshShaders(buffer, draw, false, state);
	if (!state.programs.vertex || (state.ps_active && !state.programs.pixel)) {
		// See the identical guard in DrawIndex: skip rather than dereference a null
		// state.*_input_info.stage.program below.
		ResetBindings();
		return;
	}

	// Keyed on the true (non-legacy) kRectList primitive specifically, not on "topology happens
	// to be ePatchList": both kRectList and kRectListLegacy now use ePatchList (see
	// GetDrawTopology above), but this skip's shape -- no VS param exports at all, yet the PS
	// declares inputs -- was characterized against kRectList's tessellation path. Re-keying to
	// the topology alone would newly swallow kRectListLegacy draws that render correctly and
	// always have (e.g. a procedural fullscreen-quad VS with buffers_num==0 and no PS inputs to
	// receive, which does not even match this shape, or one that does and previously rendered
	// fine as a 4-vertex strip) with no diagnostic explaining why they vanished.
	const bool rect_list = ucfg.GetPrimType() == Prospero::PrimitiveType::kRectList;
	if (rect_list && state.vs_input_info.buffers_num == 0 &&
	    state.vs_input_info.stage.program->param_export_mask == 0 &&
	    state.ps_input_info.input_num != 0) {
		static Log::RateLimit limiter {"DrawIndexAuto:SkipRectListNoExports", 128};
		if (const auto hit = limiter.Hit()) {
			LOGF("DrawIndexAuto[%llu]: skipping rect-list draw with no VS param exports and PS "
			     "inputs: frame=%d ps_inputs=%u ps=0x%016" PRIx64 " es=0x%016" PRIx64
			     " gs=0x%016" PRIx64 "\n",
			     static_cast<unsigned long long>(*hit), buffer.GetContext().GetGpu().GetFrameNum(),
			     state.ps_input_info.input_num, sh_ctx.GetPs().ps_regs.data_addr,
			     sh_ctx.GetVs().es_regs.data_addr, sh_ctx.GetVs().gs_regs.data_addr);
		}
		ResetBindings();
		return;
	}

	LogDrawStateIfNeeded(buffer, draw, state, false,
	                     ucfg.GetPrimType() == Prospero::PrimitiveType::kRectListLegacy, 0,
	                     nullptr);
	LogRectListFlatHazardIfNeeded(buffer, draw.name, topology, state);

	const bool indirect = args.offset_source == DrawOffsetSource::IndirectArgs;
	const auto vertex_offset =
	    indirect ? static_cast<int32_t>(args.first_vertex)
	             : ResolveVertexOffset(ucfg.GetIndexOffset(), state.vs_input_info) +
	                   static_cast<int32_t>(args.first_vertex);
	DrawEmitInfo emit {};
	emit.first_vertex = static_cast<uint32_t>(vertex_offset);
	emit.first_instance =
	    indirect ? args.first_instance : ResolveInstanceOffset(state.vs_input_info);

	LogLegacyRectDrawIfNeeded(buffer, draw.name, ucfg, state, draw, emit);

	DrawIndexBufferSource index_source {};
	ExecutePreparedDraw(submit_id, buffer, draw, state, topology, emit, index_source, false, false,
	                    false, true);
	ResetBindings();
}

bool RenderExecutor::ResolveColorTargets(CommandBuffer& buffer, uint32_t render_target_slice_offset) {
	const auto& hw = buffer.GetRegisters();
	if (hw.GetColorControl().mode != 3) {
		return false;
	}

	const auto& src_rt = hw.GetRenderTarget(0);
	const auto& dst_rt = hw.GetRenderTarget(1);
	if (src_rt.base.addr == 0 || dst_rt.base.addr == 0) {
		return false;
	}

	RenderColorInfo src {};
	RenderColorInfo dst {};
	ResolveRenderColorTarget(buffer, src, render_target_slice_offset, 0, true, true);
	ResolveRenderColorTarget(buffer, dst, render_target_slice_offset, 1, true, true);
	if (!src.image_id || !dst.image_id) {
		return false;
	}
	if (src.desc.info.data.address == dst.desc.info.data.address &&
	    src.guest_mip_level == dst.guest_mip_level &&
	    src.guest_array_layer == dst.guest_array_layer) {
		return true;
	}

	auto& cache = m_context.GetTextureCache();
	cache.MarkGpuWritten(dst.image_id);
	auto& source      = cache.GetImage(src.image_id);
	auto& destination = cache.GetImage(dst.image_id);
	destination.Resolve(source, {src.guest_mip_level, 1, src.guest_array_layer, 1},
	                    {dst.guest_mip_level, 1, dst.guest_array_layer, 1});
	return true;
}

} // namespace Libs::Graphics
