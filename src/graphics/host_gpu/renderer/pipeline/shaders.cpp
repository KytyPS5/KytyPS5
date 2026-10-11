#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/pipeline/bindlessImageHeap.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/rectListShader.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <span>
#include <vector>

namespace Libs::Graphics {

// IDK: maybe we can remove it?
constexpr uint8_t kTemporaryVertexAttribFormat113 =
    static_cast<uint8_t>(Prospero::VertexAttribFormat::k16_16SInt);
constexpr uint32_t kTemporaryPs5BufferFormat121 = 121u;

static bool NarrowInputFormat(vk::Format& format, uint32_t& size, uint32_t used_components) {
	if (used_components == 0 || used_components >= size) {
		return false;
	}

	switch (format) {
		case vk::Format::eR32G32B32A32Sfloat:
			switch (used_components) {
				case 1: format = vk::Format::eR32Sfloat; break;
				case 2: format = vk::Format::eR32G32Sfloat; break;
				case 3: format = vk::Format::eR32G32B32Sfloat; break;
				default: return false;
			}
			size = used_components;
			return true;
		case vk::Format::eR32G32B32Sfloat:
			switch (used_components) {
				case 1: format = vk::Format::eR32Sfloat; break;
				case 2: format = vk::Format::eR32G32Sfloat; break;
				default: return false;
			}
			size = used_components;
			return true;
		case vk::Format::eR16G16B16A16Sfloat:
			switch (used_components) {
				case 1: format = vk::Format::eR16Sfloat; break;
				case 2: format = vk::Format::eR16G16Sfloat; break;
				default: return false;
			}
			size = used_components;
			return true;
		case vk::Format::eR8G8B8A8Unorm:
			switch (used_components) {
				case 1: format = vk::Format::eR8Unorm; break;
				case 2: format = vk::Format::eR8G8Unorm; break;
				default: return false;
			}
			size = used_components;
			return true;
		case vk::Format::eR8G8B8A8Snorm:
			if (used_components != 2) {
				return false;
			}
			format = vk::Format::eR8G8Snorm;
			size   = 2;
			return true;
		case vk::Format::eR8G8B8A8Uint:
			switch (used_components) {
				case 1: format = vk::Format::eR8Uint; break;
				case 2: format = vk::Format::eR8G8Uint; break;
				default: return false;
			}
			size = used_components;
			return true;
		default: break;
	}

	return false;
}

static void GetInputFormat(const ShaderBufferResource& res, vk::Format& format, uint32_t& size,
                           uint32_t used_components) {
	const auto fmt        = res.Format();
	const auto raw_format = res.RawFormat();
	if (raw_format == kTemporaryVertexAttribFormat113) {
		static bool logged_113 = false;
		if (!logged_113) {
			LOGF("InputFormat: temporary: accepting invalid PS5 buffer format 113 as "
			     "vk::Format::eR32G32B32A32Sfloat\n");
			logged_113 = true;
		}
		format = vk::Format::eR32G32B32A32Sfloat;
		size   = 4;
		if (NarrowInputFormat(format, size, used_components)) {
			LOGF("InputFormat: narrowing fmt=%u to %s for used_components=%u\n", raw_format,
			     vk::to_string(format).c_str(), used_components);
		}
		return;
	}
	if (raw_format == kTemporaryPs5BufferFormat121) {
		static bool logged_121 = false;
		if (!logged_121) {
			LOGF("InputFormat: accepting PS5 buffer format 121 as vk::Format::eR16G16Sfloat\n");
			logged_121 = true;
		}
		format = vk::Format::eR16G16Sfloat;
		size   = 2;
		return;
	}

	format = VulkanFormat(fmt);
	size   = ShaderRecompiler::Format::GetFormatInfo(fmt).component_count;
	if (format == vk::Format::eUndefined || size == 0) {
		EXIT("unknown vertex format: fmt = %u\n", raw_format);
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

static void AddLayoutBindings(std::vector<vk::DescriptorSetLayoutBinding>&    descriptor_bindings,
                              const ShaderRecompiler::IR::CompiledShaderInfo& program,
                              vk::ShaderStageFlagBits                         stage) {
	for (auto mask = program.bindings.descriptor_mask; mask != 0; mask &= mask - 1) {
		const auto index = std::countr_zero(mask);
		const auto kind  = static_cast<ShaderRecompiler::IR::DescriptorBindingKind>(index);
		descriptor_bindings.push_back({ShaderRecompiler::IR::NativeBinding(program.stage, kind),
		                               NativeDescriptorType(kind),
		                               program.bindings.descriptor_counts[index], stage, nullptr});
	}
}

// The recompiler budgets its dense resources against host-independent constants
// (ShaderInfo::MaxImages, MaxDenseBuffers), and nothing in the tree used to read the device limits
// those constants have to live inside. A layout that exceeds one of them fails inside
// vkCreateDescriptorSetLayout or vkCreatePipelineLayout with nothing to say which limit it was, so
// name it here, where both counts are in hand. Every binding carries exactly one stage flag
// (AddLayoutBindings sets it per program), so a per-stage total is a sum over the bindings of that
// stage.
static void CheckDescriptorLayoutLimits(GraphicContext&                                 graphics,
                                        std::span<const vk::DescriptorSetLayoutBinding> bindings) {
	const auto& limits = graphics.GetPhysicalDeviceProperties().limits;
	struct Counts {
		vk::ShaderStageFlags stage {};
		uint32_t             sampled_image  = 0;
		uint32_t             storage_image  = 0;
		uint32_t             storage_buffer = 0;
		uint32_t             sampler        = 0;
		uint32_t             total          = 0;
	};
	std::array<Counts, 4> stages {};
	uint32_t              stage_count = 0;
	Counts                whole_set {};
	for (const auto& binding: bindings) {
		Counts* counts = nullptr;
		for (uint32_t index = 0; index < stage_count; index++) {
			if (stages[index].stage == binding.stageFlags) {
				counts = &stages[index];
				break;
			}
		}
		if (counts == nullptr) {
			if (stage_count == stages.size()) {
				EXIT("shader descriptor layout uses more stages than a pipeline can have\n");
			}
			stages[stage_count].stage = binding.stageFlags;
			counts                    = &stages[stage_count++];
		}
		for (auto* entry: {counts, &whole_set}) {
			switch (binding.descriptorType) {
				case vk::DescriptorType::eSampledImage:
					entry->sampled_image += binding.descriptorCount;
					break;
				case vk::DescriptorType::eStorageImage:
					entry->storage_image += binding.descriptorCount;
					break;
				case vk::DescriptorType::eStorageBuffer:
					entry->storage_buffer += binding.descriptorCount;
					break;
				case vk::DescriptorType::eSampler: entry->sampler += binding.descriptorCount; break;
				default: break;
			}
			entry->total += binding.descriptorCount;
		}
	}
	const auto Require = [](const char* what, uint32_t needed, uint32_t limit) {
		if (needed > limit) {
			EXIT("shader descriptor layout needs %u %s, the device allows %u\n", needed, what,
			     limit);
		}
	};
	for (uint32_t index = 0; index < stage_count; index++) {
		const auto& counts = stages[index];
		Require("per-stage sampled images", counts.sampled_image,
		        limits.maxPerStageDescriptorSampledImages);
		Require("per-stage storage images", counts.storage_image,
		        limits.maxPerStageDescriptorStorageImages);
		Require("per-stage storage buffers", counts.storage_buffer,
		        limits.maxPerStageDescriptorStorageBuffers);
		Require("per-stage samplers", counts.sampler, limits.maxPerStageDescriptorSamplers);
		Require("per-stage resources", counts.total, limits.maxPerStageResources);
	}
	Require("sampled images in one set", whole_set.sampled_image,
	        limits.maxDescriptorSetSampledImages);
	Require("storage images in one set", whole_set.storage_image,
	        limits.maxDescriptorSetStorageImages);
	Require("storage buffers in one set", whole_set.storage_buffer,
	        limits.maxDescriptorSetStorageBuffers);
	Require("samplers in one set", whole_set.sampler, limits.maxDescriptorSetSamplers);
}

static uint32_t DescriptorCount(std::span<const vk::DescriptorSetLayoutBinding> bindings) {
	uint32_t count = 0;
	for (const auto& binding: bindings) {
		count += binding.descriptorCount;
	}
	return count;
}

// One set layout. The push-descriptor flag is decided by the caller over *every* set the pipeline
// will have, not per set: pushing is all-or-nothing for a pipeline here, and a layout created
// without the flag cannot be pushed to.
static vk::DescriptorSetLayout
CreateOneDescriptorLayout(GraphicContext& graphics, bool push_descriptors,
                          std::span<const vk::DescriptorSetLayoutBinding> bindings) {
	vk::DescriptorSetLayoutCreateInfo create {};
	create.flags = push_descriptors ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
	                                : vk::DescriptorSetLayoutCreateFlags {};
	create.bindingCount            = static_cast<uint32_t>(bindings.size());
	create.pBindings               = bindings.data();
	vk::DescriptorSetLayout layout = nullptr;
	EXIT_IF(graphics.device.createDescriptorSetLayout(&create, nullptr, &layout) !=
	        vk::Result::eSuccess);
	return layout;
}

static void AddBindlessLimitBindings(std::vector<vk::DescriptorSetLayoutBinding>&    bindings,
                                     const ShaderRecompiler::IR::CompiledShaderInfo& program,
                                     vk::ShaderStageFlagBits                         stage) {
	if (program.bindings.uses_bindless) {
		const auto global = BindlessImageHeap::LayoutBindings(stage);
		bindings.insert(bindings.end(), global.begin(), global.end());
	}
	if (program.bindings.bindless_integer) {
		const auto integer = BindlessImageHeap::IntegerLayoutBindings(stage);
		bindings.insert(bindings.end(), integer.begin(), integer.end());
	}
}

// Set 3 holds the integer arrays and exists only in a layout whose stages load from a heap.
static constexpr uint32_t BindlessSetCount(bool bindless, bool integer) {
	return integer    ? ShaderRecompiler::IR::BindlessIntegerDescriptorSet + 1u
	       : bindless ? ShaderRecompiler::IR::BindlessDescriptorSet + 1u
	                  : 0u;
}

// Set 0 only: the compute path, and the shape the graphics path used to have.
static void CreateDescriptorLayout(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                                   std::span<const vk::DescriptorSetLayoutBinding> bindings,
                                   std::span<const vk::DescriptorSetLayoutBinding> bindless) {
	std::vector<vk::DescriptorSetLayoutBinding> checked(bindings.begin(), bindings.end());
	checked.insert(checked.end(), bindless.begin(), bindless.end());
	CheckDescriptorLayoutLimits(graphics, checked);
	pipeline.uses_push_descriptors = DescriptorCount(bindings) <= graphics.max_push_descriptors;
	pipeline.descriptor_set_layout =
	    CreateOneDescriptorLayout(graphics, pipeline.uses_push_descriptors, bindings);
}

// Set 0 for the vertex or mesh stage, set 1 for the pixel stage.
//
// The split is what makes the fragment half of a graphics pipeline layout a function of the pixel
// shader alone (IR::NativeDescriptorSet). The limit check still runs over both sets together,
// because `maxDescriptorSet*` bounds a pipeline's whole layout and not one set of it, and the
// per-stage arm of that check was always per-stage anyway.
static void
CreateGraphicsDescriptorLayouts(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                                std::span<const vk::DescriptorSetLayoutBinding> vertex_bindings,
                                std::span<const vk::DescriptorSetLayoutBinding> pixel_bindings,
                                bool                                            has_pixel_stage,
                                std::span<const vk::DescriptorSetLayoutBinding> bindless) {
	EXIT_IF(graphics.GetPhysicalDeviceProperties().limits.maxBoundDescriptorSets <
	        (bindless.empty() ? ShaderRecompiler::IR::NativeDescriptorSetCount
	                          : ShaderRecompiler::IR::BindlessDescriptorSet + 1u));

	std::vector<vk::DescriptorSetLayoutBinding> all;
	all.reserve(vertex_bindings.size() + pixel_bindings.size() + bindless.size());
	all.insert(all.end(), vertex_bindings.begin(), vertex_bindings.end());
	all.insert(all.end(), pixel_bindings.begin(), pixel_bindings.end());
	all.insert(all.end(), bindless.begin(), bindless.end());
	// Combined: maxDescriptorSet* bounds a pipeline layout, not one set of it.
	CheckDescriptorLayoutLimits(graphics, all);

	// Vulkan permits at most one push-descriptor set layout per pipeline layout, so set 0 takes it
	// and set 1 never does. Set 0 is the universal set and the one that always fits: every corpus
	// vertex shader is inside the 32-descriptor limit, against 91 % of pixel shaders.
	pipeline.uses_push_descriptors =
	    DescriptorCount(vertex_bindings) <= graphics.max_push_descriptors;

	uint32_t   push_sets = 0;
	const auto MakeSet   = [&](bool                                            push,
	                           std::span<const vk::DescriptorSetLayoutBinding> set_bindings) {
		push_sets += push ? 1u : 0u;
		EXIT_IF(push_sets > 1);
		return CreateOneDescriptorLayout(graphics, push, set_bindings);
	};

	pipeline.descriptor_set_layout = MakeSet(pipeline.uses_push_descriptors, vertex_bindings);
	if (has_pixel_stage) {
		// Created even when the pixel stage binds nothing, so a pipeline's layout shape is a
		// function of which stages it has rather than of what they happen to use.
		pipeline.pixel_descriptor_set_layout = MakeSet(false, pixel_bindings);
	}
}

static uint64_t PipelineLayoutSignature(const vk::PipelineLayoutCreateInfo&             info,
                                        std::span<const vk::DescriptorSetLayoutBinding> set0,
                                        bool                                            set0_push,
                                        std::span<const vk::DescriptorSetLayoutBinding> set1,
                                        bool                                            has_set1) {
	uint64_t   hash         = 0xcbf29ce484222325ull;
	const auto mix          = [&](uint64_t value) { hash = (hash ^ value) * 0x100000001b3ull; };
	const auto mix_bindings = [&](std::span<const vk::DescriptorSetLayoutBinding> bindings) {
		mix(bindings.size());
		for (const auto& binding: bindings) {
			mix(binding.binding);
			mix(static_cast<uint64_t>(binding.descriptorType));
			mix(binding.descriptorCount);
			mix(static_cast<uint64_t>(static_cast<VkShaderStageFlags>(binding.stageFlags)));
			mix(binding.pImmutableSamplers != nullptr ? 1u : 0u);
		}
	};
	mix(static_cast<uint64_t>(static_cast<VkPipelineLayoutCreateFlags>(info.flags)));
	mix(info.setLayoutCount);
	mix(set0_push ? 1u : 0u);
	mix_bindings(set0);
	mix(has_set1 ? 1u : 0u);
	mix_bindings(set1);
	for (uint32_t set = 2; set < info.setLayoutCount; set++) {
		mix(reinterpret_cast<uint64_t>(static_cast<VkDescriptorSetLayout>(info.pSetLayouts[set])));
	}
	for (uint32_t range = 0; range < info.pushConstantRangeCount; range++) {
		const auto& push = info.pPushConstantRanges[range];
		mix(static_cast<uint64_t>(static_cast<VkShaderStageFlags>(push.stageFlags)));
		mix(push.offset);
		mix(push.size);
	}
	return hash | 1u;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const PipelineRenderingState&          rendering,
                            const PipelineVertexInputState&        vertex_input,
                            std::span<const ShaderVertexInputInfo> vertex_info,
                            const ShaderPixelInputInfo*            ps_input_info,
                            const PipelineCache::GraphicsPrograms& programs,
                            const PipelineStaticParameters&        static_params,
                            vk::PipelineCache driver_cache, PipelineLibraries* libraries) {
	const auto& vs_input_info  = vertex_info.front();
	const auto& vertex_program = programs.vertex[0];
	const auto& pixel_program  = programs.pixel;
	const bool  tessellation   = vertex_info.size() == 3;

	const bool ps_active = ps_input_info != nullptr;
	EXIT_IF(!vertex_program || (ps_active && !pixel_program));
	const bool with_depth = rendering.depth_format != vk::Format::eUndefined ||
	                        rendering.stencil_format != vk::Format::eUndefined;
	EXIT_IF(!vs_input_info.stage);
	const bool mesh = vs_input_info.stage.program->stage == ShaderType::Mesh;
	EXIT_NOT_IMPLEMENTED(mesh && !graphics.mesh_shader_enabled);
	const bool rect_list =
	    !mesh && !tessellation && static_params.topology == vk::PrimitiveTopology::ePatchList;

	// Whether this pipeline is linked from libraries or built as one monolithic pipeline.
	// Mesh pipelines stay monolithic: linked from libraries they lose the device in RE9.
	const bool library_path = libraries != nullptr && !mesh;

	vk::ShaderModule tess_control_shader_module = nullptr;
	vk::ShaderModule tess_eval_shader_module    = nullptr;

	if (rect_list) {
		const auto shaders =
		    BuildRectListShaders(vs_input_info, ps_active ? ps_input_info : nullptr);
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
	for (uint32_t i = 0; i < vertex_info.size(); i++) {
		shader_stages[shader_stage_count++] = {.stage =
		                                           NativeShaderStage(vertex_info[i].logical_stage),
		                                       .module = programs.vertex[i].module,
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
	if (ps_active) {
		shader_stages[shader_stage_count++] = {.stage  = vk::ShaderStageFlagBits::eFragment,
		                                       .module = pixel_program.module,
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

		uint32_t   attr_size     = 4;
		const auto registers_num = vs_input_info.resources_dst[index].registers_num;
		const auto compiled_components =
		    vs_input_info.stage.program->info.vertex_fetch_components[index];
		const auto used_components =
		    compiled_components > 0 ? static_cast<int>(compiled_components) : registers_num;
		GetInputFormat(vs_input_info.resources[index], input_attr[index].format, attr_size,
		               static_cast<uint32_t>(used_components));

		if (graphics_debug_dump_enabled()) {
			static std::atomic_uint log_count = 0;
			const auto              log_id    = log_count.fetch_add(1, std::memory_order_relaxed);
			if (log_id < 128) {
				LOGF("VertexInputState[%u]: attr=%u binding=%u offset=%u stride=%u fmt=%d "
				     "src_fmt=%u dst=v%u regs=%u"
				     " fetched_components=%u attr_size=%u swizzle=%u,%u,%u,%u\n",
				     log_id, index, input_attr[index].binding, input_attr[index].offset,
				     input_desc[input_attr[index].binding].stride,
				     static_cast<int>(input_attr[index].format),
				     static_cast<uint32_t>(vs_input_info.resources[index].Format()),
				     static_cast<uint32_t>(vs_input_info.resources_dst[index].register_start),
				     static_cast<uint32_t>(registers_num), static_cast<uint32_t>(used_components),
				     attr_size, static_cast<uint32_t>(vs_input_info.resources[index].DstSelX()),
				     static_cast<uint32_t>(vs_input_info.resources[index].DstSelY()),
				     static_cast<uint32_t>(vs_input_info.resources[index].DstSelZ()),
				     static_cast<uint32_t>(vs_input_info.resources[index].DstSelW()));
			}
		}

		if (vs_input_info.resources[index].OutOfBounds() != 0) {
			static bool logged = false;
			if (!logged) {
				LOGF("VertexInput: temporary: accepting PS5 out-of-bounds behavior %" PRIu8 "\n",
				     vs_input_info.resources[index].OutOfBounds());
				logged = true;
			}
		}

		EXIT_IF(registers_num < 1 || registers_num > 4);
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
	clip_ext.depthClipEnable = static_params.depth_clip_enable ? VK_TRUE : VK_FALSE;

	vk::PipelineRasterizationStateCreateInfo rasterizer {};
	// MoltenVK lacks VK_EXT_depth_clip_enable; omit the depth-clip struct on macOS and accept
	// Vulkan's default depth clipping (enabled) instead of the PS5's clamp behavior.
#if !defined(__APPLE__)
	// The DB clamps depth to the viewport range after polygon offset is applied.
	rasterizer.depthClampEnable = VK_TRUE;
	rasterizer.pNext            = &clip_ext;
#endif
	vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT provoking_vertex {};
	EXIT_NOT_IMPLEMENTED(static_params.provoking_vtx_last &&
	                     !graphics.provoking_vertex_last_enabled);
	if (graphics.provoking_vertex_last_enabled) {
		provoking_vertex.provokingVertexMode = static_params.provoking_vtx_last
		                                           ? vk::ProvokingVertexModeEXT::eLastVertex
		                                           : vk::ProvokingVertexModeEXT::eFirstVertex;
		provoking_vertex.pNext               = rasterizer.pNext;
		rasterizer.pNext                     = &provoking_vertex;
	}
	rasterizer.cullMode    = cull_mode;
	rasterizer.frontFace   = front_face;
	rasterizer.polygonMode = static_params.polygon_mode;
	rasterizer.lineWidth   = 1.0f;

	vk::PipelineMultisampleStateCreateInfo multisampling {};
	multisampling.sampleShadingEnable  = static_params.sample_shading_enable ? VK_TRUE : VK_FALSE;
	multisampling.rasterizationSamples = vulkan_sample_count(static_params.samples);
	multisampling.minSampleShading     = 1.0f;

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
	// MoltenVK lacks VK_EXT_color_write_enable; drop the dynamic color-write struct on macOS
	// and rely on each attachment's static colorWriteMask (all channels enabled by default).
#if !defined(__APPLE__)
	color_blending.pNext = &color_write;
#endif
	color_blending.logicOp         = vk::LogicOp::eCopy;
	color_blending.attachmentCount = rendering.color_count;
	color_blending.pAttachments    = color_blend_attachment;

	std::vector<vk::DescriptorSetLayoutBinding> vertex_bindings;
	std::vector<vk::DescriptorSetLayoutBinding> pixel_bindings;
	std::vector<vk::DescriptorSetLayoutBinding> bindless_bindings;
	vk::ShaderStageFlags graphics_stages = vk::ShaderStageFlagBits::eFragment;
	bool                 vertex_bindless = false;
	bool                 vertex_integer  = false;
	for (const auto& stage: vertex_info) {
		const auto native_stage = NativeShaderStage(stage.logical_stage);
		AddLayoutBindings(vertex_bindings, *stage.stage.program, native_stage);
		AddBindlessLimitBindings(bindless_bindings, *stage.stage.program, native_stage);
		graphics_stages |= native_stage;
		vertex_bindless = vertex_bindless || stage.stage.program->bindings.uses_bindless;
		vertex_integer  = vertex_integer || stage.stage.program->bindings.bindless_integer;
	}

	bool pixel_bindless = false;
	bool pixel_integer  = false;
	if (ps_active) {
		EXIT_IF(!ps_input_info->stage);
		AddLayoutBindings(pixel_bindings, *ps_input_info->stage.program,
		                  vk::ShaderStageFlagBits::eFragment);
		AddBindlessLimitBindings(bindless_bindings, *ps_input_info->stage.program,
		                         vk::ShaderStageFlagBits::eFragment);
		pixel_bindless = ps_input_info->stage.program->bindings.uses_bindless;
		pixel_integer  = ps_input_info->stage.program->bindings.bindless_integer;
	}
	const bool uses_bindless = !bindless_bindings.empty();
	const bool uses_integer  = vertex_integer || pixel_integer;
	EXIT_IF(uses_integer && graphics.GetPhysicalDeviceProperties().limits.maxBoundDescriptorSets <
	                            BindlessSetCount(true, true));
	CreateGraphicsDescriptorLayouts(graphics, pipeline, vertex_bindings, pixel_bindings, ps_active,
	                                bindless_bindings);
	const vk::PushConstantRange push_constants {graphics_stages, 0,

	                                            ShaderRecompiler::IR::NativePushConstantSize};

	// Set 0 always; set 1 whenever there is a pixel stage, even if it binds nothing, so a layout's
	// shape follows which stages a pipeline has rather than what they happen to use.
	const vk::DescriptorSetLayout
	    set_layouts[ShaderRecompiler::IR::BindlessIntegerDescriptorSet + 1u] = {
	        pipeline.descriptor_set_layout,
	        pipeline.pixel_descriptor_set_layout != nullptr || !uses_bindless
	            ? pipeline.pixel_descriptor_set_layout
	            : BindlessImageHeap::EmptySetLayout(graphics),
	        uses_bindless ? BindlessImageHeap::SetLayout(graphics) : nullptr,
	        uses_integer ? BindlessImageHeap::IntegerSetLayout(graphics) : nullptr};
	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	// Not just the libraries': the *linked* pipeline's layout must carry the flag as well, or it is
	// undefined behaviour that this driver happens to tolerate. Found by the validation layer
	// (note 159 §1.1) after the driver had already accepted the construction without it.
	if (library_path) {
		pipeline_layout_info.flags = vk::PipelineLayoutCreateFlagBits::eIndependentSetsEXT;
	}
	const uint32_t native_sets = pipeline.pixel_descriptor_set_layout != nullptr
	                                 ? ShaderRecompiler::IR::NativeDescriptorSetCount
	                                 : 1u;
	pipeline_layout_info.setLayoutCount =
	    uses_bindless ? BindlessSetCount(true, uses_integer) : native_sets;
	pipeline_layout_info.pSetLayouts            = set_layouts;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_constants;

	EXIT_IF(pipeline.pipeline_layout != nullptr);

	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreatePipelineLayout begin VS=%" PRIu64 " PS=%" PRIu64
		     " set_layouts=1 push_constants=%" PRIu32 "\n",
		     vertex_program.id, ps_active ? pixel_program.id : 0, 1u);
	}
	auto result = graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                                   &pipeline.pipeline_layout);
	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreatePipelineLayout done result=%s layout=%p\n",
		     vk::to_string(result).c_str(), static_cast<void*>(pipeline.pipeline_layout));
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	EXIT_NOT_IMPLEMENTED(pipeline.pipeline_layout == nullptr);
	pipeline.layout_signature = PipelineLayoutSignature(
	    pipeline_layout_info, vertex_bindings, pipeline.uses_push_descriptors, pixel_bindings,
	    pipeline.pixel_descriptor_set_layout != nullptr);

	vk::PipelineDepthStencilStateCreateInfo depth_stencil_info {};

	std::vector<vk::DynamicState> dynamic_states {
	    vk::DynamicState::eViewportWithCount,
	    vk::DynamicState::eScissorWithCount,
	    vk::DynamicState::eLineWidth,
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
	if (rendering.color_count != 0) {
		dynamic_states.push_back(vk::DynamicState::eColorWriteEnableEXT);
	}
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
	tessellation_state.patchControlPoints =
	    tessellation ? vs_input_info.tess.input_control_points : 3u;
	pipeline_info.pTessellationState  = (rect_list || tessellation) ? &tessellation_state : nullptr;
	pipeline_info.pViewportState      = &viewport_state;
	pipeline_info.pRasterizationState = &rasterizer;
	pipeline_info.pMultisampleState   = &multisampling;
	pipeline_info.pDepthStencilState  = (with_depth ? &depth_stencil_info : nullptr);
	pipeline_info.pColorBlendState    = &color_blending;
	pipeline_info.pDynamicState       = &dynamic_state;
	pipeline_info.layout              = pipeline.pipeline_layout;
	pipeline_info.basePipelineIndex   = -1;

	EXIT_IF(pipeline.pipeline != nullptr);

	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreateGraphicsPipelines begin VS=%" PRIu64 " PS=%" PRIu64
		     " topology=%" PRIu32 " color_mask=0x%08" PRIx32
		     " depth=%s blend=%s dyn_states=%" PRIu32 "\n",
		     vertex_program.id, ps_active ? pixel_program.id : 0,
		     static_cast<uint32_t>(static_params.topology), static_params.color_mask[0],
		     (with_depth ? "true" : "false"), (static_params.blend_enable[0] ? "true" : "false"),
		     dynamic_state.dynamicStateCount);
	}
	if (!library_path) {
		result = graphics.device.createGraphicsPipelines(driver_cache, 1, &pipeline_info, nullptr,
		                                                 &pipeline.pipeline);
	} else {
		// Four libraries, fast-linked. The pre-rasterization and fragment ones are shared; see
		// PipelineLibraries. A depth-only pipeline's fragment library has no shader.
		const uint32_t pre_raster_stage_count = shader_stage_count - (ps_active ? 1u : 0u);
		const auto*    fragment_stage_info =
		    ps_active ? &shader_stages[pre_raster_stage_count] : nullptr;
		EXIT_IF(pre_raster_stage_count == 0);
		EXIT_IF(ps_active && fragment_stage_info->stage != vk::ShaderStageFlagBits::eFragment);

		// The dynamic state, partitioned. Every entry here was confirmed against the validation
		// layer in its stated subset (note 159 §1.2); a state in the wrong library is dropped
		// silently rather than rejected, so this list is not a reading of the spec but a result.
		std::vector<vk::DynamicState> pre_raster_dynamic {
		    vk::DynamicState::eViewportWithCount, vk::DynamicState::eScissorWithCount,
		    vk::DynamicState::eLineWidth,         vk::DynamicState::eDepthBiasEnable,
		    vk::DynamicState::eDepthBias,
		};
		std::vector<vk::DynamicState> fragment_dynamic {
		    vk::DynamicState::eDepthTestEnable,   vk::DynamicState::eDepthWriteEnable,
		    vk::DynamicState::eDepthCompareOp,    vk::DynamicState::eStencilCompareMask,
		    vk::DynamicState::eStencilReference,  vk::DynamicState::eStencilWriteMask,
		    vk::DynamicState::eStencilTestEnable, vk::DynamicState::eStencilOp,
		};
		std::vector<vk::DynamicState> output_dynamic {vk::DynamicState::eBlendConstants};
#if !defined(__APPLE__)
		fragment_dynamic.push_back(vk::DynamicState::eDepthBoundsTestEnable);
		fragment_dynamic.push_back(vk::DynamicState::eDepthBounds);
		if (rendering.color_count != 0) {
			output_dynamic.push_back(vk::DynamicState::eColorWriteEnableEXT);
		}
#endif
		if (graphics.attachment_feedback_loop_enabled) {
			// The one state the specification does not assign to a subset - see note 159 §1.3 and
			// Vulkan-Docs issue #2504. Declared in all three subsets that either reading allows, so
			// that whichever one really owns it, the linked union has it. It is a device-global, so
			// it is the same for every pipeline and cannot split the fragment key.
			pre_raster_dynamic.push_back(vk::DynamicState::eAttachmentFeedbackLoopEnableEXT);
			fragment_dynamic.push_back(vk::DynamicState::eAttachmentFeedbackLoopEnableEXT);
			output_dynamic.push_back(vk::DynamicState::eAttachmentFeedbackLoopEnableEXT);
		}

		// The fragment library must not see the colour attachments: those are fragment *output*
		// state, they are not in the key, and a library that captured them would be reused under a
		// key that does not describe it.
		vk::PipelineRenderingCreateInfo fragment_rendering {};
		fragment_rendering.depthAttachmentFormat   = rendering.depth_format;
		fragment_rendering.stencilAttachmentFormat = rendering.stencil_format;
		// Only the view mask reaches the pre-rasterization subset, and it is always zero here.
		const vk::PipelineRenderingCreateInfo pre_raster_rendering {};

		const auto build_library =
		    [&](vk::GraphicsPipelineLibraryFlagsEXT parts, const std::vector<vk::DynamicState>& dyn,
		        vk::PipelineLayout layout, const vk::PipelineShaderStageCreateInfo* stage,
		        uint32_t stage_count, const vk::PipelineRenderingCreateInfo* render) {
			    vk::PipelineDynamicStateCreateInfo dynamic {};
			    dynamic.dynamicStateCount = static_cast<uint32_t>(dyn.size());
			    dynamic.pDynamicStates    = dyn.data();

			    vk::GraphicsPipelineLibraryCreateInfoEXT library {};
			    library.pNext = render;
			    library.flags = parts;

			    vk::GraphicsPipelineCreateInfo info {};
			    info.pNext             = &library;
			    info.flags             = vk::PipelineCreateFlagBits::eLibraryKHR;
			    info.layout            = layout;
			    info.basePipelineIndex = -1;
			    info.pDynamicState     = dyn.empty() ? nullptr : &dynamic;
			    if (parts & vk::GraphicsPipelineLibraryFlagBitsEXT::eVertexInputInterface) {
				    info.pVertexInputState   = &vertex_input_info;
				    info.pInputAssemblyState = &input_assembly;
			    }
			    if (parts & vk::GraphicsPipelineLibraryFlagBitsEXT::ePreRasterizationShaders) {
				    info.stageCount          = stage_count;
				    info.pStages             = stage;
				    info.pViewportState      = &viewport_state;
				    info.pRasterizationState = &rasterizer;
				    info.pTessellationState =
				        (rect_list || tessellation) ? &tessellation_state : nullptr;
			    }
			    if (parts & vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentShader) {
				    info.stageCount = stage_count;
				    info.pStages    = stage_count != 0 ? stage : nullptr;
				    // Always, unlike the monolithic path: a fragment-shader library without
				    // fragment output state has no render pass to infer from, so the structure is
				    // required (VUID-VkGraphicsPipelineCreateInfo-renderPass-09035).
				    info.pDepthStencilState = &depth_stencil_info;
				    info.pMultisampleState  = &multisampling;
			    }
			    if (parts & vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentOutputInterface) {
				    info.pColorBlendState  = &color_blending;
				    info.pMultisampleState = &multisampling;
			    }
			    vk::Pipeline built        = nullptr;
			    const auto   built_result = graphics.device.createGraphicsPipelines(
			        driver_cache, 1, &info, nullptr, &built);
			    EXIT_NOT_IMPLEMENTED(built_result != vk::Result::eSuccess);
			    EXIT_NOT_IMPLEMENTED(built == nullptr);
			    return built;
		    };

		pipeline.library_vertex_input =
		    build_library(vk::GraphicsPipelineLibraryFlagBitsEXT::eVertexInputInterface, {},
		                  pipeline.pipeline_layout, nullptr, 0, &rendering_info);
		pipeline.library_fragment_output =
		    build_library(vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentOutputInterface,
		                  output_dynamic, pipeline.pipeline_layout, nullptr, 0, &rendering_info);

		const auto push_stages =
		    static_cast<uint32_t>(static_cast<vk::ShaderStageFlags::MaskType>(graphics_stages));

		PreRasterLibraryKey pre_raster_key {};
		for (uint32_t i = 0; i < vertex_info.size(); i++) {
			pre_raster_key.vertex_shader_ids[i] = programs.vertex[i].id;
		}
		pre_raster_key.rect_list              = rect_list;
		pre_raster_key.rect_list_ps_shader_id = rect_list && ps_active ? pixel_program.id : 0;
		pre_raster_key.push_stages            = push_stages;
		pre_raster_key.patch_control_points =
		    (rect_list || tessellation) ? tessellation_state.patchControlPoints : 0u;
		pre_raster_key.negative_one_to_one = static_params.negative_one_to_one;
		pre_raster_key.depth_clip_enable   = static_params.depth_clip_enable;
		pre_raster_key.cull_front          = static_params.cull_front;
		pre_raster_key.cull_back           = static_params.cull_back;
		pre_raster_key.face                = static_params.face;
		pre_raster_key.provoking_vtx_last  = static_params.provoking_vtx_last;
		pre_raster_key.polygon_mode        = static_params.polygon_mode;

		const auto* pre_raster = libraries->pre_raster.Find(pre_raster_key);
		if (pre_raster == nullptr) {
			// Like the fragment entry, the cache owns its own set 0 layout; `vertex_bindings` and
			// so the push-descriptor decision are a function of the vertex programs the key pins.
			PreRasterLibraryCache::Entry fresh {};
			fresh.set_layout = CreateOneDescriptorLayout(graphics, pipeline.uses_push_descriptors,
			                                             vertex_bindings);

			const vk::DescriptorSetLayout
			    pre_raster_sets[ShaderRecompiler::IR::BindlessIntegerDescriptorSet + 1u] = {
			        fresh.set_layout, nullptr,
			        vertex_bindless ? BindlessImageHeap::SetLayout(graphics) : nullptr,
			        vertex_integer ? BindlessImageHeap::IntegerSetLayout(graphics) : nullptr};
			vk::PipelineLayoutCreateInfo pre_raster_layout_info {};
			pre_raster_layout_info.flags = vk::PipelineLayoutCreateFlagBits::eIndependentSetsEXT;
			pre_raster_layout_info.setLayoutCount =
			    vertex_bindless ? BindlessSetCount(true, vertex_integer) : 1u;
			pre_raster_layout_info.pSetLayouts            = pre_raster_sets;
			pre_raster_layout_info.pushConstantRangeCount = 1;
			pre_raster_layout_info.pPushConstantRanges    = &push_constants;
			EXIT_NOT_IMPLEMENTED(graphics.device.createPipelineLayout(&pre_raster_layout_info,
			                                                          nullptr, &fresh.layout) !=
			                     vk::Result::eSuccess);

			fresh.library =
			    build_library(vk::GraphicsPipelineLibraryFlagBitsEXT::ePreRasterizationShaders,
			                  pre_raster_dynamic, fresh.layout, shader_stages,
			                  pre_raster_stage_count, &pre_raster_rendering);
			pre_raster = libraries->pre_raster.Insert(pre_raster_key, fresh);
		}
		EXIT_IF(pre_raster == nullptr);

		FragmentLibraryKey fragment_key {};
		fragment_key.ps_shader_id          = ps_active ? pixel_program.id : 0;
		fragment_key.push_stages           = push_stages;
		fragment_key.samples               = static_params.samples;
		fragment_key.sample_shading_enable = static_params.sample_shading_enable;
		fragment_key.stencil_test_enable   = static_params.stencil_test_enable;
		fragment_key.stencil_front         = static_params.stencil_front;
		fragment_key.stencil_back          = static_params.stencil_back;
		fragment_key.depth_format          = rendering.depth_format;
		fragment_key.stencil_format        = rendering.stencil_format;

		const auto* entry = libraries->fragment.Find(fragment_key);
		if (entry == nullptr) {
			// The cache owns its own copy of the set 1 layout rather than borrowing this
			// pipeline's, because the library outlives any one pipeline. Vulkan defines layout
			// compatibility by content, so the two are interchangeable at link time; they are built
			// from the same `pixel_bindings`, which the key's ps_shader_id pins.
			FragmentLibraryCache::Entry fresh {};
			fresh.set_layout =
			    ps_active ? CreateOneDescriptorLayout(graphics, false, pixel_bindings) : nullptr;

			const vk::DescriptorSetLayout
			    fragment_sets[ShaderRecompiler::IR::BindlessIntegerDescriptorSet + 1u] = {
			        nullptr, fresh.set_layout,
			        pixel_bindless ? BindlessImageHeap::SetLayout(graphics) : nullptr,
			        pixel_integer ? BindlessImageHeap::IntegerSetLayout(graphics) : nullptr};
			vk::PipelineLayoutCreateInfo fragment_layout_info {};
			fragment_layout_info.flags = vk::PipelineLayoutCreateFlagBits::eIndependentSetsEXT;
			fragment_layout_info.setLayoutCount =
			    pixel_bindless ? BindlessSetCount(true, pixel_integer)
			    : ps_active    ? ShaderRecompiler::IR::NativeDescriptorSetCount
			                   : 0u;
			fragment_layout_info.pSetLayouts            = fragment_sets;
			fragment_layout_info.pushConstantRangeCount = 1;
			fragment_layout_info.pPushConstantRanges    = &push_constants;
			EXIT_NOT_IMPLEMENTED(graphics.device.createPipelineLayout(&fragment_layout_info,
			                                                          nullptr, &fresh.layout) !=
			                     vk::Result::eSuccess);

			fresh.library = build_library(vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentShader,
			                              fragment_dynamic, fresh.layout, fragment_stage_info,
			                              ps_active ? 1u : 0u, &fragment_rendering);
			entry         = libraries->fragment.Insert(fragment_key, fresh);
		}
		EXIT_IF(entry == nullptr);

		const vk::Pipeline linked[4] = {pipeline.library_vertex_input, pre_raster->library,
		                                entry->library, pipeline.library_fragment_output};
		vk::PipelineLibraryCreateInfoKHR link {};
		link.libraryCount = 4;
		link.pLibraries   = linked;

		vk::GraphicsPipelineCreateInfo linked_info {};
		linked_info.pNext             = &link;
		linked_info.layout            = pipeline.pipeline_layout;
		linked_info.basePipelineIndex = -1;
		result = graphics.device.createGraphicsPipelines(driver_cache, 1, &linked_info, nullptr,
		                                                 &pipeline.pipeline);
	}
	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreateGraphicsPipelines done result=%s pipeline=%p\n",
		     vk::to_string(result).c_str(), static_cast<void*>(pipeline.pipeline));
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	EXIT_NOT_IMPLEMENTED(pipeline.pipeline == nullptr);

	if (tess_control_shader_module != nullptr) {
		graphics.device.destroyShaderModule(tess_control_shader_module, nullptr);
	}
	if (tess_eval_shader_module != nullptr) {
		graphics.device.destroyShaderModule(tess_eval_shader_module, nullptr);
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const ShaderComputeInputInfo& input_info,
                            vk::ShaderModule compute_module, vk::PipelineCache driver_cache) {
	EXIT_IF(compute_module == nullptr);

	vk::PipelineShaderStageCreateInfo                     comp_shader_stage_info {};
	vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo comp_subgroup_size {};
	comp_shader_stage_info.stage  = vk::ShaderStageFlagBits::eCompute;
	comp_shader_stage_info.module = compute_module;
	comp_shader_stage_info.pName  = "main";
	EXIT_IF(!input_info.stage);
	const auto wave_size = input_info.stage.program->wave_size;
	if (graphics.compute_subgroup_size_control_enabled && wave_size >= graphics.min_subgroup_size &&
	    wave_size <= graphics.max_subgroup_size) {
		comp_subgroup_size.requiredSubgroupSize = wave_size;
		comp_shader_stage_info.pNext            = &comp_subgroup_size;
	}

	std::vector<vk::DescriptorSetLayoutBinding> descriptor_bindings;
	std::vector<vk::DescriptorSetLayoutBinding> bindless_bindings;
	AddLayoutBindings(descriptor_bindings, *input_info.stage.program,
	                  vk::ShaderStageFlagBits::eCompute);
	AddBindlessLimitBindings(bindless_bindings, *input_info.stage.program,
	                         vk::ShaderStageFlagBits::eCompute);
	CreateDescriptorLayout(graphics, pipeline, descriptor_bindings, bindless_bindings);
	const vk::PushConstantRange push_constants {vk::ShaderStageFlagBits::eCompute, 0,
	                                            ShaderRecompiler::IR::NativePushConstantSize};

	const bool uses_bindless = !bindless_bindings.empty();
	const bool uses_integer  = input_info.stage.program->bindings.bindless_integer;
	EXIT_IF(uses_bindless && graphics.GetPhysicalDeviceProperties().limits.maxBoundDescriptorSets <
	                             BindlessSetCount(true, uses_integer));
	const vk::DescriptorSetLayout
	    set_layouts[ShaderRecompiler::IR::BindlessIntegerDescriptorSet + 1u] = {
	        pipeline.descriptor_set_layout,
	        uses_bindless ? BindlessImageHeap::EmptySetLayout(graphics) : nullptr,
	        uses_bindless ? BindlessImageHeap::SetLayout(graphics) : nullptr,
	        uses_integer ? BindlessImageHeap::IntegerSetLayout(graphics) : nullptr};
	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount = uses_bindless ? BindlessSetCount(true, uses_integer) : 1u;
	pipeline_layout_info.pSetLayouts    = set_layouts;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_constants;

	EXIT_IF(pipeline.pipeline_layout != nullptr);

	LOGF("PipelineTrace: vkCreatePipelineLayout CS begin set_layouts=1 push_constants=%u\n", 1u);
	auto result = graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                                   &pipeline.pipeline_layout);
	LOGF("PipelineTrace: vkCreatePipelineLayout CS done result=%s layout=%p\n",
	     vk::to_string(result).c_str(), static_cast<void*>(pipeline.pipeline_layout));
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	EXIT_NOT_IMPLEMENTED(pipeline.pipeline_layout == nullptr);

	vk::ComputePipelineCreateInfo info {};
	info.stage             = comp_shader_stage_info;
	info.layout            = pipeline.pipeline_layout;
	info.basePipelineIndex = -1;

	EXIT_IF(pipeline.pipeline != nullptr);

	LOGF("PipelineTrace: vkCreateComputePipelines begin layout=%p\n",
	     static_cast<void*>(pipeline.pipeline_layout));
	result =
	    graphics.device.createComputePipelines(driver_cache, 1, &info, nullptr, &pipeline.pipeline);
	LOGF("PipelineTrace: vkCreateComputePipelines done result=%s pipeline=%p\n",
	     vk::to_string(result).c_str(), static_cast<void*>(pipeline.pipeline));
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	EXIT_NOT_IMPLEMENTED(pipeline.pipeline == nullptr);
}

} // namespace Libs::Graphics
