#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/descriptorCache.h"
#include "graphics/host_gpu/renderer/pipelineCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/renderer/shaderSubgroup.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <limits>
#include <span>

namespace Libs::Graphics {

// IDK: maybe we can remove it?
constexpr uint32_t kTemporaryPs5BufferFormat121 = 121u;

static bool NarrowInputFormat(vk::Format* format, uint32_t* size, uint32_t used_components) {
	EXIT_IF(format == nullptr);
	EXIT_IF(size == nullptr);

	if (used_components == 0 || used_components >= *size) {
		return false;
	}

	switch (*format) {
		case vk::Format::eR32G32B32A32Sfloat:
			switch (used_components) {
				case 1: *format = vk::Format::eR32Sfloat; break;
				case 2: *format = vk::Format::eR32G32Sfloat; break;
				case 3: *format = vk::Format::eR32G32B32Sfloat; break;
				default: return false;
			}
			*size = used_components;
			return true;
		case vk::Format::eR32G32B32Sfloat:
			switch (used_components) {
				case 1: *format = vk::Format::eR32Sfloat; break;
				case 2: *format = vk::Format::eR32G32Sfloat; break;
				default: return false;
			}
			*size = used_components;
			return true;
		case vk::Format::eR16G16B16A16Sfloat:
			switch (used_components) {
				case 1: *format = vk::Format::eR16Sfloat; break;
				case 2: *format = vk::Format::eR16G16Sfloat; break;
				default: return false;
			}
			*size = used_components;
			return true;
		case vk::Format::eR8G8B8A8Unorm:
			switch (used_components) {
				case 1: *format = vk::Format::eR8Unorm; break;
				case 2: *format = vk::Format::eR8G8Unorm; break;
				default: return false;
			}
			*size = used_components;
			return true;
		case vk::Format::eR8G8B8A8Snorm:
			if (used_components != 2) {
				return false;
			}
			*format = vk::Format::eR8G8Snorm;
			*size   = 2;
			return true;
		case vk::Format::eR8G8B8A8Uint:
			switch (used_components) {
				case 1: *format = vk::Format::eR8Uint; break;
				case 2: *format = vk::Format::eR8G8Uint; break;
				default: return false;
			}
			*size = used_components;
			return true;
		default: break;
	}

	return false;
}

static void GetInputFormat(const ShaderBufferResource& res, vk::Format* format, uint32_t* size,
                           uint32_t used_components) {
	EXIT_IF(format == nullptr);
	EXIT_IF(size == nullptr);

	auto fmt = res.Format();
	if (fmt == Prospero::GpuEnumValue(Prospero::VertexAttribFormat::k16_16SInt)) {
		static bool logged_113 = false;
		if (!logged_113) {
			LOGF("InputFormat: temporary: accepting invalid PS5 buffer format 113 as "
			     "vk::Format::eR32G32B32A32Sfloat\n");
			logged_113 = true;
		}
		*format = vk::Format::eR32G32B32A32Sfloat;
		*size   = 4;
		if (NarrowInputFormat(format, size, used_components)) {
			LOGF("InputFormat: narrowing fmt=%u to %s for used_components=%u\n", fmt,
			     VulkanToString(*format).c_str(), used_components);
		}
		return;
	}
	if (fmt == kTemporaryPs5BufferFormat121) {
		static bool logged_121 = false;
		if (!logged_121) {
			LOGF("InputFormat: accepting PS5 buffer format 121 as vk::Format::eR16G16Sfloat\n");
			logged_121 = true;
		}
		*format = vk::Format::eR16G16Sfloat;
		*size   = 2;
		if (NarrowInputFormat(format, size, used_components)) {
			LOGF("InputFormat: narrowing fmt=%u to %s for used_components=%u\n", fmt,
			     VulkanToString(*format).c_str(), used_components);
		}
		return;
	}

	switch (static_cast<Prospero::BufferFormat>(fmt)) {
		case Prospero::BufferFormat::k32_32_32_32Float:
			*format = vk::Format::eR32G32B32A32Sfloat;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k32_32_32_32SInt:
			*format = vk::Format::eR32G32B32A32Sint;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k32_32_32_32UInt:
			*format = vk::Format::eR32G32B32A32Uint;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k32_32_32Float:
			*format = vk::Format::eR32G32B32Sfloat;
			*size   = 3;
			break;
		case Prospero::BufferFormat::k32_32_32SInt:
			*format = vk::Format::eR32G32B32Sint;
			*size   = 3;
			break;
		case Prospero::BufferFormat::k32_32_32UInt:
			*format = vk::Format::eR32G32B32Uint;
			*size   = 3;
			break;
		case Prospero::BufferFormat::k16_16_16_16Float:
			*format = vk::Format::eR16G16B16A16Sfloat;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k16_16_16_16SInt:
			*format = vk::Format::eR16G16B16A16Sint;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k16_16_16_16UInt:
			*format = vk::Format::eR16G16B16A16Uint;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k16_16_16_16SScaled:
			*format = vk::Format::eR16G16B16A16Sscaled;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k16_16_16_16UScaled:
			*format = vk::Format::eR16G16B16A16Uscaled;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k16_16_16_16SNorm:
			*format = vk::Format::eR16G16B16A16Snorm;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k16_16_16_16UNorm:
			*format = vk::Format::eR16G16B16A16Unorm;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k32_32Float:
			*format = vk::Format::eR32G32Sfloat;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k32_32SInt:
			*format = vk::Format::eR32G32Sint;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k32_32UInt:
			*format = vk::Format::eR32G32Uint;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k8_8_8_8UInt:
			*format = vk::Format::eR8G8B8A8Uint;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k8_8_8_8SScaled:
			*format = vk::Format::eR8G8B8A8Sscaled;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k8_8_8_8UScaled:
			*format = vk::Format::eR8G8B8A8Uscaled;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k8_8_8_8SNorm:
			*format = vk::Format::eR8G8B8A8Snorm;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k8_8_8_8UNorm:
			*format = vk::Format::eR8G8B8A8Unorm;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k10_10_10_2UNorm:
			*format = vk::Format::eA2B10G10R10UnormPack32;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k10_10_10_2SNorm:
			*format = vk::Format::eA2B10G10R10SnormPack32;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k16_16Float:
			*format = vk::Format::eR16G16Sfloat;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k16_16SInt:
			*format = vk::Format::eR16G16Sint;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k16_16UInt:
			*format = vk::Format::eR16G16Uint;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k16_16SScaled:
			*format = vk::Format::eR16G16Sscaled;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k16_16UScaled:
			*format = vk::Format::eR16G16Uscaled;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k16_16SNorm:
			*format = vk::Format::eR16G16Snorm;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k16_16UNorm:
			*format = vk::Format::eR16G16Unorm;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k32Float:
			*format = vk::Format::eR32Sfloat;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k32SInt:
			*format = vk::Format::eR32Sint;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k32UInt:
			*format = vk::Format::eR32Uint;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k8_8SInt:
			*format = vk::Format::eR8G8Sint;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k8_8UInt:
			*format = vk::Format::eR8G8Uint;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k8_8SScaled:
			*format = vk::Format::eR8G8Sscaled;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k8_8UScaled:
			*format = vk::Format::eR8G8Uscaled;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k8_8SNorm:
			*format = vk::Format::eR8G8Snorm;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k8_8UNorm:
			*format = vk::Format::eR8G8Unorm;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k16Float:
			*format = vk::Format::eR16Sfloat;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k16SInt:
			*format = vk::Format::eR16Sint;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k16UInt:
			*format = vk::Format::eR16Uint;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k16SScaled:
			*format = vk::Format::eR16Sscaled;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k16UScaled:
			*format = vk::Format::eR16Uscaled;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k16SNorm:
			*format = vk::Format::eR16Snorm;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k16UNorm:
			*format = vk::Format::eR16Unorm;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k8SInt:
			*format = vk::Format::eR8Sint;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k8UInt:
			*format = vk::Format::eR8Uint;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k8SScaled:
			*format = vk::Format::eR8Sscaled;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k8UScaled:
			*format = vk::Format::eR8Uscaled;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k8SNorm:
			*format = vk::Format::eR8Snorm;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k8UNorm:
			*format = vk::Format::eR8Unorm;
			*size   = 1;
			break;
		default:
			EXIT("unknown format: fmt = %u\n", fmt);
			*format = vk::Format::eUndefined;
			*size   = 4;
			break;
	}

	if (NarrowInputFormat(format, size, used_components)) {
		static std::atomic<uint64_t> log_count = 0;
		auto                         log_id    = log_count.fetch_add(1);
		if (log_id < 32) {
			LOGF("VertexInput: narrowed vertex format to %" PRIu32
			     " component(s) for shader fetch\n",
			     used_components);
		}
	}
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

static void CreateLayout(vk::DescriptorSetLayout* set_layouts, uint32_t* set_layouts_num,
                         vk::PushConstantRange*               push_constant_info,
                         uint32_t*                            push_constant_info_num,
                         const ShaderRecompiler::IR::Program& program,
                         vk::ShaderStageFlags vk_stage, DescriptorCache::Stage stage) {
	EXIT_IF(set_layouts == nullptr);
	EXIT_IF(set_layouts_num == nullptr);
	EXIT_IF(push_constant_info == nullptr);
	EXIT_IF(push_constant_info_num == nullptr);

	const auto& bindings        = program.bindings;
	const bool  need_descriptor = !bindings.descriptors.empty();

	if (bindings.push_constant_size != 0) {
		auto index = *push_constant_info_num;

		push_constant_info[index].stageFlags = vk_stage;
		push_constant_info[index].offset     = bindings.push_constant_offset;
		push_constant_info[index].size       = bindings.push_constant_size;
		(*push_constant_info_num)++;
	}

	if (need_descriptor) {
		EXIT_IF(bindings.descriptor_set != *set_layouts_num);

		set_layouts[*set_layouts_num] =
		    g_render_ctx->GetDescriptorCache()->GetDescriptorSetLayout(stage, program);
		(*set_layouts_num)++;
	}
}

static void ConfigureSubgroupSize(const GraphicContext* gctx, vk::ShaderStageFlagBits vk_stage,
                                  const ShaderRecompiler::IR::Program&                   program,
                                  vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo* required,
                                  vk::PipelineShaderStageCreateInfo*                     stage) {
	EXIT_IF(gctx == nullptr || required == nullptr || stage == nullptr);
	const auto config = ConfigureShaderSubgroup(*gctx, vk_stage, program);
	switch (config.mode) {
		case ShaderSubgroupMode::Natural: return;
		case ShaderSubgroupMode::PerInvocationGraphics: {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
				LOGF("Vulkan running %u-lane graphics shader stage 0x%08x with per-invocation "
				     "EXEC/VCC on the host-native subgroup\n",
				     program.wave_size, static_cast<uint32_t>(vk_stage));
			}
			return;
		}
		case ShaderSubgroupMode::FlattenedMasks: {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
				LOGF("Vulkan running %u-lane shader stage 0x%08x with flattened per-invocation "
				     "EXEC/VCC\n",
				     program.wave_size, static_cast<uint32_t>(vk_stage));
			}
			return;
		}
		case ShaderSubgroupMode::Controlled: break;
		case ShaderSubgroupMode::Unsupported:
		default:
			EXIT("Vulkan cannot run %u-lane shader stage 0x%08x: default=%u min=%u max=%u "
			     "controlled_stages=0x%08x\n",
			     program.wave_size, static_cast<uint32_t>(vk_stage), gctx->subgroup_size,
			     gctx->min_subgroup_size, gctx->max_subgroup_size,
			     static_cast<vk::ShaderStageFlags::MaskType>(gctx->required_subgroup_size_stages));
	}

	required->sType = vk::StructureType::ePipelineShaderStageRequiredSubgroupSizeCreateInfo;
	required->requiredSubgroupSize = config.required_size;
	stage->pNext                   = required;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void CreatePipelineInternal(PipelineCache::GraphicsPipeline* pipeline, vk::RenderPass render_pass,
                            const ShaderVertexInputInfo*    vs_input_info,
                            std::span<const uint32_t>       vs_shader,
                            const ShaderPixelInputInfo*     ps_input_info,
                            std::span<const uint32_t>       ps_shader,
                            const PipelineStaticParameters& static_params, uint32_t vs_hash0,
                            uint32_t vs_crc32, uint32_t ps_hash0, uint32_t ps_crc32,
                            bool ps_active) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(pipeline == nullptr);
	EXIT_IF(render_pass == nullptr);
	EXIT_IF(ps_active && ps_input_info == nullptr);

	auto* gctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(gctx == nullptr);

	vk::ShaderModule vert_shader_module = nullptr;
	vk::ShaderModule frag_shader_module = nullptr;

	vk::ShaderModuleCreateInfo create_info {};

	create_info.sType = vk::StructureType::eShaderModuleCreateInfo;
	create_info.pNext = nullptr;
	create_info.flags = {};

	create_info.codeSize = vs_shader.size() * 4;
	create_info.pCode    = vs_shader.data();
	auto result = gctx->device.createShaderModule(&create_info, nullptr, &vert_shader_module);
	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreateShaderModule VS done result=%s module=%p\n",
		     VulkanToString(result).c_str(), static_cast<void*>(vert_shader_module));
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	if (ps_active) {
		create_info.codeSize = ps_shader.size() * 4;
		create_info.pCode    = ps_shader.data();
		result = gctx->device.createShaderModule(&create_info, nullptr, &frag_shader_module);
		if (graphics_debug_dump_enabled()) {
			LOGF("PipelineTrace: vkCreateShaderModule PS done result=%s module=%p\n",
			     VulkanToString(result).c_str(), static_cast<void*>(frag_shader_module));
		}
		EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	}

	EXIT_NOT_IMPLEMENTED(vert_shader_module == nullptr);
	EXIT_NOT_IMPLEMENTED(ps_active && frag_shader_module == nullptr);

	vk::PipelineShaderStageCreateInfo                     vert_shader_stage_info {};
	vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo vert_subgroup_size {};

	vert_shader_stage_info.sType               = vk::StructureType::ePipelineShaderStageCreateInfo;
	vert_shader_stage_info.pNext               = nullptr;
	vert_shader_stage_info.flags               = {};
	vert_shader_stage_info.stage               = vk::ShaderStageFlagBits::eVertex;
	vert_shader_stage_info.module              = vert_shader_module;
	vert_shader_stage_info.pName               = "main";
	vert_shader_stage_info.pSpecializationInfo = nullptr;
	EXIT_IF(!vs_input_info->stage);
	ConfigureSubgroupSize(gctx, vk::ShaderStageFlagBits::eVertex, *vs_input_info->stage.program,
	                      &vert_subgroup_size, &vert_shader_stage_info);

	vk::PipelineShaderStageCreateInfo                     frag_shader_stage_info {};
	vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo frag_subgroup_size {};
	frag_shader_stage_info.sType               = vk::StructureType::ePipelineShaderStageCreateInfo;
	frag_shader_stage_info.pNext               = nullptr;
	frag_shader_stage_info.flags               = {};
	frag_shader_stage_info.stage               = vk::ShaderStageFlagBits::eFragment;
	frag_shader_stage_info.module              = frag_shader_module;
	frag_shader_stage_info.pName               = "main";
	frag_shader_stage_info.pSpecializationInfo = nullptr;
	if (ps_active) {
		EXIT_IF(!ps_input_info->stage);
		ConfigureSubgroupSize(gctx, vk::ShaderStageFlagBits::eFragment,
		                      *ps_input_info->stage.program, &frag_subgroup_size,
		                      &frag_shader_stage_info);
	}

	vk::PipelineShaderStageCreateInfo shader_stages[]    = {vert_shader_stage_info,
	                                                        frag_shader_stage_info};
	const uint32_t                    shader_stage_count = ps_active ? 2u : 1u;

	vk::VertexInputAttributeDescription input_attr[ShaderVertexInputInfo::RES_MAX];
	vk::VertexInputBindingDescription   input_desc[ShaderVertexInputInfo::RES_MAX];

	for (int bi = 0; bi < vs_input_info->buffers_num; bi++) {
		const auto& b          = vs_input_info->buffers[bi];
		input_desc[bi].binding = bi;
		input_desc[bi].stride  = b.stride;
		input_desc[bi].inputRate =
		    (b.fetch_index == 0 ? vk::VertexInputRate::eVertex : vk::VertexInputRate::eInstance);

		for (int ai = 0; ai < b.attr_num; ai++) {
			auto index                 = b.attr_indices[ai];
			input_attr[index].binding  = bi;
			input_attr[index].location = index;
			input_attr[index].offset   = b.attr_offsets[ai];

			uint32_t attr_size       = 4;
			auto     registers_num   = vs_input_info->resources_dst[index].registers_num;
			auto     used_components = (vs_input_info->resource_fetch_components[index] > 0
			                                ? vs_input_info->resource_fetch_components[index]
			                                : registers_num);
			GetInputFormat(vs_input_info->resources[index], &input_attr[index].format, &attr_size,
			               static_cast<uint32_t>(used_components));

			if (graphics_debug_dump_enabled()) {
				static std::atomic_uint log_count = 0;
				const auto              log_id = log_count.fetch_add(1, std::memory_order_relaxed);
				if (log_id < 128) {
					LOGF("VertexInputState[%u]: attr=%u binding=%u offset=%u stride=%u fmt=%d "
					     "src_fmt=%u dst=v%u regs=%u"
					     " fetched_components=%u attr_size=%u swizzle=%u,%u,%u,%u\n",
					     log_id, index, input_attr[index].binding, input_attr[index].offset,
					     input_desc[bi].stride, static_cast<int>(input_attr[index].format),
					     static_cast<uint32_t>(vs_input_info->resources[index].Format()),
					     static_cast<uint32_t>(vs_input_info->resources_dst[index].register_start),
					     static_cast<uint32_t>(registers_num),
					     static_cast<uint32_t>(used_components), attr_size,
					     static_cast<uint32_t>(vs_input_info->resources[index].DstSelX()),
					     static_cast<uint32_t>(vs_input_info->resources[index].DstSelY()),
					     static_cast<uint32_t>(vs_input_info->resources[index].DstSelZ()),
					     static_cast<uint32_t>(vs_input_info->resources[index].DstSelW()));
				}
			}

			if (vs_input_info->resources[index].OutOfBounds() != 0) {
				static bool logged = false;
				if (!logged) {
					LOGF("VertexInput: temporary: accepting PS5 out-of-bounds behavior %" PRIu8
					     "\n",
					     vs_input_info->resources[index].OutOfBounds());
					logged = true;
				}
			}

			EXIT_NOT_IMPLEMENTED(vs_input_info->resources[index].AddTid());
			EXIT_NOT_IMPLEMENTED(vs_input_info->resources[index].SwizzleEnabled());

			auto log_unsupported_vertex_swizzle = [index, attr_size, registers_num](
			                                          uint32_t swizzle, uint32_t expected) {
				static bool logged = false;
				if (!logged) {
					LOGF(
					    "VertexInput: temporary: accepting unsupported dst swizzle at attr %" PRIu32
					    " (attr_size=%" PRIu32 ", regs=%" PRIu32 ", swizzle=0x%03" PRIx32
					    ", expected=0x%03" PRIx32 ")\n",
					    static_cast<uint32_t>(index), attr_size, registers_num, swizzle, expected);
					logged = true;
				}
			};

			switch (registers_num) {
				case 1: {
					auto swizzle = vs_input_info->resources[index].DstSelX();
					if (swizzle != DstSel(4)) {
						log_unsupported_vertex_swizzle(swizzle, DstSel(4));
					}
					break;
				}
				case 2: {
					auto swizzle  = vs_input_info->resources[index].DstSelXY();
					auto expected = (attr_size == 1 ? DstSel(4, 0) : DstSel(4, 5));
					if (swizzle != expected) {
						log_unsupported_vertex_swizzle(swizzle, expected);
					}
					break;
				}
				case 3: {
					auto swizzle = vs_input_info->resources[index].DstSelXYZ();
					auto expected =
					    (attr_size == 1 ? DstSel(4, 0, 0)
					                    : (attr_size == 2 ? DstSel(4, 5, 0) : DstSel(4, 5, 6)));
					if (swizzle != expected) {
						log_unsupported_vertex_swizzle(swizzle, expected);
					}
					break;
				}
				case 4: {
					auto swizzle   = vs_input_info->resources[index].DstSelXYZW();
					auto expected  = DstSel(4, 5, 6, 7);
					bool supported = false;
					switch (attr_size) {
						case 1:
							expected  = DstSel(4, 0, 0, 1);
							supported = (swizzle == expected);
							break;
						case 2:
							expected  = DstSel(4, 5, 0, 1);
							supported = (swizzle == expected);
							break;
						case 3:
							expected  = DstSel(4, 5, 6, 1);
							supported = (swizzle == expected || swizzle == DstSel(4, 5, 6, 0));
							break;
						default:
							supported = (swizzle == expected || swizzle == DstSel(4, 5, 6, 1) ||
							             swizzle == DstSel(4, 5, 6, 0));
							break;
					}
					if (!supported) {
						log_unsupported_vertex_swizzle(swizzle, expected);
					}
					break;
				}
				default: EXIT("invalid registers_num");
			}
		}
	}

	vk::PipelineVertexInputStateCreateInfo vertex_input_info {};
	vertex_input_info.sType = vk::StructureType::ePipelineVertexInputStateCreateInfo;
	vertex_input_info.pNext = nullptr;
	vertex_input_info.flags = {};
	vertex_input_info.vertexBindingDescriptionCount   = vs_input_info->buffers_num;
	vertex_input_info.pVertexBindingDescriptions      = input_desc;
	vertex_input_info.vertexAttributeDescriptionCount = vs_input_info->resources_num;
	vertex_input_info.pVertexAttributeDescriptions    = input_attr;

	vk::PipelineInputAssemblyStateCreateInfo input_assembly {};
	input_assembly.sType    = vk::StructureType::ePipelineInputAssemblyStateCreateInfo;
	input_assembly.pNext    = nullptr;
	input_assembly.flags    = {};
	input_assembly.topology = static_params.topology;
	input_assembly.primitiveRestartEnable = VK_FALSE;

	vk::Viewport viewport {};
	viewport.x        = static_params.viewport_offset[0] - static_params.viewport_scale[0];
	viewport.y        = static_params.viewport_offset[1] - static_params.viewport_scale[1];
	viewport.width    = static_params.viewport_scale[0] * 2.0f;
	viewport.height   = static_params.viewport_scale[1] * 2.0f;
	viewport.minDepth = static_params.viewport_offset[2];
	viewport.maxDepth = static_params.viewport_scale[2] + static_params.viewport_offset[2];

	vk::Rect2D scissor {};
	scissor.offset = {static_params.scissor_ltrb[0], static_params.scissor_ltrb[1]};
	scissor.extent = {
	    static_cast<uint32_t>(static_params.scissor_ltrb[2] - static_params.scissor_ltrb[0]),
	    static_cast<uint32_t>(static_params.scissor_ltrb[3] - static_params.scissor_ltrb[1])};

	vk::PipelineViewportDepthClipControlCreateInfoEXT depth_clip_control {};
	depth_clip_control.sType = vk::StructureType::ePipelineViewportDepthClipControlCreateInfoEXT;
	depth_clip_control.pNext = nullptr;
	depth_clip_control.negativeOneToOne = (static_params.negative_one_to_one ? VK_TRUE : VK_FALSE);

	vk::PipelineViewportStateCreateInfo viewport_state {};
	viewport_state.sType         = vk::StructureType::ePipelineViewportStateCreateInfo;
	viewport_state.pNext         = &depth_clip_control;
	viewport_state.flags         = {};
	viewport_state.viewportCount = 1;
	viewport_state.pViewports    = &viewport;
	viewport_state.scissorCount  = 1;
	viewport_state.pScissors     = &scissor;

	vk::CullModeFlags cull_mode = vk::CullModeFlagBits::eNone;
	if (static_params.cull_back) {
		cull_mode |= vk::CullModeFlagBits::eBack;
	}
	if (static_params.cull_front) {
		cull_mode |= vk::CullModeFlagBits::eFront;
	}

	vk::FrontFace front_face =
	    (static_params.face ? vk::FrontFace::eClockwise : vk::FrontFace::eCounterClockwise);

	vk::PipelineRasterizationDepthClipStateCreateInfoEXT clip_ext {};
	clip_ext.sType           = vk::StructureType::ePipelineRasterizationDepthClipStateCreateInfoEXT;
	clip_ext.pNext           = nullptr;
	clip_ext.flags           = {};
	clip_ext.depthClipEnable = VK_FALSE;

	vk::PipelineRasterizationStateCreateInfo rasterizer {};
	rasterizer.sType                   = vk::StructureType::ePipelineRasterizationStateCreateInfo;
	rasterizer.pNext                   = &clip_ext;
	rasterizer.flags                   = {};
	rasterizer.depthClampEnable        = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode             = vk::PolygonMode::eFill;
	rasterizer.cullMode                = cull_mode;
	rasterizer.frontFace               = front_face;
	rasterizer.depthBiasEnable         = VK_FALSE;
	rasterizer.depthBiasConstantFactor = 0.0f;
	rasterizer.depthBiasClamp          = 0.0f;
	rasterizer.depthBiasSlopeFactor    = 0.0f;
	rasterizer.lineWidth               = 1.0f;

	vk::PipelineMultisampleStateCreateInfo multisampling {};
	multisampling.sType                 = vk::StructureType::ePipelineMultisampleStateCreateInfo;
	multisampling.pNext                 = nullptr;
	multisampling.flags                 = {};
	multisampling.sampleShadingEnable   = VK_FALSE;
	multisampling.rasterizationSamples  = vk::SampleCountFlagBits::e1;
	multisampling.minSampleShading      = 1.0f;
	multisampling.pSampleMask           = nullptr;
	multisampling.alphaToCoverageEnable = VK_FALSE;
	multisampling.alphaToOneEnable      = VK_FALSE;

	vk::PipelineColorBlendAttachmentState color_blend_attachment[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t i = 0; i < static_params.color_count; i++) {
		vk::ColorComponentFlags color_write_mask = {};
		EXIT_NOT_IMPLEMENTED((static_params.color_mask[i] & ~0x0fu) != 0);
		if ((static_params.color_mask[i] & 0x1u) != 0) {
			color_write_mask |=
			    static_cast<vk::ColorComponentFlags>(vk::ColorComponentFlagBits::eR);
		}
		if ((static_params.color_mask[i] & 0x2u) != 0) {
			color_write_mask |=
			    static_cast<vk::ColorComponentFlags>(vk::ColorComponentFlagBits::eG);
		}
		if ((static_params.color_mask[i] & 0x4u) != 0) {
			color_write_mask |=
			    static_cast<vk::ColorComponentFlags>(vk::ColorComponentFlagBits::eB);
		}
		if ((static_params.color_mask[i] & 0x8u) != 0) {
			color_write_mask |=
			    static_cast<vk::ColorComponentFlags>(vk::ColorComponentFlagBits::eA);
		}

		color_blend_attachment[i].colorWriteMask = color_write_mask;
		color_blend_attachment[i].blendEnable =
		    (static_params.blend_enable[i] && !static_params.blend_bypass[i]) ? VK_TRUE : VK_FALSE;
		color_blend_attachment[i].srcColorBlendFactor =
		    GetBlendFactor(static_params.color_srcblend[i]);
		color_blend_attachment[i].dstColorBlendFactor =
		    GetBlendFactor(static_params.color_destblend[i]);
		color_blend_attachment[i].colorBlendOp = GetBlendOp(static_params.color_comb_fcn[i]);
		color_blend_attachment[i].srcAlphaBlendFactor =
		    (static_params.separate_alpha_blend[i] ? GetBlendFactor(static_params.alpha_srcblend[i])
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
	for (uint32_t i = 0; i < static_params.color_count; i++) {
		color_write_enable[i] = VK_TRUE;
	}

	vk::PipelineColorWriteCreateInfoEXT color_write {};
	color_write.sType              = vk::StructureType::ePipelineColorWriteCreateInfoEXT;
	color_write.pNext              = nullptr;
	color_write.attachmentCount    = static_params.color_count;
	color_write.pColorWriteEnables = color_write_enable;

	vk::PipelineColorBlendStateCreateInfo color_blending {};
	color_blending.sType             = vk::StructureType::ePipelineColorBlendStateCreateInfo;
	color_blending.pNext             = &color_write;
	color_blending.flags             = {};
	color_blending.logicOpEnable     = VK_FALSE;
	color_blending.logicOp           = vk::LogicOp::eCopy;
	color_blending.attachmentCount   = static_params.color_count;
	color_blending.pAttachments      = color_blend_attachment;
	color_blending.blendConstants[0] = static_params.blend_color_red;
	color_blending.blendConstants[1] = static_params.blend_color_green;
	color_blending.blendConstants[2] = static_params.blend_color_blue;
	color_blending.blendConstants[3] = static_params.blend_color_alpha;

	vk::DescriptorSetLayout set_layouts[2]  = {};
	uint32_t                set_layouts_num = 0;

	vk::PushConstantRange push_constant_info[2];
	uint32_t              push_constant_info_num = 0;

	EXIT_IF(!vs_input_info->stage);
	CreateLayout(set_layouts, &set_layouts_num, push_constant_info, &push_constant_info_num,
	             *vs_input_info->stage.program, vk::ShaderStageFlagBits::eVertex,
	             DescriptorCache::Stage::Vertex);
	if (ps_active) {
		EXIT_IF(!ps_input_info->stage);
		CreateLayout(set_layouts, &set_layouts_num, push_constant_info, &push_constant_info_num,
		             *ps_input_info->stage.program, vk::ShaderStageFlagBits::eFragment,
		             DescriptorCache::Stage::Pixel);
	}

	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.sType                  = vk::StructureType::ePipelineLayoutCreateInfo;
	pipeline_layout_info.pNext                  = nullptr;
	pipeline_layout_info.flags                  = {};
	pipeline_layout_info.setLayoutCount         = set_layouts_num;
	pipeline_layout_info.pSetLayouts            = (set_layouts_num > 0 ? set_layouts : nullptr);
	pipeline_layout_info.pushConstantRangeCount = push_constant_info_num;
	pipeline_layout_info.pPushConstantRanges =
	    push_constant_info_num > 0 ? push_constant_info : nullptr;

	EXIT_IF(pipeline->pipeline_layout != nullptr);

	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreatePipelineLayout begin VS=0x%08" PRIx32 "/0x%08" PRIx32
		     " PS=0x%08" PRIx32 "/0x%08" PRIx32 " set_layouts=%" PRIu32 " push_constants=%" PRIu32
		     "\n",
		     vs_hash0, vs_crc32, ps_hash0, ps_crc32, set_layouts_num, push_constant_info_num);
	}
	result = gctx->device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                           &pipeline->pipeline_layout);
	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreatePipelineLayout done result=%s layout=%p\n",
		     VulkanToString(result).c_str(), static_cast<void*>(pipeline->pipeline_layout));
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	EXIT_NOT_IMPLEMENTED(pipeline->pipeline_layout == nullptr);

	vk::PipelineDepthStencilStateCreateInfo depth_stencil_info {};
	depth_stencil_info.sType            = vk::StructureType::ePipelineDepthStencilStateCreateInfo;
	depth_stencil_info.pNext            = nullptr;
	depth_stencil_info.flags            = {};
	depth_stencil_info.depthTestEnable  = (static_params.depth_test_enable ? VK_TRUE : VK_FALSE);
	depth_stencil_info.depthWriteEnable = (static_params.depth_write_enable ? VK_TRUE : VK_FALSE);
	depth_stencil_info.depthCompareOp   = static_params.depth_compare_op;
	depth_stencil_info.depthBoundsTestEnable =
	    (static_params.depth_bounds_test_enable ? VK_TRUE : VK_FALSE);
	depth_stencil_info.stencilTestEnable = (static_params.stencil_test_enable ? VK_TRUE : VK_FALSE);
	depth_stencil_info.front.failOp      = static_params.stencil_front.failOp;
	depth_stencil_info.front.passOp      = static_params.stencil_front.passOp;
	depth_stencil_info.front.depthFailOp = static_params.stencil_front.depthFailOp;
	depth_stencil_info.front.compareOp   = static_params.stencil_front.compareOp;
	depth_stencil_info.back.failOp       = static_params.stencil_back.failOp;
	depth_stencil_info.back.passOp       = static_params.stencil_back.passOp;
	depth_stencil_info.back.depthFailOp  = static_params.stencil_back.depthFailOp;
	depth_stencil_info.back.compareOp    = static_params.stencil_back.compareOp;
	depth_stencil_info.minDepthBounds    = static_params.depth_min_bounds;
	depth_stencil_info.maxDepthBounds    = static_params.depth_max_bounds;

	const vk::DynamicState dynamic_states[] = {
	    vk::DynamicState::eViewport,
	    vk::DynamicState::eScissor,
	    vk::DynamicState::eLineWidth,
	    vk::DynamicState::eStencilCompareMask,
	    vk::DynamicState::eStencilReference,
	    vk::DynamicState::eStencilWriteMask,
	    vk::DynamicState::eColorWriteEnableEXT,
	};
	const auto dynamic_states_count =
	    static_cast<uint32_t>(sizeof(dynamic_states) / sizeof(dynamic_states[0]));

	vk::PipelineDynamicStateCreateInfo dynamic_state {};
	dynamic_state.sType             = vk::StructureType::ePipelineDynamicStateCreateInfo;
	dynamic_state.pNext             = nullptr;
	dynamic_state.flags             = {};
	dynamic_state.dynamicStateCount = dynamic_states_count;
	dynamic_state.pDynamicStates    = dynamic_states;

	vk::GraphicsPipelineCreateInfo pipeline_info {};
	pipeline_info.sType               = vk::StructureType::eGraphicsPipelineCreateInfo;
	pipeline_info.pNext               = nullptr;
	pipeline_info.flags               = {};
	pipeline_info.stageCount          = shader_stage_count;
	pipeline_info.pStages             = shader_stages;
	pipeline_info.pVertexInputState   = &vertex_input_info;
	pipeline_info.pInputAssemblyState = &input_assembly;
	pipeline_info.pTessellationState  = nullptr;
	pipeline_info.pViewportState      = &viewport_state;
	pipeline_info.pRasterizationState = &rasterizer;
	pipeline_info.pMultisampleState   = &multisampling;
	pipeline_info.pDepthStencilState  = (static_params.with_depth ? &depth_stencil_info : nullptr);
	pipeline_info.pColorBlendState    = &color_blending;
	pipeline_info.pDynamicState       = &dynamic_state;
	pipeline_info.layout              = pipeline->pipeline_layout;
	pipeline_info.renderPass          = render_pass;
	pipeline_info.subpass             = 0;
	pipeline_info.basePipelineHandle  = nullptr;
	pipeline_info.basePipelineIndex   = -1;

	EXIT_IF(pipeline->pipeline != nullptr);

	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreateGraphicsPipelines begin VS=0x%08" PRIx32 "/0x%08" PRIx32
		     " PS=0x%08" PRIx32 "/0x%08" PRIx32 " topology=%" PRIu32 " color_mask=0x%08" PRIx32
		     " depth=%s blend=%s dyn_states=%" PRIu32 " viewport=%f,%f %fx%f"
		     " scissor=%d,%d %ux%u\n",
		     vs_hash0, vs_crc32, ps_hash0, ps_crc32, static_cast<uint32_t>(static_params.topology),
		     static_params.color_mask[0], (static_params.with_depth ? "true" : "false"),
		     (static_params.blend_enable[0] ? "true" : "false"), dynamic_states_count, viewport.x,
		     viewport.y, viewport.width, viewport.height, scissor.offset.x, scissor.offset.y,
		     scissor.extent.width, scissor.extent.height);
	}
	result = gctx->device.createGraphicsPipelines(nullptr, 1, &pipeline_info, nullptr,
	                                              &pipeline->pipeline);
	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreateGraphicsPipelines done result=%s pipeline=%p\n",
		     VulkanToString(result).c_str(), static_cast<void*>(pipeline->pipeline));
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	EXIT_NOT_IMPLEMENTED(pipeline->pipeline == nullptr);

	if (frag_shader_module != nullptr) {
		gctx->device.destroyShaderModule(frag_shader_module, nullptr);
	}
	gctx->device.destroyShaderModule(vert_shader_module, nullptr);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void CreatePipelineInternal(PipelineCache::ComputePipeline* pipeline,
                            const ShaderComputeInputInfo*   input_info,
                            std::span<const uint32_t>       cs_shader) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(pipeline == nullptr);

	auto* gctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(gctx == nullptr);

	vk::ShaderModule comp_shader_module = nullptr;

	vk::ShaderModuleCreateInfo create_info {};

	create_info.sType    = vk::StructureType::eShaderModuleCreateInfo;
	create_info.pNext    = nullptr;
	create_info.flags    = {};
	create_info.codeSize = cs_shader.size() * 4;
	create_info.pCode    = cs_shader.data();
	LOGF("PipelineTrace: vkCreateShaderModule CS begin words=%" PRIu64 "\n",
	     static_cast<uint64_t>(cs_shader.size()));
	auto result = gctx->device.createShaderModule(&create_info, nullptr, &comp_shader_module);
	LOGF("PipelineTrace: vkCreateShaderModule CS done result=%s module=%p\n",
	     VulkanToString(result).c_str(), static_cast<void*>(comp_shader_module));
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	EXIT_NOT_IMPLEMENTED(comp_shader_module == nullptr);

	vk::PipelineShaderStageCreateInfo                     comp_shader_stage_info {};
	vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo comp_subgroup_size {};
	comp_shader_stage_info.sType               = vk::StructureType::ePipelineShaderStageCreateInfo;
	comp_shader_stage_info.pNext               = nullptr;
	comp_shader_stage_info.flags               = {};
	comp_shader_stage_info.stage               = vk::ShaderStageFlagBits::eCompute;
	comp_shader_stage_info.module              = comp_shader_module;
	comp_shader_stage_info.pName               = "main";
	comp_shader_stage_info.pSpecializationInfo = nullptr;
	EXIT_IF(!input_info->stage);
	ConfigureSubgroupSize(gctx, vk::ShaderStageFlagBits::eCompute, *input_info->stage.program,
	                      &comp_subgroup_size, &comp_shader_stage_info);

	vk::DescriptorSetLayout set_layouts[1]  = {};
	uint32_t                set_layouts_num = 0;

	vk::PushConstantRange push_constant_info[1];
	uint32_t              push_constant_info_num = 0;

	EXIT_IF(!input_info->stage);
	CreateLayout(set_layouts, &set_layouts_num, push_constant_info, &push_constant_info_num,
	             *input_info->stage.program, vk::ShaderStageFlagBits::eCompute,
	             DescriptorCache::Stage::Compute);

	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.sType                  = vk::StructureType::ePipelineLayoutCreateInfo;
	pipeline_layout_info.pNext                  = nullptr;
	pipeline_layout_info.flags                  = {};
	pipeline_layout_info.setLayoutCount         = set_layouts_num;
	pipeline_layout_info.pSetLayouts            = (set_layouts_num > 0 ? set_layouts : nullptr);
	pipeline_layout_info.pushConstantRangeCount = push_constant_info_num;
	pipeline_layout_info.pPushConstantRanges =
	    push_constant_info_num > 0 ? push_constant_info : nullptr;

	EXIT_IF(pipeline->pipeline_layout != nullptr);

	LOGF("PipelineTrace: vkCreatePipelineLayout CS begin set_layouts=%u push_constants=%u\n",
	     set_layouts_num, push_constant_info_num);
	result = gctx->device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                           &pipeline->pipeline_layout);
	LOGF("PipelineTrace: vkCreatePipelineLayout CS done result=%s layout=%p\n",
	     VulkanToString(result).c_str(), static_cast<void*>(pipeline->pipeline_layout));
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	EXIT_NOT_IMPLEMENTED(pipeline->pipeline_layout == nullptr);

	vk::ComputePipelineCreateInfo info {};
	info.sType              = vk::StructureType::eComputePipelineCreateInfo;
	info.pNext              = nullptr;
	info.flags              = {};
	info.stage              = comp_shader_stage_info;
	info.layout             = pipeline->pipeline_layout;
	info.basePipelineHandle = nullptr;
	info.basePipelineIndex  = -1;

	EXIT_IF(pipeline->pipeline != nullptr);

	LOGF("PipelineTrace: vkCreateComputePipelines begin layout=%p\n",
	     static_cast<void*>(pipeline->pipeline_layout));
	result = gctx->device.createComputePipelines(nullptr, 1, &info, nullptr, &pipeline->pipeline);
	LOGF("PipelineTrace: vkCreateComputePipelines done result=%s pipeline=%p\n",
	     VulkanToString(result).c_str(), static_cast<void*>(pipeline->pipeline));
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	EXIT_NOT_IMPLEMENTED(pipeline->pipeline == nullptr);

	gctx->device.destroyShaderModule(comp_shader_module, nullptr);
}

} // namespace Libs::Graphics
