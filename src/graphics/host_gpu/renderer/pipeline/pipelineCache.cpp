#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/shaderCompiler.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

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

struct PipelineCache::ProgramCache {
	struct SourceKey {
		ShaderType stage = ShaderType::Unknown;
		uint64_t   hash  = 0;

		bool operator==(const SourceKey&) const = default;
	};

	struct Permutation {
		std::vector<uint32_t>                               static_key;
		std::shared_ptr<const ShaderRecompiler::IR::Program> program;
		ShaderProgram                                       handle;
	};

	struct SourceKeyHash {
		std::size_t operator()(const SourceKey& key) const {
			std::size_t hash = static_cast<std::size_t>(key.stage);
			PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash));
			if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
				PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash >> 32u));
			}
			return hash;
		}
	};

	static uint64_t MixId(uint64_t hash, uint64_t value) {
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u));
	}

	static constexpr std::size_t MaxStaticKeyWords =
	    5 + ShaderVertexInputInfo::RES_MAX * 17;

	template <typename InputInfo>
	ShaderProgram Get(const ShaderParams& params, InputInfo& input_info) {
		constexpr ShaderType stage = [] {
			if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
				return ShaderType::Vertex;
			} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
				return ShaderType::Pixel;
			} else {
				static_assert(std::is_same_v<InputInfo, ShaderComputeInputInfo>);
				return ShaderType::Compute;
			}
		}();

		BuildStageStaticKey(input_info, key_scratch);
		auto& permutations = programs[{stage, params.hash}];
		for (const auto& permutation: permutations) {
			if (permutation.static_key == key_scratch &&
			    MaterializeProgram(permutation.program, params, input_info)) {
				return permutation.handle;
			}
		}

		const auto module = CompileProgram(device, params, input_info);
		EXIT_IF(module == nullptr || !input_info.stage);
		uint64_t id = MixId(MixId(params.hash, static_cast<uint64_t>(stage)), permutations.size());
		if (id == 0) {
			id = 1;
		}
		const ShaderProgram handle {.id = id, .module = module};
		permutations.push_back({key_scratch, input_info.stage.program, handle});

		std::printf("Num compiled %u shaders\n", ++num_compiled);
		return handle;
	}

	explicit ProgramCache(vk::Device device): device(device) {
		key_scratch.reserve(MaxStaticKeyWords);
	}

	std::unordered_map<SourceKey, std::vector<Permutation>, SourceKeyHash> programs;
	std::vector<uint32_t> key_scratch;
	vk::Device device;
	uint32_t   num_compiled = 0;
};

PipelineCache::PipelineCache(GraphicContext& graphics)
    : m_graphics(graphics), m_program_cache(std::make_unique<ProgramCache>(graphics.device)) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
}

PipelineCache::~PipelineCache() {
	auto destroy = [this](const auto& pipelines) {
		for (const auto& [key, pipeline]: pipelines) {
			(void)key;
			m_graphics.device.destroyPipeline(pipeline->pipeline, nullptr);
			m_graphics.device.destroyPipelineLayout(pipeline->pipeline_layout, nullptr);
			m_graphics.device.destroyDescriptorSetLayout(pipeline->descriptor_set_layout, nullptr);
		}
	};
	destroy(m_graphics_pipelines);
	destroy(m_compute_pipelines);
	for (const auto& [key, permutations]: m_program_cache->programs) {
		(void)key;
		for (const auto& permutation: permutations) {
			m_graphics.device.destroyShaderModule(permutation.handle.module, nullptr);
		}
	}
}

ShaderProgram PipelineCache::GetVertexProgram(const HW::VertexShaderInfo& regs,
                                              const HW::ShaderRegisters&  sh,
                                              ShaderVertexInputInfo&      input_info) {
	const auto params = PrepareProgram(regs, sh, input_info);
	Common::LockGuard lock(m_mutex);
	return m_program_cache->Get(params, input_info);
}

ShaderProgram PipelineCache::GetPixelProgram(
    const HW::PixelShaderInfo& regs, const HW::ShaderRegisters& sh,
    const ShaderVertexInputInfo&                        vertex_info,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
    ShaderPixelInputInfo&                               input_info) {
	const auto params = PrepareProgram(regs, sh, vertex_info, target_export_mapping, input_info);
	Common::LockGuard lock(m_mutex);
	return m_program_cache->Get(params, input_info);
}

ShaderProgram PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                               const HW::ShaderRegisters&   sh,
                                               ShaderComputeInputInfo&      input_info) {
	input_info.needs_lds_barriers = !m_graphics.compute_wave64_supported;
	const auto params             = PrepareProgram(regs, sh, input_info);
	Common::LockGuard lock(m_mutex);
	return m_program_cache->Get(params, input_info);
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const noexcept {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

PipelineCache::GraphicsPipeline& PipelineCache::CreateGraphicsPipeline(
    std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
    const ShaderVertexInputInfo& vs_input_info, CommandBuffer& command,
    const ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
    bool primitive_restart_enable, const ShaderProgram& vertex_program,
    const ShaderProgram& pixel_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(colors.size() > RENDER_COLOR_ATTACHMENTS_MAX);
	EXIT_IF(!vertex_program);
	const bool ps_active = ps_input_info != nullptr;
	EXIT_IF(ps_active && !pixel_program);
	const auto color_count = static_cast<uint32_t>(colors.size());

	Common::LockGuard lock(m_mutex);
	auto&             ctx = command.GetRegisters();

	const HW::BlendColor& bclr                                     = ctx.GetBlendColor();
	uint32_t              color_mask[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t i = 0; i < color_count; i++) {
		color_mask[i] =
		    (colors[i].image_id ? colors[i].export_mapping.ApplyMask(render_target_mask_slot(
		                              ctx.GetRenderTargetMask(), colors[i].target_slot))
		                        : 0);
	}
	const HW::ModeControl& mc = ctx.GetModeControl();

	const auto vs_id = vertex_program.id;
	const auto ps_id = ps_active ? pixel_program.id : 0;

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
	if (color_count == 0 && !with_depth) {
		attachment_samples = render_sample_count(ctx.GetAaConfig().msaa_num_samples);
		EXIT_IF(!static_cast<bool>(
		    m_graphics.GetPhysicalDeviceProperties().limits.framebufferNoAttachmentsSampleCounts &
		    vulkan_sample_count(attachment_samples)));
	}
	EXIT_IF(attachment_samples == 0 ||
	        vulkan_sample_count(attachment_samples) == vk::SampleCountFlagBits {});

	if (ps_active && depth.depth_test_enable && ps_input_info->ps_execute_on_noop) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("Pipeline: temporary: accepting EXEC_ON_NOOP with depth test enabled\n");
		}
	}

	const auto& clip_control               = ctx.GetClipControl();
	static_params.negative_one_to_one      = !clip_control.dx_clip_space;
	static_params.depth_clip_enable        = clip_control.IsZClipEnabled();
	static_params.topology                 = topology;
	static_params.primitive_restart_enable = primitive_restart_enable;
	static_params.samples                  = attachment_samples;
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
		LOGF("PipelineTrace: shader modules VS=%" PRIu64 " module=%p PS=%" PRIu64
		     " module=%p\n",
		     vs_id, static_cast<void*>(vertex_program.module), ps_id,
		     static_cast<void*>(pixel_program.module));
	}

	auto cached = std::make_unique<GraphicsPipeline>(p);
	LogPipelineTrace("CreatePipelineInternal begin", vs_id, ps_id);
	CreatePipelineInternal(m_graphics, *cached, rendering, vs_input_info, vertex_program.module,
	                       ps_input_info, pixel_program.module, static_params);
	LogPipelineTrace("CreatePipelineInternal done", vs_id, ps_id);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_graphics_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);

	return *iter->second;
}

PipelineCache::ComputePipeline&
PipelineCache::CreateComputePipeline(ShaderComputeInputInfo& input_info,
                                     const ShaderProgram&    compute_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(!compute_program);

	Common::LockGuard lock(m_mutex);

	ComputePipeline p {};
	p.cs_shader_id = compute_program.id;

	ComputePipelineKey key {};
	key.cs_shader_id = p.cs_shader_id;

	if (auto iter = m_compute_pipelines.find(key); iter != m_compute_pipelines.end()) {
		return *iter->second;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}

	auto cached = std::make_unique<ComputePipeline>(p);
	CreatePipelineInternal(m_graphics, *cached, input_info, compute_program.module);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_compute_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);

	return *iter->second;
}
} // namespace Libs::Graphics
