#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/hostLowering.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/rectListShader.h"
#include "graphics/shader/shader.h"
#include "graphics/shader/triangleVertexValueShader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace Libs::Graphics {

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

static void CreateDescriptorLayout(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                                   std::span<const vk::DescriptorSetLayoutBinding> bindings) {
	const auto&              limits = graphics.GetPhysicalDeviceProperties().limits;
	DescriptorLayoutShape    shape;
	uint64_t                 samplers = 0;
	std::array<uint64_t, 32> stage_buffers {}, stage_resources {}, stage_samplers {};
	for (const auto& binding: bindings) {
		shape.descriptors += binding.descriptorCount;
		const bool storage = binding.descriptorType == vk::DescriptorType::eStorageBuffer;
		const bool sampler = binding.descriptorType == vk::DescriptorType::eSampler;
		if (storage) {
			shape.storage_buffers += binding.descriptorCount;
		}
		if (sampler) {
			samplers += binding.descriptorCount;
		}
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
	if (shape.storage_buffers > limits.maxDescriptorSetStorageBuffers) {
		EXIT("native storage-buffer descriptors exceed device limit: %" PRIu64 "/%u\n",
		     static_cast<uint64_t>(shape.storage_buffers), limits.maxDescriptorSetStorageBuffers);
	}
	if (samplers > limits.maxDescriptorSetSamplers) {
		EXIT("native sampler descriptors exceed device limit: %" PRIu64 "/%u\n",
		     samplers, limits.maxDescriptorSetSamplers);
	}
	pipeline.uses_push_descriptors =
	    ChooseDescriptorDelivery(graphics.host, graphics.max_push_descriptors, shape) ==
	    DescriptorDelivery::Pushed;

	vk::DescriptorSetLayoutCreateInfo create {};
	create.flags        = pipeline.uses_push_descriptors
	                          ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
	                          : vk::DescriptorSetLayoutCreateFlags {};
	create.bindingCount = static_cast<uint32_t>(bindings.size());
	create.pBindings    = bindings.data();
	EXIT_IF(graphics.device.createDescriptorSetLayout(
	            &create, nullptr, &pipeline.descriptor_set_layout) != vk::Result::eSuccess);
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
	// TODO(upstream merge stage 2): mesh draws on a host without mesh shaders (a compute shader
	// writes the records, see capturedVertexLayout.h) and per-vertex values on a host without
	// per-vertex inputs (triangleVertexValueShader.h) come back on the merged pipeline cache.
	EXIT_NOT_IMPLEMENTED(mesh && !graphics.host.capabilities.mesh_shader);
	const bool mesh_emulated = false;
	const bool rect_list = static_params.patch_control_points != 0 && stages[1] == nullptr;

	vk::ShaderModule tess_control_shader_module = nullptr;
	vk::ShaderModule tess_eval_shader_module    = nullptr;

	if (rect_list) {
		std::span<const RectListParameter> parameters;
		if (pixel != nullptr) parameters = pixel->rect_parameters;
		const auto shaders = BuildRectListShaders(
		    parameters, vertex.info.param_export_mask,
		    ShaderRecompiler::Spirv::VertexClipDistanceCount(vertex.info.stage, vertex.info.info));
		tess_control_shader_module = CompileSPV(shaders.control, graphics.device);
		if (graphics_debug_dump_enabled()) {
			LOGF("PipelineTrace: vkCreateShaderModule RectList TCS done module=%p\n",
			     static_cast<void*>(tess_control_shader_module));
		}

		tess_eval_shader_module = CompileSPV(shaders.evaluation, graphics.device);
		if (graphics_debug_dump_enabled()) {
			LOGF("PipelineTrace: vkCreateShaderModule RectList TES done module=%p\n",
			     static_cast<void*>(tess_eval_shader_module));
		}
	}

	EXIT_NOT_IMPLEMENTED(
	    rect_list && (tess_control_shader_module == nullptr || tess_eval_shader_module == nullptr));

	vk::PipelineShaderStageCreateInfo shader_stages[4] {};
	uint32_t                          shader_stage_count = 0;
	for (const auto* stage: stages) {
		if (stage != nullptr) {
			shader_stages[shader_stage_count++] = {.stage  = NativeShaderStage(stage->info.stage),
			                                       .module = stage->module,
			                                       .pName  = "main"};
		}
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
	// An emulated mesh draw is a triangle list without vertex input.
	input_assembly.topology =
	    mesh_emulated ? vk::PrimitiveTopology::eTriangleList : static_params.topology;
	input_assembly.primitiveRestartEnable =
	    !mesh_emulated && static_params.primitive_restart_enable ? VK_TRUE : VK_FALSE;

	vk::PipelineViewportDepthClipControlCreateInfoEXT depth_clip_control {};
	depth_clip_control.negativeOneToOne = (static_params.negative_one_to_one ? VK_TRUE : VK_FALSE);

	vk::PipelineViewportStateCreateInfo viewport_state {};
	viewport_state.pNext = &depth_clip_control;

	vk::PipelineRasterizationDepthClipStateCreateInfoEXT clip_ext {};
	clip_ext.depthClipEnable = static_params.depth_clip_enable ? VK_TRUE : VK_FALSE;

	vk::PipelineRasterizationStateCreateInfo rasterizer {};
	const auto&                              host = graphics.host.capabilities;
	// The DB clamps depth to the viewport range after polygon offset is applied, and clips
	// against the near and far planes only when the guest asks for it. Vulkan turns clipping off
	// when it clamps, unless VK_EXT_depth_clip_enable sets the two apart.
	if (host.depth_clamp && host.depth_clip_enable) {
		rasterizer.depthClampEnable = VK_TRUE;
		rasterizer.pNext            = &clip_ext;
	} else if (!static_params.depth_clip_enable) {
		// No clipping and a clamp: what depth clamping alone does.
		if (!HostDepthIsExact(host, false, false)) {
			EXIT("draw with depth clipping off needs depthClamp, which the host GPU does not "
			     "enable\n");
		}
		rasterizer.depthClampEnable = VK_TRUE;
	}
	// Clipping on without VK_EXT_depth_clip_enable: clipped as the guest asks and not clamped.
	// Exact while polygon offset does not move depth past the range (HostDepthIsExact); the draw
	// warns once when it does not hold, and goes on.
	vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT provoking_vertex {};
	// The vertex shader of a captured mesh draw puts the guest's provoking vertex first, so such
	// a draw takes flat values from the first vertex on every host.
	const bool provoking_last = static_params.provoking_vtx_last && !mesh_emulated;
	EXIT_NOT_IMPLEMENTED(provoking_last && !graphics.host.capabilities.provoking_vertex_last);
	if (graphics.host.capabilities.provoking_vertex_last) {
		provoking_vertex.provokingVertexMode = provoking_last
		                                           ? vk::ProvokingVertexModeEXT::eLastVertex
		                                           : vk::ProvokingVertexModeEXT::eFirstVertex;
		provoking_vertex.pNext = rasterizer.pNext;
		rasterizer.pNext = &provoking_vertex;
	}
	rasterizer.polygonMode = static_params.polygon_mode;
	rasterizer.lineWidth = 1.0f;

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

	vk::Bool32 color_write_enable[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t i = 0; i < rendering.color_count; i++) {
		color_write_enable[i] = VK_TRUE;
	}

	vk::PipelineColorWriteCreateInfoEXT color_write {};
	color_write.attachmentCount    = rendering.color_count;
	color_write.pColorWriteEnables = color_write_enable;

	vk::PipelineColorBlendStateCreateInfo color_blending {};
	// Colour writes are turned off at draw time only for a slot with no image, and a slot with no
	// image takes no writes anyway. So without VK_EXT_color_write_enable nothing is lost.
	if (host.color_write_enable) {
		color_blending.pNext = &color_write;
	}
	color_blending.logicOp         = vk::LogicOp::eCopy;
	color_blending.attachmentCount = rendering.color_count;
	color_blending.pAttachments    = color_blend_attachment;

	std::vector<vk::DescriptorSetLayoutBinding> descriptor_bindings;
	vk::ShaderStageFlags graphics_stages = vk::ShaderStageFlagBits::eFragment;
	for (const auto* stage: stages) {
		if (stage != nullptr) {
			const auto native_stage = NativeShaderStage(stage->info.stage);
			AddLayoutBindings(descriptor_bindings, stage->info, native_stage);
			pipeline.uses_image_heap |= stage->info.info.uses_image_heap;
			graphics_stages |= native_stage;
		}
	}
	CreateDescriptorLayout(graphics, pipeline, descriptor_bindings);
	// An emulated mesh program is bound to the compute pipeline: the graphics layout pushes to the
	// pixel shader, and to the vertex shader its record address. One range covers every stage, so
	// a push of any of them names the same stages.
	static_assert(ShaderRecompiler::IR::PushData::MeshEmulatedDrawDwordCount * sizeof(uint32_t) <=
	              ShaderRecompiler::IR::NativePushConstantSize);
	pipeline.push_constant_stages =
	    mesh_emulated ? vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment
	                  : graphics_stages;
	const vk::PushConstantRange push_constants {pipeline.push_constant_stages, 0,
	                                            ShaderRecompiler::IR::NativePushConstantSize};
	const std::array set_layouts {pipeline.descriptor_set_layout, graphics.image_heap_layout};

	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = pipeline.uses_image_heap ? 2u : 1u;
	pipeline_layout_info.pSetLayouts            = set_layouts.data();
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_constants;

	EXIT_IF(pipeline.pipeline_layout != nullptr);

	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreatePipelineLayout begin VS=%" PRIu64 " PS=%" PRIu64
		     " set_layouts=1 push_constants=%" PRIu32 "\n",
		     key.stage_ids[0], key.stage_ids[3], 1u);
	}
	auto result = graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                                   &pipeline.pipeline_layout);
	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreatePipelineLayout done result=%s layout=%p\n",
		     vk::to_string(result).c_str(), static_cast<void*>(pipeline.pipeline_layout));
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

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
	if (host.depth_bounds) {
		dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
		dynamic_states.push_back(vk::DynamicState::eDepthBounds);
	}
	if (host.color_write_enable && rendering.color_count != 0) {
		dynamic_states.push_back(vk::DynamicState::eColorWriteEnableEXT);
	}
	if (graphics.host.capabilities.attachment_feedback_loop) {
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
	const vk::PipelineVertexInputStateCreateInfo no_vertex_input {};
	pipeline_info.pVertexInputState =
	    mesh_emulated ? &no_vertex_input : (mesh ? nullptr : &vertex_input_info);
	pipeline_info.pInputAssemblyState = mesh && !mesh_emulated ? nullptr : &input_assembly;
	vk::PipelineTessellationStateCreateInfo tessellation_state {};
	tessellation_state.patchControlPoints = static_params.patch_control_points;
	pipeline_info.pTessellationState =
	    static_params.patch_control_points != 0 ? &tessellation_state : nullptr;
	pipeline_info.pViewportState          = &viewport_state;
	pipeline_info.pRasterizationState     = &rasterizer;
	pipeline_info.pMultisampleState       = &multisampling;
	pipeline_info.pDepthStencilState      = (with_depth ? &depth_stencil_info : nullptr);
	pipeline_info.pColorBlendState        = &color_blending;
	pipeline_info.pDynamicState           = &dynamic_state;
	pipeline_info.layout                  = pipeline.pipeline_layout;
	pipeline_info.basePipelineIndex       = -1;

	EXIT_IF(pipeline.pipeline != nullptr);

	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreateGraphicsPipelines begin VS=%" PRIu64 " PS=%" PRIu64
		     " topology=%" PRIu32 " color_mask=0x%08" PRIx32
		     " depth=%s blend=%s dyn_states=%" PRIu32 "\n",
		     key.stage_ids[0], key.stage_ids[3],
		     static_cast<uint32_t>(static_params.topology), static_params.color_mask[0],
		     (with_depth ? "true" : "false"), (static_params.blend_enable[0] ? "true" : "false"),
		     dynamic_state.dynamicStateCount);
	}
	result = graphics.device.createGraphicsPipelines(driver_cache, 1, &pipeline_info, nullptr,
	                                                 &pipeline.pipeline);
	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreateGraphicsPipelines done result=%s pipeline=%p\n",
		     vk::to_string(result).c_str(), static_cast<void*>(pipeline.pipeline));
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	graphics.device.destroyShaderModule(tess_control_shader_module, nullptr);
	graphics.device.destroyShaderModule(tess_eval_shader_module, nullptr);
}

// KYTY_PIPELINE_IR=<guest shader hashes in hex, comma-separated>: the compute pipelines of those
// shaders.
static bool CapturesPipelineIr(const GraphicContext& graphics, uint64_t shader_hash) {
	static const char* wanted = std::getenv("KYTY_PIPELINE_IR");
	if (!graphics.pipeline_executable_info_enabled || wanted == nullptr) return false;
	for (const char* cursor = wanted; *cursor != '\0';) {
		char* end = nullptr;
		if (std::strtoull(cursor, &end, 16) == shader_hash) return true;
		if (end == cursor) break;
		cursor = *end == ',' ? end + 1 : end;
	}
	return false;
}

// Writes pipeline_ir_<hash>_<n>.txt in the working directory: each executable's statistics and
// the text of its internal representations (for RADV: NIR, ACO IR and the assembly).
static void DumpPipelineIr(GraphicContext& graphics, vk::Pipeline pipeline, uint64_t shader_hash) {
	static std::atomic<uint32_t> count {0};
	const auto path = fmt::format("pipeline_ir_{:016x}_{}.txt", shader_hash, count++);
	FILE* out = std::fopen(path.c_str(), "w");
	if (out == nullptr) return;
	const vk::PipelineInfoKHR pipeline_info {.pipeline = pipeline};
	const auto executables = graphics.device.getPipelineExecutablePropertiesKHR(pipeline_info).value;
	for (uint32_t index = 0; index < executables.size(); ++index) {
		std::fprintf(out, "=== %s: %s (subgroup %u)\n", executables[index].name.data(),
		             executables[index].description.data(), executables[index].subgroupSize);
		const vk::PipelineExecutableInfoKHR executable {.pipeline = pipeline, .executableIndex = index};
		for (const auto& stat: graphics.device.getPipelineExecutableStatisticsKHR(executable).value) {
			std::fprintf(out, "%s: %llu\n", stat.name.data(),
			             static_cast<unsigned long long>(stat.value.u64));
		}
		uint32_t ir_count = 0;
		(void)graphics.device.getPipelineExecutableInternalRepresentationsKHR(&executable, &ir_count, nullptr);
		std::vector<vk::PipelineExecutableInternalRepresentationKHR> irs(ir_count);
		(void)graphics.device.getPipelineExecutableInternalRepresentationsKHR(&executable, &ir_count, irs.data());
		std::vector<std::vector<char>> texts(ir_count);
		for (uint32_t i = 0; i < ir_count; ++i) {
			texts[i].resize(irs[i].dataSize);
			irs[i].pData = texts[i].data();
		}
		(void)graphics.device.getPipelineExecutableInternalRepresentationsKHR(&executable, &ir_count, irs.data());
		for (uint32_t i = 0; i < ir_count; ++i) {
			std::fprintf(out, "--- %s: %s\n", irs[i].name.data(), irs[i].description.data());
			if (irs[i].isText) std::fwrite(texts[i].data(), 1, strnlen(texts[i].data(), texts[i].size()), out);
			std::fprintf(out, "\n");
		}
	}
	std::fclose(out);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const ShaderModule& module, vk::PipelineCache driver_cache) {
	vk::PipelineShaderStageCreateInfo                     comp_shader_stage_info {};
	vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo comp_subgroup_size {};
	comp_shader_stage_info.stage  = vk::ShaderStageFlagBits::eCompute;
	comp_shader_stage_info.module = module.module;
	comp_shader_stage_info.pName  = "main";
	const auto wave_size = module.info.wave_size;
	if (graphics.host.capabilities.compute_subgroup_size_control &&
	    wave_size >= graphics.min_subgroup_size && wave_size <= graphics.max_subgroup_size) {
		comp_subgroup_size.requiredSubgroupSize = wave_size;
		comp_shader_stage_info.pNext            = &comp_subgroup_size;
	}

	std::vector<vk::DescriptorSetLayoutBinding> descriptor_bindings;
	AddLayoutBindings(descriptor_bindings, module.info, vk::ShaderStageFlagBits::eCompute);
	pipeline.uses_image_heap = module.info.info.uses_image_heap;
	CreateDescriptorLayout(graphics, pipeline, descriptor_bindings);
	const vk::PushConstantRange push_constants {vk::ShaderStageFlagBits::eCompute, 0,
	                                            ShaderRecompiler::IR::NativePushConstantSize};
	const std::array set_layouts {pipeline.descriptor_set_layout, graphics.image_heap_layout};

	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = pipeline.uses_image_heap ? 2u : 1u;
	pipeline_layout_info.pSetLayouts            = set_layouts.data();
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_constants;

	EXIT_IF(pipeline.pipeline_layout != nullptr);

	auto result = graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                                  &pipeline.pipeline_layout);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	vk::ComputePipelineCreateInfo info {};
	if (module.info.info.uses_gds) {
		info.flags = vk::PipelineCreateFlagBits::eDispatchBase;
	}
	info.stage             = comp_shader_stage_info;
	info.layout            = pipeline.pipeline_layout;
	info.basePipelineIndex = -1;

	EXIT_IF(pipeline.pipeline != nullptr);

	const bool capture = CapturesPipelineIr(graphics, module.info.shader_hash);
	if (capture) {
		info.flags |= vk::PipelineCreateFlagBits::eCaptureStatisticsKHR |
		              vk::PipelineCreateFlagBits::eCaptureInternalRepresentationsKHR;
	}
	// A pipeline from the driver's cache may come without its internal representations.
	result = graphics.device.createComputePipelines(capture ? nullptr : driver_cache, 1, &info, nullptr,
	                                                &pipeline.pipeline);
	if (capture && result == vk::Result::eSuccess) {
		DumpPipelineIr(graphics, pipeline.pipeline, module.info.shader_hash);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

} // namespace Libs::Graphics
