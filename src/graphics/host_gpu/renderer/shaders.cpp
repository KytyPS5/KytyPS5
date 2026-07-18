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
#include "graphics/shader/recompiler/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <shared_mutex>
#include <limits>
#include <span>
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

namespace Libs::Graphics {

//IDK: maybe we can remove it?	
constexpr uint32_t kTemporaryPs5BufferFormat121 = 121u;

static bool NarrowInputFormat(VkFormat* format, uint32_t* size, uint32_t used_components) {
	EXIT_IF(format == nullptr);
	EXIT_IF(size == nullptr);

	if (used_components == 0 || used_components >= *size) {
		return false;
	}

	switch (*format) {
		case VK_FORMAT_R32G32B32A32_SFLOAT:
			switch (used_components) {
				case 1: *format = VK_FORMAT_R32_SFLOAT; break;
				case 2: *format = VK_FORMAT_R32G32_SFLOAT; break;
				case 3: *format = VK_FORMAT_R32G32B32_SFLOAT; break;
				default: return false;
			}
			*size = used_components;
			return true;
		case VK_FORMAT_R32G32B32_SFLOAT:
			switch (used_components) {
				case 1: *format = VK_FORMAT_R32_SFLOAT; break;
				case 2: *format = VK_FORMAT_R32G32_SFLOAT; break;
				default: return false;
			}
			*size = used_components;
			return true;
		case VK_FORMAT_R16G16B16A16_SFLOAT:
			switch (used_components) {
				case 1: *format = VK_FORMAT_R16_SFLOAT; break;
				case 2: *format = VK_FORMAT_R16G16_SFLOAT; break;
				default: return false;
			}
			*size = used_components;
			return true;
		case VK_FORMAT_R8G8B8A8_UNORM:
			switch (used_components) {
				case 1: *format = VK_FORMAT_R8_UNORM; break;
				case 2: *format = VK_FORMAT_R8G8_UNORM; break;
				default: return false;
			}
			*size = used_components;
			return true;
		case VK_FORMAT_R8G8B8A8_SNORM:
			if (used_components != 2) {
				return false;
			}
			*format = VK_FORMAT_R8G8_SNORM;
			*size   = 2;
			return true;
		case VK_FORMAT_R8G8B8A8_UINT:
			switch (used_components) {
				case 1: *format = VK_FORMAT_R8_UINT; break;
				case 2: *format = VK_FORMAT_R8G8_UINT; break;
				default: return false;
			}
			*size = used_components;
			return true;
		default: break;
	}

	return false;
}

static void GetInputFormat(const ShaderBufferResource& res, VkFormat* format, uint32_t* size,
                           uint32_t used_components) {
	EXIT_IF(format == nullptr);
	EXIT_IF(size == nullptr);

	auto fmt = res.Format();
	if (fmt == Prospero::GpuEnumValue(Prospero::VertexAttribFormat::k16_16SInt)) {
		static bool logged_113 = false;
		if (!logged_113) {
			LOGF("InputFormat: temporary: accepting invalid PS5 buffer format 113 as "
			     "VK_FORMAT_R32G32B32A32_SFLOAT\n");
			logged_113 = true;
		}
		*format = VK_FORMAT_R32G32B32A32_SFLOAT;
		*size   = 4;
		if (NarrowInputFormat(format, size, used_components)) {
			LOGF("InputFormat: narrowing fmt=%u to %s for used_components=%u\n", fmt,
			     string_VkFormat(*format), used_components);
		}
		return;
	}
	if (fmt == kTemporaryPs5BufferFormat121) {
		static bool logged_121 = false;
		if (!logged_121) {
			LOGF("InputFormat: accepting PS5 buffer format 121 as VK_FORMAT_R16G16_SFLOAT\n");
			logged_121 = true;
		}
		*format = VK_FORMAT_R16G16_SFLOAT;
		*size   = 2;
		if (NarrowInputFormat(format, size, used_components)) {
			LOGF("InputFormat: narrowing fmt=%u to %s for used_components=%u\n", fmt,
			     string_VkFormat(*format), used_components);
		}
		return;
	}

	switch (static_cast<Prospero::BufferFormat>(fmt)) {
		case Prospero::BufferFormat::k32_32_32_32Float:
			*format = VK_FORMAT_R32G32B32A32_SFLOAT;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k32_32_32_32SInt:
			*format = VK_FORMAT_R32G32B32A32_SINT;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k32_32_32_32UInt:
			*format = VK_FORMAT_R32G32B32A32_UINT;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k32_32_32Float:
			*format = VK_FORMAT_R32G32B32_SFLOAT;
			*size   = 3;
			break;
		case Prospero::BufferFormat::k32_32_32SInt:
			*format = VK_FORMAT_R32G32B32_SINT;
			*size   = 3;
			break;
		case Prospero::BufferFormat::k32_32_32UInt:
			*format = VK_FORMAT_R32G32B32_UINT;
			*size   = 3;
			break;
		case Prospero::BufferFormat::k16_16_16_16Float:
			*format = VK_FORMAT_R16G16B16A16_SFLOAT;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k16_16_16_16SInt:
			*format = VK_FORMAT_R16G16B16A16_SINT;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k16_16_16_16UInt:
			*format = VK_FORMAT_R16G16B16A16_UINT;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k16_16_16_16SScaled:
			*format = VK_FORMAT_R16G16B16A16_SSCALED;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k16_16_16_16UScaled:
			*format = VK_FORMAT_R16G16B16A16_USCALED;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k16_16_16_16SNorm:
			*format = VK_FORMAT_R16G16B16A16_SNORM;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k16_16_16_16UNorm:
			*format = VK_FORMAT_R16G16B16A16_UNORM;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k32_32Float:
			*format = VK_FORMAT_R32G32_SFLOAT;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k32_32SInt:
			*format = VK_FORMAT_R32G32_SINT;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k32_32UInt:
			*format = VK_FORMAT_R32G32_UINT;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k8_8_8_8UInt:
			*format = VK_FORMAT_R8G8B8A8_UINT;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k8_8_8_8SScaled:
			*format = VK_FORMAT_R8G8B8A8_SSCALED;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k8_8_8_8UScaled:
			*format = VK_FORMAT_R8G8B8A8_USCALED;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k8_8_8_8SNorm:
			*format = VK_FORMAT_R8G8B8A8_SNORM;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k8_8_8_8UNorm:
			*format = VK_FORMAT_R8G8B8A8_UNORM;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k10_10_10_2UNorm:
			*format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k10_10_10_2SNorm:
			*format = VK_FORMAT_A2B10G10R10_SNORM_PACK32;
			*size   = 4;
			break;
		case Prospero::BufferFormat::k16_16Float:
			*format = VK_FORMAT_R16G16_SFLOAT;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k16_16SInt:
			*format = VK_FORMAT_R16G16_SINT;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k16_16UInt:
			*format = VK_FORMAT_R16G16_UINT;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k16_16SScaled:
			*format = VK_FORMAT_R16G16_SSCALED;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k16_16UScaled:
			*format = VK_FORMAT_R16G16_USCALED;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k16_16SNorm:
			*format = VK_FORMAT_R16G16_SNORM;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k16_16UNorm:
			*format = VK_FORMAT_R16G16_UNORM;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k32Float:
			*format = VK_FORMAT_R32_SFLOAT;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k32SInt:
			*format = VK_FORMAT_R32_SINT;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k32UInt:
			*format = VK_FORMAT_R32_UINT;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k8_8SInt:
			*format = VK_FORMAT_R8G8_SINT;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k8_8UInt:
			*format = VK_FORMAT_R8G8_UINT;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k8_8SScaled:
			*format = VK_FORMAT_R8G8_SSCALED;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k8_8UScaled:
			*format = VK_FORMAT_R8G8_USCALED;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k8_8SNorm:
			*format = VK_FORMAT_R8G8_SNORM;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k8_8UNorm:
			*format = VK_FORMAT_R8G8_UNORM;
			*size   = 2;
			break;
		case Prospero::BufferFormat::k16Float:
			*format = VK_FORMAT_R16_SFLOAT;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k16SInt:
			*format = VK_FORMAT_R16_SINT;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k16UInt:
			*format = VK_FORMAT_R16_UINT;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k16SScaled:
			*format = VK_FORMAT_R16_SSCALED;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k16UScaled:
			*format = VK_FORMAT_R16_USCALED;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k16SNorm:
			*format = VK_FORMAT_R16_SNORM;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k16UNorm:
			*format = VK_FORMAT_R16_UNORM;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k8SInt:
			*format = VK_FORMAT_R8_SINT;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k8UInt:
			*format = VK_FORMAT_R8_UINT;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k8SScaled:
			*format = VK_FORMAT_R8_SSCALED;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k8UScaled:
			*format = VK_FORMAT_R8_USCALED;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k8SNorm:
			*format = VK_FORMAT_R8_SNORM;
			*size   = 1;
			break;
		case Prospero::BufferFormat::k8UNorm:
			*format = VK_FORMAT_R8_UNORM;
			*size   = 1;
			break;
		default:
			EXIT("unknown format: fmt = %u\n", fmt);
			*format = VK_FORMAT_UNDEFINED;
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

static VkBlendFactor GetBlendFactor(uint32_t factor) {
	switch (static_cast<Prospero::BlendFactor>(factor)) {
		case Prospero::BlendFactor::kZero: return VK_BLEND_FACTOR_ZERO;
		case Prospero::BlendFactor::kOne: return VK_BLEND_FACTOR_ONE;
		case Prospero::BlendFactor::kSrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
		case Prospero::BlendFactor::kOneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		case Prospero::BlendFactor::kSrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
		case Prospero::BlendFactor::kOneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case Prospero::BlendFactor::kDstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
		case Prospero::BlendFactor::kOneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		case Prospero::BlendFactor::kDstColor: return VK_BLEND_FACTOR_DST_COLOR;
		case Prospero::BlendFactor::kOneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
		case Prospero::BlendFactor::kSrcAlphaSaturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
		case Prospero::BlendFactor::kConstantColor: return VK_BLEND_FACTOR_CONSTANT_COLOR;
		case Prospero::BlendFactor::kOneMinusConstantColor:
			return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
		case Prospero::BlendFactor::kSrc1Color: return VK_BLEND_FACTOR_SRC1_COLOR;
		case Prospero::BlendFactor::kOneMinusSrc1Color: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
		case Prospero::BlendFactor::kSrc1Alpha: return VK_BLEND_FACTOR_SRC1_ALPHA;
		case Prospero::BlendFactor::kOneMinusSrc1Alpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
		case Prospero::BlendFactor::kConstantAlpha: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
		case Prospero::BlendFactor::kOneMinusConstantAlpha:
			return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
		default: EXIT("unknown factor: %u\n", factor);
	}
	return VK_BLEND_FACTOR_ZERO;
}

static VkBlendOp GetBlendOp(uint32_t op) {
	switch (static_cast<Prospero::BlendOp>(op)) {
		case Prospero::BlendOp::kAdd: return VK_BLEND_OP_ADD;
		case Prospero::BlendOp::kSubtract: return VK_BLEND_OP_SUBTRACT;
		case Prospero::BlendOp::kMin: return VK_BLEND_OP_MIN;
		case Prospero::BlendOp::kMax: return VK_BLEND_OP_MAX;
		case Prospero::BlendOp::kReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
		default: EXIT("unknown op: %u\n", op);
	}
	return VK_BLEND_OP_ADD;
}

static void CreateLayout(VkDescriptorSetLayout* set_layouts, uint32_t* set_layouts_num,
                         VkPushConstantRange* push_constant_info, uint32_t* push_constant_info_num,
                         const ShaderRecompiler::IR::Program& program, VkShaderStageFlags vk_stage,
                         DescriptorCache::Stage stage) {
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

static void ConfigureSubgroupSize(const GraphicContext* gctx, VkShaderStageFlagBits vk_stage,
                                  const ShaderRecompiler::IR::Program&                 program,
                                  VkPipelineShaderStageRequiredSubgroupSizeCreateInfo* required,
                                  VkPipelineShaderStageCreateInfo*                     stage) {
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
			     gctx->required_subgroup_size_stages);
	}

	required->sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO;
	required->requiredSubgroupSize = config.required_size;
	stage->pNext                   = required;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void CreatePipelineInternal(PipelineCache::GraphicsPipeline* pipeline, VkRenderPass render_pass,
                            const ShaderVertexInputInfo*    vs_input_info,
                            std::span<const uint32_t>       vs_shader,
                            const ShaderPixelInputInfo*     ps_input_info,
                            std::span<const uint32_t>       ps_shader,
                            const PipelineStaticParameters& static_params, uint32_t vs_hash0,
                            uint32_t vs_crc32, uint32_t ps_hash0, uint32_t ps_crc32,
                            bool ps_active, VkPipelineCache pipeline_cache) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(pipeline == nullptr);
	EXIT_IF(render_pass == nullptr);
	EXIT_IF(ps_active && ps_input_info == nullptr);

	auto* gctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(gctx == nullptr);

	VkShaderModule vert_shader_module = nullptr;
	VkShaderModule frag_shader_module = nullptr;

	VkShaderModuleCreateInfo create_info {};

	create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	create_info.pNext = nullptr;
	create_info.flags = 0;

	create_info.codeSize = vs_shader.size() * 4;
	create_info.pCode    = vs_shader.data();
	auto result = vkCreateShaderModule(gctx->device, &create_info, nullptr, &vert_shader_module);
	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreateShaderModule VS done result=%s module=%p\n",
		     string_VkResult(result), static_cast<void*>(vert_shader_module));
	}
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

	if (ps_active) {
		create_info.codeSize = ps_shader.size() * 4;
		create_info.pCode    = ps_shader.data();
		result = vkCreateShaderModule(gctx->device, &create_info, nullptr, &frag_shader_module);
		if (graphics_debug_dump_enabled()) {
			LOGF("PipelineTrace: vkCreateShaderModule PS done result=%s module=%p\n",
			     string_VkResult(result), static_cast<void*>(frag_shader_module));
		}
		EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
	}

	EXIT_NOT_IMPLEMENTED(vert_shader_module == nullptr);
	EXIT_NOT_IMPLEMENTED(ps_active && frag_shader_module == nullptr);

	VkPipelineShaderStageCreateInfo                     vert_shader_stage_info {};
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfo vert_subgroup_size {};

	vert_shader_stage_info.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	vert_shader_stage_info.pNext  = nullptr;
	vert_shader_stage_info.flags  = 0;
	vert_shader_stage_info.stage  = VK_SHADER_STAGE_VERTEX_BIT;
	vert_shader_stage_info.module = vert_shader_module;
	vert_shader_stage_info.pName  = "main";
	vert_shader_stage_info.pSpecializationInfo = nullptr;
	if (vs_input_info->stage && vs_input_info->stage.program != nullptr) {
		ConfigureSubgroupSize(gctx, VK_SHADER_STAGE_VERTEX_BIT, *vs_input_info->stage.program,
		                      &vert_subgroup_size, &vert_shader_stage_info);
	}

	VkPipelineShaderStageCreateInfo                     frag_shader_stage_info {};
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfo frag_subgroup_size {};
	frag_shader_stage_info.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	frag_shader_stage_info.pNext  = nullptr;
	frag_shader_stage_info.flags  = 0;
	frag_shader_stage_info.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
	frag_shader_stage_info.module = frag_shader_module;
	frag_shader_stage_info.pName  = "main";
	frag_shader_stage_info.pSpecializationInfo = nullptr;
	if (ps_active) {
		if (ps_input_info->stage && ps_input_info->stage.program != nullptr) {
			ConfigureSubgroupSize(gctx, VK_SHADER_STAGE_FRAGMENT_BIT, *ps_input_info->stage.program,
			                      &frag_subgroup_size, &frag_shader_stage_info);
		}
	}

	VkPipelineShaderStageCreateInfo shader_stages[]    = {vert_shader_stage_info,
	                                                      frag_shader_stage_info};
	const uint32_t                  shader_stage_count = ps_active ? 2u : 1u;

	VkVertexInputAttributeDescription input_attr[ShaderVertexInputInfo::RES_MAX];
	VkVertexInputBindingDescription   input_desc[ShaderVertexInputInfo::RES_MAX];

	for (int bi = 0; bi < vs_input_info->buffers_num; bi++) {
		const auto& b          = vs_input_info->buffers[bi];
		input_desc[bi].binding = bi;
		input_desc[bi].stride  = b.stride;
		input_desc[bi].inputRate =
		    (b.fetch_index == 0 ? VK_VERTEX_INPUT_RATE_VERTEX : VK_VERTEX_INPUT_RATE_INSTANCE);

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

	VkPipelineVertexInputStateCreateInfo vertex_input_info {};
	vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input_info.pNext = nullptr;
	vertex_input_info.flags = 0;
	vertex_input_info.vertexBindingDescriptionCount   = vs_input_info->buffers_num;
	vertex_input_info.pVertexBindingDescriptions      = input_desc;
	vertex_input_info.vertexAttributeDescriptionCount = vs_input_info->resources_num;
	vertex_input_info.pVertexAttributeDescriptions    = input_attr;

	VkPipelineInputAssemblyStateCreateInfo input_assembly {};
	input_assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	input_assembly.pNext    = nullptr;
	input_assembly.flags    = 0;
	input_assembly.topology = static_params.topology;
	input_assembly.primitiveRestartEnable = VK_FALSE;

	VkViewport viewport {};
	viewport.x        = static_params.viewport_offset[0] - static_params.viewport_scale[0];
	viewport.y        = static_params.viewport_offset[1] - static_params.viewport_scale[1];
	viewport.width    = static_params.viewport_scale[0] * 2.0f;
	viewport.height   = static_params.viewport_scale[1] * 2.0f;
	viewport.minDepth = static_params.viewport_offset[2];
	viewport.maxDepth = static_params.viewport_scale[2] + static_params.viewport_offset[2];

	VkRect2D scissor {};
	scissor.offset = {static_params.scissor_ltrb[0], static_params.scissor_ltrb[1]};
	scissor.extent = {
	    static_cast<uint32_t>(static_params.scissor_ltrb[2] - static_params.scissor_ltrb[0]),
	    static_cast<uint32_t>(static_params.scissor_ltrb[3] - static_params.scissor_ltrb[1])};

	VkPipelineViewportDepthClipControlCreateInfoEXT depth_clip_control {};
	depth_clip_control.sType =
	    VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT;
	depth_clip_control.pNext            = nullptr;
	depth_clip_control.negativeOneToOne = (static_params.negative_one_to_one ? VK_TRUE : VK_FALSE);

	VkPipelineViewportStateCreateInfo viewport_state {};
	viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport_state.pNext         = &depth_clip_control;
	viewport_state.flags         = 0;
	viewport_state.viewportCount = 1;
	viewport_state.pViewports    = &viewport;
	viewport_state.scissorCount  = 1;
	viewport_state.pScissors     = &scissor;

	VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;
	cull_mode |= (static_params.cull_back ? VK_CULL_MODE_BACK_BIT : 0u);
	cull_mode |= (static_params.cull_front ? VK_CULL_MODE_FRONT_BIT : 0u);

	VkFrontFace front_face =
	    (static_params.face ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);

	VkPipelineRasterizationDepthClipStateCreateInfoEXT clip_ext {};
	clip_ext.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT;
	clip_ext.pNext = nullptr;
	clip_ext.flags = 0;
	clip_ext.depthClipEnable = VK_FALSE;

	VkPipelineRasterizationStateCreateInfo rasterizer {};
	rasterizer.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.pNext                   = &clip_ext;
	rasterizer.flags                   = 0;
	rasterizer.depthClampEnable        = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode             = VK_POLYGON_MODE_FILL;
	rasterizer.cullMode                = cull_mode;
	rasterizer.frontFace               = front_face;
	rasterizer.depthBiasEnable         = VK_FALSE;
	rasterizer.depthBiasConstantFactor = 0.0f;
	rasterizer.depthBiasClamp          = 0.0f;
	rasterizer.depthBiasSlopeFactor    = 0.0f;
	rasterizer.lineWidth               = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisampling {};
	multisampling.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.pNext                 = nullptr;
	multisampling.flags                 = 0;
	multisampling.sampleShadingEnable   = VK_FALSE;
	multisampling.rasterizationSamples  = VK_SAMPLE_COUNT_1_BIT;
	multisampling.minSampleShading      = 1.0f;
	multisampling.pSampleMask           = nullptr;
	multisampling.alphaToCoverageEnable = VK_FALSE;
	multisampling.alphaToOneEnable      = VK_FALSE;

	VkPipelineColorBlendAttachmentState color_blend_attachment[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t i = 0; i < static_params.color_count; i++) {
		VkColorComponentFlags color_write_mask = 0;
		EXIT_NOT_IMPLEMENTED((static_params.color_mask[i] & ~0x0fu) != 0);
		if ((static_params.color_mask[i] & 0x1u) != 0) {
			color_write_mask |= static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_R_BIT);
		}
		if ((static_params.color_mask[i] & 0x2u) != 0) {
			color_write_mask |= static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_G_BIT);
		}
		if ((static_params.color_mask[i] & 0x4u) != 0) {
			color_write_mask |= static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_B_BIT);
		}
		if ((static_params.color_mask[i] & 0x8u) != 0) {
			color_write_mask |= static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_A_BIT);
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

	VkBool32 color_write_enable[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t i = 0; i < static_params.color_count; i++) {
		color_write_enable[i] = VK_TRUE;
	}

	VkPipelineColorWriteCreateInfoEXT color_write {};
	color_write.sType              = VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT;
	color_write.pNext              = nullptr;
	color_write.attachmentCount    = static_params.color_count;
	color_write.pColorWriteEnables = color_write_enable;

	VkPipelineColorBlendStateCreateInfo color_blending {};
	color_blending.sType             = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	color_blending.pNext             = &color_write;
	color_blending.flags             = 0;
	color_blending.logicOpEnable     = VK_FALSE;
	color_blending.logicOp           = VK_LOGIC_OP_COPY;
	color_blending.attachmentCount   = static_params.color_count;
	color_blending.pAttachments      = color_blend_attachment;
	color_blending.blendConstants[0] = static_params.blend_color_red;
	color_blending.blendConstants[1] = static_params.blend_color_green;
	color_blending.blendConstants[2] = static_params.blend_color_blue;
	color_blending.blendConstants[3] = static_params.blend_color_alpha;

	VkDescriptorSetLayout set_layouts[2]  = {};
	uint32_t              set_layouts_num = 0;

	VkPushConstantRange push_constant_info[2];
	uint32_t            push_constant_info_num = 0;

	if (vs_input_info->stage && vs_input_info->stage.program != nullptr) {
		CreateLayout(set_layouts, &set_layouts_num, push_constant_info, &push_constant_info_num,
		             *vs_input_info->stage.program, VK_SHADER_STAGE_VERTEX_BIT,
		             DescriptorCache::Stage::Vertex);
	}
	if (ps_active) {
		if (ps_input_info->stage && ps_input_info->stage.program != nullptr) {
			CreateLayout(set_layouts, &set_layouts_num, push_constant_info, &push_constant_info_num,
			             *ps_input_info->stage.program, VK_SHADER_STAGE_FRAGMENT_BIT,
			             DescriptorCache::Stage::Pixel);
		}
	}

	VkPipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout_info.pNext                  = nullptr;
	pipeline_layout_info.flags                  = 0;
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
	result = vkCreatePipelineLayout(gctx->device, &pipeline_layout_info, nullptr,
	                                &pipeline->pipeline_layout);
	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreatePipelineLayout done result=%s layout=%p\n",
		     string_VkResult(result), static_cast<void*>(pipeline->pipeline_layout));
	}
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

	EXIT_NOT_IMPLEMENTED(pipeline->pipeline_layout == nullptr);

	VkPipelineDepthStencilStateCreateInfo depth_stencil_info {};
	depth_stencil_info.sType           = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depth_stencil_info.pNext           = nullptr;
	depth_stencil_info.flags           = 0;
	depth_stencil_info.depthTestEnable = (static_params.depth_test_enable ? VK_TRUE : VK_FALSE);
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

	const VkDynamicState dynamic_states[] = {
	    VK_DYNAMIC_STATE_VIEWPORT,
	    VK_DYNAMIC_STATE_SCISSOR,
	    VK_DYNAMIC_STATE_LINE_WIDTH,
	    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
	    VK_DYNAMIC_STATE_STENCIL_REFERENCE,
	    VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
	    VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT,
	};
	const auto dynamic_states_count =
	    static_cast<uint32_t>(sizeof(dynamic_states) / sizeof(dynamic_states[0]));

	VkPipelineDynamicStateCreateInfo dynamic_state {};
	dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic_state.pNext             = nullptr;
	dynamic_state.flags             = 0;
	dynamic_state.dynamicStateCount = dynamic_states_count;
	dynamic_state.pDynamicStates    = dynamic_states;

	VkGraphicsPipelineCreateInfo pipeline_info {};
	pipeline_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline_info.pNext               = nullptr;
	pipeline_info.flags               = 0;
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
	{
		std::shared_lock<std::shared_mutex> lock(g_pipeline_cache_mutex);
		result = vkCreateGraphicsPipelines(gctx->device, pipeline_cache, 1, &pipeline_info, nullptr,
		                                   &pipeline->pipeline);
	}
	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreateGraphicsPipelines done result=%s pipeline=%p\n",
		     string_VkResult(result), static_cast<void*>(pipeline->pipeline));
	}
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

	EXIT_NOT_IMPLEMENTED(pipeline->pipeline == nullptr);

	if (frag_shader_module != nullptr) {
		vkDestroyShaderModule(gctx->device, frag_shader_module, nullptr);
	}
	vkDestroyShaderModule(gctx->device, vert_shader_module, nullptr);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void CreatePipelineInternal(PipelineCache::ComputePipeline* pipeline,
                            const ShaderComputeInputInfo*   input_info,
                            std::span<const uint32_t>       cs_shader,
                            VkPipelineCache                 pipeline_cache) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(pipeline == nullptr);

	auto* gctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(gctx == nullptr);

	VkShaderModule comp_shader_module = nullptr;

	VkShaderModuleCreateInfo create_info {};

	create_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	create_info.pNext    = nullptr;
	create_info.flags    = 0;
	create_info.codeSize = cs_shader.size() * 4;
	create_info.pCode    = cs_shader.data();
	LOGF("PipelineTrace: vkCreateShaderModule CS begin words=%" PRIu64 "\n",
	     static_cast<uint64_t>(cs_shader.size()));
	auto result = vkCreateShaderModule(gctx->device, &create_info, nullptr, &comp_shader_module);
	LOGF("PipelineTrace: vkCreateShaderModule CS done result=%s module=%p\n",
	     string_VkResult(result), static_cast<void*>(comp_shader_module));
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

	EXIT_NOT_IMPLEMENTED(comp_shader_module == nullptr);

	VkPipelineShaderStageCreateInfo                     comp_shader_stage_info {};
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfo comp_subgroup_size {};
	comp_shader_stage_info.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	comp_shader_stage_info.pNext  = nullptr;
	comp_shader_stage_info.flags  = 0;
	comp_shader_stage_info.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	comp_shader_stage_info.module = comp_shader_module;
	comp_shader_stage_info.pName  = "main";
	comp_shader_stage_info.pSpecializationInfo = nullptr;
	EXIT_IF(!input_info->stage);
	ConfigureSubgroupSize(gctx, VK_SHADER_STAGE_COMPUTE_BIT, *input_info->stage.program,
	                      &comp_subgroup_size, &comp_shader_stage_info);

	VkDescriptorSetLayout set_layouts[1]  = {};
	uint32_t              set_layouts_num = 0;

	VkPushConstantRange push_constant_info[1];
	uint32_t            push_constant_info_num = 0;

	EXIT_IF(!input_info->stage);
	CreateLayout(set_layouts, &set_layouts_num, push_constant_info, &push_constant_info_num,
	             *input_info->stage.program, VK_SHADER_STAGE_COMPUTE_BIT,
	             DescriptorCache::Stage::Compute);

	VkPipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout_info.pNext                  = nullptr;
	pipeline_layout_info.flags                  = 0;
	pipeline_layout_info.setLayoutCount         = set_layouts_num;
	pipeline_layout_info.pSetLayouts            = (set_layouts_num > 0 ? set_layouts : nullptr);
	pipeline_layout_info.pushConstantRangeCount = push_constant_info_num;
	pipeline_layout_info.pPushConstantRanges =
	    push_constant_info_num > 0 ? push_constant_info : nullptr;

	EXIT_IF(pipeline->pipeline_layout != nullptr);

	LOGF("PipelineTrace: vkCreatePipelineLayout CS begin set_layouts=%u push_constants=%u\n",
	     set_layouts_num, push_constant_info_num);
	result = vkCreatePipelineLayout(gctx->device, &pipeline_layout_info, nullptr,
	                                &pipeline->pipeline_layout);
	LOGF("PipelineTrace: vkCreatePipelineLayout CS done result=%s layout=%p\n",
	     string_VkResult(result), static_cast<void*>(pipeline->pipeline_layout));
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

	EXIT_NOT_IMPLEMENTED(pipeline->pipeline_layout == nullptr);

	VkComputePipelineCreateInfo info {};
	info.sType              = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.pNext              = nullptr;
	info.flags              = 0;
	info.stage              = comp_shader_stage_info;
	info.layout             = pipeline->pipeline_layout;
	info.basePipelineHandle = nullptr;
	info.basePipelineIndex  = -1;

	EXIT_IF(pipeline->pipeline != nullptr);

	LOGF("PipelineTrace: vkCreateComputePipelines begin layout=%p\n",
	     static_cast<void*>(pipeline->pipeline_layout));
	{
		std::shared_lock<std::shared_mutex> lock(g_pipeline_cache_mutex);
		result =
		    vkCreateComputePipelines(gctx->device, pipeline_cache, 1, &info, nullptr, &pipeline->pipeline);
	}
	LOGF("PipelineTrace: vkCreateComputePipelines done result=%s pipeline=%p\n",
	     string_VkResult(result), static_cast<void*>(pipeline->pipeline));
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

	EXIT_NOT_IMPLEMENTED(pipeline->pipeline == nullptr);

	vkDestroyShaderModule(gctx->device, comp_shader_module, nullptr);
}

} // namespace Libs::Graphics
