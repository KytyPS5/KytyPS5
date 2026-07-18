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
#include "graphics/host_gpu/objects/label.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/descriptorCache.h"
#include "graphics/host_gpu/renderer/framebufferCache.h"
#include "graphics/host_gpu/renderer/pipelineCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderDraw.h"
#include "graphics/host_gpu/renderer/renderInternal.h"
#include "graphics/host_gpu/renderer/renderVertex.h"
#include "graphics/host_gpu/renderer/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/shaderSubgroup.h"
#include "graphics/host_gpu/utils.h"
#include "graphics/shader/recompiler/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ShaderIR.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <unordered_map>
#include <vector>
#if defined(__linux__)
inline const char* string_VkResult(VkResult result) {
	switch (result) {
		case VK_SUCCESS: return "VK_SUCCESS";
		case VK_NOT_READY: return "VK_NOT_READY";
		case VK_TIMEOUT: return "VK_TIMEOUT";
		case VK_EVENT_SET: return "VK_EVENT_SET";
		case VK_EVENT_RESET: return "VK_EVENT_RESET";
		case VK_INCOMPLETE: return "VK_INCOMPLETE";
		case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
		case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
		case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
		case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
		default: return "VK_UNKNOWN_RESULT";
	}
}
inline const char* string_VkFormat(VkFormat format) {
	switch (format) {
		case VK_FORMAT_UNDEFINED: return "VK_FORMAT_UNDEFINED";
		case VK_FORMAT_R8G8B8A8_UNORM: return "VK_FORMAT_R8G8B8A8_UNORM";
		case VK_FORMAT_B8G8R8A8_UNORM: return "VK_FORMAT_B8G8R8A8_UNORM";
		case VK_FORMAT_R32G32B32A32_SFLOAT: return "VK_FORMAT_R32G32B32A32_SFLOAT";
		default: return "VK_UNKNOWN_FORMAT";
	}
}
#else
#include <vulkan/vk_enum_string_helper.h>
#endif

// IWYU pragma: no_forward_declare VkImageView_T

namespace Libs::Graphics {

static std::atomic<uint32_t> g_draw_state_log_count       = 0;
static std::atomic<uint32_t> g_draw_input_log_count       = 0;
static std::atomic<uint32_t> g_mrt_state_log_count        = 0;
static std::atomic<uint32_t> g_shader_stage_log_count     = 0;
static std::atomic<uint32_t> g_framebuffer_skip_log_count = 0;

static bool ResolveColorTargets(uint64_t submit_id, CommandBuffer* buffer, const HW::Context& hw,
                                uint32_t render_target_slice_offset);

static const char* RenderColorTypeName(RenderColorType type) {
	switch (type) {
		case RenderColorType::NoColorOutput: return "NoColorOutput";
		case RenderColorType::DisplayBuffer: return "DisplayBuffer";
		case RenderColorType::RenderTexture: return "RenderTexture";
		default: return "Unknown";
	}
}

static bool IsDualSourceBlendFactor(uint32_t factor) {
	return factor >= 0x0fu && factor <= 0x12u;
}

static void LogFramebufferSkip(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const HW::Context* ctx,
                               const HW::UserConfig* ucfg, uint32_t index_count, uint32_t flags) {
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	auto log_id = g_framebuffer_skip_log_count.fetch_add(1, std::memory_order_relaxed);
	if (log_id >= 128) {
		return;
	}

	LOGF(
	    "DrawFramebufferSkip[%u]: %s color=%s color_addr=0x%010" PRIx64 " color_size=0x%016" PRIx64
	    " color_image=%s depth_format=%s depth_image=%s depth_vaddr_num=%d target_mask=0x%08" PRIx32
	    " prim=%u index_count=%u flags=0x%08" PRIx32 "\n",
	    log_id, draw_name, RenderColorTypeName(color.type), color.base_addr, color.buffer_size,
	    color.vulkan_buffer != nullptr ? "yes" : "no", string_VkFormat(depth.format),
	    depth.vulkan_buffer != nullptr ? "yes" : "no", depth.vaddr_num,
	    ctx != nullptr ? ctx->GetRenderTargetMask() : 0, ucfg != nullptr ? ucfg->GetPrimType() : 0,
	    index_count, flags);
}

static void LogMrtState(const char* draw_name, const HW::Context* ctx,
                        const ShaderPixelInputInfo& ps_input_info) {
	EXIT_IF(ctx == nullptr);

	const auto& sh_regs        = ctx->GetShaderRegisters();
	const auto  rt_mask        = ctx->GetRenderTargetMask();
	const auto  cb_shader_mask = sh_regs.m_cbShaderMask;
	const auto& bc0            = ctx->GetBlendControl(0);

	bool interesting = rt_mask != 0x0f || (cb_shader_mask & ~0x0fu) != 0 ||
	                   IsDualSourceBlendFactor(bc0.color_srcblend) ||
	                   IsDualSourceBlendFactor(bc0.color_destblend) ||
	                   (bc0.separate_alpha_blend && (IsDualSourceBlendFactor(bc0.alpha_srcblend) ||
	                                                 IsDualSourceBlendFactor(bc0.alpha_destblend)));

	for (uint32_t i = 1; i < 8; i++) {
		const auto& rt = ctx->GetRenderTarget(i);
		if (rt.base.addr != 0 || ps_input_info.target_output_mode[i] != 0 ||
		    ((rt_mask >> (i * 4u)) & 0x0fu) != 0 || ((cb_shader_mask >> (i * 4u)) & 0x0fu) != 0) {
			interesting = true;
		}
	}

	if (!interesting) {
		return;
	}

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
		const auto& rt  = ctx->GetRenderTarget(i);
		const auto& bc  = ctx->GetBlendControl(i);
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
		     log_id, i, rt.base.addr, ctm, csm, ps_input_info.target_output_mode[i], rt.info.format,
		     rt.info.channel_type, rt.info.channel_order, rt.attrib2.width + 1,
		     rt.attrib2.height + 1, rt.attrib3.tile_mode, bc.enable ? "true" : "false",
		     bc.color_srcblend, bc.color_destblend, bc.alpha_srcblend, bc.alpha_destblend);
	}
}

static void LogDrawTargetState(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const HW::Context* ctx,
                               const HW::UserConfig*       ucfg,
                               const ShaderPixelInputInfo& ps_input_info, uint32_t index_count,
                               uint32_t flags) {
	if (color.type == RenderColorType::NoColorOutput) {
		return;
	}

	auto log_id = g_draw_state_log_count.fetch_add(1);
	if (log_id >= 192 && color.type != RenderColorType::DisplayBuffer) {
		return;
	}

	EXIT_IF(ctx == nullptr);
	EXIT_IF(ucfg == nullptr);

	const auto& cc             = ctx->GetColorControl();
	const auto& bc             = ctx->GetBlendControl(color.target_slot);
	const auto& dc             = ctx->GetDepthControl();
	const auto& vp             = ctx->GetScreenViewport();
	const auto& vp0            = vp.viewports[0];
	const auto& ps_resources   = ps_input_info.stage.program->info;
	const auto  sampled_images = std::count_if(
	    ps_resources.images.begin(), ps_resources.images.end(), [](const auto& image) {
		    return image.kind == ShaderRecompiler::IR::ResourceKind::Image ||
		           image.kind == ShaderRecompiler::IR::ResourceKind::ImageUint;
	    });

	VkExtent2D extent = color.vulkan_buffer != nullptr ? color.extent : VkExtent2D {};
	auto       sc     = calc_final_scissor(vp, ctx->GetScanModeControl(), extent);

	LOGF(
	    "DrawTargetState[%u]: frame=%d %s target=%s addr=0x%010" PRIx64
	    " extent=%ux%u prim=%u index_count=%u flags=0x%08" PRIx32 " color_mask=0x%08" PRIx32
	    " clear=%s clear_rgba=(%.3f,%.3f,%.3f,%.3f) cc_mode=%u cc_op=0x%02x"
	    " blend=%s src=%u dst=%u comb=%u ps_tex=%d sampled=%d storage=%d ps_kill=%s target_mode0=%u"
	    " depth_test=%s depth_write=%s depth_func=%u depth_clear=%s viewport=(%.1f,%.1f %.1fx%.1f) "
	    "scissor=(%d,%d)-(%d,%d)\n",
	    log_id, GraphicsRunGetFrameNum(), draw_name, RenderColorTypeName(color.type),
	    color.base_addr, extent.width, extent.height, ucfg->GetPrimType(), index_count, flags,
	    ctx->GetRenderTargetMask(), color.color_clear_enable ? "true" : "false",
	    color.color_clear_value.float32[0], color.color_clear_value.float32[1],
	    color.color_clear_value.float32[2], color.color_clear_value.float32[3], cc.mode, cc.op,
	    bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.color_comb_fcn,
	    static_cast<int>(ps_resources.images.size()), static_cast<int>(sampled_images),
	    static_cast<int>(ps_resources.images.size() - sampled_images),
	    ps_input_info.ps_pixel_kill_enable ? "true" : "false", ps_input_info.target_output_mode[0],
	    dc.z_enable ? "true" : "false", dc.z_write_enable ? "true" : "false", dc.zfunc,
	    depth.depth_clear_enable ? "true" : "false", vp0.xoffset - vp0.xscale,
	    vp0.yoffset - vp0.yscale, vp0.xscale * 2.0f, vp0.yscale * 2.0f, sc.left, sc.top, sc.right,
	    sc.bottom);

	LogMrtState(draw_name, ctx, ps_input_info);
}

static void LogDrawInputState(const RenderColorInfo&       color,
                              const ShaderVertexInputInfo& vs_input_info,
                              uint32_t index_type_and_size, uint32_t index_count,
                              const void* index_addr) {
	auto log_id = g_draw_input_log_count.fetch_add(1);
	if (log_id >= 64 && color.type != RenderColorType::DisplayBuffer) {
		return;
	}

	LOGF("DrawInputState[%u]: frame=%d target=%s addr=0x%010" PRIx64
	     " index_type=%u index_count=%u index_addr=0x%016" PRIx64
	     " vs_resources=%d vs_buffers=%d\n",
	     log_id, GraphicsRunGetFrameNum(), RenderColorTypeName(color.type), color.base_addr,
	     index_type_and_size, index_count, reinterpret_cast<uint64_t>(index_addr),
	     vs_input_info.resources_num, vs_input_info.buffers_num);

	for (int bi = 0; bi < vs_input_info.buffers_num; bi++) {
		const auto& b = vs_input_info.buffers[bi];
		LOGF("DrawInputState[%u]: vb[%d] addr=0x%010" PRIx64
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
				LOGF("DrawInputState[%u]: vb[%d].rec[%u] stride=%u dwords=%u raw=%08" PRIx32
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
					    r.Format() ==
					        Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8UNorm)) {
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

		for (int ai = 0; ai < b.attr_num; ai++) {
			const auto  res_index = b.attr_indices[ai];
			const auto& r         = vs_input_info.resources[res_index];
			const auto& rd        = vs_input_info.resources_dst[res_index];
			LOGF("DrawInputState[%u]: attr[%d] res=%d offset=%u dst=v%d regs=%d fetch_index=%u "
			     "sharp=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
			     log_id, ai, res_index, b.attr_offsets[ai], rd.register_start, rd.registers_num,
			     rd.fetch_index, r.fields[0], r.fields[1], r.fields[2], r.fields[3]);
		}
	}
}

[[maybe_unused]] static void LogDrawTextureState(const char*                 draw_name,
                                                 const RenderColorInfo&      color,
                                                 const ShaderPixelInputInfo& ps_input_info) {
	static std::atomic<uint32_t> log_count {0};
	const auto                   log_id = log_count.fetch_add(1, std::memory_order_relaxed);
	if (log_id >= 256) {
		return;
	}

	const auto& ps_program     = *ps_input_info.stage.program;
	const auto& ps_resources   = *ps_input_info.stage.resources;
	const auto  sampled_images = std::count_if(
	    ps_program.info.images.begin(), ps_program.info.images.end(), [](const auto& image) {
		    return image.kind == ShaderRecompiler::IR::ResourceKind::Image ||
		           image.kind == ShaderRecompiler::IR::ResourceKind::ImageUint;
	    });
	LOGF("DrawTextureState[%u]: frame=%d %s target=%s addr=0x%010" PRIx64
	     " textures=%zu sampled=%zu storage=%zu samplers=%zu\n",
	     log_id, GraphicsRunGetFrameNum(), draw_name, RenderColorTypeName(color.type),
	     color.base_addr, ps_program.info.images.size(), sampled_images,
	     ps_program.info.images.size() - sampled_images, ps_program.info.samplers.size());

	for (uint32_t i = 0; i < ps_program.info.images.size(); i++) {
		const auto& image = ps_program.info.images[i];
		const auto  r     = DecodeNativeDescriptor<ShaderTextureResource>(ps_resources.images[i]);
		LOGF("DrawTextureState[%u]: tex[%u] source=%u usage=%s sampled=%s "
		     "addr=0x%010" PRIx64 " type=%u fmt=%u extent=%ux%u depth=%u levels=%u base_level=%u "
		     "tile=%u swizzle=0x%03" PRIx32 " fields=%08" PRIx32 " %08" PRIx32 " %08" PRIx32
		     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
		     log_id, i, image.source, image.written ? "read-write" : "read-only",
		     (image.kind == ShaderRecompiler::IR::ResourceKind::Image ||
		      image.kind == ShaderRecompiler::IR::ResourceKind::ImageUint)
		         ? "true"
		         : "false",
		     r.Base40(), static_cast<uint32_t>(r.Type()), static_cast<uint32_t>(r.Format()),
		     static_cast<uint32_t>(r.Width5()) + 1u, static_cast<uint32_t>(r.Height5()) + 1u,
		     static_cast<uint32_t>(r.Depth()) + 1u,
		     std::max<uint32_t>(static_cast<uint32_t>(r.LastLevel()),
		                        static_cast<uint32_t>(r.MaxMip())) +
		         1u,
		     static_cast<uint32_t>(r.BaseLevel()), static_cast<uint32_t>(r.TileMode()),
		     r.DstSelXYZW(), r.fields[0], r.fields[1], r.fields[2], r.fields[3], r.fields[4],
		     r.fields[5], r.fields[6], r.fields[7]);
	}

	for (uint32_t i = 0; i < ps_program.info.samplers.size(); i++) {
		const auto& sampler = ps_program.info.samplers[i];
		const auto  r = DecodeNativeDescriptor<ShaderSamplerResource>(ps_resources.samplers[i]);
		LOGF("DrawTextureState[%u]: samp[%u] source=%u clamp=%u/%u/%u filter=%u/%u/%u "
		     "mip=%u lod=%u-%u"
		     " bias=%d fields=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
		     log_id, i, sampler.source, static_cast<uint32_t>(r.ClampX()),
		     static_cast<uint32_t>(r.ClampY()), static_cast<uint32_t>(r.ClampZ()),
		     static_cast<uint32_t>(r.XyMagFilter()), static_cast<uint32_t>(r.XyMinFilter()),
		     static_cast<uint32_t>(r.ZFilter()), static_cast<uint32_t>(r.MipFilter()),
		     static_cast<uint32_t>(r.MinLod()), static_cast<uint32_t>(r.MaxLod()),
		     static_cast<int32_t>(r.LodBias()), r.fields[0], r.fields[1], r.fields[2], r.fields[3]);
	}
}

static void VulkanCmdSetColorWriteEnableEXT(GraphicContext* ctx, VkCommandBuffer command_buffer,
                                            uint32_t        attachment_count,
                                            const VkBool32* p_color_write_enables) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->instance == nullptr);

	static auto func = reinterpret_cast<PFN_vkCmdSetColorWriteEnableEXT>(
	    vkGetInstanceProcAddr(ctx->instance, "vkCmdSetColorWriteEnableEXT"));

	if (func != nullptr) {
		func(command_buffer, attachment_count, p_color_write_enables);
	} else {
		EXIT("vkCmdSetColorWriteEnableEXT not present\n");
	}
}

static PipelineDynamicParameters BuildGraphicsDynamicParams(const HW::Context&     ctx,
                                                            const RenderColorInfo* colors,
                                                            uint32_t               color_count,
                                                            const RenderDepthInfo& depth) {
	EXIT_IF(colors == nullptr);

	PipelineDynamicParameters ret {};
	ret.color_write_count   = color_count;
	ret.stencil_test_enable = depth.stencil_test_enable;

	const auto& vp = ctx.GetScreenViewport();
	VkExtent2D  framebuffer_extent {};
	if (color_count > 0 && colors[0].vulkan_buffer != nullptr) {
		framebuffer_extent = colors[0].extent;
	} else if (depth.vulkan_buffer != nullptr) {
		framebuffer_extent = depth.vulkan_buffer->extent;
	}

	const auto final_scissor = calc_final_scissor(vp, ctx.GetScanModeControl(), framebuffer_extent);

	ret.viewport_scale[0]  = vp.viewports[0].xscale;
	ret.viewport_scale[1]  = vp.viewports[0].yscale;
	ret.viewport_scale[2]  = vp.viewports[0].zscale;
	ret.viewport_offset[0] = vp.viewports[0].xoffset;
	ret.viewport_offset[1] = vp.viewports[0].yoffset;
	ret.viewport_offset[2] = vp.viewports[0].zoffset;
	ret.scissor_ltrb[0]    = final_scissor.left;
	ret.scissor_ltrb[1]    = final_scissor.top;
	ret.scissor_ltrb[2]    = final_scissor.right;
	ret.scissor_ltrb[3]    = final_scissor.bottom;
	ret.line_width         = ctx.GetLineWidth();
	ret.stencil_front      = depth.stencil_dynamic_front;
	ret.stencil_back       = depth.stencil_dynamic_back;

	// CB_COLOR_CONTROL.operation controls special CB operations, not the normal color component
	// write mask. Use CB_TARGET_MASK here so scanout passes with mode=Disable do not go black.
	for (uint32_t i = 0; i < RENDER_COLOR_ATTACHMENTS_MAX; i++) {
		ret.color_write_enable[i] =
		    (i < color_count &&
		     render_target_mask_slot(ctx.GetRenderTargetMask(), colors[i].target_slot) != 0);
	}

	return ret;
}

static void SetDynamicParams(VkCommandBuffer                  vk_buffer,
                             const PipelineDynamicParameters& dynamic_params) {
	KYTY_PROFILER_FUNCTION();

	VkViewport viewport {};
	viewport.x        = dynamic_params.viewport_offset[0] - dynamic_params.viewport_scale[0];
	viewport.y        = dynamic_params.viewport_offset[1] - dynamic_params.viewport_scale[1];
	viewport.width    = dynamic_params.viewport_scale[0] * 2.0f;
	viewport.height   = dynamic_params.viewport_scale[1] * 2.0f;
	viewport.minDepth = dynamic_params.viewport_offset[2];
	viewport.maxDepth = dynamic_params.viewport_scale[2] + dynamic_params.viewport_offset[2];
	vkCmdSetViewport(vk_buffer, 0, 1, &viewport);

	VkRect2D scissor {};
	scissor.offset = {dynamic_params.scissor_ltrb[0], dynamic_params.scissor_ltrb[1]};
	scissor.extent = {
	    static_cast<uint32_t>(dynamic_params.scissor_ltrb[2] - dynamic_params.scissor_ltrb[0]),
	    static_cast<uint32_t>(dynamic_params.scissor_ltrb[3] - dynamic_params.scissor_ltrb[1])};
	vkCmdSetScissor(vk_buffer, 0, 1, &scissor);

	float line_width = dynamic_params.line_width;
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
	vkCmdSetLineWidth(vk_buffer, line_width);

	if (dynamic_params.stencil_test_enable) {
		vkCmdSetStencilCompareMask(vk_buffer, VK_STENCIL_FACE_FRONT_BIT,
		                           dynamic_params.stencil_front.compareMask);
		vkCmdSetStencilCompareMask(vk_buffer, VK_STENCIL_FACE_BACK_BIT,
		                           dynamic_params.stencil_back.compareMask);
		vkCmdSetStencilWriteMask(vk_buffer, VK_STENCIL_FACE_FRONT_BIT,
		                         dynamic_params.stencil_front.writeMask);
		vkCmdSetStencilWriteMask(vk_buffer, VK_STENCIL_FACE_BACK_BIT,
		                         dynamic_params.stencil_back.writeMask);
		vkCmdSetStencilReference(vk_buffer, VK_STENCIL_FACE_FRONT_BIT,
		                         dynamic_params.stencil_front.reference);
		vkCmdSetStencilReference(vk_buffer, VK_STENCIL_FACE_BACK_BIT,
		                         dynamic_params.stencil_back.reference);
	}

	VkBool32 enable[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t i = 0; i < dynamic_params.color_write_count; i++) {
		enable[i] = (dynamic_params.color_write_enable[i] ? VK_TRUE : VK_FALSE);
	}
	VulkanCmdSetColorWriteEnableEXT(g_render_ctx->GetGraphicCtx(), vk_buffer,
	                                dynamic_params.color_write_count, enable);
}

static bool DrawHasValidVertexShader(const HW::Shader* sh_ctx) {
	EXIT_IF(sh_ctx == nullptr);

	const auto& vs = sh_ctx->GetVs();
	return vs.gs_regs.chksum != 0 && ShaderAddressValid(vs.es_regs.data_addr);
}

static bool PixelShaderHasDepthOrCoverageSideEffects(const HW::ShaderRegisters& sh_regs) {
	const auto& db = sh_regs.db_shader_control;
	return sh_regs.shader_z_format != 0 || db.shader_kill_enable || db.shader_z_export_enable ||
	       db.shader_mask_export_enable || db.shader_dual_export_enable ||
	       db.shader_execute_on_noop;
}

static bool ShouldSkipGeShader(const HW::Context* ctx, const HW::UserConfig* ucfg,
                               const HW::Shader* sh_ctx) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ucfg == nullptr);
	EXIT_IF(sh_ctx == nullptr);

	const auto& sh_regs     = ctx->GetShaderRegisters();
	const auto& ge_cntl     = ucfg->GetGeControl();
	const auto& vertex_info = sh_ctx->GetVs();
	const auto  stages      = ctx->GetShaderStages();

	const auto is_known_gs_out_prim_type = [](uint32_t value) {
		switch (static_cast<Prospero::GsOutputPrimitiveType>(value)) {
			case Prospero::GsOutputPrimitiveType::kPoints:
			case Prospero::GsOutputPrimitiveType::kLines:
			case Prospero::GsOutputPrimitiveType::kTriangles:
			case Prospero::GsOutputPrimitiveType::k2dRectangle:
			case Prospero::GsOutputPrimitiveType::kRectList: return true;
		}

		return false;
	};

	const bool ps5_ngg_vertex_path = stages == 0x02002000 && vertex_info.es_regs.data_addr != 0 &&
	                                 vertex_info.gs_regs.chksum != 0 &&
	                                 sh_regs.m_vgtGsMaxVertOut == 0x00000000 &&
	                                 is_known_gs_out_prim_type(sh_regs.m_vgtGsOutPrimType);

	const bool unsupported_stage_mask = (stages != 0 && stages != 0x02002000);
	const bool unsupported_gs_stage = (vertex_info.es_regs.data_addr != 0 &&
	                                   vertex_info.gs_regs.data_addr != 0 && !ps5_ngg_vertex_path);
	const bool ge_group_size =
	    ge_cntl.primitive_group_size > 0x0040 || ge_cntl.vertex_group_size > 0x0040;
	const bool ge_shader_regs =
	    (sh_regs.m_geNggSubgrpCntl != 0x00000000 && sh_regs.m_geNggSubgrpCntl != 0x00000001) ||
	    sh_regs.m_vgtGsMaxVertOut != 0x00000000 ||
	    !is_known_gs_out_prim_type(sh_regs.m_vgtGsOutPrimType) ||
	    sh_regs.m_geMaxOutputPerSubgroup > 0x00000040;

	if (unsupported_stage_mask || unsupported_gs_stage || ge_group_size || ge_shader_regs) {
		const auto log_id = g_shader_stage_log_count.fetch_add(1);
		if (log_id < 32) {
			LOGF("Skipping unsupported GE shader draw: stages=0x%08" PRIx32
			     " prim_group=0x%04" PRIx16 " vert_group=0x%04" PRIx16 " ngg=0x%08" PRIx32
			     " max_out=0x%08" PRIx32 " gs_max_vert=0x%08" PRIx32 " gs_out_prim=0x%08" PRIx32
			     " es=0x%016" PRIx64 " gs=0x%016" PRIx64 "\n",
			     stages, ge_cntl.primitive_group_size, ge_cntl.vertex_group_size,
			     sh_regs.m_geNggSubgrpCntl, sh_regs.m_geMaxOutputPerSubgroup,
			     sh_regs.m_vgtGsMaxVertOut, sh_regs.m_vgtGsOutPrimType,
			     vertex_info.es_regs.data_addr, vertex_info.gs_regs.data_addr);
		}
		return true;
	}

	return false;
}

struct DrawRenderState {
	RenderDepthInfo           depth_info;
	RenderColorInfo           color_info[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	uint32_t                  color_count                              = 0;
	bool                      ps_active                                = true;
	VulkanFramebuffer*        framebuffer                              = nullptr;
	VkCommandBuffer           vk_buffer                                = nullptr;
	ShaderVertexInputInfo     vs_input_info;
	ShaderPixelInputInfo      ps_input_info;
	std::span<const uint32_t> vs_shader;
	std::span<const uint32_t> ps_shader;
};

struct DrawCallInfo {
	const char*          name           = nullptr;
	CommandBufferDebugOp debug_op       = CommandBufferDebugOp::DrawIndex;
	uint32_t             index_count    = 0;
	uint32_t             flags          = 0;
	uint32_t             instance_count = 0;
	uint32_t             first_instance = 0;
};

static bool DrawHasActivePixelShader(const HW::Context* ctx, const HW::Shader* sh_ctx,
                                     const DrawRenderState& state, const DrawCallInfo& draw) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(sh_ctx == nullptr);
	EXIT_IF(draw.name == nullptr);

	const bool with_depth = (state.depth_info.format != VK_FORMAT_UNDEFINED &&
	                         state.depth_info.vulkan_buffer != nullptr);
	if (state.color_count != 0 || !with_depth) {
		return true;
	}

	const auto& sh_regs = ctx->GetShaderRegisters();
	const auto& ps      = sh_ctx->GetPs();
	return ShaderAddressValid(ps.ps_regs.data_addr) &&
	       PixelShaderHasDepthOrCoverageSideEffects(sh_regs);
}

enum class CbColorMode : uint8_t {
	Disable            = 0,
	Normal             = 1,
	EliminateFastClear = 2,
	Resolve            = 3,
	FmaskDecompress    = 5,
	DccDecompress      = 6,
};

static bool ConsumeMetadataColorOperation(const HW::Context& ctx) {
	const auto mode = ctx.GetColorControl().mode;
	// These AGC CB modes run color-buffer metadata/decompression operations. The shader is a
	// dummy vehicle for the CB, and its exported color must not be applied as a normal draw.
	// Kyty currently stores host images as expanded Vulkan images and does not track CMASK/DCC
	// metadata state, so the matching host operation is a no-op.
	return mode == static_cast<uint8_t>(CbColorMode::EliminateFastClear) ||
	       mode == static_cast<uint8_t>(CbColorMode::FmaskDecompress) ||
	       mode == static_cast<uint8_t>(CbColorMode::DccDecompress);
}

struct DrawEmitInfo {
	bool     indexed           = false;
	bool     draw_prim7_as_ngg = false;
	uint32_t draw_vertex_count = 0;
	int32_t  vertex_offset     = 0;
	uint32_t first_vertex      = 0;
};

struct DrawIndexBufferSource {
	bool        enabled   = false;
	uint64_t    address   = 0;
	const void* host_data = nullptr;
	uint64_t    size      = 0;
	VkIndexType type      = VK_INDEX_TYPE_UINT16;
};

static uint64_t VertexBufferDescriptorSize(const ShaderVertexInputBuffer& buffer) {
	return (buffer.stride != 0 ? static_cast<uint64_t>(buffer.stride) * buffer.num_records
	                           : buffer.num_records);
}

static void SetDrawDebugPhase(CommandBuffer* buffer, uint64_t submit_id, const DrawCallInfo& draw,
                              uint32_t phase) {
	EXIT_IF(buffer == nullptr);
	EXIT_IF(draw.name == nullptr);

	buffer->SetDebugInfo(static_cast<uint32_t>(draw.debug_op), submit_id, phase, draw.index_count,
	                     draw.flags, draw.instance_count, draw.first_instance);
}

static bool GetDrawTopology(const HW::UserConfig* ucfg, bool auto_draw, bool use_ngg_rectlist_draw,
                            VkPrimitiveTopology* topology) {
	EXIT_IF(ucfg == nullptr);
	EXIT_IF(topology == nullptr);

	*topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

	switch (static_cast<Prospero::PrimitiveType>(ucfg->GetPrimType())) {
		case Prospero::PrimitiveType::kNone: return false;
		case Prospero::PrimitiveType::kPointList:
			*topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
			break;
		case Prospero::PrimitiveType::kLineList: *topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
		case Prospero::PrimitiveType::kLineStrip:
			*topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
			break;
		case Prospero::PrimitiveType::kTriList:
			*topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			break;
		case Prospero::PrimitiveType::kTriFan:
			*topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
			break;
		case Prospero::PrimitiveType::kTriStrip:
			*topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
			break;
		case Prospero::PrimitiveType::kRectList:
			*topology = (auto_draw && use_ngg_rectlist_draw ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
			                                                : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
			break;
		case Prospero::PrimitiveType::kRectListLegacy:
			if (!auto_draw) {
				EXIT("unknown primitive type: %u\n", ucfg->GetPrimType());
			}
			*topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			*topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
			break;
		default: EXIT("unknown primitive type: %u\n", ucfg->GetPrimType());
	}

	return true;
}

static bool PrepareDrawRenderState(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx,
                                   HW::UserConfig* ucfg, HW::Shader* sh_ctx,
                                   const DrawCallInfo& draw, uint32_t render_target_slice_offset,
                                   bool skip_null_framebuffer, bool log_setup_phases,
                                   DrawRenderState* state) {
	EXIT_IF(buffer == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ucfg == nullptr);
	EXIT_IF(sh_ctx == nullptr);
	EXIT_IF(draw.name == nullptr);
	EXIT_IF(state == nullptr);

	if (log_setup_phases) {
		LogDrawPhase(draw.name, "ResolveRenderDepthTarget");
	}
	ResolveRenderDepthTarget(submit_id, buffer, *ctx, &state->depth_info);

	if (ResolveColorTargets(submit_id, buffer, *ctx, render_target_slice_offset)) {
		MarkRenderTargetGpuWritten(state->depth_info);
		return false;
	}
	MarkRenderTargetGpuWritten(state->depth_info);

	if (log_setup_phases) {
		LogDrawPhase(draw.name, "ResolveRenderColorTarget");
	}
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		if (slot == 0 || (render_target_mask_slot(ctx->GetRenderTargetMask(), slot) != 0 &&
		                  ctx->GetRenderTarget(slot).base.addr != 0)) {
			ResolveRenderColorTarget(submit_id, buffer, *ctx,
			                         &state->color_info[state->color_count],
			                         render_target_slice_offset, slot);
			if (state->color_info[state->color_count].vulkan_buffer != nullptr) {
				MarkRenderTargetGpuWritten(state->color_info[state->color_count]);
				state->color_count++;
			}
		}
	}

	const bool with_depth = (state->depth_info.format != VK_FORMAT_UNDEFINED &&
	                         state->depth_info.vulkan_buffer != nullptr);
	if (state->color_count == 0 && !with_depth) {
		LogFramebufferSkip(draw.name, state->color_info[0], state->depth_info, ctx, ucfg,
		                   draw.index_count, draw.flags);
		return false;
	}
	state->ps_active = DrawHasActivePixelShader(ctx, sh_ctx, *state, draw);

	if (log_setup_phases) {
		LogDrawPhase(draw.name, "CreateFramebuffer");
	}
	state->framebuffer = g_render_ctx->GetFramebufferCache()->CreateFramebuffer(
	    state->color_info, state->color_count, &state->depth_info);

	if (state->framebuffer == nullptr && skip_null_framebuffer) {
		LogFramebufferSkip(draw.name, state->color_info[0], state->depth_info, ctx, ucfg,
		                   draw.index_count, draw.flags);
		return false;
	}
	EXIT_NOT_IMPLEMENTED(state->framebuffer == nullptr);
	EXIT_NOT_IMPLEMENTED(state->framebuffer->render_pass == nullptr);

	state->vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	return true;
}

static void RefreshShaders(HW::Context* ctx, HW::Shader* sh_ctx, const DrawCallInfo& draw,
                           bool log_phases, DrawRenderState* state) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(sh_ctx == nullptr);
	EXIT_IF(draw.name == nullptr);
	EXIT_IF(state == nullptr);

	const auto& vertex_shader_info = sh_ctx->GetVs();
	const auto& pixel_shader_info  = sh_ctx->GetPs();
	const auto& shader_regs        = ctx->GetShaderRegisters();

	state->vs_shader     = {};
	state->ps_shader     = {};
	state->ps_input_info = {};
	std::array<Prospero::ColorComponentMapping, RENDER_COLOR_ATTACHMENTS_MAX>
	    target_export_mapping {};
	for (uint32_t i = 0; i < state->color_count; i++) {
		target_export_mapping[state->color_info[i].target_slot] =
		    state->color_info[i].export_mapping;
	}
	EXIT_IF(g_render_ctx == nullptr || g_render_ctx->GetGraphicCtx() == nullptr);
	const auto lane_mask_mode = SelectGraphicsLaneMaskMode(*g_render_ctx->GetGraphicCtx(), 64u);

	if (log_phases) {
		LogDrawPhase(draw.name, "ShaderCompileInfoVS");
	}
	if (!ShaderCompileInfoVS(&vertex_shader_info, &shader_regs, lane_mask_mode,
	                         &state->vs_input_info, &state->vs_shader)) {
		EXIT("ShaderCompileInfoVS failed for draw %s\n", draw.name);
	}

	if (!state->ps_active) {
		return;
	}
	if (log_phases) {
		LogDrawPhase(draw.name, "ShaderCompileInfoPS");
	}
	if (!ShaderCompileInfoPS(&pixel_shader_info, &shader_regs, lane_mask_mode,
	                         &state->vs_input_info, target_export_mapping, &state->ps_input_info,
	                         &state->ps_shader)) {
		EXIT("ShaderCompileInfoPS failed for draw %s\n", draw.name);
	}
}

static void BindDrawVertexBuffers(uint64_t submit_id, CommandBuffer* buffer,
                                  const DrawCallInfo& draw, VkCommandBuffer vk_buffer,
                                  const ShaderVertexInputInfo& vs_input_info) {
	EXIT_IF(buffer == nullptr);
	EXIT_IF(draw.name == nullptr);
	(void)submit_id;

	LogDrawPhase(draw.name, "BindVertexBuffers");
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto&   b        = vs_input_info.buffers[i];
		uint64_t      addr     = b.addr;
		uint64_t      size     = VertexBufferDescriptorSize(b);
		VulkanBuffer* vertices = nullptr;
		VkDeviceSize  offset   = 0;

		if (size == 0) {
			vertices = g_render_ctx->GetBufferCache()->ObtainNullBuffer(
			    buffer, g_render_ctx->GetGraphicCtx());
		} else {
			auto binding = g_render_ctx->GetBufferCache()->ObtainBuffer(
			    buffer, g_render_ctx->GetGraphicCtx(), addr, size);
			vertices = binding.first;
			offset   = binding.second;
		}
		EXIT_NOT_IMPLEMENTED(vertices == nullptr);

		vkCmdBindVertexBuffers(vk_buffer, i, 1, &vertices->buffer, &offset);
	}
}

static void BindDrawIndexBuffer(CommandBuffer* buffer, VkCommandBuffer vk_buffer,
                                const DrawIndexBufferSource& source) {
	if (!source.enabled) {
		return;
	}
	EXIT_IF(source.size == 0);

	VulkanBuffer* index_buffer = nullptr;
	VkDeviceSize  index_offset = 0;
	if (source.host_data != nullptr) {
		VkDeviceSize range = 0;
		if (!g_render_ctx->GetBufferCache()->UploadHostData(
		        buffer, g_render_ctx->GetGraphicCtx(), source.host_data, source.size, 16,
		        &index_buffer, &index_offset, &range)) {
			EXIT("failed to upload host index buffer\n");
		}
	} else {
		auto binding = g_render_ctx->GetBufferCache()->ObtainBuffer(
		    buffer, g_render_ctx->GetGraphicCtx(), source.address, source.size);
		index_buffer = binding.first;
		index_offset = binding.second;
	}
	EXIT_IF(index_buffer == nullptr);
	vkCmdBindIndexBuffer(vk_buffer, index_buffer->buffer, index_offset, source.type);
}

static void LogDrawStateIfNeeded(HW::Context* ctx, HW::UserConfig* ucfg, const DrawCallInfo& draw,
                                 const DrawRenderState& state, bool always_log,
                                 bool force_legacy_rect_log, uint32_t index_type_and_size,
                                 const void* index_addr) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ucfg == nullptr);
	EXIT_IF(draw.name == nullptr);

	if (!graphics_debug_dump_enabled()) {
		return;
	}

	if (!always_log && !force_legacy_rect_log) {
		return;
	}

	LogDrawTargetState(draw.name, state.color_info[0], state.depth_info, ctx, ucfg,
	                   state.ps_input_info, draw.index_count, draw.flags);
	LogDrawInputState(state.color_info[0], state.vs_input_info, index_type_and_size,
	                  draw.index_count, index_addr);
	// LogDrawTextureState(draw.name, state.color_info[0], state.ps_input_info);
}

static bool IsHostExpandedRectListDrawSupported(const ShaderVertexInputInfo& vs_input_info,
                                                const DrawCallInfo&          draw,
                                                const DrawEmitInfo&          emit) {
	if (!emit.draw_prim7_as_ngg) {
		return true;
	}

	if (vs_input_info.buffers_num != 0) {
		return false;
	}

	return draw.index_count == 3 || draw.index_count == emit.draw_vertex_count;
}

static void EmitDrawPrimitives(const HW::UserConfig* ucfg, VkCommandBuffer vk_buffer,
                               const ShaderVertexInputInfo& vs_input_info, const DrawCallInfo& draw,
                               const DrawEmitInfo& emit) {
	EXIT_IF(ucfg == nullptr);
	EXIT_IF(draw.name == nullptr);

	switch (static_cast<Prospero::PrimitiveType>(ucfg->GetPrimType())) {
		case Prospero::PrimitiveType::kPointList:
		case Prospero::PrimitiveType::kLineList:
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriList:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip:
			if (emit.indexed) {
				vkCmdDrawIndexed(vk_buffer, draw.index_count, draw.instance_count, 0,
				                 emit.vertex_offset, draw.first_instance);
			} else {
				vkCmdDraw(vk_buffer, draw.index_count, draw.instance_count, emit.first_vertex,
				          draw.first_instance);
			}
			break;
		case Prospero::PrimitiveType::kRectList:
			if (emit.indexed) {
				vkCmdDrawIndexed(vk_buffer, draw.index_count, draw.instance_count, 0,
				                 emit.vertex_offset, draw.first_instance);
			} else {
				EXIT_NOT_IMPLEMENTED(
				    !IsHostExpandedRectListDrawSupported(vs_input_info, draw, emit));
				vkCmdDraw(vk_buffer, emit.draw_vertex_count, draw.instance_count, emit.first_vertex,
				          draw.first_instance);
			}
			break;
		case Prospero::PrimitiveType::kRectListLegacy:
			if (emit.indexed) {
				EXIT("unknown primitive type: %u\n", ucfg->GetPrimType());
			}
			// Sarah
			EXIT_NOT_IMPLEMENTED(!(draw.index_count == 3 && vs_input_info.buffers_num == 0));
			vkCmdDraw(vk_buffer, 4, draw.instance_count, emit.first_vertex, draw.first_instance);
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			EXIT_NOT_IMPLEMENTED((draw.index_count & 0x3u) != 0);
			for (uint32_t i = 0; i < draw.index_count; i += 4) {
				if (emit.indexed) {
					vkCmdDrawIndexed(vk_buffer, 4, draw.instance_count, i, emit.vertex_offset,
					                 draw.first_instance);
				} else {
					vkCmdDraw(vk_buffer, 4, draw.instance_count, i + emit.first_vertex,
					          draw.first_instance);
				}
			}
			break;
		default: EXIT("unknown primitive type: %u\n", ucfg->GetPrimType());
	}
}

static void ExecutePreparedDraw(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx,
                                HW::UserConfig* ucfg, HW::Shader* sh_ctx, const DrawCallInfo& draw,
                                DrawRenderState* state, VkPrimitiveTopology topology,
                                const DrawEmitInfo& emit, const DrawIndexBufferSource& index_source,
                                bool log_pipeline_phase, bool set_bind_debug, bool set_auto_debug) {
	EXIT_IF(buffer == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ucfg == nullptr);
	EXIT_IF(sh_ctx == nullptr);
	EXIT_IF(draw.name == nullptr);
	EXIT_IF(state == nullptr);

	for (;;) {
		const auto recording_generation = buffer->GetRecordingGeneration();
		if (log_pipeline_phase) {
			LogDrawPhase(draw.name, "CreatePipeline");
		}
		auto* pipeline = g_render_ctx->GetPipelineCache()->CreateGraphicsPipeline(
		    state->framebuffer, state->color_info, state->color_count, &state->depth_info,
		    &state->vs_input_info, ctx, sh_ctx, &state->ps_input_info, topology, state->ps_active,
		    state->vs_shader, state->ps_shader);

		if (pipeline == nullptr || pipeline->pipeline == nullptr) {
			return; // Controlled bypass: wait/skip drawing until pipeline finishes background compilation
		}

		if (set_bind_debug) {
			SetDrawDebugPhase(buffer, submit_id, draw, 0x100u);
		}
		vkCmdBindPipeline(state->vk_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
		const auto dynamic_params = BuildGraphicsDynamicParams(
		    *ctx, state->color_info, state->color_count, state->depth_info);
		SetDynamicParams(state->vk_buffer, dynamic_params);

		// EXIT_NOT_IMPLEMENTED(vs_input_info.buffers_num > 1);
		BindDrawVertexBuffers(submit_id, buffer, draw, state->vk_buffer, state->vs_input_info);

		LogDrawPhase(draw.name, "BindDescriptorsVS");
		if (set_auto_debug) {
			SetDrawDebugPhase(buffer, submit_id, draw, 0x200u);
		}
		const auto vs_address_writes = BindDescriptors(
		    submit_id, buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout,
		    state->vs_input_info.stage, VK_SHADER_STAGE_VERTEX_BIT, DescriptorCache::Stage::Vertex);

		std::vector<ShaderAddressWriteRange> ps_address_writes;
		if (state->ps_active) {
			LogDrawPhase(draw.name, "BindDescriptorsPS");
			if (set_auto_debug) {
				SetDrawDebugPhase(buffer, submit_id, draw, 0x300u);
			}
			ps_address_writes =
			    BindDescriptors(submit_id, buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
			                    pipeline->pipeline_layout, state->ps_input_info.stage,
			                    VK_SHADER_STAGE_FRAGMENT_BIT, DescriptorCache::Stage::Pixel);
		}
		if (buffer->GetRecordingGeneration() != recording_generation) {
			continue;
		}
		// Index data may use this command buffer's host stream. Resolve it only after the other
		// fault-capable bindings, and rebuild it whenever a fault reset changes the generation.
		BindDrawIndexBuffer(buffer, state->vk_buffer, index_source);
		if (buffer->GetRecordingGeneration() != recording_generation) {
			continue;
		}

		LogDrawPhase(draw.name, "BeginRenderPass");
		if (set_auto_debug) {
			SetDrawDebugPhase(buffer, submit_id, draw, 0x400u);
		}
		buffer->BeginRenderPass(state->framebuffer, state->color_info, state->color_count,
		                        &state->depth_info);
		if (set_auto_debug) {
			SetDrawDebugPhase(buffer, submit_id, draw, 0x500u);
		}

		EmitDrawPrimitives(ucfg, state->vk_buffer, state->vs_input_info, draw, emit);

		if (set_auto_debug) {
			SetDrawDebugPhase(buffer, submit_id, draw, 0x600u);
		}
		buffer->EndRenderPass();
		VkPipelineStageFlags shader_write_stages = 0;
		const bool           vs_wrote_buffers   = HasShaderBufferWrites(state->vs_input_info.stage);
		const bool           vs_wrote_addresses = MarkShaderAddressWrites(vs_address_writes);
		if (vs_wrote_buffers || vs_wrote_addresses) {
			shader_write_stages |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
		}
		if (state->ps_active) {
			const bool ps_wrote_buffers   = HasShaderBufferWrites(state->ps_input_info.stage);
			const bool ps_wrote_addresses = MarkShaderAddressWrites(ps_address_writes);
			if (ps_wrote_buffers || ps_wrote_addresses) {
				shader_write_stages |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			}
		}
		if (shader_write_stages != 0) {
			ShaderWriteBarrier(state->vk_buffer, shader_write_stages);
		}
		LogDrawPhase(draw.name, "EndRenderPass");
		if (set_auto_debug) {
			SetDrawDebugPhase(buffer, submit_id, draw, 0x700u);
		}
		break;
	}
}

void RenderDrawIndex(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx,
                     HW::UserConfig* ucfg, HW::Shader* sh_ctx, uint32_t index_type_and_size,
                     uint32_t index_count, const void* index_addr, uint32_t flags, uint32_t type,
                     uint32_t instance_count, uint32_t render_target_slice_offset,
                     int32_t vertex_offset_add, uint32_t first_instance) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(ucfg == nullptr);
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndex), submit_id,
	                     index_count, flags, type, instance_count,
	                     reinterpret_cast<uint64_t>(index_addr));

	Common::LockGuard lock(g_render_ctx->GetMutex());

	if (index_count == 0) {
		return;
	}

	if (ConsumeMetadataColorOperation(*ctx)) {
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return;
	}

	if (ShouldSkipGeShader(ctx, ucfg, sh_ctx)) {
		return;
	}

	if (graphics_debug_dump_enabled()) {
		sh_print("GraphicsRenderDrawIndex():Shader:", *sh_ctx);
		uc_print("GraphicsRenderDrawIndex():UserConfig:", *ucfg);
		hw_print(*ctx);

		LOGF("GraphicsRenderDrawIndex():Parameters:\n"
		     "\t index_type_and_size = 0x%08" PRIx32 "\n"
		     "\t index_count         = 0x%08" PRIx32 "\n"
		     "\t index_addr          = 0x%016" PRIx64 "\n"
		     "\t flags               = 0x%08" PRIx32 "\n"
		     "\t type                = 0x%08" PRIx32 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t rt_slice_offset     = 0x%08" PRIx32 "\n"
		     "\t vertex_offset_add   = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     index_type_and_size, index_count, reinterpret_cast<uint64_t>(index_addr), flags, type,
		     instance_count, render_target_slice_offset, static_cast<uint32_t>(vertex_offset_add),
		     first_instance);
	}
	sh_check(*sh_ctx);

	uc_check(*ucfg);

	hw_check(*ctx);

	VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	if (!GetDrawTopology(ucfg, false, false, &topology)) {
		return;
	}

	VkIndexType index_type           = VK_INDEX_TYPE_UINT16;
	uint64_t    index_size           = 0;
	bool        expand_index8_to_u16 = false;

	switch (static_cast<Prospero::IndexType>(index_type_and_size)) {
		case Prospero::IndexType::kIndex16:
			index_type = VK_INDEX_TYPE_UINT16;
			index_size = 2 * static_cast<uint64_t>(index_count);
			break;
		case Prospero::IndexType::kIndex32:
			index_type = VK_INDEX_TYPE_UINT32;
			index_size = 4 * static_cast<uint64_t>(index_count);
			break;
		// Some games use it - need vulkan extension
		case Prospero::IndexType::kIndex8:
			index_type           = VK_INDEX_TYPE_UINT16;
			index_size           = static_cast<uint64_t>(index_count);
			expand_index8_to_u16 = true;
			break;
		default: EXIT("unknown index_type_and_size: %u\n", index_type_and_size);
	}

	EXIT_NOT_IMPLEMENTED(flags != 0);
	EXIT_NOT_IMPLEMENTED(type != 1);
	if (instance_count == 0) {
		instance_count = 1;
	}

	const DrawCallInfo draw {"DrawIndex",    CommandBufferDebugOp::DrawIndex,
	                         index_count,    flags,
	                         instance_count, first_instance};
	std::vector<uint16_t> expanded_indices;
	if (expand_index8_to_u16) {
		EXIT_NOT_IMPLEMENTED(index_addr == nullptr);
		const auto* src = static_cast<const uint8_t*>(index_addr);
		expanded_indices.resize(index_count);
		for (uint32_t i = 0; i < index_count; i++) {
			expanded_indices[i] = src[i];
		}
	}

	DrawIndexBufferSource index_source {};
	index_source.enabled = true;
	index_source.address = reinterpret_cast<uint64_t>(index_addr);
	index_source.host_data =
	    expanded_indices.empty() ? nullptr : static_cast<const void*>(expanded_indices.data());
	index_source.size = expanded_indices.empty()
	                        ? index_size
	                        : expanded_indices.size() * sizeof(uint16_t);
	index_source.type = index_type;

	DrawRenderState state {};
	if (!PrepareDrawRenderState(submit_id, buffer, ctx, ucfg, sh_ctx, draw,
	                            render_target_slice_offset, false, true, &state)) {
		return;
	}

	RefreshShaders(ctx, sh_ctx, draw, true, &state);

	LogDrawStateIfNeeded(ctx, ucfg, draw, state, true, false, index_type_and_size, index_addr);

	const auto vertex_offset =
	    ResolveVertexOffset(ucfg->GetIndexOffset(), state.vs_input_info) + vertex_offset_add;

	DrawEmitInfo emit {};
	emit.indexed       = true;
	emit.vertex_offset = vertex_offset;

	ExecutePreparedDraw(submit_id, buffer, ctx, ucfg, sh_ctx, draw, &state, topology, emit,
	                    index_source, true, true, false);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RenderDrawIndexAuto(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx,
                         HW::UserConfig* ucfg, HW::Shader* sh_ctx, uint32_t index_count,
                         uint32_t flags, uint32_t render_target_slice_offset,
                         uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr || ucfg == nullptr);
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr || buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndexAuto), submit_id,
	                     index_count, flags, first_vertex, instance_count, first_instance);

	Common::LockGuard lock(g_render_ctx->GetMutex());

	if (index_count == 0) {
		return;
	}

	if (ConsumeMetadataColorOperation(*ctx)) {
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return;
	}

	if (ShouldSkipGeShader(ctx, ucfg, sh_ctx)) {
		return;
	}

	if (graphics_debug_dump_enabled()) {
		sh_print("GraphicsRenderDrawIndexAuto():Shader:", *sh_ctx);
		uc_print("GraphicsRenderDrawIndexAuto():UserConfig:", *ucfg);
		hw_print(*ctx);

		LOGF("GraphicsRenderDrawIndexAuto():Parameters:\n"
		     "\t index_count         = 0x%08" PRIx32 "\n"
		     "\t flags               = 0x%08" PRIx32 "\n"
		     "\t rt_slice_offset     = 0x%08" PRIx32 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t first_vertex        = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     index_count, flags, render_target_slice_offset, instance_count, first_vertex,
		     first_instance);
	}
	sh_check(*sh_ctx);

	uc_check(*ucfg);

	hw_check(*ctx);

	EXIT_NOT_IMPLEMENTED(flags != 0);
	if (instance_count == 0) {
		instance_count = 1;
	}

	const DrawCallInfo draw {"DrawIndexAuto", CommandBufferDebugOp::DrawIndexAuto,
	                         index_count,     flags,
	                         instance_count,  first_instance};

	DrawRenderState state {};
	if (!PrepareDrawRenderState(submit_id, buffer, ctx, ucfg, sh_ctx, draw,
	                            render_target_slice_offset, true, false, &state)) {
		return;
	}

	VkPrimitiveTopology topology              = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	const bool          use_ngg_rectlist_draw = Config::NggRectlistDrawEnabled();

	if (!GetDrawTopology(ucfg, true, use_ngg_rectlist_draw, &topology)) {
		return;
	}
	const bool draw_prim7_as_ngg =
	    (use_ngg_rectlist_draw &&
	     ucfg->GetPrimType() == Prospero::GpuEnumValue(Prospero::PrimitiveType::kRectList));

	RefreshShaders(ctx, sh_ctx, draw, false, &state);

	if (draw_prim7_as_ngg && state.vs_input_info.buffers_num == 0 &&
	    state.vs_input_info.param_export_mask == 0 && state.ps_input_info.input_num != 0) {
		if (graphics_debug_dump_enabled()) {
			LOGF("DrawIndexAuto: skipping rect-list draw with no VS param exports and PS inputs: "
			     "ps_inputs=%u ps=0x%016" PRIx64 " es=0x%016" PRIx64 " gs=0x%016" PRIx64 "\n",
			     state.ps_input_info.input_num, sh_ctx->GetPs().ps_regs.chksum,
			     sh_ctx->GetVs().es_regs.data_addr, sh_ctx->GetVs().gs_regs.data_addr);
		}
		return;
	}

	LogDrawStateIfNeeded(ctx, ucfg, draw, state, false,
	                     ucfg->GetPrimType() ==
	                         Prospero::GpuEnumValue(Prospero::PrimitiveType::kRectListLegacy),
	                     0, nullptr);

	const uint32_t draw_vertex_count = (draw_prim7_as_ngg ? 4u : index_count);
	const auto   vertex_offset = ResolveVertexOffset(ucfg->GetIndexOffset(), state.vs_input_info) +
	                             static_cast<int32_t>(first_vertex);
	DrawEmitInfo emit {};
	emit.draw_prim7_as_ngg = draw_prim7_as_ngg;
	emit.draw_vertex_count = draw_vertex_count;
	emit.first_vertex      = static_cast<uint32_t>(vertex_offset);

	DrawIndexBufferSource index_source {};
	ExecutePreparedDraw(submit_id, buffer, ctx, ucfg, sh_ctx, draw, &state, topology, emit,
	                    index_source, false, false, true);
}

bool IsSameColorResolveSubresource(const RenderColorInfo& src, const RenderColorInfo& dst) {
	return src.base_addr == dst.base_addr && src.base_mip_level == dst.base_mip_level &&
	       src.base_array_layer == dst.base_array_layer;
}

ImageImageCopy MakeColorResolveCopy(const RenderColorInfo& src, const RenderColorInfo& dst,
                                    uint32_t width, uint32_t height) {
	ImageImageCopy region {};
	region.src_image = src.vulkan_buffer;
	region.src_level = src.base_mip_level;
	region.dst_level = dst.base_mip_level;
	region.width     = width;
	region.height    = height;
	region.src_layer = src.base_array_layer;
	region.dst_layer = dst.base_array_layer;
	return region;
}

static bool ResolveColorTargets(uint64_t submit_id, CommandBuffer* buffer, const HW::Context& hw,
                                uint32_t render_target_slice_offset) {
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
	ResolveRenderColorTarget(submit_id, buffer, hw, &src, render_target_slice_offset, 0, true,
	                         true);
	ResolveRenderColorTarget(submit_id, buffer, hw, &dst, render_target_slice_offset, 1, true);
	if (src.vulkan_buffer == nullptr || dst.vulkan_buffer == nullptr ||
	    src.type == RenderColorType::NoColorOutput || dst.type == RenderColorType::NoColorOutput) {
		return false;
	}
	if (IsSameColorResolveSubresource(src, dst)) {
		return true;
	}

	const uint32_t width  = std::min(src.extent.width, dst.extent.width);
	const uint32_t height = std::min(src.extent.height, dst.extent.height);
	if (width == 0 || height == 0) {
		return false;
	}

	const std::array regions {MakeColorResolveCopy(src, dst, width, height)};

	MarkRenderTargetGpuWritten(dst);
	UtilImageToImage(buffer, regions, dst.vulkan_buffer, dst.vulkan_buffer->layout);
	return true;
}

} // namespace Libs::Graphics
