#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {

constexpr const char* kVulkanPipelineCachePath = "_PipelineCache/vulkan_pipelines.bin";

std::filesystem::path PipelineCacheFilePath() {
	return std::filesystem::path(kVulkanPipelineCachePath);
}

std::vector<uint8_t> LoadPipelineCacheBlob() {
	const auto path = PipelineCacheFilePath();
	if (!Common::File::IsFileExisting(path)) {
		return {};
	}
	Common::File file(path, Common::File::Mode::Read);
	if (file.IsInvalid()) {
		return {};
	}
	const auto size = file.Size();
	if (size == 0 || size > (256ull * 1024ull * 1024ull)) {
		return {};
	}
	auto buf = file.ReadWholeBuffer();
	if (buf.Size() == 0) {
		return {};
	}
	std::vector<uint8_t> out(static_cast<size_t>(buf.Size()));
	std::memcpy(out.data(), buf.GetData(), out.size());
	LOGF("VulkanPipelineCache: loaded %zu bytes from %s\n", out.size(),
	     path.string().c_str());
	return out;
}

void SavePipelineCacheBlob(GraphicContext& graphics) {
	if (graphics.pipeline_cache == nullptr || graphics.device == nullptr) {
		return;
	}
	const auto path = PipelineCacheFilePath();
	Common::File::CreateDirectories(path.parent_path());

	size_t data_size = 0;
	auto   result    = graphics.device.getPipelineCacheData(graphics.pipeline_cache, &data_size,
	                                                        nullptr);
	if (result != vk::Result::eSuccess || data_size == 0) {
		LOGF("VulkanPipelineCache: getPipelineCacheData size failed (%s)\n",
		     VulkanToString(result).c_str());
		return;
	}
	std::vector<uint8_t> data(data_size);
	result = graphics.device.getPipelineCacheData(graphics.pipeline_cache, &data_size, data.data());
	if (result != vk::Result::eSuccess) {
		LOGF("VulkanPipelineCache: getPipelineCacheData failed (%s)\n",
		     VulkanToString(result).c_str());
		return;
	}
	data.resize(data_size);

	Common::File file;
	if (!file.Create(path)) {
		LOGF("VulkanPipelineCache: cannot create %s\n", path.string().c_str());
		return;
	}
	file.Write(data.data(), static_cast<uint32_t>(data.size()));
	file.Close();
	LOGF("VulkanPipelineCache: saved %zu bytes to %s\n", data.size(), path.string().c_str());
}

void CreateVulkanPipelineCache(GraphicContext& graphics) {
	if (graphics.pipeline_cache != nullptr) {
		return;
	}
	auto initial = LoadPipelineCacheBlob();

	vk::PipelineCacheCreateInfo info {};
	info.sType           = vk::StructureType::ePipelineCacheCreateInfo;
	info.initialDataSize = initial.size();
	info.pInitialData    = initial.empty() ? nullptr : initial.data();

	const auto result =
	    graphics.device.createPipelineCache(&info, nullptr, &graphics.pipeline_cache);
	if (result != vk::Result::eSuccess) {
		// Corrupt/incompatible blob (driver update): retry empty.
		LOGF("VulkanPipelineCache: create with seed failed (%s), retrying empty\n",
		     VulkanToString(result).c_str());
		info.initialDataSize = 0;
		info.pInitialData    = nullptr;
		const auto retry =
		    graphics.device.createPipelineCache(&info, nullptr, &graphics.pipeline_cache);
		EXIT_NOT_IMPLEMENTED(retry != vk::Result::eSuccess);
	}
}

void DestroyVulkanPipelineCache(GraphicContext& graphics) {
	if (graphics.pipeline_cache == nullptr) {
		return;
	}
	SavePipelineCacheBlob(graphics);
	graphics.device.destroyPipelineCache(graphics.pipeline_cache, nullptr);
	graphics.pipeline_cache = nullptr;
}

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

PipelineCache::PipelineCache(GraphicContext& graphics, DescriptorCache& descriptor_cache)
    : m_graphics(graphics), m_descriptor_cache(descriptor_cache) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	CreateVulkanPipelineCache(m_graphics);
	if (Config::AsyncShaderCompileEnabled()) {
		m_worker = std::jthread([this] { WorkerMain(); });
	}
}

PipelineCache::~PipelineCache() {
	{
		Common::LockGuard lock(m_mutex);
		m_shutdown = true;
		m_job_cv.SignalAll();
	}
	if (m_worker.joinable()) {
		m_worker.join();
	}
	auto destroy = [this](const auto& pipelines) {
		for (const auto& [key, pipeline]: pipelines) {
			(void)key;
			if (pipeline->pipeline) {
				m_graphics.device.destroyPipeline(pipeline->pipeline, nullptr);
			}
			if (pipeline->pipeline_layout) {
				m_graphics.device.destroyPipelineLayout(pipeline->pipeline_layout, nullptr);
			}
		}
	};
	destroy(m_graphics_pipelines);
	destroy(m_compute_pipelines);
	DestroyVulkanPipelineCache(m_graphics);
}

void PipelineCache::WorkerMain() {
	for (;;) {
		CompileJob job;
		{
			Common::LockGuard lock(m_mutex);
			while (m_jobs.empty() && !m_shutdown) {
				m_job_cv.Wait(&m_mutex);
			}
			if (m_jobs.empty()) {
				EXIT_IF(!m_shutdown);
				return;
			}
			job = std::move(m_jobs.front());
			m_jobs.pop_front();
		}

		if (job.graphics) {
			auto& g = *job.graphics;
			EXIT_IF(g.target == nullptr);
			LogPipelineTrace("CreatePipelineInternal begin", g.vs_hash0, g.vs_crc32, g.ps_hash0,
			                 g.ps_crc32);
			{
				Common::LockGuard create_lock(m_create_mutex);
				CreatePipelineInternal(
				    m_graphics, m_descriptor_cache, *g.target, g.rendering, g.vs_input_info,
				    g.vs_spirv, g.ps_input_info ? &*g.ps_input_info : nullptr, g.ps_spirv,
				    g.static_params, g.vs_hash0, g.vs_crc32, g.ps_hash0, g.ps_crc32, g.ps_active);
			}
			LogPipelineTrace("CreatePipelineInternal done", g.vs_hash0, g.vs_crc32, g.ps_hash0,
			                 g.ps_crc32);
			EXIT_NOT_IMPLEMENTED(g.target->pipeline == nullptr);
			EXIT_NOT_IMPLEMENTED(g.target->pipeline_layout == nullptr);
			Common::LockGuard lock(m_mutex);
			g.target->state = CompileState::Ready;
			m_ready_cv.SignalAll();
		} else {
			EXIT_IF(job.compute == nullptr);
			auto& c = *job.compute;
			EXIT_IF(c.target == nullptr);
			{
				Common::LockGuard create_lock(m_create_mutex);
				CreatePipelineInternal(m_graphics, m_descriptor_cache, *c.target, c.input_info,
				                       c.cs_spirv);
			}
			EXIT_NOT_IMPLEMENTED(c.target->pipeline == nullptr);
			EXIT_NOT_IMPLEMENTED(c.target->pipeline_layout == nullptr);
			Common::LockGuard lock(m_mutex);
			c.target->state = CompileState::Ready;
			m_ready_cv.SignalAll();
		}
	}
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const noexcept {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

PipelineCache::GraphicsPipeline& PipelineCache::RequestGraphicsPipeline(
    RenderColorInfo* colors, uint32_t color_count, RenderDepthInfo& depth,
    ShaderVertexInputInfo& vs_input_info, RenderCommandBuffer& command,
    ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology, bool ps_active,
    std::span<const uint32_t> vs_spirv, std::span<const uint32_t> ps_spirv) {
	KYTY_PROFILER_BLOCK("PipelineCache::RequestPipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(colors == nullptr);
	EXIT_IF(color_count > RENDER_COLOR_ATTACHMENTS_MAX);
	EXIT_IF(vs_spirv.empty());
	EXIT_IF(ps_active && ps_spirv.empty());

	Common::LockGuard lock(m_mutex);
	auto&             ctx    = command.GetRegisters();
	auto&             sh_ctx = command.GetShaders();

	const auto&           vertex_info                              = sh_ctx.GetVs();
	const auto&           ps_regs                                  = sh_ctx.GetPs();
	const HW::BlendColor& bclr                                     = ctx.GetBlendColor();
	uint32_t              color_mask[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t i = 0; i < color_count; i++) {
		color_mask[i] =
		    (colors[i].image_id ? colors[i].export_mapping.ApplyMask(render_target_mask_slot(
		                              ctx.GetRenderTargetMask(), colors[i].target_slot))
		                        : 0);
	}
	const HW::ModeControl& mc = ctx.GetModeControl();

	auto     vs_id = ShaderGetIdVS(vertex_info, vs_input_info, true);
	ShaderId ps_id {};
	if (ps_active) {
		ps_id = ShaderGetIdPS(ps_regs, *ps_input_info, true);
	}

	PipelineStaticParameters static_params {};
	GraphicsPipeline         p {};
	p.ps_shader_id = ps_id;
	p.vs_shader_id = vs_id;

	static_params.color_count = color_count;
	PipelineRenderingState rendering {};
	rendering.color_count       = color_count;
	uint32_t attachment_samples = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		EXIT_IF(!colors[i].image_id || colors[i].format == vk::Format::eUndefined);
		rendering.color_formats[i] = colors[i].format;
		if (attachment_samples == 0) {
			attachment_samples = colors[i].samples;
		} else if (attachment_samples != colors[i].samples) {
			EXIT("mixed color attachment sample counts are unsupported: %u and %u\n",
			     attachment_samples, colors[i].samples);
		}
	}
	const bool with_depth =
	    depth.format != vk::Format::eUndefined && static_cast<bool>(depth.image_id);
	if (with_depth) {
		const auto aspects = ImageViewOps::DepthAspectMask(depth.format);
		rendering.depth_format =
		    aspects & vk::ImageAspectFlagBits::eDepth ? depth.format : vk::Format::eUndefined;
		rendering.stencil_format =
		    aspects & vk::ImageAspectFlagBits::eStencil ? depth.format : vk::Format::eUndefined;
		if (attachment_samples == 0) {
			attachment_samples = depth.samples;
		} else if (attachment_samples != depth.samples) {
			EXIT("mixed color/depth sample counts are unsupported: %u and %u\n", attachment_samples,
			     depth.samples);
		}
	}
	EXIT_IF(attachment_samples == 0 ||
	        vulkan_sample_count(attachment_samples) == vk::SampleCountFlagBits {});

	if (ps_active && depth.depth_test_enable && ps_input_info->ps_execute_on_noop) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("Pipeline: temporary: accepting EXEC_ON_NOOP with depth test enabled\n");
		}
	}

	const auto& clip_control = ctx.GetClipControl();
	EXIT_NOT_IMPLEMENTED(!clip_control.IsZClipModeRepresentable());
	static_params.negative_one_to_one = !clip_control.dx_clip_space;
	static_params.depth_clip_enable   = clip_control.IsZClipEnabled();
	static_params.topology            = topology;
	static_params.samples             = attachment_samples;
	static_params.sample_shading_enable =
	    ps_active && attachment_samples > 1 && ps_input_info->ps_sample_shading;
	if (static_params.sample_shading_enable && !m_graphics.sample_rate_shading_enabled) {
		EXIT("Pipeline: sample-rate shading is required but unsupported by the host\n");
	}
	static_params.with_depth         = with_depth;
	static_params.depth_test_enable  = depth.depth_test_enable;
	static_params.depth_write_enable = (depth.depth_write_enable && !depth.depth_clear_enable);
	static_params.depth_compare_op   = depth.depth_compare_op;
	static_params.depth_bounds_test_enable = depth.depth_bounds_test_enable;
	static_params.depth_min_bounds         = depth.depth_min_bounds;
	static_params.depth_max_bounds         = depth.depth_max_bounds;
	static_params.stencil_test_enable      = depth.stencil_test_enable;
	static_params.stencil_front            = depth.stencil_static_front;
	static_params.stencil_back             = depth.stencil_static_back;
	for (uint32_t i = 0; i < RENDER_COLOR_ATTACHMENTS_MAX; i++) {
		static_params.color_mask[i] = color_mask[i];
	}
	const bool rect_list     = topology == vk::PrimitiveTopology::ePatchList;
	static_params.cull_back  = !rect_list && mc.cull_back;
	static_params.cull_front = !rect_list && mc.cull_front;
	static_params.face       = mc.face;

	for (uint32_t i = 0; i < color_count; i++) {
		const auto& rt                        = ctx.GetRenderTarget(colors[i].target_slot);
		const auto& bc                        = ctx.GetBlendControl(colors[i].target_slot);
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
	key.rendering     = rendering;
	key.vs_shader_id  = p.vs_shader_id;
	key.ps_shader_id  = p.ps_shader_id;
	key.static_params = static_params;

	if (auto iter = m_graphics_pipelines.find(key); iter != m_graphics_pipelines.end()) {
		return *iter->second;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(vs_input_info);
		if (ps_active) {
			ShaderDbgDumpInputInfo(*ps_input_info);
		}
		LOGF("PipelineTrace: shader binaries VS=0x%08" PRIx32 "/0x%08" PRIx32 " words=%" PRIu64
		     " PS=0x%08" PRIx32 "/0x%08" PRIx32 " words=%" PRIu64 "\n",
		     vs_id.hash0, vs_id.crc32, static_cast<uint64_t>(vs_spirv.size()), ps_id.hash0,
		     ps_id.crc32, static_cast<uint64_t>(ps_spirv.size()));
	}

	auto  cached = std::make_unique<GraphicsPipeline>(p);
	auto* raw    = cached.get();
	const bool async =
	    Config::AsyncShaderCompileEnabled() && static_cast<bool>(m_worker.joinable());
	raw->state = async ? CompileState::Compiling : CompileState::Ready;

	auto [iter, inserted] = m_graphics_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);

	if (!async) {
		LogPipelineTrace("CreatePipelineInternal begin", vs_id.hash0, vs_id.crc32, ps_id.hash0,
		                 ps_id.crc32);
		{
			Common::LockGuard create_lock(m_create_mutex);
			CreatePipelineInternal(m_graphics, m_descriptor_cache, *raw, rendering, vs_input_info,
			                       vs_spirv, ps_input_info, ps_spirv, static_params, vs_id.hash0,
			                       vs_id.crc32, ps_id.hash0, ps_id.crc32, ps_active);
		}
		LogPipelineTrace("CreatePipelineInternal done", vs_id.hash0, vs_id.crc32, ps_id.hash0,
		                 ps_id.crc32);
		EXIT_NOT_IMPLEMENTED(raw->pipeline == nullptr);
		EXIT_NOT_IMPLEMENTED(raw->pipeline_layout == nullptr);
		raw->state = CompileState::Ready;
		return *raw;
	}

	auto job           = std::make_unique<GraphicsCompileJob>();
	job->target        = raw;
	job->rendering     = rendering;
	job->static_params = static_params;
	job->vs_input_info = vs_input_info;
	if (ps_active) {
		job->ps_input_info = *ps_input_info;
	}
	job->vs_spirv.assign(vs_spirv.begin(), vs_spirv.end());
	if (ps_active) {
		job->ps_spirv.assign(ps_spirv.begin(), ps_spirv.end());
	}
	job->ps_active = ps_active;
	job->vs_hash0  = vs_id.hash0;
	job->vs_crc32  = vs_id.crc32;
	job->ps_hash0  = ps_id.hash0;
	job->ps_crc32  = ps_id.crc32;

	CompileJob queued;
	queued.graphics = std::move(job);
	m_jobs.push_back(std::move(queued));
	m_job_cv.Signal();
	return *raw;
}

void PipelineCache::WaitGraphicsPipeline(GraphicsPipeline& pipeline) {
	Common::LockGuard lock(m_mutex);
	while (pipeline.state == CompileState::Compiling) {
		m_ready_cv.Wait(&m_mutex);
	}
	EXIT_NOT_IMPLEMENTED(pipeline.pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(pipeline.pipeline_layout == nullptr);
}

PipelineCache::GraphicsPipeline& PipelineCache::CreateGraphicsPipeline(
    RenderColorInfo* colors, uint32_t color_count, RenderDepthInfo& depth,
    ShaderVertexInputInfo& vs_input_info, RenderCommandBuffer& command,
    ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology, bool ps_active,
    std::span<const uint32_t> vs_spirv, std::span<const uint32_t> ps_spirv) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);
	auto& pipeline =
	    RequestGraphicsPipeline(colors, color_count, depth, vs_input_info, command, ps_input_info,
	                            topology, ps_active, vs_spirv, ps_spirv);
	WaitGraphicsPipeline(pipeline);
	return pipeline;
}

PipelineCache::ComputePipeline&
PipelineCache::RequestComputePipeline(ShaderComputeInputInfo&      input_info,
                                      const HW::ComputeShaderInfo& cs_regs,
                                      std::span<const uint32_t>    cs_spirv) {
	KYTY_PROFILER_BLOCK("PipelineCache::RequestPipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(cs_spirv.empty());

	Common::LockGuard lock(m_mutex);

	auto cs_id = ShaderGetIdCS(cs_regs, input_info, true);

	ComputePipeline p {};
	p.cs_shader_id = cs_id;

	ComputePipelineKey key {};
	key.cs_shader_id = p.cs_shader_id;

	if (auto iter = m_compute_pipelines.find(key); iter != m_compute_pipelines.end()) {
		return *iter->second;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}

	auto  cached = std::make_unique<ComputePipeline>(p);
	auto* raw    = cached.get();
	const bool async =
	    Config::AsyncShaderCompileEnabled() && static_cast<bool>(m_worker.joinable());
	raw->state = async ? CompileState::Compiling : CompileState::Ready;

	auto [iter, inserted] = m_compute_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);

	if (!async) {
		Common::LockGuard create_lock(m_create_mutex);
		CreatePipelineInternal(m_graphics, m_descriptor_cache, *raw, input_info, cs_spirv);
		EXIT_NOT_IMPLEMENTED(raw->pipeline == nullptr);
		EXIT_NOT_IMPLEMENTED(raw->pipeline_layout == nullptr);
		raw->state = CompileState::Ready;
		return *raw;
	}

	auto job         = std::make_unique<ComputeCompileJob>();
	job->target      = raw;
	job->input_info  = input_info;
	job->cs_spirv.assign(cs_spirv.begin(), cs_spirv.end());

	CompileJob queued;
	queued.compute = std::move(job);
	m_jobs.push_back(std::move(queued));
	m_job_cv.Signal();
	return *raw;
}

void PipelineCache::WaitComputePipeline(ComputePipeline& pipeline) {
	Common::LockGuard lock(m_mutex);
	while (pipeline.state == CompileState::Compiling) {
		m_ready_cv.Wait(&m_mutex);
	}
	EXIT_NOT_IMPLEMENTED(pipeline.pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(pipeline.pipeline_layout == nullptr);
}

PipelineCache::ComputePipeline&
PipelineCache::CreateComputePipeline(ShaderComputeInputInfo&      input_info,
                                     const HW::ComputeShaderInfo& cs_regs,
                                     std::span<const uint32_t>    cs_spirv) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);
	auto& pipeline = RequestComputePipeline(input_info, cs_regs, cs_spirv);
	WaitComputePipeline(pipeline);
	return pipeline;
}
} // namespace Libs::Graphics
