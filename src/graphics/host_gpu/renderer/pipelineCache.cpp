#include "graphics/host_gpu/renderer/pipelineCache.h"
#include "graphics/host_gpu/AsyncPipelineBuilder.h"

#include "common/assert.h"
#include "loader/systemContent.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/framebufferCache.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/utils.h"

#include <atomic>
#include <cstring>
#include <fmt/format.h>
#include <span>
#include <utility>

namespace Libs::Graphics {

std::unique_ptr<Libs::Graphics::AsyncPipelineBuilder> g_AsyncPipelineBuilder;
std::shared_mutex g_pipeline_cache_mutex;

std::string GetCurrentGameTitleId() {
	std::string title_id;
	if (!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) {
		title_id = "UNKNOWN";
	}
	return title_id;
}

VkPipelineCache GetVkPipelineCacheHandle() {
	if (!g_AsyncPipelineBuilder) {
		auto* gctx = g_render_ctx->GetGraphicCtx();
		std::string title_id = GetCurrentGameTitleId();
		std::string cache_path = "cache/pipelines/" + title_id + "_pipeline.cache";
		Common::File::CreateDirectories("cache/pipelines");
		g_AsyncPipelineBuilder = std::make_unique<Libs::Graphics::AsyncPipelineBuilder>(
		    gctx->device, gctx->physical_device, cache_path);

		g_render_ctx->GetPipelineCache()->InitializeFallbackPipeline();
	}
	return g_AsyncPipelineBuilder->GetVkPipelineCache();
}

static const std::vector<uint32_t> k_fallback_vert_shader = {
	0x07230203u, 0x00010000u, 0x0008000bu, 0x00000006u, 0x00000000u, 0x00020011u, 0x00000001u,
	0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu,
	0x00000000u, 0x00000001u, 0x0005000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u,
	0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u,
	0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00050036u, 0x00000002u,
	0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x000100fdu, 0x00010038u
};

static const std::vector<uint32_t> k_fallback_frag_shader = {
	0x07230203u, 0x00010000u, 0x0008000bu, 0x00000006u, 0x00000000u, 0x00020011u, 0x00000001u,
	0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu,
	0x00000000u, 0x00000001u, 0x0005000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u,
	0x00030010u, 0x00000004u, 0x00000007u, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u,
	0x00000004u, 0x6e69616du, 0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u,
	0x00000002u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u,
	0x00000005u, 0x000100fdu, 0x00010038u
};

PipelineCache::GraphicsPipeline* PipelineCache::GetOrCreateDynamicFallbackPipeline(
	const GraphicsPipelineKey& key,
	VulkanFramebuffer* framebuffer,
	ShaderVertexInputInfo* vs_input_info,
	ShaderPixelInputInfo* ps_input_info,
	const PipelineStaticParameters& static_params,
	const ShaderId& vs_id,
	const ShaderId& ps_id,
	bool ps_active,
	VkPipelineCache pipeline_cache_handle,
	const GraphicsPipeline& p
) {
	if (auto iter = m_fallback_pipelines.find(key); iter != m_fallback_pipelines.end()) {
		return iter->second.get();
	}

	auto fallback_cached = std::make_unique<GraphicsPipeline>(p);
	CreatePipelineInternal(fallback_cached.get(), framebuffer->render_pass,
	                       vs_input_info, k_fallback_vert_shader,
	                       ps_input_info, k_fallback_frag_shader,
	                       static_params, vs_id.hash0, vs_id.crc32,
	                       ps_id.hash0, ps_id.crc32, ps_active,
	                       pipeline_cache_handle);

	auto* ptr = fallback_cached.get();
	m_fallback_pipelines.emplace(key, std::move(fallback_cached));
	return ptr;
}

void PipelineCache::InitializeFallbackPipeline() {
	if (m_fallback_pipeline.pipeline != nullptr) {
		return;
	}

	auto* gctx = g_render_ctx->GetGraphicCtx();
	if (gctx == nullptr || gctx->device == nullptr) {
		return;
	}

	VkAttachmentDescription attachment{};
	attachment.format = VK_FORMAT_B8G8R8A8_UNORM;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkAttachmentReference color_attachment{};
	color_attachment.attachment = 0;
	color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &color_attachment;

	VkRenderPassCreateInfo rp_info{};
	rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rp_info.attachmentCount = 1;
	rp_info.pAttachments = &attachment;
	rp_info.subpassCount = 1;
	rp_info.pSubpasses = &subpass;

	auto result = vkCreateRenderPass(gctx->device, &rp_info, nullptr, &m_fallback_render_pass);
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

	ShaderVertexInputInfo vs_input_info{};
	ShaderPixelInputInfo ps_input_info{};
	PipelineStaticParameters static_params{};
	static_params.color_count = 1;

	CreatePipelineInternal(&m_fallback_pipeline, m_fallback_render_pass,
	                       &vs_input_info, k_fallback_vert_shader,
	                       &ps_input_info, k_fallback_frag_shader,
	                       static_params, 0, 0, 0, 0,
	                       true, g_AsyncPipelineBuilder->GetVkPipelineCache());
}

namespace {

void NormalizeStaticParamsForDynamicState(PipelineStaticParameters& static_params) {
	static_params.viewport_scale[0]  = 0.5f;
	static_params.viewport_scale[1]  = 0.5f;
	static_params.viewport_scale[2]  = 1.0f;
	static_params.viewport_offset[0] = 0.5f;
	static_params.viewport_offset[1] = 0.5f;
	static_params.viewport_offset[2] = 0.0f;

	static_params.scissor_ltrb[0] = 0;
	static_params.scissor_ltrb[1] = 0;
	static_params.scissor_ltrb[2] = 1;
	static_params.scissor_ltrb[3] = 1;
}

} // namespace

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const noexcept {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

void PipelineCache::DeletePipelineInternal(Pipeline* p) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(p == nullptr);
	EXIT_IF(p->pipeline == nullptr);
	EXIT_IF(p->pipeline_layout == nullptr);

	DumpPipeline("delete", *p);

	auto* gctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(gctx == nullptr);

	VulkanDeviceWaitIdle(gctx);
	vkDestroyPipeline(gctx->device, p->pipeline, nullptr);
	vkDestroyPipelineLayout(gctx->device, p->pipeline_layout, nullptr);

	p->pipeline        = nullptr;
	p->pipeline_layout = nullptr;
}

PipelineCache::GraphicsPipeline* PipelineCache::CreateGraphicsPipeline(
    VulkanFramebuffer* framebuffer, RenderColorInfo* colors, uint32_t color_count,
    RenderDepthInfo* depth, ShaderVertexInputInfo* vs_input_info, HW::Context* ctx,
    HW::Shader* sh_ctx, ShaderPixelInputInfo* ps_input_info, VkPrimitiveTopology topology,
    bool ps_active, std::span<const uint32_t> vs_spirv, std::span<const uint32_t> ps_spirv) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(framebuffer == nullptr);
	EXIT_IF(depth == nullptr);
	EXIT_IF(colors == nullptr);
	EXIT_IF(color_count > RENDER_COLOR_ATTACHMENTS_MAX);
	EXIT_IF(vs_spirv.empty());
	EXIT_IF(ps_active && ps_spirv.empty());

	Common::LockGuard lock(m_mutex);

	const auto&           vertex_info                              = sh_ctx->GetVs();
	const auto&           ps_regs                                  = sh_ctx->GetPs();
	const HW::BlendColor& bclr                                     = ctx->GetBlendColor();
	uint32_t              color_mask[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t i = 0; i < color_count; i++) {
		color_mask[i] = (colors[i].vulkan_buffer != nullptr
		                     ? colors[i].export_mapping.ApplyMask(render_target_mask_slot(
		                           ctx->GetRenderTargetMask(), colors[i].target_slot))
		                     : 0);
	}
	const HW::ModeControl& mc = ctx->GetModeControl();

	auto     vs_id = ShaderGetIdVS(&vertex_info, vs_input_info, true);
	ShaderId ps_id {};
	if (ps_active) {
		ps_id = ShaderGetIdPS(&ps_regs, ps_input_info, true);
	}

	PipelineStaticParameters static_params {};
	GraphicsPipeline         p {};
	p.render_pass_id = framebuffer->render_pass_id;
	p.ps_shader_id   = ps_id;
	p.vs_shader_id   = vs_id;

	static_params.color_count = color_count;

	if (ps_active && depth->depth_test_enable && ps_input_info->ps_execute_on_noop) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("Pipeline: temporary: accepting EXEC_ON_NOOP with depth test enabled\n");
		}
	}

	static_params.negative_one_to_one = !ctx->GetClipControl().dx_clip_space;
	static_params.topology            = topology;
	static_params.with_depth =
	    (depth->format != VK_FORMAT_UNDEFINED && depth->vulkan_buffer != nullptr);
	static_params.depth_test_enable  = depth->depth_test_enable;
	static_params.depth_write_enable = (depth->depth_write_enable && !depth->depth_clear_enable);
	static_params.depth_compare_op   = depth->depth_compare_op;
	static_params.depth_bounds_test_enable = depth->depth_bounds_test_enable;
	static_params.depth_min_bounds         = depth->depth_min_bounds;
	static_params.depth_max_bounds         = depth->depth_max_bounds;
	static_params.stencil_test_enable      = depth->stencil_test_enable;
	static_params.stencil_front            = depth->stencil_static_front;
	static_params.stencil_back             = depth->stencil_static_back;
	for (uint32_t i = 0; i < RENDER_COLOR_ATTACHMENTS_MAX; i++) {
		static_params.color_mask[i] = color_mask[i];
	}
	static_params.cull_back  = mc.cull_back;
	static_params.cull_front = mc.cull_front;
	static_params.face       = mc.face;

	for (uint32_t i = 0; i < color_count; i++) {
		const auto& rt                        = ctx->GetRenderTarget(colors[i].target_slot);
		const auto& bc                        = ctx->GetBlendControl(colors[i].target_slot);
		static_params.color_srcblend[i]       = bc.color_srcblend;
		static_params.color_comb_fcn[i]       = bc.color_comb_fcn;
		static_params.color_destblend[i]      = bc.color_destblend;
		static_params.alpha_srcblend[i]       = bc.alpha_srcblend;
		static_params.alpha_comb_fcn[i]       = bc.alpha_comb_fcn;
		static_params.alpha_destblend[i]      = bc.alpha_destblend;
		static_params.separate_alpha_blend[i] = bc.separate_alpha_blend;
		static_params.blend_enable[i]         = bc.enable;
		static_params.blend_bypass[i]         = rt.info.blend_bypass;
	}
	static_params.blend_color_red   = bclr.red;
	static_params.blend_color_green = bclr.green;
	static_params.blend_color_blue  = bclr.blue;
	static_params.blend_color_alpha = bclr.alpha;

	NormalizeStaticParamsForDynamicState(static_params);

	GraphicsPipelineKey key {};
	key.render_pass_id = p.render_pass_id;
	key.vs_shader_id   = p.vs_shader_id;
	key.ps_shader_id   = p.ps_shader_id;
	key.static_params  = static_params;

	if (auto iter = m_graphics_pipelines.find(key); iter != m_graphics_pipelines.end()) {
		return iter->second.get();
	}

	VkPipelineCache pipeline_cache_handle = GetVkPipelineCacheHandle();

	VkPipeline       pipeline_handle = nullptr;
	VkPipelineLayout layout_handle   = nullptr;
	auto status = g_AsyncPipelineBuilder->QueryPipeline(key, pipeline_handle, layout_handle);

	if (status == AsyncPipelineBuilder::Status::Completed) {
		auto cached = std::make_unique<GraphicsPipeline>(p);
		cached->pipeline        = pipeline_handle;
		cached->pipeline_layout = layout_handle;

		auto [iter, inserted] = m_graphics_pipelines.emplace(std::move(key), std::move(cached));
		EXIT_IF(!inserted);
		DumpPipeline("create", *iter->second);

		// Fallback pipelines are kept alive in m_fallback_pipelines and cleaned up at teardown (DeleteAllPipelines)
		// to avoid wait-idle stutters or destroying a pipeline currently in use by an in-flight frame.

		return iter->second.get();
	}

	if (status == AsyncPipelineBuilder::Status::Pending) {
		return GetOrCreateDynamicFallbackPipeline(key, framebuffer, vs_input_info, ps_input_info, static_params, vs_id, ps_id, ps_active, pipeline_cache_handle, p);
	}

	// If NotFound or Error, dispatch compilation task
	ShaderPixelInputInfo ps_info_copy {};
	if (ps_active && ps_input_info != nullptr) {
		ps_info_copy = *ps_input_info;
	}

	g_AsyncPipelineBuilder->RequestPipeline(
	    key,
	    framebuffer->render_pass,
	    *vs_input_info,
	    std::vector<uint32_t>(vs_spirv.begin(), vs_spirv.end()),
	    ps_info_copy,
	    std::vector<uint32_t>(ps_spirv.begin(), ps_spirv.end()),
	    static_params,
	    vs_id.hash0, vs_id.crc32,
	    ps_id.hash0, ps_id.crc32,
	    ps_active
	);

	return GetOrCreateDynamicFallbackPipeline(key, framebuffer, vs_input_info, ps_input_info, static_params, vs_id, ps_id, ps_active, pipeline_cache_handle, p);
}

PipelineCache::ComputePipeline*
PipelineCache::CreateComputePipeline(ShaderComputeInputInfo*      input_info,
                                     const HW::ComputeShaderInfo* cs_regs,
                                     std::span<const uint32_t>    cs_spirv) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(cs_regs == nullptr);
	EXIT_IF(cs_spirv.empty());

	Common::LockGuard lock(m_mutex);

	auto cs_id = ShaderGetIdCS(cs_regs, input_info, true);

	ComputePipeline p {};
	p.cs_shader_id = cs_id;

	ComputePipelineKey key {};
	key.cs_shader_id = p.cs_shader_id;

	if (auto iter = m_compute_pipelines.find(key); iter != m_compute_pipelines.end()) {
		return iter->second.get();
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}

	auto cached = std::make_unique<ComputePipeline>(p);
	CreatePipelineInternal(cached.get(), input_info, cs_spirv, GetVkPipelineCacheHandle());

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_compute_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);

	if (g_AsyncPipelineBuilder) {
		g_AsyncPipelineBuilder->MarkCacheDirty();
	}

	return iter->second.get();
}

void PipelineCache::DeletePipeline(Pipeline* pipeline) {
	Common::LockGuard lock(m_mutex);

	EXIT_IF(pipeline == nullptr);

	for (auto iter = m_graphics_pipelines.begin(); iter != m_graphics_pipelines.end(); iter++) {
		if (iter->second.get() == pipeline) {
			DeletePipelineInternal(iter->second.get());
			m_graphics_pipelines.erase(iter);
			return;
		}
	}

	for (auto iter = m_compute_pipelines.begin(); iter != m_compute_pipelines.end(); iter++) {
		if (iter->second.get() == pipeline) {
			DeletePipelineInternal(iter->second.get());
			m_compute_pipelines.erase(iter);
			return;
		}
	}

	EXIT_IF(true);
}

void PipelineCache::DeleteAllPipelines() {
	Common::LockGuard lock(m_mutex);

	if (m_fallback_pipeline.pipeline != nullptr) {
		DeletePipelineInternal(&m_fallback_pipeline);
	}

	if (m_fallback_render_pass != nullptr) {
		auto* gctx = g_render_ctx->GetGraphicCtx();
		vkDestroyRenderPass(gctx->device, m_fallback_render_pass, nullptr);
		m_fallback_render_pass = nullptr;
	}

	for (auto& item: m_graphics_pipelines) {
		DeletePipelineInternal(item.second.get());
	}
	m_graphics_pipelines.clear();

	for (auto& item: m_fallback_pipelines) {
		DeletePipelineInternal(item.second.get());
	}
	m_fallback_pipelines.clear();

	for (auto& item: m_compute_pipelines) {
		DeletePipelineInternal(item.second.get());
	}
	m_compute_pipelines.clear();
}

void PipelineCache::DumpToFile(Common::File* f, const Pipeline& p) {}

void PipelineCache::DumpPipeline(const char* action, const Pipeline& p) {
	EXIT_IF(action == nullptr);

	static std::atomic_int dump_id = 0;

	if (!Config::PipelineDumpEnabled()) {
		return;
	}

	Common::File f;
	const auto   file_name =
	    Config::GetPipelineDumpFolder() /
	    fmt::format("{:04d}_{:04d}_pipeline_{}.log", GraphicsRunGetFrameNum(), dump_id++, action);
	Common::File::CreateDirectories(file_name.parent_path());
	f.Create(file_name);
	if (f.IsInvalid()) {
		auto file_name_text = Common::PathToString(file_name);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", file_name_text.c_str());
		return;
	}
	DumpToFile(&f, p);
	f.Close();
}

} // namespace Libs::Graphics
