#include "graphics/host_gpu/renderer/renderDraw.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/spscQueue.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
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
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics {

std::pair<int32_t, uint32_t> ResolveDrawOffsets(uint32_t index_offset,
	                                           const ShaderVertexInputInfo& vs_input_info) {
	auto     vertex_offset   = static_cast<int32_t>(index_offset);
	uint32_t instance_offset = 0;
	if (!vs_input_info.has_dynamic_vertex_fetch) {
		return {vertex_offset, instance_offset};
	}

	EXIT_IF(!vs_input_info.stage);
	const auto& program   = vs_input_info.stage.module->info;
	const auto& resources = *vs_input_info.stage.resources;
	if (index_offset == 0 &&
	    program.info.vertex_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.vertex_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			vertex_offset = static_cast<int32_t>(resources.user_data[index]);
		}
	}
	if (program.info.instance_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.instance_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			instance_offset = resources.user_data[index];
		}
	}

	return {vertex_offset, instance_offset};
}

static std::atomic<uint32_t> g_draw_state_log_count   = 0;
static std::atomic<uint32_t> g_draw_input_log_count   = 0;
static std::atomic<uint32_t> g_mrt_state_log_count    = 0;

static std::atomic<uint32_t> g_framebuffer_skip_log_count = 0;

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

static void LogFramebufferSkip(const char* draw_name, const RenderDepthInfo& depth,
                               const CommandBuffer& buffer, uint32_t index_count, uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	auto log_id = g_framebuffer_skip_log_count.fetch_add(1, std::memory_order_relaxed);
	if (log_id >= 128) {
		return;
	}

	LOGF("DrawFramebufferSkip[%u]: %s depth_format=%s depth_image=%s depth_vaddr_num=%d "
	     "target_mask=0x%08" PRIx32 " prim=%u index_count=%u flags=0x%08" PRIx32 "\n",
	     log_id, draw_name, vk::to_string(depth.desc.view_info.format).c_str(),
	     depth.image_id ? "yes" : "no",
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

	auto log_id = g_mrt_state_log_count.fetch_add(1);
	if (log_id >= 32) {
		return;
	}

	LOGF("MrtState[%u]: %s rt_mask=0x%08" PRIx32 " cb_shader_mask=0x%08" PRIx32
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

		LOGF("MrtState[%u]: slot=%u addr=0x%010" PRIx64
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
                               uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!color.image_id) {
		return;
	}

	auto log_id = g_draw_state_log_count.fetch_add(1);
	if (log_id >= 192) {
		return;
	}

	const auto& cc             = ctx.GetColorControl();
	const auto& bc             = ctx.GetBlendControl(color.target_slot);
	const auto& dc             = ctx.GetDepthControl();
	const auto& vp             = ctx.GetScreenViewport();
	const auto& vp0            = vp.viewports[0];
	const auto& ps_resources   = ps_input_info.stage.module->info.info;
	const auto  sampled_images = std::count_if(
	    ps_resources.images.begin(), ps_resources.images.end(), [](const auto& image) {
		    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
	    });

	const auto extent = color.Extent();
	const auto sc     = calc_final_scissor(vp, ctx.GetScanModeControl(), extent, 0);

	LOGF(
	    "DrawTargetState[%u]: frame=%d %s target=%s addr=0x%010" PRIx64
	    " extent=%ux%u prim=%u index_count=%u flags=0x%08" PRIx32 " color_mask=0x%08" PRIx32
	    " cc_mode=%u cc_op=0x%02x"
	    " blend=%s src=%u dst=%u comb=%u ps_tex=%d sampled=%d storage=%d ps_kill=%s target_mode0=%u"
	    " depth_test=%s depth_write=%s depth_func=%u depth_clear=%s viewport=(%.1f,%.1f %.1fx%.1f) "
	    "scissor=(%d,%d)-(%d,%d)\n",
	    log_id, buffer.GetContext().GetGpu().GetFrameNum(), draw_name, RenderColorTypeName(color),
	    color.desc.info.data.address, extent.width, extent.height,
	    static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags, ctx.GetRenderTargetMask(),
	    cc.mode, cc.op,
	    bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.color_comb_fcn,
	    static_cast<int>(ps_resources.images.size()), static_cast<int>(sampled_images),
	    static_cast<int>(ps_resources.images.size() - sampled_images),
	    ps_input_info.ps_pixel_kill_enable ? "true" : "false", ps_input_info.target_output_mode[0],
	    dc.z_enable ? "true" : "false", dc.z_write_enable ? "true" : "false", dc.zfunc,
	    depth.depth_clear_enable ? "true" : "false", vp0.xoffset - vp0.xscale,
	    vp0.yoffset - vp0.yscale, vp0.xscale * 2.0f, vp0.yscale * 2.0f, sc.left, sc.top, sc.right,
	    sc.bottom);

	LogMrtState(draw_name, buffer, ps_input_info);
}

static void LogDrawInputState(const CommandBuffer& buffer, const RenderColorInfo& color,
                              const ShaderVertexInputInfo& vs_input_info,
                              uint32_t index_type_and_size, uint32_t index_count,
                              const void* index_addr) {
	auto log_id = g_draw_input_log_count.fetch_add(1);
	if (log_id >= 64) {
		return;
	}

	LOGF("DrawInputState[%u]: frame=%d target=%s addr=0x%010" PRIx64
	     " index_type=%u index_count=%u index_addr=0x%016" PRIx64
	     " vs_resources=%d vs_buffers=%d\n",
	     log_id, buffer.GetContext().GetGpu().GetFrameNum(), RenderColorTypeName(color),
	     color.desc.info.data.address, index_type_and_size, index_count,
	     reinterpret_cast<uint64_t>(index_addr), vs_input_info.resources_num,
	     vs_input_info.buffers_num);

	for (int bi = 0; bi < vs_input_info.buffers_num; bi++) {
		const auto& b = vs_input_info.buffers[bi];
		LOGF("DrawInputState[%u]: vb[%d] addr=0x%010" PRIx64
		     " stride=%u records=%u fetch_index=%u\n",
		     log_id, bi, b.addr, b.stride, b.num_records, b.fetch_index);

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
				LOGF("DrawInputState[%u]: vb[%d].rec[%u] stride=%u dwords=%u raw=%08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " f=(%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f)\n",
				     log_id, bi, rec, b.stride, dword_num, raw[0], raw[1], raw[2], raw[3], raw[4],
				     raw[5], raw[6], raw[7], raw[8], flt[0], flt[1], flt[2], flt[3], flt[4], flt[5],
				     flt[6], flt[7], flt[8]);

				for (int ai = 0; ai < vs_input_info.resources_num; ai++) {
					const auto& r  = vs_input_info.resources[ai];
					const auto& rd = vs_input_info.resources_dst[ai];
					if (rd.buffer_index != bi) {
						continue;
					}
					const auto offset = static_cast<uint32_t>(r.Base48() - b.addr);
					if (offset + 4u <= b.stride &&
					    r.Format() == Prospero::BufferFormat::k8_8_8_8UNorm) {
						uint32_t packed = 0;
						std::memcpy(&packed, rec_bytes + offset, sizeof(packed));
						const auto r8 = (packed >> 0u) & 0xffu;
						const auto g8 = (packed >> 8u) & 0xffu;
						const auto b8 = (packed >> 16u) & 0xffu;
						const auto a8 = (packed >> 24u) & 0xffu;
						LOGF("DrawInputState[%u]: vb[%d].rec[%u].attr[%d] dst=v%d fmt=56 "
						     "rgba8=%02" PRIx32 "%02" PRIx32 "%02" PRIx32 "%02" PRIx32
						     " rgba=(%.3f,%.3f,%.3f,%.3f)\n",
						     log_id, bi, rec, ai, rd.register_start, r8, g8, b8, a8,
						     static_cast<double>(r8) / 255.0, static_cast<double>(g8) / 255.0,
						     static_cast<double>(b8) / 255.0, static_cast<double>(a8) / 255.0);
					}
				}
			}
		}

		for (int ai = 0; ai < vs_input_info.resources_num; ai++) {
			const auto& r  = vs_input_info.resources[ai];
			const auto& rd = vs_input_info.resources_dst[ai];
			if (rd.buffer_index != bi) {
				continue;
			}
			LOGF("DrawInputState[%u]: attr[%d] offset=%u dst=v%d regs=%d fetch_index=%u "
			     "sharp=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
			     log_id, ai, static_cast<uint32_t>(r.Base48() - b.addr), rd.register_start,
			     rd.registers_num, rd.fetch_index, r.fields[0], r.fields[1], r.fields[2], r.fields[3]);
		}
	}
}

static void UpdateGraphicsDynamicState(const CommandBuffer& buffer, DynamicState& dynamic_state,
                                       const ShaderVertexInputInfo& vs_input_info,
                                       const RenderDepthInfo& depth, const RenderState& rendering) {
	KYTY_PROFILER_FUNCTION();

	const auto& ctx = buffer.GetRegisters();
	const auto&        vp  = ctx.GetScreenViewport();
	const vk::Extent2D framebuffer_extent {rendering.width, rendering.height};
	const auto& outputs = vs_input_info.stage.module->info.info.outputs;
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
	dynamic_state.SetViewports(std::span {viewports}.first(viewport_count));
	dynamic_state.SetScissors(std::span {scissors}.first(viewport_count));

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
	dynamic_state.SetLineWidth(line_width);
	const auto&      blend = ctx.GetBlendColor();
	const std::array blend_constants {blend.red, blend.green, blend.blue, blend.alpha};
	dynamic_state.SetBlendConstants(blend_constants);
	dynamic_state.SetDepthTestEnabled(depth.depth_test_enable);
	dynamic_state.SetDepthWriteEnabled(depth.depth_write_enable);
	dynamic_state.SetDepthCompareOp(depth.depth_compare_op);
#if !defined(__APPLE__)
	dynamic_state.SetDepthBoundsTestEnabled(depth.depth_bounds_test_enable);
	if (depth.depth_bounds_test_enable) {
		dynamic_state.SetDepthBounds(ctx.GetDepthBoundsMin(), ctx.GetDepthBoundsMax());
	}
#endif

	const auto& mode              = ctx.GetModeControl();
	const auto& poly_offset       = ctx.GetPolyOffset();
	const bool  use_front         = mode.poly_offset_front_enable && !mode.cull_front;
	const bool  use_back          = mode.poly_offset_back_enable && !mode.cull_back;
	const bool  depth_bias_enable = use_front || use_back;
	dynamic_state.SetDepthBiasEnabled(depth_bias_enable);
	if (depth_bias_enable) {
		// Vulkan has one bias for both faces. Prefer a visible front face when both are enabled.
		const float guest_constant_factor =
		    use_front ? poly_offset.front_offset : poly_offset.back_offset;
		const float constant_factor = ConvertPolygonOffsetConstantFactor(
		    guest_constant_factor, poly_offset, depth.desc.view_info.format);
		const float slope_factor =
		    (use_front ? poly_offset.front_scale : poly_offset.back_scale) / 16.0f;
		dynamic_state.SetDepthBias(constant_factor, poly_offset.clamp, slope_factor);
	}

	// Rect lists are drawn without culling.
	vk::CullModeFlags cull_mode {};
	if (!Prospero::IsRectList(buffer.GetUserConfig().GetPrimType())) {
		if (mode.cull_front) {
			cull_mode |= vk::CullModeFlagBits::eFront;
		}
		if (mode.cull_back) {
			cull_mode |= vk::CullModeFlagBits::eBack;
		}
	}
	dynamic_state.SetCullMode(cull_mode);
	dynamic_state.SetFrontFace(mode.face ? vk::FrontFace::eClockwise
	                                     : vk::FrontFace::eCounterClockwise);

	dynamic_state.SetStencilTestEnabled(depth.stencil_test_enable);
	if (depth.stencil_test_enable) {
		const auto& front = depth.stencil_front;
		const auto& back  = depth.stencil_back;
		dynamic_state.SetStencilOps({front.failOp, front.passOp, front.depthFailOp, front.compareOp},
		                            {back.failOp, back.passOp, back.depthFailOp, back.compareOp});
		dynamic_state.SetStencilCompareMasks(front.compareMask, back.compareMask);
		dynamic_state.SetStencilWriteMasks(front.writeMask, back.writeMask);
		dynamic_state.SetStencilReferences(front.reference, back.reference);
	}
}

static bool DrawHasValidVertexShader(const HW::Shader& sh_ctx) {

	const auto& vs = sh_ctx.GetVs();
	return vs.es_regs.data_addr != 0;
}

struct RenderExecutor::DrawRenderStorage {
	DrawRenderState state {};
	// Only tessellation draws write the input infos of the second and third vertex stages.
	bool tessellation_stages_written = false;
};
static_assert(std::is_trivially_destructible_v<DrawRenderState>);

template <typename T>
static void ValueInitialize(T& object) {
	::new (static_cast<void*>(std::addressof(object))) T {};
}

// A draw state is 36 KB, 30 KB of it the input infos of the three vertex stages, and
// value-initializing one on the stack took ~0.6 us per draw. Reuse one state and restore its
// value-initialized contents, skipping the tessellation stages when no draw wrote them.
// A resolution ahead of execution (see ResolveDrawAhead): the draw state with its shader
// programs, the storage their stages point to, and whether the execution thread still uses it.
struct RenderExecutor::ResolvedDraw {
	DrawRenderStorage storage;
	ProgramResolution programs;
	// A dispatch resolved ahead: its module and stage input (pointing into `programs`).
	ShaderComputeInputInfo compute;
	const ShaderModule*    compute_module = nullptr;
	std::atomic_bool       busy {false};
};

// More resolutions than operations can wait between the resolve and execution threads, so the
// resolve thread only ever waits for room in that queue.
struct RenderExecutor::ResolvedDrawRing {
	static constexpr size_t Size = 320;

	std::array<ResolvedDraw, Size> draws;
	size_t                         next = 0;
};

void RenderExecutor::ReleaseResolvedDraw(ResolvedDraw* resolved) noexcept {
	resolved->busy.store(false, std::memory_order_release);
}

// The render targets the last draw acquired. Nearly every draw renders to the targets of the
// draw before it, with the same target registers, and acquiring them again repeats the same
// lookups (image discovery, color metadata, attachment views, HTile state) with the same results.
// A draw reuses them while nothing those lookups depend on can have changed:
//  - only draws executed since the targets were acquired: any other operation and any
//    submission invalidate them, so no guest-visible effect let the guest write in between;
//  - the target registers and the slice offset are the same;
//  - no image was registered or unregistered, and each target image (and stencil association)
//    still has the tracking, dirty and ownership state the acquisition left it in;
//  - no work wrote cached buffers since, and no page of the color/HTile metadata became CPU
//    dirty. The guest writes metadata through the GPU; a CPU write between two draws would need
//    the GPU to wait for it (WAIT_REG_MEM), and the first operation after a wait invalidates the
//    targets too.
// Draws with depth or stencil clears are not reused. Image layout transitions, attachment
// layouts and binding state are still set by every draw.
struct RenderExecutor::RenderTargetMemo {
	struct MetadataEpoch {
		GuestRange range;
		uint64_t   epoch = 0;
	};

	// Registers ResolveRenderColorTarget() and ResolveRenderDepthTarget() read.
	uint64_t                                                   context_id   = 0;
	uint32_t                                                   mrt_mask     = 0;
	uint32_t                                                   target_mask  = 0;
	std::array<HW::RenderTarget, RENDER_COLOR_ATTACHMENTS_MAX> targets {};
	HW::DepthRenderTarget                                      depth {};
	HW::RenderControl                                          render_control {};
	std::array<float, 3>                                       depth_values {};
	uint8_t                                                    stencil_clear = 0;
	bool                                                       depth_wanted  = false;

	// A draw captured its registers and is acquiring its targets.
	bool pending = false;
	// The outputs below are the targets of the last draw.
	bool                                                           valid                = false;
	uint64_t                                                       tick                 = 0;
	uint64_t                                                       image_set_epoch      = 0;
	uint64_t                                                       gpu_write_generation = 0;
	std::array<MetadataEpoch, RENDER_COLOR_ATTACHMENTS_MAX + 1>    metadata {};
	uint32_t                                                       metadata_count = 0;
	std::array<ImageLookupState, RENDER_COLOR_ATTACHMENTS_MAX + 2> images {};
	uint32_t                                                       image_count = 0;
	DrawTargets                                                    draw_targets;
	std::array<vk::ImageView, RENDER_COLOR_ATTACHMENTS_MAX>        color_views {};
	vk::ImageView                                                  depth_view = nullptr;
};

template <typename T>
static bool SameBytes(const T& a, const T& b) {
	static_assert(std::is_trivially_copyable_v<T>);
	return std::memcmp(&a, &b, sizeof(T)) == 0;
}

template <typename T>
static void CopyBytes(T& destination, const T& source) {
	static_assert(std::is_trivially_copyable_v<T>);
	std::memcpy(&destination, &source, sizeof(T));
}

void RenderExecutor::InvalidateRenderTargetMemo() noexcept {
	m_draw_window++;
	if (m_render_target_memo != nullptr) {
		m_render_target_memo->valid   = false;
		m_render_target_memo->pending = false;
	}
	m_render_targets_reused = false;
}

void RenderExecutor::CaptureRenderTargetKey(const HW::Context& hw, uint32_t mrt_mask) {
	if (m_render_target_memo == nullptr) {
		m_render_target_memo = {new RenderTargetMemo {},
		                        [](RenderTargetMemo* memo) { delete memo; }};
	}
	auto& memo   = *m_render_target_memo;
	memo.valid   = false;
	memo.pending = !graphics_debug_dump_enabled();
	if (!memo.pending) {
		return;
	}
	memo.context_id   = m_context_id;
	memo.mrt_mask     = mrt_mask;
	memo.target_mask  = hw.GetRenderTargetMask();
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		if ((mrt_mask & (1u << slot)) != 0) {
			CopyBytes(memo.targets[slot], hw.GetRenderTarget(slot));
		}
	}
	CopyBytes(memo.depth, hw.GetDepthRenderTarget());
	CopyBytes(memo.render_control, hw.GetRenderControl());
	memo.depth_wanted  = DepthStencilTargetWanted(hw);
	memo.depth_values  = {hw.GetDepthClearValue(), hw.GetDepthBoundsMin(), hw.GetDepthBoundsMax()};
	memo.stencil_clear = hw.GetStencilClearValue();
}

bool RenderExecutor::RenderTargetMemoCurrent() {
	const auto& memo  = *m_render_target_memo;
	auto&       cache = m_context.GetTextureCache();
	if (memo.tick != m_context.GetCommandScheduler().CurrentTick() ||
	    memo.image_set_epoch != cache.ImageSetEpoch() ||
	    memo.gpu_write_generation != m_context.GetBufferCache().GpuWriteGeneration()) {
		return false;
	}
	for (uint32_t i = 0; i < memo.image_count; i++) {
		const auto& state = memo.images[i];
		const auto* image = cache.m_slot_images.try_get(state.id);
		if (image == nullptr || !image->registered || image->binding.needs_rebind ||
		    ImageLookupState::Of(state.id, *image) != state) {
			return false;
		}
	}
	auto& buffers = m_context.GetBufferCache();
	for (uint32_t i = 0; i < memo.metadata_count; i++) {
		const auto& metadata = memo.metadata[i];
		if (buffers.CpuDirtyEpoch(metadata.range.address, metadata.range.size) != metadata.epoch) {
			return false;
		}
	}
	return true;
}

bool RenderExecutor::TryReuseRenderTargets(const HW::Context& hw, uint32_t mrt_mask) {
	if (m_render_target_memo == nullptr || !m_render_target_memo->valid) {
		return false;
	}
	auto& memo = *m_render_target_memo;
	if (memo.mrt_mask != mrt_mask) {
		return false;
	}
	// Operations with the same context snapshot have the same registers.
	if (m_context_id == 0 || memo.context_id != m_context_id) {
		// The depth bounds only set dynamic state: draws that differ in them (a deferred light
		// each) reuse the targets and take the bounds below.
		// Neither do the depth and stencil test controls, as long as the draw still wants a
		// depth/stencil target: they are refreshed below (layouts follow per draw).
		const float depth_clear = hw.GetDepthClearValue();
		if (memo.target_mask != hw.GetRenderTargetMask() ||
		    !SameBytes(memo.depth, hw.GetDepthRenderTarget()) ||
		    !SameBytes(memo.render_control, hw.GetRenderControl()) ||
		    memo.depth_wanted != DepthStencilTargetWanted(hw) ||
		    !SameBytes(memo.depth_values[0], depth_clear) ||
		    memo.stencil_clear != hw.GetStencilClearValue()) {
			return false;
		}
		for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
			if ((mrt_mask & (1u << slot)) != 0 &&
			    !SameBytes(memo.targets[slot], hw.GetRenderTarget(slot))) {
				return false;
			}
		}
		memo.context_id = m_context_id;
	}
	if (!RenderTargetMemoCurrent()) {
		return false;
	}
	// The draw uses the memo's targets in place: only a draw that acquires its targets itself
	// stores new ones.
	auto& targets = memo.draw_targets;
	if (targets.depth.image_id) {
		ApplyDepthStencilTestState(hw, targets.depth);
	}
	m_draw_targets = &targets;
	for (uint32_t i = 0; i < targets.color_count; i++) {
		BindRenderTarget(targets.colors[i].image_id);
	}
	if (targets.depth.image_id) {
		BindRenderTarget(targets.depth.image_id);
	}
	return true;
}

void RenderExecutor::StoreRenderTargetMemo(const RenderColorInfo* colors, uint32_t color_count,
                                           const RenderDepthInfo& depth, const vk::ImageView* views,
                                           vk::ImageView depth_view, ImageId stencil) {
	auto& memo   = *m_render_target_memo;
	memo.pending = false;
	// Clears consume their state; the next draw acquires its targets itself.
	if (depth.depth_clear_enable || depth.load_clear_aspects || depth.stencil_clear_enable) {
		return;
	}
	auto& cache             = m_context.GetTextureCache();
	auto& buffers           = m_context.GetBufferCache();
	memo.metadata_count     = 0;
	memo.image_count        = 0;
	const auto add_metadata = [&](const ImageInfo& info) {
		if (info.metadata.kind == ImageMetadataKind::None || info.metadata.range.Empty()) {
			return true;
		}
		const auto range                     = info.metadata.range;
		memo.metadata[memo.metadata_count++] = {range,
		                                        buffers.CpuDirtyEpoch(range.address, range.size)};
		return true;
	};
	const auto add_image = [&](ImageId id) {
		memo.images[memo.image_count++] = ImageLookupState::Of(id, cache.m_slot_images[id]);
	};
	for (uint32_t i = 0; i < color_count; i++) {
		if (!add_metadata(colors[i].desc.info)) {
			return;
		}
		add_image(colors[i].image_id);
		memo.draw_targets.colors[i] = colors[i];
		memo.color_views[i]         = views[i];
	}
	if (depth.image_id) {
		if (!add_metadata(depth.desc.info)) {
			return;
		}
		add_image(depth.image_id);
		if (stencil) {
			add_image(stencil);
		}
	}
	memo.draw_targets.color_count = color_count;
	memo.draw_targets.depth       = depth;
	memo.depth_view               = depth_view;
	memo.tick                 = m_context.GetCommandScheduler().CurrentTick();
	memo.image_set_epoch      = cache.ImageSetEpoch();
	memo.gpu_write_generation = buffers.GpuWriteGeneration();
	memo.valid                = true;
}

// Keep it in sync with the members of DrawRenderState.
static void ResetDrawRenderState(DrawRenderState& state, bool tessellation_stages) {
	state.mrt_mask                 = 0;
	state.topology                 = vk::PrimitiveTopology::ePointList;
	state.primitive_restart_enable = false;
	state.ps_active                = false;
	ValueInitialize(state.vertex_info[0]);
	if (tessellation_stages) {
		ValueInitialize(state.vertex_info[1]);
		ValueInitialize(state.vertex_info[2]);
	}
	ValueInitialize(state.ps_input_info);
	state.indirect_offsets = {};
	state.indirect_draw    = false;
}

DrawRenderState& RenderExecutor::AcquireDrawRenderState(bool tessellation, bool& shaders_resolved,
                                                        const ShaderIndirectOffsets& indirect_offsets,
                                                        bool indirect_draw) {
	// The operations before this draw executed: the resolution holds when the guest memory it
	// read still has the same bytes.
	auto* resolved = std::exchange(m_resolved_draw, nullptr);
	// Resolutions ahead of execution read the shaders with the offsets of GPU-read indirect
	// records; draws that patch the SGPRs instead resolve themselves.
	shaders_resolved = resolved != nullptr && resolved->compute_module == nullptr &&
	                   resolved->storage.state.indirect_offsets == indirect_offsets &&
	                   resolved->storage.state.indirect_draw == indirect_draw &&
	                   resolved->programs.Revalidate();
	if (shaders_resolved) {
		return resolved->storage.state;
	}
	if (m_draw_state == nullptr) {
		m_draw_state = {new DrawRenderStorage {},
		                [](DrawRenderStorage* storage) { delete storage; }};
	} else {
		ResetDrawRenderState(m_draw_state->state, m_draw_state->tessellation_stages_written);
	}
	m_draw_state->tessellation_stages_written = tessellation;
	return m_draw_state->state;
}

struct DrawCallInfo {
	bool     indexed        = true;
	uint32_t index_count    = 0;
	uint32_t instance_count = 0;

	[[nodiscard]] const char* Name() const { return indexed ? "DrawIndex" : "DrawIndexAuto"; }
};

RenderState RenderExecutor::AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
                                                 uint32_t color_count, RenderDepthInfo& depth,
                                                 vk::ImageAspectFlags& feedback_aspects,
                                                 std::span<PreparedBindings* const> stages) {
	EXIT_IF(colors == nullptr || color_count > RENDER_COLOR_ATTACHMENTS_MAX);
	feedback_aspects = {};
	auto&       cache = m_context.GetTextureCache();
	// The targets of the draw before, when this draw reused them and they are still current (the
	// texture bindings of this draw may have rediscovered them).
	const RenderTargetMemo* memo = nullptr;
	if (m_render_targets_reused && m_render_target_memo->valid &&
	    m_render_target_memo->draw_targets.color_count == color_count &&
	    m_render_target_memo->draw_targets.depth.image_id == depth.image_id &&
	    RenderTargetMemoCurrent()) {
		memo = m_render_target_memo.get();
		for (uint32_t i = 0; i < color_count && memo != nullptr; i++) {
			if (memo->draw_targets.colors[i].image_id != colors[i].image_id) {
				memo = nullptr;
			}
		}
	}
	if (m_render_targets_reused && memo == nullptr) {
		// The draw acquires the memo's targets in place (see TryReuseRenderTargets()) and may
		// update them: the memo no longer describes the draw before.
		m_render_target_memo->valid = false;
	}
	const bool store =
	    memo == nullptr && m_render_target_memo != nullptr && m_render_target_memo->pending;
	std::array<vk::ImageView, RENDER_COLOR_ATTACHMENTS_MAX> color_views {};
	vk::ImageView                                           depth_view = nullptr;
	ImageId                                                 stencil_id {};
	RenderState state {};
	state.width                 = std::numeric_limits<uint32_t>::max();
	state.height                = std::numeric_limits<uint32_t>::max();
	state.num_layers            = std::numeric_limits<uint32_t>::max();
	state.num_color_attachments = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		auto& target = colors[i];
		EXIT_IF(!target.image_id);
		const auto image_view = memo != nullptr
		                            ? memo->color_views[i]
		                            : cache.FindRenderTarget(target.image_id, target.desc);
		color_views[i]        = image_view;
		auto&      image      = cache.GetImage(target.image_id);
		EXIT_IF(image.backing.samples != target.desc.info.samples || image_view == nullptr);
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
		state.num_color_attachments = std::max(state.num_color_attachments, target.target_slot + 1);
		auto& attachment            = state.color_attachments[target.target_slot];
		attachment.image_view   = image_view;
		attachment.image_layout = layout;
	}
	if (depth.image_id) {
		// A reused acquisition had no depth clear and no HTile clear to consume.
		const auto image_view =
		    memo != nullptr ? memo->depth_view
		                    : cache.FindDepthTarget(depth.image_id, depth.desc, &stencil_id);
		depth_view = image_view;
		if (memo == nullptr) {
			const auto& metadata = depth.desc.info.metadata;
			const bool meta_clear =
			    metadata.kind == ImageMetadataKind::Htile &&
			    cache.IsMetaCleared(metadata.range.address, depth.desc.view_info.base_layer);
			depth.load_clear_aspects = {};
			if (depth.depth_clear_enable || meta_clear)
				depth.load_clear_aspects |= vk::ImageAspectFlagBits::eDepth;
			// Apply pending clears to compressed stencil as well as depth.
			if (depth.stencil_clear_enable || (meta_clear && metadata.stencil_compressed))
				depth.load_clear_aspects |= vk::ImageAspectFlagBits::eStencil;
			if (meta_clear &&
			    !cache.TouchMeta(metadata.range.address, depth.desc.view_info.base_layer, false)) {
				EXIT("failed to consume HTile clear state\n");
			}
		}
		EXIT_IF(image_view == nullptr ||
		        cache.GetImage(depth.image_id).backing.samples != depth.desc.info.samples);
		const auto draw_writes = depth.AttachmentWriteAspects();
		const bool feedback_loops = m_context.GetGraphics().attachment_feedback_loop_enabled;
		vk::ImageAspectFlags sampled_aspects;
		for (auto* stage: stages) {
			for (auto& binding: stage->images) {
				if (binding.image_id != depth.image_id ||
				    binding.desc.type != TextureCache::BindingType::Texture) continue;
				const auto& views = cache.GetImage(depth.image_id).views;
				const auto  native =
				    std::ranges::find(views, binding.image_view, &CachedImageView::view);
				EXIT_IF(native == views.end());
				const auto info     = native->info;
				const auto feedback = DepthFeedbackAspects(draw_writes, depth.desc.view_info, info);
				if (feedback && !feedback_loops) {
					// The host cannot sample an attachment the draw writes: sample a copy of the
					// depth image from before the draw instead. This is a limit of hosts without
					// VK_EXT_attachment_feedback_loop_layout, not of the emulation.
					static std::atomic_bool logged = false;
					if (!logged.exchange(true, std::memory_order_relaxed)) {
						std::printf("depth attachment feedback loop is not supported by the host: "
						            "draws sample a pre-draw copy of the depth target\n");
					}
					std::vector<ImageViewInfo> mip_infos;
					for (const auto mip_view: binding.mip_views) {
						const auto found =
						    std::ranges::find(views, mip_view, &CachedImageView::view);
						EXIT_IF(found == views.end());
						mip_infos.push_back(found->info);
					}
					const auto copy_id = cache.CopyDepthForFeedback(
					    depth.image_id,
					    {info.base_level, info.level_count, info.base_layer, info.layer_count},
					    info.aspect);
					auto& copy         = cache.GetImage(copy_id);
					binding.image_id   = copy_id;
					binding.image_view = copy.FindView(info);
					for (size_t i = 0; i < mip_infos.size(); i++) {
						binding.mip_views[i] = copy.FindView(mip_infos[i]);
					}
					continue;
				}
				sampled_aspects |= info.aspect;
				feedback_aspects |= feedback;
			}
			// Bindless table elements sample the depth target the same way. The bindings
			// visit every depth element of a table (FinishTableBindings()), so the elements
			// below are all the table elements this draw can sample it through.
			const auto table_element = [&](const TextureBinding& binding) {
				if (binding.image_id != depth.image_id ||
				    binding.desc.type != TextureCache::BindingType::Texture) return;
				const auto& views = cache.GetImage(depth.image_id).views;
				const auto  native =
				    std::ranges::find(views, binding.image_view, &CachedImageView::view);
				EXIT_IF(native == views.end());
				const auto feedback =
				    DepthFeedbackAspects(draw_writes, depth.desc.view_info, native->info);
				// A table element is shared by the draws that bind the table: it cannot be
				// redirected to a pre-draw copy for this draw only. Without
				// VK_EXT_attachment_feedback_loop_layout the draw samples the attachment it writes in
				// the general layout, as upstream does: undefined on such hosts, but not an exit.
				if (feedback && !feedback_loops) {
					static std::atomic_bool logged = false;
					if (!logged.exchange(true, std::memory_order_relaxed)) {
						std::printf("depth attachment feedback loop is not supported by the host: a "
						            "bindless table element samples the depth target its draw writes\n");
					}
				}
				sampled_aspects |= native->info.aspect;
				feedback_aspects |= feedback;
			};
			if (stage->tables_reused) {
				const auto& table_state = *static_cast<const TableBindingState*>(stage->table_state);
				for (const auto index: stage->table_visits) {
					table_element(table_state.elements[index].resolution->texture);
				}
			} else {
				for (const auto* binding: stage->table_images) {
					table_element(*binding);
				}
			}
		}
		auto& image = cache.GetImage(depth.image_id);
		auto layout = depth_attachment_layout(depth);
		if (sampled_aspects & ~DepthReadableAspects(layout)) {
			layout = m_context.GetGraphics().attachment_feedback_loop_enabled
			             ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
			             : vk::ImageLayout::eGeneral;
		}
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
		attachment.depth_clear =
		    static_cast<bool>(depth.load_clear_aspects & vk::ImageAspectFlagBits::eDepth);
		attachment.has_stencil    = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eStencil);
		attachment.stencil_clear =
		    static_cast<bool>(depth.load_clear_aspects & vk::ImageAspectFlagBits::eStencil);
	}
	if (color_count == 0 && !depth.image_id) {
		const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
		state.width        = limits.maxFramebufferWidth;
		state.height       = limits.maxFramebufferHeight;
	}
	if (state.num_layers == std::numeric_limits<uint32_t>::max()) {
		state.num_layers = 1;
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.width == std::numeric_limits<uint32_t>::max() ||
	        state.height == std::numeric_limits<uint32_t>::max());
	if (store) {
		StoreRenderTargetMemo(colors, color_count, depth, color_views.data(), depth_view,
		                      stencil_id);
	}
	return state;
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
	if (mode == static_cast<uint8_t>(CbColorMode::EliminateFastClear)) {
		// FCE can precede any ordinary draw. Discover the target to materialize its native
		// clear metadata before the expanded image is sampled.
		RenderColorInfo target {};
		ResolveRenderColorTarget(buffer, target, 0, true);
		return true;
	}
	// Metadata/decompression shaders must not export colors into the expanded image.
	return mode == static_cast<uint8_t>(CbColorMode::FmaskDecompress) ||
	       mode == static_cast<uint8_t>(CbColorMode::DccDecompress);
}

// vertex_offset is the first vertex of non-indexed draws.
struct DrawEmitInfo {
	int32_t          vertex_offset  = 0;
	uint32_t         first_instance = 0;
	DrawIndirectArgs indirect;
};

struct DrawIndexBufferSource {
	uint64_t    address            = 0;
	const void* host_data          = nullptr;
	uint64_t    size               = 0;
	uint32_t    guest_element_size = 0;
};

// The task shader's inputs for an indirect mesh draw: Params in gpu_mesh_indirect.task.
struct MeshTaskParams {
	vk::DeviceAddress records        = 0;
	vk::DeviceAddress index_address  = 0;
	uint32_t          stride         = 0;
	uint32_t          index_size     = 0;
	uint32_t          index_limit    = 0;
	uint32_t          primitive_size = 0;
	uint32_t          group_step     = 0;
	uint32_t          max_groups     = 0;
	uint32_t          max_instances  = 0;
	uint32_t          max_total      = 0;
};

struct PreparedIndexBuffer {
	vk::Buffer     buffer = nullptr;
	vk::DeviceSize offset = 0;
	vk::IndexType  type   = vk::IndexType::eUint16;
};

static uint64_t VertexBufferDescriptorSize(int binding, const ShaderVertexInputInfo& info) {
	const auto& buffer = info.buffers[binding];
	if (buffer.stride != 0 || buffer.num_records == 0) {
		return static_cast<uint64_t>(buffer.stride) * buffer.num_records;
	}

	uint64_t size = 0;
	for (int i = 0; i < info.resources_num; i++) {
		if (info.resources_dst[i].buffer_index != binding) {
			continue;
		}
		const auto& resource = info.resources[i];
		// RDNA2 OOB_SELECT=2 only checks NumRecords != 0. A constant attribute still
		// fetches its entire format; NumRecords is not a byte count in this mode.
		const uint64_t extent = resource.OutOfBounds() == 2
		                            ? resource.Base48() - buffer.addr +
		                                  ShaderRecompiler::Format::GetFormatInfo(resource.Format()).byte_size
		                            : buffer.num_records;
		size = std::max(size, extent);
	}
	return size;
}

// Trivial: draws fill only the entries they use (no 3.5 KiB of zeroing per draw).
struct VertexBufferRange {
	uint64_t base_address;
	uint64_t requested_end;
	uint64_t acquired_end;
	Buffer*  buffer;
	uint64_t offset;

	[[nodiscard]] uint64_t RequestedSize() const { return requested_end - base_address; }
};

struct PreparedVertexBuffers {
	static constexpr uint32_t MaxBuffers = ShaderVertexInputInfo::RES_MAX;

	// Filled only up to count.
	std::array<vk::Buffer, MaxBuffers>     buffers;
	std::array<vk::DeviceSize, MaxBuffers> offsets;
	std::array<vk::DeviceSize, MaxBuffers> sizes;
	uint32_t                               count = 0;
};

static void AcquireVertexBuffers(CommandBuffer& buffer, const ShaderVertexInputInfo& vs_input_info,
                                 PreparedVertexBuffers& prepared) {
	EXIT_IF(vs_input_info.buffers_num < 0 ||
	        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX);

	// Collect the non-empty guest vertex ranges.
	std::array<uint64_t, ShaderVertexInputInfo::RES_MAX>          sizes;
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> ranges;
	uint32_t                                                      range_count = 0;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = VertexBufferDescriptorSize(i, vs_input_info);
		sizes[i]           = size;
		if (size == 0) {
			continue;
		}
		if (vertex.addr == 0 || size > UINT64_MAX - vertex.addr) {
			EXIT("invalid vertex buffer range: addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     vertex.addr, size);
		}
		ranges[range_count++] = {.base_address = vertex.addr, .requested_end = vertex.addr + size};
	}

	std::sort(ranges.begin(), ranges.begin() + range_count,
	          [](const VertexBufferRange& left, const VertexBufferRange& right) {
		          return left.base_address < right.base_address;
	          });

	// Merge overlapping or touching ranges in place before acquiring host buffers.
	uint32_t merged_count = 0;
	for (uint32_t i = 0; i < range_count; i++) {
		const auto& range = ranges[i];
		if (merged_count != 0 && ranges[merged_count - 1].requested_end >= range.base_address) {
			auto& merged         = ranges[merged_count - 1];
			merged.requested_end = std::max(merged.requested_end, range.requested_end);
			continue;
		}
		ranges[merged_count++] = range;
	}
	const auto merged_ranges = std::span(ranges).first(merged_count);

	auto& cache = buffer.GetContext().GetBufferCache();
	for (auto& range: merged_ranges) {
		// PPSA20298
		const auto size =
		    Libs::LibKernel::Memory::ClampRangeSize(range.base_address, range.RequestedSize());
		range.acquired_end = range.base_address + size;
		std::tie(range.buffer, range.offset) = cache.ObtainBuffer(range.base_address, size, false);
	}

	// Rebuild slot bindings, offsetting non-empty slots into their acquired merged range.
	prepared.count         = static_cast<uint32_t>(vs_input_info.buffers_num);
	vk::Buffer null_buffer = nullptr;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = sizes[i];
		if (size == 0) {
			if (null_buffer == nullptr) {
				null_buffer = cache.GetBuffer(NULL_BUFFER_ID).Handle();
			}
			prepared.buffers[i] = null_buffer;
			prepared.offsets[i] = 0;
			prepared.sizes[i]   = 0;
			continue;
		}

		const auto range = std::ranges::find_if(merged_ranges, [&](const VertexBufferRange& value) {
			return vertex.addr >= value.base_address && vertex.addr < value.acquired_end;
		});
		if (range == merged_ranges.end()) {
			EXIT("vertex buffer address is outside the acquired range: addr=0x%016" PRIx64 "\n",
			     vertex.addr);
		}

		prepared.buffers[i] = range->buffer->Handle();
		prepared.offsets[i] = range->offset + vertex.addr - range->base_address;
		prepared.sizes[i]   = std::min(size, range->acquired_end - vertex.addr);
	}
}

static bool GetDrawTopology(const HW::UserConfig& ucfg, vk::PrimitiveTopology& topology) {

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
		case Prospero::PrimitiveType::kPolygon:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		case Prospero::PrimitiveType::kTriStrip:
			topology = vk::PrimitiveTopology::eTriangleStrip;
			break;
		case Prospero::PrimitiveType::kPatch:
			if (!Config::TessellationEnabled()) return false;
			[[fallthrough]];
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kRectListLegacy:
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

RenderExecutor::ResolvedDraw* RenderExecutor::ResolveDrawAhead(
    const HW::Context& context, const HW::UserConfig& user_config, const HW::Shader& shaders,
    const ShaderIndirectOffsets& indirect_offsets, bool indirect_draw) {
	if (!DrawHasValidVertexShader(shaders)) {
		return nullptr;
	}
	if (m_resolved_ring == nullptr) {
		m_resolved_ring = {new ResolvedDrawRing {}, [](ResolvedDrawRing* ring) { delete ring; }};
	}
	auto& ring = *m_resolved_ring;
	auto& draw = ring.draws[ring.next];
	// The execution thread releases resolutions in order.
	for (Common::SpinWait wait; draw.busy.load(std::memory_order_acquire);) {
		if (!wait.Spin()) {
			std::this_thread::yield();
		}
	}
	ring.next           = (ring.next + 1) % ResolvedDrawRing::Size;
	draw.compute_module = nullptr;
	auto& state         = draw.storage.state;
	ResetDrawRenderState(state, draw.storage.tessellation_stages_written);
	draw.storage.tessellation_stages_written =
	    user_config.GetPrimType() == Prospero::PrimitiveType::kPatch;
	state.indirect_offsets = indirect_offsets;
	state.indirect_draw    = indirect_draw;
	draw.programs.Reset();
	if (!m_context.GetPipelineCache().GetGraphicsProgramsAhead(context, user_config, shaders, state,
	                                                          draw.programs) ||
	    draw.programs.reads.Failed() || state.vertex_info[0].stage.module == nullptr) {
		return nullptr;
	}
	// Published to the execution thread with the operation that carries it.
	draw.busy.store(true, std::memory_order_relaxed);
	return &draw;
}

RenderExecutor::ResolvedDraw* RenderExecutor::ResolveDispatchAhead(const HW::Context& context,
                                                                   const HW::Shader&  shaders,
                                                                   const uint32_t (&groups)[3],
                                                                   uint32_t mode,
                                                                   bool async_compute) {
	(void)context;
	const auto& cs_regs = shaders.GetCs();
	if (cs_regs.cs_regs.data_addr == 0) {
		return nullptr;
	}
	if (m_resolved_ring == nullptr) {
		m_resolved_ring = {new ResolvedDrawRing {}, [](ResolvedDrawRing* ring) { delete ring; }};
	}
	auto& ring = *m_resolved_ring;
	auto& slot = ring.draws[ring.next];
	// The execution thread releases resolutions in order.
	for (Common::SpinWait wait; slot.busy.load(std::memory_order_acquire);) {
		if (!wait.Spin()) {
			std::this_thread::yield();
		}
	}
	ring.next = (ring.next + 1) % ResolvedDrawRing::Size;
	slot.programs.Reset();
	ValueInitialize(slot.compute);
	slot.compute.async_compute = async_compute;
	// DISPATCH_INITIATOR.USE_THREAD_DIMENSIONS and the workgroup counts (see DispatchDirect()).
	slot.compute.dispatch_thread_dimensions = (mode & (1u << 5u)) != 0;
	const uint32_t group_sizes[] = {cs_regs.cs_regs.num_thread_x, cs_regs.cs_regs.num_thread_y,
	                                cs_regs.cs_regs.num_thread_z};
	for (uint32_t axis = 0; axis < 3u; ++axis) {
		const auto size = slot.compute.dispatch_thread_dimensions ? std::max(group_sizes[axis], 1u) : 1u;
		slot.compute.workgroup_counts[axis] = groups[axis] / size + (groups[axis] % size != 0u);
	}
	slot.compute_module =
	    m_context.GetPipelineCache().GetComputeProgramAhead(cs_regs, slot.compute, slot.programs);
	if (slot.programs.reads.Failed() || slot.compute_module == nullptr) {
		slot.compute_module = nullptr;
		return nullptr;
	}
	// Published to the execution thread with the operation that carries it.
	slot.busy.store(true, std::memory_order_relaxed);
	return &slot;
}

RenderExecutor::ResolvedDraw* RenderExecutor::ResolveIndirectDispatchAhead(
    const HW::Context& context, const HW::Shader& shaders, uint32_t mode, uint64_t args_addr,
    bool async_compute) {
	(void)context;
	const auto& cs_regs = shaders.GetCs();
	if (cs_regs.cs_regs.data_addr == 0) {
		return nullptr;
	}
	if (m_resolved_ring == nullptr) {
		m_resolved_ring = {new ResolvedDrawRing {}, [](ResolvedDrawRing* ring) { delete ring; }};
	}
	auto& ring = *m_resolved_ring;
	auto& slot = ring.draws[ring.next];
	// The execution thread releases resolutions in order.
	for (Common::SpinWait wait; slot.busy.load(std::memory_order_acquire);) {
		if (!wait.Spin()) {
			std::this_thread::yield();
		}
	}
	ring.next = (ring.next + 1) % ResolvedDrawRing::Size;
	slot.programs.Reset();
	ValueInitialize(slot.compute);
	slot.compute.async_compute = async_compute;
	// As DispatchIndirect() and DispatchIndirectThreads() look the program up: the workgroup
	// counts are unknown until the dispatch executes.
	if ((mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0) {
		slot.compute.dispatch_thread_dimensions = true;
		slot.compute.dispatch_indirect_threads  = true;
	} else {
		slot.compute.indirect_args_address = args_addr;
	}
	slot.compute_module =
	    m_context.GetPipelineCache().GetComputeProgramAhead(cs_regs, slot.compute, slot.programs);
	if (slot.programs.reads.Failed() || slot.compute_module == nullptr) {
		slot.compute_module = nullptr;
		return nullptr;
	}
	slot.busy.store(true, std::memory_order_relaxed);
	return &slot;
}

bool RenderExecutor::TakeResolvedDispatch(ShaderComputeInputInfo& input_info,
                                          const ShaderModule*&    module) {
	auto* resolved = std::exchange(m_resolved_draw, nullptr);
	if (resolved == nullptr || resolved->compute_module == nullptr ||
	    !resolved->programs.Revalidate(true)) {
		return false;
	}
	input_info = resolved->compute;
	module     = resolved->compute_module;
	return true;
}

static bool RecordsNoMipStats(const ShaderStageRuntime& stage);

bool RenderExecutor::PrepareDrawRenderState(CommandBuffer& buffer, bool shaders_resolved,
                                            const DrawCallInfo& draw,
                                            DrawRenderState&    state) {
	if (!shaders_resolved) {
		m_context.GetImageHeap().BeginDraw();
		m_context.GetPipelineCache().GetGraphicsPrograms(buffer, state);
	}
	// Vertex programs the host cannot run are not compiled.
	if (state.vertex_info[0].stage.module == nullptr) {
		return false;
	}
	// An empty screen scissor rasterizes nothing in any viewport, so only vertex-stage buffer
	// writes would remain (PPSA02110 ends mip reductions with a 0x0 pass past the last level).
	const auto& vp = buffer.GetRegisters().GetScreenViewport();
	if ((vp.screen_scissor_right <= vp.screen_scissor_left ||
	     vp.screen_scissor_bottom <= vp.screen_scissor_top) &&
	    std::ranges::none_of(state.VertexStages(), HasShaderBufferWrites,
	                         &ShaderVertexInputInfo::stage)) {
		return false;
	}
	const auto mrt_mask = state.mrt_mask;
	m_render_targets_reused = TryReuseRenderTargets(buffer.GetRegisters(), mrt_mask);
	if (!m_render_targets_reused) {
		m_draw_targets      = &m_targets;
		auto& targets       = m_targets;
		targets.color_count = 0;
		CaptureRenderTargetKey(buffer.GetRegisters(), mrt_mask);
		for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
			if ((mrt_mask & (1u << slot)) != 0) {
				ResolveRenderColorTarget(buffer, targets.colors[targets.color_count], slot);
				if (targets.colors[targets.color_count].image_id) {
					targets.color_count++;
				}
			}
		}
		ResolveRenderDepthTarget(buffer, targets.depth);
		// TODO(PPSA09243): Vulkan passes take one sample count. Where PS5 attachments mix counts,
		// keep those matching the raster count instead of resolving multisampled shadows.
		const auto colors       = std::span {targets.colors, targets.color_count};
		const auto pass_samples = targets.depth.image_id ? targets.depth.desc.info.samples
		                                                 : targets.colors[0].desc.info.samples;
		if (std::ranges::any_of(colors, [&](const RenderColorInfo& color) {
			    return color.desc.info.samples != pass_samples;
		    })) {
			const auto raster =
			    render_sample_count(buffer.GetRegisters().GetAaConfig().msaa_num_samples);
			const auto dropped = std::ranges::remove_if(colors, [&](const RenderColorInfo& color) {
				return color.desc.info.samples != raster;
			});
			targets.color_count -= static_cast<uint32_t>(dropped.size());
			if (targets.depth.image_id && targets.depth.desc.info.samples != raster) {
				targets.depth = {};
			}
			static std::atomic_bool logged = false;
			if (!logged.exchange(true, std::memory_order_relaxed)) {
				Log::WriteToConsoleAndLog(
				    "Warning: dropping attachments whose sample count differs from the raster.\n");
			}
		}
	}

	if (m_draw_targets->color_count == 0 && !m_draw_targets->depth.image_id && !state.ps_active) {
		LogFramebufferSkip(draw.Name(), m_draw_targets->depth, buffer, draw.index_count, 0);
		return false;
	}

	state.ps_plain = state.ps_active && RecordsNoMipStats(state.ps_input_info.stage);
	return true;
}

static PreparedIndexBuffer PrepareIndexBuffer(CommandBuffer&               buffer,
                                              const DrawIndexBufferSource& source) {
	PreparedIndexBuffer prepared;
	if (source.size == 0) {
		return prepared;
	}
	prepared.type =
	    source.guest_element_size == 4 ? vk::IndexType::eUint32 : vk::IndexType::eUint16;
	if (source.host_data != nullptr) {
		auto& cache     = buffer.GetContext().GetBufferCache();
		auto& stream    = cache.GetUtilityBuffer(MemoryUsage::Stream);
		prepared.offset = cache.StreamUpload(source.host_data, source.size, 16);
		prepared.buffer = stream.Handle();
	} else {
		auto [buffer_ptr, offset] =
		    buffer.GetContext().GetBufferCache().ObtainBuffer(source.address, source.size, false);
		prepared.buffer = buffer_ptr->Handle();
		prepared.offset = offset;
	}
	return prepared;
}

static void CommitVertexBuffers(CommandRecorder              vk_buffer,
                                const PreparedVertexBuffers& prepared) {
	if (prepared.count != 0) {
		// Guest descriptor bounds must survive allocation merging in the cache.
		vk_buffer.bindVertexBuffers2(0, prepared.count, prepared.buffers.data(),
		                             prepared.offsets.data(), prepared.sizes.data(), nullptr);
	}
}

static void CommitIndexBuffer(CommandRecorder vk_buffer, const PreparedIndexBuffer& prepared) {
	if (prepared.buffer == nullptr) {
		return;
	}
	vk_buffer.bindIndexBuffer(prepared.buffer, prepared.offset, prepared.type);
}

static void LogDrawStateIfNeeded(const CommandBuffer& buffer, const DrawCallInfo& draw,
                                 const DrawRenderState& state, const DrawTargets& targets,
                                 uint32_t index_type_and_size, const void* index_addr) {
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	if (!draw.indexed && !Prospero::IsRectList(buffer.GetUserConfig().GetPrimType())) {
		return;
	}

	static const RenderColorInfo none {};

	const auto& color = targets.color_count != 0 ? targets.colors[0] : none;
	if (state.ps_active) {
		LogDrawTargetState(draw.Name(), color, targets.depth, buffer, state.ps_input_info,
		                   draw.index_count, 0);
	}
	LogDrawInputState(buffer, color, state.vertex_info[0], index_type_and_size, draw.index_count,
	                  index_addr);
}

static void EmitDrawPrimitives(const HW::UserConfig& ucfg, CommandRecorder vk_buffer,
                               const DrawCallInfo& draw, const DrawEmitInfo& emit) {
	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kPointList:
		case Prospero::PrimitiveType::kLineList:
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriList:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip:
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kPolygon:
		case Prospero::PrimitiveType::kRectListLegacy:
		case Prospero::PrimitiveType::kPatch:
			if (draw.indexed) {
				vk_buffer.drawIndexed(draw.index_count, draw.instance_count, 0, emit.vertex_offset,
				                      emit.first_instance);
			} else {
				vk_buffer.draw(draw.index_count, draw.instance_count,
				               static_cast<uint32_t>(emit.vertex_offset), emit.first_instance);
			}
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			EXIT_NOT_IMPLEMENTED((draw.index_count & 0x3u) != 0);
			for (uint32_t i = 0; i < draw.index_count; i += 4) {
				if (draw.indexed) {
					vk_buffer.drawIndexed(4, draw.instance_count, i, emit.vertex_offset,
					                      emit.first_instance);
				} else {
					vk_buffer.draw(4, draw.instance_count,
					               i + static_cast<uint32_t>(emit.vertex_offset),
					               emit.first_instance);
				}
			}
			break;
		default: EXIT("unknown primitive type: %u\n", static_cast<uint32_t>(ucfg.GetPrimType()));
	}
}

bool RenderExecutor::CopyDepthToColor(CommandBuffer& buffer, const DrawCallInfo& draw,
                                      const DrawEmitInfo& emit, const DrawRenderState& state) {
	if (!buffer.GetRegisters().GetRenderControl().copy_depth_to_color) {
		return false;
	}
	auto& ucfg = buffer.GetUserConfig();
	const auto& hw = buffer.GetRegisters();
	auto&       depth = m_draw_targets->depth;
	auto&       color = m_draw_targets->colors[0];
	EXIT_NOT_IMPLEMENTED(
	    !m_context.GetGraphics().maintenance8_enabled ||
	    !Prospero::IsRectList(ucfg.GetPrimType()) || draw.indexed || draw.index_count != 3 ||
	    draw.instance_count != 1 || emit.vertex_offset != 0 || emit.first_instance != 0 ||
	    m_draw_targets->color_count != 1 || color.target_slot != 0 || !depth.image_id ||
	    color.desc.info.samples != 1 || depth.desc.info.extent.width != color.Extent().width ||
	    depth.desc.info.extent.height != color.Extent().height ||
	    depth.desc.view_info.layer_count != 1 || color.desc.view_info.layer_count != 1 ||
	    hw.GetBlendControl(0).enable || state.ps_input_info.ps_pixel_kill_enable ||
	    state.ps_input_info.ps_depth_export_enable ||
	    state.ps_input_info.ps_sample_mask_export_enable);
	vk::ImageAspectFlags feedback;
	const auto rendering = AcquireRenderTargets(buffer, &color, 1, depth, feedback);
	if (depth.load_clear_aspects) {
		buffer.BeginRendering(rendering);
	}
	auto& cache = m_context.GetTextureCache();
	auto& source = cache.GetImage(depth.image_id);
	auto& destination = cache.GetImage(color.image_id);
	EXIT_NOT_IMPLEMENTED((source.backing.format != vk::Format::eD32Sfloat &&
	                      source.backing.format != vk::Format::eD32SfloatS8Uint) ||
	                     destination.backing.format != vk::Format::eR32Sfloat);
	const auto rect = calc_final_scissor(hw.GetScreenViewport(), hw.GetScanModeControl(),
	                                    {rendering.width, rendering.height}, 0);
	if (rect.right > rect.left && rect.bottom > rect.top) {
		const vk::ImageCopy region {
		    {vk::ImageAspectFlagBits::eDepth, depth.desc.view_info.base_level,
		     depth.desc.view_info.base_layer, 1}, {rect.left, rect.top, 0},
		    {vk::ImageAspectFlagBits::eColor, color.desc.view_info.base_level,
		     color.desc.view_info.base_layer, 1}, {rect.left, rect.top, 0},
		    {static_cast<uint32_t>(rect.right - rect.left),
		     static_cast<uint32_t>(rect.bottom - rect.top), 1}};
		buffer.EndRendering();
		const auto command = buffer.Handle();
		source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
		               {}, command);
		destination.Transit(vk::ImageLayout::eTransferDstOptimal,
		                    vk::AccessFlagBits2::eTransferWrite, {}, command);
		command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal,
		                  destination.backing.image, vk::ImageLayout::eTransferDstOptimal,
		                  1, &region);
	}
	return true;
}

// GET_LOD_STATS: whether a pixel program instrumented for mip statistics records nothing in this
// draw, as no image whose T# the draw supplies has a counter. Its plain variant then renders the
// same without the storage buffer atomics, which make the driver run the depth and stencil tests
// after a shader that can discard.
static bool RecordsNoMipStats(const ShaderStageRuntime& stage) {
	if (stage.module == nullptr || stage.module->plain == nullptr || stage.resources == nullptr) {
		return false;
	}
	const auto& images = stage.module->info.info.images;
	const auto& values = stage.resources->images;
	for (uint32_t i = 0; i < images.size() && i < values.size(); i++) {
		if (!images[i].heap && images[i].table_capacity == 0u && values[i].dword_count >= 8u &&
		    LodStatsReport::HasCounter(values[i].dwords.data())) {
			return false;
		}
	}
	return true;
}

// Whether a draw may be skipped while the driver compiles its pipeline: the picture is all it
// changes. Its shaders write no memory, its samples reach no guest occlusion counter and it
// renders to no linear image (the CPU reads those back, e.g. GTA V's vehicle damage).
static bool DrawOnlyChangesPicture(RenderContext& context, const DrawRenderState& state,
                                   const DrawTargets&                     targets,
                                   std::span<const ShaderVertexInputInfo> vertex_stages) {
	const auto writes_memory = [](const ShaderStageRuntime& stage) {
		if (stage.module == nullptr) {
			return true;
		}
		const auto& program = stage.module->info;
		return program.has_address_writes ||
		       std::ranges::any_of(program.info.buffers,
		                           [](const auto& buffer) { return buffer.written; }) ||
		       std::ranges::any_of(program.info.images,
		                           [](const auto& image) { return image.written; });
	};
	if (context.GetOcclusionQueries().Counting()) {
		return false;
	}
	for (const auto& stage: vertex_stages) {
		if (writes_memory(stage.stage)) {
			return false;
		}
	}
	if (state.ps_active && writes_memory(state.ps_input_info.stage)) {
		return false;
	}
	for (uint32_t i = 0; i < targets.color_count; i++) {
		if (!targets.colors[i].desc.info.IsTiled()) {
			return false;
		}
	}
	return !static_cast<bool>(targets.depth.image_id) || targets.depth.desc.info.IsTiled();
}

void RenderExecutor::ExecutePreparedDraw(CommandBuffer& buffer, const DrawCallInfo& draw,
                                         const DrawRenderState& state, const DrawEmitInfo& emit,
                                         const DrawIndexBufferSource& index_source) {
	auto&      ucfg                     = buffer.GetUserConfig();
	const bool primitive_restart_enable = state.primitive_restart_enable;
	if (CopyDepthToColor(buffer, draw, emit, state)) {
		return;
	}
	const auto  vertex_stages = state.VertexStages();
	const bool  mesh_active   = state.vertex_info[0].stage.module->info.stage == ShaderType::Mesh;
	const bool  mesh_task     = state.vertex_info[0].stage.module->mesh_task;
	const auto& mesh          = state.vertex_info[0].mesh;
	const auto& limits        = m_context.GetGraphics().mesh_shader_properties;
	uint32_t    mesh_groups   = 0;
	if (mesh_active) {
		EXIT_NOT_IMPLEMENTED(mesh.fast_launch && (draw.indexed || primitive_restart_enable));
		static std::atomic_bool restart_warned = false;
		if (primitive_restart_enable && !restart_warned.exchange(true, std::memory_order_relaxed)) {
			std::printf("Warning: primitive restart is not implemented for mesh shaders; "
			            "continuing draw (primitive=%u indexed=%u)\n",
			            static_cast<uint32_t>(ucfg.GetPrimType()), draw.indexed);
		}
		if (mesh.primitives_per_group == 0) {
			EXIT("unsupported mesh draw: primitive=%u indexed=%u restart=%u\n",
			     static_cast<uint32_t>(ucfg.GetPrimType()), draw.indexed, primitive_restart_enable);
		}
	}
	if (mesh_task) {
		EXIT_NOT_IMPLEMENTED(emit.indirect.count > std::min(limits.maxTaskWorkGroupCount[0],
		                                                    limits.maxTaskWorkGroupTotalCount));
	} else if (mesh_active) {
		const auto primitives = mesh.InputPrimitiveCount(draw.index_count);
		if (primitives == 0 || draw.instance_count == 0) {
			return;
		}
		mesh_groups = (primitives - 1u) / mesh.primitives_per_group + 1u;
		if (mesh_groups > limits.maxMeshWorkGroupCount[0] ||
		    draw.instance_count > limits.maxMeshWorkGroupCount[1] ||
		    static_cast<uint64_t>(mesh_groups) * draw.instance_count >
		        limits.maxMeshWorkGroupTotalCount) {
			EXIT("mesh draw exceeds host workgroup limits: %ux%u\n", mesh_groups,
			     draw.instance_count);
		}
	}

	auto* const pipeline_ptr = m_context.GetPipelineCache().GetGraphicsPipeline(
	    buffer, state, *m_draw_targets,
	    Config::AsyncPipelinesEnabled() &&
	        DrawOnlyChangesPicture(m_context, state, *m_draw_targets, vertex_stages) &&
	        !SmallColorTargetDraw(buffer.GetRegisters()));
	if (pipeline_ptr == nullptr) {
		// Skipped until a background thread created the pipeline.
		return;
	}
	auto& pipeline = *pipeline_ptr;

	// Mesh shaders read the original guest indices in whole DWORDs by device address. Their buffer
	// exists before the descriptors bind, so creating it merges no buffer this draw binds.
	uint64_t index_begin = 0;
	uint64_t index_end   = 0;
	if (mesh_active && draw.indexed) {
		const auto size = static_cast<uint64_t>(draw.index_count) * index_source.guest_element_size;
		index_begin     = index_source.address & ~uint64_t {3};
		index_end       = (index_source.address + size + 3u) & ~uint64_t {3};
		(void)m_context.GetBufferCache().FindBuffer(index_begin, index_end - index_begin);
	}
	auto&                            bindings = m_graphics_bindings;
	std::array<PreparedBindings*, 4> descriptor_stages {};
	uint32_t                         stage_count = 0;
	for (uint32_t i = 0; i < vertex_stages.size(); i++) {
		PrepareBindings(state.vertex_info[i].stage, bindings.vertex[i]);
		descriptor_stages[stage_count++] = &bindings.vertex[i];
	}
	if (state.ps_active) {
		PrepareBindings(state.ps_input_info.stage, bindings.pixel);
		descriptor_stages[stage_count++] = &bindings.pixel;
	}
	const auto stages = std::span {descriptor_stages.data(), stage_count};
	PrepareGraphicsBindings(stages, std::span {m_draw_targets->colors, m_draw_targets->color_count});
	PreparedVertexBuffers vertex_bindings;
	PreparedIndexBuffer   index_binding;
	vk::DeviceAddress     mesh_indices = 0;
	if (!mesh_active) {
		AcquireVertexBuffers(buffer, state.vertex_info[0], vertex_bindings);
		index_binding = PrepareIndexBuffer(buffer, index_source);
	} else if (draw.indexed) {
		const auto [indices, offset] =
		    m_context.GetBufferCache().ObtainBuffer(index_begin, index_end - index_begin, false);
		mesh_indices = indices->BufferDeviceAddress() + offset + (index_source.address & 3u);
	}
	std::pair<Buffer*, uint64_t> args_buffer {};
	std::pair<Buffer*, uint64_t> count_buffer {};
	if (emit.indirect.address != 0) {
		const auto size = static_cast<uint64_t>(emit.indirect.count - 1) * emit.indirect.stride +
		                  (draw.indexed ? sizeof(vk::DrawIndexedIndirectCommand)
		                                : sizeof(vk::DrawIndirectCommand));
		args_buffer = m_context.GetBufferCache().ObtainBuffer(emit.indirect.address, size, false);
		EXIT_IF((args_buffer.second & 3u) != 0);
		if (emit.indirect.count_address != 0) {
			count_buffer = m_context.GetBufferCache().ObtainBuffer(emit.indirect.count_address,
			                                                       sizeof(uint32_t), false);
			EXIT_IF(count_buffer.first == nullptr || (count_buffer.second & 3u) != 0);
		}
	}
	vk::ImageAspectFlags feedback_aspects;
	const auto rendering = AcquireRenderTargets(buffer, m_draw_targets->colors, m_draw_targets->color_count,
	                                            m_draw_targets->depth, feedback_aspects, stages);
	vk::DeviceAddress task_params = 0;
	if (mesh_task) {
		const MeshTaskParams params {
		    .records        = args_buffer.first->BufferDeviceAddress() + args_buffer.second,
		    .index_address  = mesh_indices,
		    .stride         = emit.indirect.stride,
		    .index_size     = index_source.guest_element_size,
		    .index_limit    = draw.index_count,
		    .primitive_size = mesh.InputPrimitiveSize(),
		    .group_step     = mesh.primitives_per_group * mesh.InputPrimitiveStep(),
		    .max_groups     = limits.maxMeshWorkGroupCount[0],
		    .max_instances  = limits.maxMeshWorkGroupCount[1],
		    .max_total      = limits.maxMeshWorkGroupTotalCount,
		};
		auto& stream = m_context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
		task_params  = stream.BufferDeviceAddress() + stream.Copy(&params, sizeof(params), 8);
	}

	// Occlusion counters of the command buffer under the ranges the draw reads.
	m_context.GetOcclusionQueries().WriteForReaders();
	// Resource preparation above may synchronously finish and restart the scheduler. From this
	// point onward, every operation targets the current command buffer and cannot touch guest
	// memory.
	auto vk_buffer = buffer.Handle();
	if (!mesh_active) {
		CommitVertexBuffers(vk_buffer, vertex_bindings);
	}
	CommitBindings(buffer, vk::PipelineBindPoint::eGraphics, pipeline, stages);
	if (!mesh_active) {
		CommitIndexBuffer(vk_buffer, index_binding);
	}
	using ShaderRecompiler::IR::PushData;
	const auto draw_dwords =
	    PushData::GraphicsDrawDwords(mesh_active, state.ps_input_info.object_primitive_id);
	if (draw_dwords != 0) {
		std::array<uint32_t, PushData::MeshDrawDwordCount> draw_data {};
		if (mesh_task) {
			draw_data[0] = static_cast<uint32_t>(task_params);
			draw_data[1] = static_cast<uint32_t>(task_params >> 32u);
		} else if (mesh_active) {
			draw_data = {draw.index_count,
			             static_cast<uint32_t>(emit.vertex_offset),
			             index_source.guest_element_size,
			             static_cast<uint32_t>(mesh_indices),
			             static_cast<uint32_t>(mesh_indices >> 32u)};
		} else {
			draw_data[0] = buffer.GetUserConfig().GetObjectId();
		}
		vk_buffer.pushConstants(pipeline.pipeline_layout, pipeline.push_stages, 0,
		                        draw_dwords * sizeof(uint32_t), draw_data.data());
	}
	if (const auto planes = vertex_stages.back().user_clip_planes; planes != 0) {
		vk_buffer.pushConstants(pipeline.pipeline_layout, pipeline.push_stages,
		                        PushData::UserClipPlaneDword * sizeof(uint32_t),
		                        std::bit_width(planes) * 4u * sizeof(uint32_t),
		                        buffer.GetRegisters().GetUserClipPlanes().data());
	}

	auto& dynamic_state = m_context.GetCommandScheduler().GetDynamicState();
	UpdateGraphicsDynamicState(buffer, dynamic_state, vertex_stages.back(), m_draw_targets->depth,
	                           rendering);
	dynamic_state.SetAttachmentFeedbackLoopAspects(feedback_aspects);
	dynamic_state.Commit(m_context.GetGraphics(), vk_buffer);

	vk::PipelineStageFlags shader_write_stages = {};
	for (const auto& stage: vertex_stages) {
		if (HasShaderBufferWrites(stage.stage)) {
			shader_write_stages |= ShaderPipelineStages(NativeShaderStage(stage.logical_stage));
		}
	}
	if (state.ps_active && HasShaderBufferWrites(state.ps_input_info.stage)) {
		shader_write_stages |= vk::PipelineStageFlagBits::eFragmentShader;
	}
	if (shader_write_stages) {
		// As for dispatches, buffer writes wait for earlier accesses to the memory.
		buffer.EndRendering();
		ShaderWriteHazardBarrier(vk_buffer, shader_write_stages);
	}
	buffer.BeginRendering(rendering);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
	if (mesh_task) {
		vk_buffer.drawMeshTasksEXT(emit.indirect.count, 1, 1);
	} else if (mesh_active) {
		vk_buffer.drawMeshTasksEXT(mesh_groups, draw.instance_count, 1);
	} else if (args_buffer.first == nullptr) {
		EmitDrawPrimitives(ucfg, vk_buffer, draw, emit);
	} else if (count_buffer.first != nullptr) {
		if (draw.indexed) {
			vk_buffer.drawIndexedIndirectCount(args_buffer.first->Handle(), args_buffer.second,
			                                   count_buffer.first->Handle(), count_buffer.second,
			                                   emit.indirect.count, emit.indirect.stride);
		} else {
			vk_buffer.drawIndirectCount(args_buffer.first->Handle(), args_buffer.second,
			                            count_buffer.first->Handle(), count_buffer.second,
			                            emit.indirect.count, emit.indirect.stride);
		}
	} else if (draw.indexed) {
		vk_buffer.drawIndexedIndirect(args_buffer.first->Handle(), args_buffer.second,
		                              emit.indirect.count, emit.indirect.stride);
	} else {
		vk_buffer.drawIndirect(args_buffer.first->Handle(), args_buffer.second,
		                       emit.indirect.count, emit.indirect.stride);
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::Draws);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::DrawInstances, draw.instance_count);

	if (shader_write_stages) {
		buffer.EndRendering();
		ShaderWriteBarrier(vk_buffer, shader_write_stages);
	}
}

// Vulkan offsets for a prepared draw, or nullopt when its indirect records need the CPU:
// quad-list draws for counts, tessellation draws for the index/SGPR prolog and mesh draws without
// room for the task payload. Indirect records carry their own offsets.
static std::optional<DrawEmitInfo> ResolveDrawEmit(const HW::UserConfig&        ucfg,
                                                   const ShaderVertexInputInfo& vs,
                                                   DrawOffsetSource             source,
                                                   int32_t                      first_vertex,
                                                   uint32_t                     first_instance,
                                                   const DrawIndirectArgs&      indirect) {
	if ((indirect.address != 0 || source == DrawOffsetSource::IndirectArgs) &&
	    ucfg.GetIndexOffset() != 0) {
		static std::once_flag warning_once;
		std::call_once(warning_once, [&] {
			Log::WriteToConsoleAndLog(fmt::format(
			    "GPU: indirect draw ignores GE_INDX_OFFSET=0x{:08x}; geometry may be incorrect.\n",
			    ucfg.GetIndexOffset()));
		});
	}
	if (indirect.address != 0) {
		if (!vs.indirect_draw ||
		    ucfg.GetPrimType() == Prospero::PrimitiveType::kQuadListLegacy ||
		    vs.indirect_offsets != indirect.offsets) {
			return std::nullopt;
		}
		return DrawEmitInfo {.indirect = indirect};
	}
	if (source == DrawOffsetSource::IndirectArgs) {
		// Native fetches consume the patched SGPRs; rewritten fetches need Vulkan offsets.
		return vs.has_dynamic_vertex_fetch ? DrawEmitInfo {first_vertex, first_instance} : DrawEmitInfo {};
	}
	const auto [vertex_offset, instance_offset] = ResolveDrawOffsets(ucfg.GetIndexOffset(), vs);
	return DrawEmitInfo {vertex_offset + first_vertex, instance_offset};
}

bool RenderExecutor::DrawIndex(CommandBuffer& buffer, const DrawIndexArgs& args) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	m_context.GetCommandScheduler().PopPendingOperations();
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	Common::LockGuard lock(m_context.GetMutex());
	if (args.index_count == 0 || (args.instance_count == 0 && args.indirect.address == 0)) {
		return true;
	}

	if (ConsumeMetadataColorOperation(buffer) || DepthStencilCopy(buffer) ||
	    ResolveColorTargets(buffer)) {
		InvalidateRenderTargetMemo();
		ResetBindings();
		return true;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return true;
	}

	if (graphics_debug_dump_enabled()) {
		LOGF("GraphicsRenderDrawIndex():Shader:\n");
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

	// A pure check of the context registers: once per register snapshot.
	if (m_context_id == 0 || m_context_id != m_checked_context_id ||
	    graphics_debug_dump_enabled()) {
		hw_check(buffer);
		m_checked_context_id = m_context_id;
	}

	bool  shaders_resolved = false;
	auto& state = AcquireDrawRenderState(ucfg.GetPrimType() == Prospero::PrimitiveType::kPatch,
	                                     shaders_resolved, args.indirect.offsets,
	                                     args.indirect.address != 0);
	state.indirect_offsets = args.indirect.offsets;
	state.indirect_draw    = args.indirect.address != 0;
	if (!GetDrawTopology(ucfg, state.topology)) {
		return true;
	}

	DrawIndexBufferSource index_source {};
	index_source.address = reinterpret_cast<uint64_t>(args.index_addr);
	switch (static_cast<Prospero::IndexType>(args.index_type_and_size)) {
		case Prospero::IndexType::kIndex16: index_source.guest_element_size = 2; break;
		case Prospero::IndexType::kIndex32: index_source.guest_element_size = 4; break;
		case Prospero::IndexType::kIndex8: index_source.guest_element_size = 1; break;
		default: EXIT("unknown index_type_and_size: %u\n", args.index_type_and_size);
	}
	index_source.size = static_cast<uint64_t>(args.index_count) * index_source.guest_element_size;
	state.primitive_restart_enable = ResolvePrimitiveRestart(buffer, index_source);

	std::vector<uint16_t> expanded_indices;
	if (index_source.guest_element_size == 1) {
		EXIT_NOT_IMPLEMENTED(args.index_addr == nullptr);
		const auto* src = static_cast<const uint8_t*>(args.index_addr);
		expanded_indices.resize(args.index_count);
		for (uint32_t i = 0; i < args.index_count; i++) {
			expanded_indices[i] =
			    state.primitive_restart_enable && src[i] == 0xffu ? 0xffffu : src[i];
		}
		index_source.host_data = expanded_indices.data();
		index_source.size      = expanded_indices.size() * sizeof(uint16_t);
	}

	const DrawCallInfo draw {true, args.index_count, args.instance_count};
	if (!PrepareDrawRenderState(buffer, shaders_resolved, draw, state)) {
		ResetBindings();
		return true;
	}

	LogDrawStateIfNeeded(buffer, draw, state, *m_draw_targets, args.index_type_and_size, args.index_addr);
	const auto emit = ResolveDrawEmit(ucfg, state.vertex_info[0], args.offset_source,
	                                  args.base_vertex, args.first_instance, args.indirect);
	if (emit) {
		ExecutePreparedDraw(buffer, draw, state, *emit, index_source);
	}
	ResetBindings();
	return emit.has_value();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool RenderExecutor::DrawAuto(CommandBuffer& buffer, const DrawAutoArgs& args) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	m_context.GetCommandScheduler().PopPendingOperations();
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	Common::LockGuard lock(m_context.GetMutex());
	if (args.indirect.address == 0 && (args.vertex_count == 0 || args.instance_count == 0)) {
		return true;
	}

	if (ConsumeMetadataColorOperation(buffer) || DepthStencilCopy(buffer) ||
	    ResolveColorTargets(buffer)) {
		InvalidateRenderTargetMemo();
		ResetBindings();
		return true;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return true;
	}

	if (graphics_debug_dump_enabled()) {
		LOGF("GraphicsRenderDrawIndexAuto():Shader:\n");
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

	// A pure check of the context registers: once per register snapshot.
	if (m_context_id == 0 || m_context_id != m_checked_context_id ||
	    graphics_debug_dump_enabled()) {
		hw_check(buffer);
		m_checked_context_id = m_context_id;
	}

	const DrawCallInfo draw {false, args.vertex_count, args.instance_count};

	bool  shaders_resolved = false;
	auto& state = AcquireDrawRenderState(ucfg.GetPrimType() == Prospero::PrimitiveType::kPatch,
	                                     shaders_resolved, args.indirect.offsets,
	                                     args.indirect.address != 0);
	state.indirect_offsets = args.indirect.offsets;
	state.indirect_draw    = args.indirect.address != 0;
	if (!GetDrawTopology(ucfg, state.topology)) {
		ResetBindings();
		return true;
	}
	if (!PrepareDrawRenderState(buffer, shaders_resolved, draw, state)) {
		ResetBindings();
		return true;
	}

	const bool rect_list = Prospero::IsRectList(ucfg.GetPrimType());
	if (rect_list && state.vertex_info[0].buffers_num == 0 &&
	    state.vertex_info[0].stage.module->info.param_export_mask == 0 &&
	    state.ps_input_info.input_num != 0) {
		if (graphics_debug_dump_enabled()) {
			LOGF("DrawIndexAuto: skipping rect-list draw with no VS param exports and PS inputs: "
			     "ps_inputs=%u ps=0x%016" PRIx64 " es=0x%016" PRIx64 " gs=0x%016" PRIx64 "\n",
			     state.ps_input_info.input_num, sh_ctx.GetPs().ps_regs.data_addr,
			     sh_ctx.GetVs().es_regs.data_addr, sh_ctx.GetVs().gs_regs.data_addr);
		}
		ResetBindings();
		return true;
	}

	LogDrawStateIfNeeded(buffer, draw, state, *m_draw_targets, 0, nullptr);
	const auto emit = ResolveDrawEmit(ucfg, state.vertex_info[0], args.offset_source,
	                                  static_cast<int32_t>(args.first_vertex), args.first_instance,
	                                  args.indirect);
	if (emit) {
		ExecutePreparedDraw(buffer, draw, state, *emit, {});
	}
	ResetBindings();
	return emit.has_value();
}

bool RenderExecutor::ResolveColorTargets(CommandBuffer& buffer) {
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
	ResolveRenderColorTarget(buffer, src, 0, true, true);
	ResolveRenderColorTarget(buffer, dst, 1, true, true);
	if (src.desc.info.data.address == dst.desc.info.data.address &&
	    src.guest_mip_level == dst.guest_mip_level &&
	    src.guest_array_layer == dst.guest_array_layer) {
		return true;
	}

	auto& cache = m_context.GetTextureCache();
	cache.MarkGpuWritten(dst.image_id);
	auto& source      = cache.GetImage(src.image_id);
	auto& destination = cache.GetImage(dst.image_id);
	destination.Resolve(source,
	                    {src.desc.view_info.base_level, 1, src.desc.view_info.base_layer, 1},
	                    {dst.desc.view_info.base_level, 1, dst.desc.view_info.base_layer, 1});
	return true;
}

} // namespace Libs::Graphics
