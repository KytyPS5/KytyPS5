#include "common/assert.h"
#include "common/common.h"
#include "common/profiler.h"
#include "gpu_blit_shaders/gpu_mesh_indirect_spv.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/pipeline/descriptorHeap.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/rectListShader.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cinttypes>
#include <fmt/format.h>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics {

namespace {

// The guest pipeline the driver is creating on this thread (DescribeDriverPipelineCreation).
struct DriverCreation {
	bool        active = false;
	const char* names[4] {};
	uint64_t    hashes[4] {};
};

thread_local DriverCreation t_driver_creation;

class ScopedDriverCreation {
public:
	ScopedDriverCreation(const std::array<const char*, 4>& names,
	                     const std::array<uint64_t, 4>&    hashes) {
		t_driver_creation.active = true;
		for (size_t i = 0; i < 4; i++) {
			t_driver_creation.names[i]  = names[i];
			t_driver_creation.hashes[i] = hashes[i];
		}
	}
	~ScopedDriverCreation() { t_driver_creation = {}; }

	ScopedDriverCreation(const ScopedDriverCreation&)            = delete;
	ScopedDriverCreation& operator=(const ScopedDriverCreation&) = delete;
};

} // namespace

void DescribeDriverPipelineCreation(std::string& report) {
	const auto& creation = t_driver_creation;
	if (!creation.active) {
		return;
	}
	report += "pipeline being created by the driver:";
	for (size_t i = 0; i < 4; i++) {
		if (creation.names[i] != nullptr) {
			report += fmt::format(" {} 0x{:016x}", creation.names[i], creation.hashes[i]);
		}
	}
	report += "\n";
}

static vk::BlendFactor GetBlendFactor(uint32_t factor) {
	switch (static_cast<Prospero::BlendFactor>(factor)) {
		case Prospero::BlendFactor::kZero: return vk::BlendFactor::eZero;
		case Prospero::BlendFactor::kOne: return vk::BlendFactor::eOne;
		case Prospero::BlendFactor::kSrcColor: return vk::BlendFactor::eSrcColor;
		case Prospero::BlendFactor::kOneMinusSrcColor: return vk::BlendFactor::eOneMinusSrcColor;
		case Prospero::BlendFactor::kSrcAlpha: return vk::BlendFactor::eSrcAlpha;
		case Prospero::BlendFactor::kOneMinusSrcAlpha: return vk::BlendFactor::eOneMinusSrcAlpha;
		case Prospero::BlendFactor::kDstAlpha: return vk::BlendFactor::eDstAlpha;
		case Prospero::BlendFactor::kOneMinusDstAlpha: return vk::BlendFactor::eOneMinusDstAlpha;
		case Prospero::BlendFactor::kDstColor: return vk::BlendFactor::eDstColor;
		case Prospero::BlendFactor::kOneMinusDstColor: return vk::BlendFactor::eOneMinusDstColor;
		case Prospero::BlendFactor::kSrcAlphaSaturate: return vk::BlendFactor::eSrcAlphaSaturate;
		case Prospero::BlendFactor::kConstantColor: return vk::BlendFactor::eConstantColor;
		case Prospero::BlendFactor::kOneMinusConstantColor:
			return vk::BlendFactor::eOneMinusConstantColor;
		case Prospero::BlendFactor::kSrc1Color: return vk::BlendFactor::eSrc1Color;
		case Prospero::BlendFactor::kOneMinusSrc1Color: return vk::BlendFactor::eOneMinusSrc1Color;
		case Prospero::BlendFactor::kSrc1Alpha: return vk::BlendFactor::eSrc1Alpha;
		case Prospero::BlendFactor::kOneMinusSrc1Alpha: return vk::BlendFactor::eOneMinusSrc1Alpha;
		case Prospero::BlendFactor::kConstantAlpha: return vk::BlendFactor::eConstantAlpha;
		case Prospero::BlendFactor::kOneMinusConstantAlpha:
			return vk::BlendFactor::eOneMinusConstantAlpha;
		default: EXIT("unknown factor: %u\n", factor);
	}
	return vk::BlendFactor::eZero;
}

static vk::BlendOp GetBlendOp(uint32_t op) {
	switch (static_cast<Prospero::BlendOp>(op)) {
		case Prospero::BlendOp::kAdd: return vk::BlendOp::eAdd;
		case Prospero::BlendOp::kSubtract: return vk::BlendOp::eSubtract;
		case Prospero::BlendOp::kMin: return vk::BlendOp::eMin;
		case Prospero::BlendOp::kMax: return vk::BlendOp::eMax;
		case Prospero::BlendOp::kReverseSubtract: return vk::BlendOp::eReverseSubtract;
		default: EXIT("unknown op: %u\n", op);
	}
	return vk::BlendOp::eAdd;
}

static void AddLayoutBindings(std::vector<vk::DescriptorSetLayoutBinding>& descriptor_bindings,
                              const ShaderRecompiler::IR::CompiledShaderInfo& program,
                              vk::ShaderStageFlagBits              stage) {
	for (auto mask = program.bindings.descriptor_mask; mask != 0; mask &= mask - 1) {
		const auto index = std::countr_zero(mask);
		const auto kind = static_cast<ShaderRecompiler::IR::DescriptorBindingKind>(index);
		descriptor_bindings.push_back(
		    {ShaderRecompiler::IR::NativeBinding(program.stage, kind), NativeDescriptorType(kind),
		     program.bindings.descriptor_counts[index], stage, nullptr});
	}
}

// Creates the descriptor set layout and the pipeline layout that uses it.
static void CreateLayouts(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                          std::span<const vk::DescriptorSetLayoutBinding> bindings) {
	const auto& limits = graphics.GetPhysicalDeviceProperties().limits;
	uint64_t descriptor_count = 0;
	uint64_t storage_buffers = 0;
	uint64_t samplers = 0;
	std::array<uint64_t, 32> stage_buffers {}, stage_resources {}, stage_samplers {};
	for (const auto& binding: bindings) {
		descriptor_count += binding.descriptorCount;
		const bool storage = binding.descriptorType == vk::DescriptorType::eStorageBuffer;
		const bool sampler = binding.descriptorType == vk::DescriptorType::eSampler;
		if (storage) storage_buffers += binding.descriptorCount;
		if (sampler) samplers += binding.descriptorCount;
		for (auto stages = static_cast<uint32_t>(binding.stageFlags); stages != 0; stages &= stages - 1u) {
			const auto stage = std::countr_zero(stages);
			if (storage) stage_buffers[stage] += binding.descriptorCount;
			if (sampler) stage_samplers[stage] += binding.descriptorCount;
			else
				stage_resources[stage] += binding.descriptorCount;
			if (stage_buffers[stage] > limits.maxPerStageDescriptorStorageBuffers ||
			    stage_resources[stage] > limits.maxPerStageResources ||
			    stage_samplers[stage] > limits.maxPerStageDescriptorSamplers) {
				EXIT("native descriptor stage %u exceeds device limits: buffers=%" PRIu64
				     "/%u resources=%" PRIu64 "/%u samplers=%" PRIu64 "/%u\n", stage, stage_buffers[stage],
				     limits.maxPerStageDescriptorStorageBuffers, stage_resources[stage], limits.maxPerStageResources,
				     stage_samplers[stage], limits.maxPerStageDescriptorSamplers);
			}
		}
	}
	if (storage_buffers > limits.maxDescriptorSetStorageBuffers) {
		EXIT("native storage-buffer descriptors exceed device limit: %" PRIu64 "/%u\n",
		     storage_buffers, limits.maxDescriptorSetStorageBuffers);
	}
	if (samplers > limits.maxDescriptorSetSamplers) {
		EXIT("native sampler descriptors exceed device limit: %" PRIu64 "/%u\n",
		     samplers, limits.maxDescriptorSetSamplers);
	}
	// Bindless image tables can need more sampled images than the device or a descriptor pool
	// provides: fail clearly instead of creating an invalid layout.
	uint64_t                 sampled_images = 0;
	std::array<uint64_t, 32> stage_sampled {};
	for (const auto& binding: bindings) {
		if (binding.descriptorType != vk::DescriptorType::eSampledImage) continue;
		sampled_images += binding.descriptorCount;
		for (auto stages = static_cast<uint32_t>(binding.stageFlags); stages != 0; stages &= stages - 1u)
			stage_sampled[std::countr_zero(stages)] += binding.descriptorCount;
	}
	const auto max_stage_sampled = *std::ranges::max_element(stage_sampled);
	if (sampled_images > limits.maxDescriptorSetSampledImages ||
	    max_stage_sampled > limits.maxPerStageDescriptorSampledImages ||
	    sampled_images > DescriptorHeap::MaxSampledImages) {
		EXIT("pipeline needs %" PRIu64 " sampled images (%" PRIu64 " in one stage); the device allows "
		     "%u per set and %u per stage, a descriptor pool holds %u\n",
		     sampled_images, max_stage_sampled, limits.maxDescriptorSetSampledImages,
		     limits.maxPerStageDescriptorSampledImages, DescriptorHeap::MaxSampledImages);
	}
	pipeline.uses_push_descriptors = descriptor_count <= graphics.max_push_descriptors;

	vk::DescriptorSetLayoutCreateInfo create {};
	create.flags        = pipeline.uses_push_descriptors
	                          ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
	                          : vk::DescriptorSetLayoutCreateFlags {};
	create.bindingCount = static_cast<uint32_t>(bindings.size());
	create.pBindings    = bindings.data();
	EXIT_IF(graphics.device.createDescriptorSetLayout(
	            &create, nullptr, &pipeline.descriptor_set_layout) != vk::Result::eSuccess);
	if (!pipeline.uses_push_descriptors) {
		DescriptorHeap::RegisterLayout(pipeline.descriptor_set_layout, bindings);
	}
	const vk::PushConstantRange push_constants {pipeline.push_stages, 0,
	                                            ShaderRecompiler::IR::NativePushConstantSize};
	const std::array set_layouts {pipeline.descriptor_set_layout, graphics.image_heap_layout};

	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = pipeline.uses_image_heap ? 2u : 1u;
	pipeline_layout_info.pSetLayouts            = set_layouts.data();
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_constants;

	EXIT_NOT_IMPLEMENTED(graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                                          &pipeline.pipeline_layout) !=
	                     vk::Result::eSuccess);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const GraphicsPipelineKey&                 key,
                            const std::array<const ShaderModule*, 4>& stages,
                            vk::PipelineCache                          driver_cache) {
	const auto& rendering     = key.rendering;
	const auto& vertex_input  = key.vertex_input;
	const auto& static_params = key.static_params;
	const auto& vertex        = *stages[0];
	const auto* pixel         = stages[3];
	const bool  with_depth    = rendering.depth_format != vk::Format::eUndefined ||
	                            rendering.stencil_format != vk::Format::eUndefined;
	const bool  mesh          = vertex.info.stage == ShaderType::Mesh;
	EXIT_NOT_IMPLEMENTED(mesh && !graphics.mesh_shader_enabled);
	const bool rect_list = static_params.patch_control_points != 0 && stages[1] == nullptr;

	vk::ShaderModule tess_control_shader_module = nullptr;
	vk::ShaderModule tess_eval_shader_module    = nullptr;
	const auto       task_module =
	    vertex.mesh_task ? CompileSPV(GPU_MESH_INDIRECT_SPV, graphics.device) : vk::ShaderModule {};

	if (rect_list) {
		std::span<const RectListParameter> parameters;
		if (pixel != nullptr) parameters = pixel->rect_parameters;
		const auto shaders = BuildRectListShaders(parameters, vertex.info.param_export_mask);
		tess_control_shader_module = CompileSPV(shaders.control, graphics.device);
		tess_eval_shader_module    = CompileSPV(shaders.evaluation, graphics.device);
	}

	vk::PipelineShaderStageCreateInfo shader_stages[4] {};
	uint32_t                          shader_stage_count = 0;
	for (const auto* stage: stages) {
		if (stage != nullptr) {
			shader_stages[shader_stage_count++] = {.stage  = NativeShaderStage(stage->info.stage),
			                                       .module = stage->module,
			                                       .pName  = "main"};
		}
	}
	if (task_module) {
		shader_stages[shader_stage_count++] = {.stage  = vk::ShaderStageFlagBits::eTaskEXT,
		                                       .module = task_module,
		                                       .pName  = "main"};
	}
	if (rect_list) {
		shader_stages[shader_stage_count++] = {.stage =
		                                           vk::ShaderStageFlagBits::eTessellationControl,
		                                       .module = tess_control_shader_module,
		                                       .pName  = "main"};
		shader_stages[shader_stage_count++] = {.stage =
		                                           vk::ShaderStageFlagBits::eTessellationEvaluation,
		                                       .module = tess_eval_shader_module,
		                                       .pName  = "main"};
	}

	vk::VertexInputAttributeDescription input_attr[ShaderVertexInputInfo::RES_MAX] {};
	vk::VertexInputBindingDescription   input_desc[ShaderVertexInputInfo::RES_MAX] {};

	for (uint32_t binding = 0; binding < vertex_input.binding_count; binding++) {
		input_desc[binding].binding   = binding;
		input_desc[binding].stride    = vertex_input.bindings[binding].stride;
		input_desc[binding].inputRate = vertex_input.bindings[binding].instance
		                                    ? vk::VertexInputRate::eInstance
		                                    : vk::VertexInputRate::eVertex;
	}
	for (uint32_t index = 0; index < vertex_input.attribute_count; index++) {
		input_attr[index].binding  = vertex_input.attributes[index].binding;
		input_attr[index].location = index;
		input_attr[index].offset   = vertex_input.attributes[index].offset;
		input_attr[index].format   = vertex.vertex_formats[index];
	}

	vk::PipelineVertexInputStateCreateInfo vertex_input_info {};
	vertex_input_info.vertexBindingDescriptionCount   = vertex_input.binding_count;
	vertex_input_info.pVertexBindingDescriptions      = input_desc;
	vertex_input_info.vertexAttributeDescriptionCount = vertex_input.attribute_count;
	vertex_input_info.pVertexAttributeDescriptions    = input_attr;

	vk::PipelineInputAssemblyStateCreateInfo input_assembly {};
	input_assembly.topology = static_params.topology;
	input_assembly.primitiveRestartEnable =
	    static_params.primitive_restart_enable ? VK_TRUE : VK_FALSE;

	vk::PipelineViewportDepthClipControlCreateInfoEXT depth_clip_control {};
	depth_clip_control.negativeOneToOne = (static_params.negative_one_to_one ? VK_TRUE : VK_FALSE);

	vk::PipelineViewportStateCreateInfo viewport_state {};
	viewport_state.pNext = &depth_clip_control;

	vk::PipelineRasterizationDepthClipStateCreateInfoEXT clip_ext {};
	clip_ext.depthClipEnable = static_params.depth_clip_enable ? VK_TRUE : VK_FALSE;

	vk::PipelineRasterizationStateCreateInfo rasterizer {};
	// MoltenVK lacks VK_EXT_depth_clip_enable; omit the depth-clip struct on macOS and accept
	// Vulkan's default depth clipping (enabled) instead of the PS5's clamp behavior.
#if !defined(__APPLE__)
	// The DB clamps depth to the viewport range after polygon offset is applied.
	rasterizer.depthClampEnable = VK_TRUE;
	rasterizer.pNext = &clip_ext;
#endif
	vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT provoking_vertex {};
	EXIT_NOT_IMPLEMENTED(static_params.provoking_vtx_last &&
	                     !graphics.provoking_vertex_last_enabled);
	if (graphics.provoking_vertex_last_enabled) {
		provoking_vertex.provokingVertexMode = static_params.provoking_vtx_last
		    ? vk::ProvokingVertexModeEXT::eLastVertex : vk::ProvokingVertexModeEXT::eFirstVertex;
		provoking_vertex.pNext = rasterizer.pNext;
		rasterizer.pNext = &provoking_vertex;
	}
	rasterizer.polygonMode = static_params.polygon_mode;

	vk::PipelineMultisampleStateCreateInfo multisampling {};
	multisampling.sampleShadingEnable  = static_params.sample_shading_enable ? VK_TRUE : VK_FALSE;
	multisampling.rasterizationSamples = vulkan_sample_count(static_params.samples);
	multisampling.minSampleShading     = 1.0f;
	multisampling.alphaToCoverageEnable =
	    static_params.alpha_to_coverage_enable ? VK_TRUE : VK_FALSE;

	vk::PipelineColorBlendAttachmentState color_blend_attachment[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t i = 0; i < rendering.color_count; i++) {
		EXIT_NOT_IMPLEMENTED((static_params.color_mask[i] & ~0x0fu) != 0);
		color_blend_attachment[i].colorWriteMask =
		    vk::ColorComponentFlags {static_params.color_mask[i]};
		color_blend_attachment[i].blendEnable = static_params.blend_enable[i] ? VK_TRUE : VK_FALSE;
		color_blend_attachment[i].srcColorBlendFactor =
		    GetBlendFactor(static_params.color_srcblend[i]);
		color_blend_attachment[i].dstColorBlendFactor =
		    GetBlendFactor(static_params.color_destblend[i]);
		color_blend_attachment[i].colorBlendOp = GetBlendOp(static_params.color_comb_fcn[i]);
		color_blend_attachment[i].srcAlphaBlendFactor =
		    (static_params.separate_alpha_blend[i]
		         ? GetBlendFactor(static_params.alpha_srcblend[i])
		         : color_blend_attachment[i].srcColorBlendFactor);
		color_blend_attachment[i].dstAlphaBlendFactor =
		    (static_params.separate_alpha_blend[i]
		         ? GetBlendFactor(static_params.alpha_destblend[i])
		         : color_blend_attachment[i].dstColorBlendFactor);
		color_blend_attachment[i].alphaBlendOp =
		    (static_params.separate_alpha_blend[i] ? GetBlendOp(static_params.alpha_comb_fcn[i])
		                                           : color_blend_attachment[i].colorBlendOp);
	}

	vk::PipelineColorBlendStateCreateInfo color_blending {};
	color_blending.attachmentCount = rendering.color_count;
	color_blending.pAttachments    = color_blend_attachment;

	std::vector<vk::DescriptorSetLayoutBinding> descriptor_bindings;
	pipeline.push_stages = vk::ShaderStageFlagBits::eFragment;
	if (task_module) {
		pipeline.push_stages |= vk::ShaderStageFlagBits::eTaskEXT;
	}
	for (const auto* stage: stages) {
		if (stage != nullptr) {
			const auto native_stage = NativeShaderStage(stage->info.stage);
			AddLayoutBindings(descriptor_bindings, stage->info, native_stage);
			pipeline.uses_image_heap |= stage->info.info.uses_image_heap;
			pipeline.push_stages |= native_stage;
		}
	}
	CreateLayouts(graphics, pipeline, descriptor_bindings);

	vk::PipelineDepthStencilStateCreateInfo depth_stencil_info {};

	std::vector<vk::DynamicState> dynamic_states {
	    vk::DynamicState::eViewportWithCount,
	    vk::DynamicState::eScissorWithCount,
	    vk::DynamicState::eLineWidth,
	    vk::DynamicState::eCullMode,
	    vk::DynamicState::eFrontFace,
	    vk::DynamicState::eDepthTestEnable,
	    vk::DynamicState::eDepthWriteEnable,
	    vk::DynamicState::eDepthCompareOp,
	    vk::DynamicState::eDepthBiasEnable,
	    vk::DynamicState::eDepthBias,
	    vk::DynamicState::eStencilTestEnable,
	    vk::DynamicState::eStencilOp,
	    vk::DynamicState::eStencilCompareMask,
	    vk::DynamicState::eStencilReference,
	    vk::DynamicState::eStencilWriteMask,
	    vk::DynamicState::eBlendConstants,
	};
#if !defined(__APPLE__)
	dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
	dynamic_states.push_back(vk::DynamicState::eDepthBounds);
#endif
	if (graphics.attachment_feedback_loop_enabled) {
		dynamic_states.push_back(vk::DynamicState::eAttachmentFeedbackLoopEnableEXT);
	}

	vk::PipelineDynamicStateCreateInfo dynamic_state {};
	dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
	dynamic_state.pDynamicStates    = dynamic_states.data();

	vk::GraphicsPipelineCreateInfo  pipeline_info {};
	vk::PipelineRenderingCreateInfo rendering_info {};
	rendering_info.colorAttachmentCount    = rendering.color_count;
	rendering_info.pColorAttachmentFormats = rendering.color_formats.data();
	rendering_info.depthAttachmentFormat   = rendering.depth_format;
	rendering_info.stencilAttachmentFormat = rendering.stencil_format;
	pipeline_info.pNext                    = &rendering_info;
	pipeline_info.stageCount               = shader_stage_count;
	pipeline_info.pStages                  = shader_stages;
	pipeline_info.pVertexInputState        = mesh ? nullptr : &vertex_input_info;
	pipeline_info.pInputAssemblyState      = mesh ? nullptr : &input_assembly;
	vk::PipelineTessellationStateCreateInfo tessellation_state {};
	tessellation_state.patchControlPoints = static_params.patch_control_points;
	pipeline_info.pTessellationState =
	    static_params.patch_control_points != 0 ? &tessellation_state : nullptr;
	pipeline_info.pViewportState      = &viewport_state;
	pipeline_info.pRasterizationState = &rasterizer;
	pipeline_info.pMultisampleState   = &multisampling;
	pipeline_info.pDepthStencilState  = (with_depth ? &depth_stencil_info : nullptr);
	pipeline_info.pColorBlendState    = &color_blending;
	pipeline_info.pDynamicState       = &dynamic_state;
	pipeline_info.layout              = pipeline.pipeline_layout;

	const auto hash = [&](size_t stage) {
		return stages[stage] != nullptr ? stages[stage]->info.shader_hash : uint64_t {0};
	};
	const auto name = [&](size_t stage, const char* stage_name) {
		return stages[stage] != nullptr ? stage_name : nullptr;
	};
	vk::Result result = vk::Result::eSuccess;
	{
		const ScopedDriverCreation creation(
		    {name(0, mesh ? "ms" : "vs"), name(1, "hs"), name(2, "ds"), name(3, "ps")},
		    {hash(0), hash(1), hash(2), hash(3)});
		result = graphics.device.createGraphicsPipelines(driver_cache, 1, &pipeline_info, nullptr,
		                                                 &pipeline.pipeline);
	}
	if (result != vk::Result::eSuccess) {
		EXIT("vkCreateGraphicsPipelines failed: %s, %s 0x%016" PRIx64 ", hs 0x%016" PRIx64
		     ", ds 0x%016" PRIx64 ", ps 0x%016" PRIx64 "\n",
		     vk::to_string(result).c_str(), mesh ? "ms" : "vs", hash(0), hash(1), hash(2), hash(3));
	}

	graphics.device.destroyShaderModule(tess_control_shader_module, nullptr);
	graphics.device.destroyShaderModule(tess_eval_shader_module, nullptr);
	graphics.device.destroyShaderModule(task_module, nullptr);
}

// With `lookup`, only from the cache (VK_PIPELINE_COMPILE_REQUIRED when it does not hold it).
static vk::Result CreateComputePipeline(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                                        const ShaderModule& module, vk::PipelineCache driver_cache,
                                        bool lookup) {
	vk::PipelineShaderStageCreateInfo                     comp_shader_stage_info {};
	vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo comp_subgroup_size {};
	comp_shader_stage_info.stage  = vk::ShaderStageFlagBits::eCompute;
	comp_shader_stage_info.module = module.module;
	comp_shader_stage_info.pName  = "main";
	const auto wave_size = module.info.wave_size;
	if (graphics.compute_subgroup_size_control_enabled &&
	    wave_size >= graphics.min_subgroup_size && wave_size <= graphics.max_subgroup_size) {
		comp_subgroup_size.requiredSubgroupSize = wave_size;
		comp_shader_stage_info.pNext            = &comp_subgroup_size;
	}

	std::vector<vk::DescriptorSetLayoutBinding> descriptor_bindings;
	AddLayoutBindings(descriptor_bindings, module.info, vk::ShaderStageFlagBits::eCompute);
	pipeline.uses_image_heap = module.info.info.uses_image_heap;
	pipeline.push_stages     = vk::ShaderStageFlagBits::eCompute;
	CreateLayouts(graphics, pipeline, descriptor_bindings);

	vk::ComputePipelineCreateInfo info {};
	if (module.info.info.uses_gds) {
		info.flags = vk::PipelineCreateFlagBits::eDispatchBase;
	}
	if (lookup) {
		info.flags |= vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired;
	}
	info.stage  = comp_shader_stage_info;
	info.layout = pipeline.pipeline_layout;

	const ScopedDriverCreation creation({"cs", nullptr, nullptr, nullptr},
	                                    {module.info.shader_hash, 0, 0, 0});
	return graphics.device.createComputePipelines(driver_cache, 1, &info, nullptr, &pipeline.pipeline);
}

void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const ShaderModule& module, vk::PipelineCache driver_cache) {
	const auto result = CreateComputePipeline(graphics, pipeline, module, driver_cache, false);
	if (result != vk::Result::eSuccess) {
		EXIT("vkCreateComputePipelines failed: %s, cs 0x%016" PRIx64 "\n", vk::to_string(result).c_str(),
		     module.info.shader_hash);
	}
}

bool FindComputePipeline(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                         const ShaderModule& module, vk::PipelineCache cache) {
	if (!graphics.pipeline_cache_lookup_enabled || cache == nullptr) {
		return false;
	}
	if (CreateComputePipeline(graphics, pipeline, module, cache, true) == vk::Result::eSuccess &&
	    pipeline.pipeline != nullptr) {
		return true;
	}
	graphics.device.destroyPipeline(pipeline.pipeline, nullptr);
	graphics.device.destroyPipelineLayout(pipeline.pipeline_layout, nullptr);
	graphics.device.destroyDescriptorSetLayout(pipeline.descriptor_set_layout, nullptr);
	pipeline = {};
	return false;
}

} // namespace Libs::Graphics
