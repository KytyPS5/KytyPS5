#include "graphics/host_gpu/renderer/perVertexPrototype.h"

#include "common/file.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/perVertexEmbeddedSpv.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "kytyGitVersion.h"
#include "per_vertex_unpack_spv.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <map>
#include <regex>
#include <set>
#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>
#include <sstream>
#include <string>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics {
namespace {
uint32_t HalfToFloatBits(uint16_t half) {
	const uint32_t sign     = static_cast<uint32_t>(half & 0x8000u) << 16u;
	const uint32_t fraction = half & 0x03ffu;
	const uint32_t exponent = (half >> 10u) & 0x1fu;
	if (exponent == 0u) {
		if (fraction == 0u) return sign;
		uint32_t normalized        = fraction;
		int32_t  unbiased_exponent = -14;
		while ((normalized & 0x0400u) == 0u) {
			normalized <<= 1u;
			--unbiased_exponent;
		}
		return sign | (static_cast<uint32_t>(unbiased_exponent + 127) << 23u) |
		       ((normalized & 0x03ffu) << 13u);
	}
	if (exponent == 0x1fu) return sign | 0x7f800000u | (fraction << 13u);
	return sign | ((exponent - 15u + 127u) << 23u) | (fraction << 13u);
}

struct Original {
	ShaderType            stage;
	bool                  per_vertex;
	std::vector<uint32_t> words;
};
std::map<uint64_t, Original>                                        originals;
std::map<std::pair<uint64_t, uint64_t>, PerVertexPrototypePrograms> variants;
PerVertexUnpackPipeline                                             s_unpack_pipeline {};
bool                                                                s_unpack_initialized = false;
} // namespace

#include "graphics/host_gpu/renderer/perVertexTransform.h"

bool PerVertexPrototypeEnabled() {
	return std::getenv("KYTY_PER_VERTEX_PROTOTYPE") != nullptr;
}

bool HasPerVertexPrototypeInput(std::span<const uint32_t> words) {
	if (words.size() < 5 || words[0] != spv::MagicNumber) return false;
	bool found = false;
	for (size_t offset = 5; offset < words.size();) {
		const auto count = words[offset] >> 16;
		if (count == 0 || count > words.size() - offset) return false;
		if ((words[offset] & 0xffffu) == spv::OpDecorate && count == 3 &&
		    words[offset + 2] == spv::DecorationPerVertexKHR)
			found = true;
		offset += count;
	}
	return found;
}

bool DecodePerVertexPrototypeAttribute(Prospero::BufferFormat   format,
                                       std::span<const uint8_t> bytes,
                                       std::span<uint32_t, 4>   components) {
	const auto info = ShaderRecompiler::Format::GetFormatInfo(format);
	using Type      = ShaderRecompiler::Format::ComponentType;
	if (info.component_count == 0 || info.component_count > 4 || bytes.size() < info.byte_size)
		return false;
	if (info.type != Type::Float && info.type != Type::Unorm &&
	    !(format == Prospero::BufferFormat::k16_16_16_16SNorm && info.type == Type::Snorm))
		return false;
	for (uint32_t c = 0; c < info.component_count; ++c) {
		if (info.type == Type::Float && format != Prospero::BufferFormat::k16_16_16_16Float &&
		    info.component_bits[c] != 32)
			return false;
		if (format == Prospero::BufferFormat::k16_16_16_16Float && info.component_bits[c] != 16)
			return false;
		if (info.type == Type::Unorm &&
		    (info.component_bits[c] == 0 || info.component_bits[c] > 16))
			return false;
		if (format == Prospero::BufferFormat::k16_16_16_16SNorm && info.component_bits[c] != 16)
			return false;
	}
	for (uint32_t c = 0; c < 4; ++c) {
		uint32_t value = c == 3 ? 0x3f800000u : 0u;
		if (c < info.component_count) {
			if (format == Prospero::BufferFormat::k16_16_16_16Float) {
				uint16_t raw = 0;
				std::memcpy(&raw, bytes.data() + info.component_bit_offset[c] / 8, sizeof(raw));
				value = HalfToFloatBits(raw);
			} else if (info.type == Type::Float) {
				std::memcpy(&value, bytes.data() + info.component_bit_offset[c] / 8, 4);
			} else if (format == Prospero::BufferFormat::k16_16_16_16SNorm) {
				int16_t raw = 0;
				std::memcpy(&raw, bytes.data() + info.component_bit_offset[c] / 8, sizeof(raw));
				value =
				    std::bit_cast<uint32_t>(std::max(static_cast<float>(raw) / 32767.0f, -1.0f));
			} else {
				uint32_t raw = 0;
				for (uint32_t bit = 0; bit < info.component_bits[c]; ++bit) {
					const auto position = info.component_bit_offset[c] + bit;
					raw |= ((bytes[position / 8] >> (position % 8)) & 1u) << bit;
				}
				const auto mask = (1u << info.component_bits[c]) - 1u;
				value = std::bit_cast<uint32_t>(static_cast<float>(raw) / static_cast<float>(mask));
			}
		}
		components[c] = value;
	}
	return true;
}

void RememberPerVertexPrototypeShader(ShaderType stage, const ShaderProgram& program,
                                      std::span<const uint32_t> words) {
	if (!PerVertexPrototypeEnabled()) return;
	const bool per_vertex = stage == ShaderType::Pixel && HasPerVertexPrototypeInput(words);
	if (stage == ShaderType::Vertex || per_vertex) {
		originals.emplace(program.id, Original {stage, per_vertex, {words.begin(), words.end()}});
	}
}

const PerVertexUnpackPipeline* GetPerVertexUnpackPipeline(GraphicContext&   graphics,
                                                          vk::PipelineCache driver_cache) {
	if (s_unpack_initialized) return &s_unpack_pipeline;

	s_unpack_pipeline.module = CompileSPV(PERVERTEX_UNPACK_SPV, graphics.device);

	const vk::DescriptorSetLayoutBinding bindings[] {
	    {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
	    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
	    {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
	    {3, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute}};
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.bindingCount = 4;
	layout_info.pBindings    = bindings;
	EXIT_IF(graphics.device.createDescriptorSetLayout(
	            &layout_info, nullptr, &s_unpack_pipeline.set_layout) != vk::Result::eSuccess);

	const vk::PushConstantRange  push(vk::ShaderStageFlagBits::eCompute, 0,
	                                  sizeof(UnpackPushConstants));
	vk::PipelineLayoutCreateInfo pipe_layout_info {};
	pipe_layout_info.setLayoutCount         = 1;
	pipe_layout_info.pSetLayouts            = &s_unpack_pipeline.set_layout;
	pipe_layout_info.pushConstantRangeCount = 1;
	pipe_layout_info.pPushConstantRanges    = &push;
	EXIT_IF(graphics.device.createPipelineLayout(&pipe_layout_info, nullptr,
	                                             &s_unpack_pipeline.pipeline_layout) !=
	        vk::Result::eSuccess);

	vk::ComputePipelineCreateInfo compute_info {};
	compute_info.stage.stage  = vk::ShaderStageFlagBits::eCompute;
	compute_info.stage.module = s_unpack_pipeline.module;
	compute_info.stage.pName  = "main";
	compute_info.layout       = s_unpack_pipeline.pipeline_layout;
	vk::Result res = graphics.device.createComputePipelines(driver_cache, 1, &compute_info, nullptr,
	                                                        &s_unpack_pipeline.pipeline);
	if (res != vk::Result::eSuccess && driver_cache != nullptr) {
		LOGF("PerVertexPrototype: unpack createComputePipelines failed with driver_cache (%s), "
		     "retrying with nullptr\n",
		     vk::to_string(res).c_str());
		res = graphics.device.createComputePipelines(nullptr, 1, &compute_info, nullptr,
		                                             &s_unpack_pipeline.pipeline);
	}
	EXIT_IF(res != vk::Result::eSuccess);

	s_unpack_initialized = true;
	return &s_unpack_pipeline;
}

const PerVertexPrototypePrograms* GetPerVertexPrototypePrograms(
    GraphicContext& graphics, const PipelineCache::GraphicsPrograms& programs,
    const ShaderVertexInputInfo& vs_info, vk::PipelineCache driver_cache) {
	if (!PerVertexPrototypeEnabled()) return nullptr;
	if (vs_info.logical_stage != ShaderType::Vertex) return nullptr;
	const auto ps = originals.find(programs.pixel.id);
	if (ps == originals.end() || !ps->second.per_vertex) return nullptr;
	const auto vs = originals.find(programs.vertex[0].id);
	EXIT_IF(vs == originals.end() || vs->second.stage != ShaderType::Vertex ||
	        programs.VertexStageCount() != 1);
	const auto key = std::pair {programs.vertex[0].id, programs.pixel.id};
	if (auto it = variants.find(key); it != variants.end()) return &it->second;

	const uint64_t vs_hash =
	    XXH3_64bits(vs->second.words.data(), vs->second.words.size() * sizeof(uint32_t));
	const uint64_t ps_hash =
	    XXH3_64bits(ps->second.words.data(), ps->second.words.size() * sizeof(uint32_t));
	const auto        title_id           = PipelineCacheTitleId();
	const std::string effective_title_id = title_id.empty() ? "default" : title_id;

	PerVertexLayout       layout {};
	std::vector<uint32_t> cap_words, frag_words, replay_words;
	const auto            t0               = std::chrono::steady_clock::now();
	bool                  loaded_from_disk = TryLoadTransformedShadersFromDisk(
	    effective_title_id, vs_hash, ps_hash, layout, cap_words, frag_words, replay_words);

	if (loaded_from_disk) {
		const auto load_us = std::chrono::duration_cast<std::chrono::microseconds>(
		                         std::chrono::steady_clock::now() - t0)
		                         .count();
		LOGF("PerVertexPrototype: bytecode cache HIT for VS=0x%016llx PS=0x%016llx (loaded in %lld "
		     "us)\n",
		     static_cast<unsigned long long>(vs_hash), static_cast<unsigned long long>(ps_hash),
		     static_cast<long long>(load_us));
	} else {
		spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
		tools.SetMessageConsumer([](spv_message_level_t, const char*,
		                            const spv_position_t& position, const char* message) {
			LOGF("PerVertexPrototype SPIR-V word %zu: %s\n", position.index, message);
		});
		std::string vs_dis, ps_dis;
		EXIT_IF(!tools.Disassemble(vs->second.words.data(), vs->second.words.size(), &vs_dis,
		                           SPV_BINARY_TO_TEXT_OPTION_INDENT |
		                               SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES));
		EXIT_IF(!tools.Disassemble(ps->second.words.data(), ps->second.words.size(), &ps_dis,
		                           SPV_BINARY_TO_TEXT_OPTION_INDENT |
		                               SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES));

		std::map<uint32_t, std::string> vs_param_vars;
		if (!DerivePerVertexLayout(vs_dis, ps_dis, layout, vs_param_vars)) {
			LOGF("PerVertexPrototype: interface contract validation failed for VS=%llu PS=%llu -> "
			     "rejecting\n",
			     static_cast<unsigned long long>(key.first),
			     static_cast<unsigned long long>(key.second));
			return nullptr;
		}

		std::string cap_dis    = LowerVertexToCompute(vs_dis, layout, vs_param_vars);
		std::string frag_dis   = LowerFragmentToBufferReplay(ps_dis, layout);
		std::string replay_dis = GenerateReplayVertexSpvasm(layout);

		EXIT_IF(
		    !tools.Assemble(cap_dis, &cap_words, SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS));
		EXIT_IF(
		    !tools.Assemble(frag_dis, &frag_words, SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS));
		EXIT_IF(!tools.Assemble(replay_dis, &replay_words,
		                        SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS));
		EXIT_IF(!tools.Validate(cap_words));
		EXIT_IF(!tools.Validate(frag_words));
		EXIT_IF(!tools.Validate(replay_words));

		SaveTransformedShadersToDisk(effective_title_id, vs_hash, ps_hash, layout, cap_words,
		                             frag_words, replay_words);
		const auto comp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		                         std::chrono::steady_clock::now() - t0)
		                         .count();
		LOGF("PerVertexPrototype: bytecode cache MISS for VS=0x%016llx PS=0x%016llx (lowered & "
		     "assembled in %lld ms)\n",
		     static_cast<unsigned long long>(vs_hash), static_cast<unsigned long long>(ps_hash),
		     static_cast<long long>(comp_ms));
	}

	PerVertexPrototypePrograms value {};
	value.graphics                  = programs;
	value.layout                    = layout;
	value.capture                   = CompileSPV(cap_words, graphics.device);
	value.graphics.vertex[0].module = CompileSPV(replay_words, graphics.device);
	value.graphics.pixel.module     = CompileSPV(frag_words, graphics.device);
	value.graphics.vertex[0].id |= 1ull << 63;
	value.graphics.pixel.id |= 1ull << 63;

	const vk::DescriptorSetLayoutBinding bindings[] {
	    {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
	    {1, vk::DescriptorType::eStorageBuffer, 1,
	     vk::ShaderStageFlagBits::eCompute | vk::ShaderStageFlagBits::eVertex |
	         vk::ShaderStageFlagBits::eFragment},
	    {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute}};
	vk::DescriptorSetLayoutCreateInfo create {};
	create.bindingCount = 3;
	create.pBindings    = bindings;
	EXIT_IF(graphics.device.createDescriptorSetLayout(&create, nullptr, &value.extra_layout) !=
	        vk::Result::eSuccess);
	value.graphics.prototype_extra_layout = value.extra_layout;

	// Build capture compute pipeline once for this pair
	value.capture_pipeline.prototype_vertex_capture = true;
	value.capture_pipeline.uses_push_descriptors    = false;
	std::vector<vk::DescriptorSetLayoutBinding> resources;
	for (const auto& binding: vs_info.stage.program->bindings.descriptors) {
		resources.emplace_back(
		    ShaderRecompiler::IR::NativeBinding(ShaderType::Vertex, binding.kind),
		    NativeDescriptorType(binding.kind), NativeDescriptorCount(binding),
		    vk::ShaderStageFlagBits::eCompute);
	}
	vk::DescriptorSetLayoutCreateInfo resource_info {};
	resource_info.bindingCount = static_cast<uint32_t>(resources.size());
	resource_info.pBindings    = resources.data();
	EXIT_IF(graphics.device.createDescriptorSetLayout(
	            &resource_info, nullptr, &value.capture_pipeline.descriptor_set_layout) !=
	        vk::Result::eSuccess);

	const vk::DescriptorSetLayout layouts[] {value.capture_pipeline.descriptor_set_layout,
	                                         value.extra_layout};
	const vk::PushConstantRange   push(vk::ShaderStageFlagBits::eCompute, 0,
	                                   ShaderRecompiler::IR::NativePushConstantSize);
	vk::PipelineLayoutCreateInfo  layout_info {};
	layout_info.setLayoutCount         = 2;
	layout_info.pSetLayouts            = layouts;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &push;
	EXIT_IF(graphics.device.createPipelineLayout(&layout_info, nullptr,
	                                             &value.capture_pipeline.pipeline_layout) !=
	        vk::Result::eSuccess);

	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage.stage  = vk::ShaderStageFlagBits::eCompute;
	pipeline_info.stage.module = value.capture;
	pipeline_info.stage.pName  = "main";
	pipeline_info.layout       = value.capture_pipeline.pipeline_layout;
	vk::Result pipe_res        = graphics.device.createComputePipelines(
	    driver_cache, 1, &pipeline_info, nullptr, &value.capture_pipeline.pipeline);
	if (pipe_res != vk::Result::eSuccess && driver_cache != nullptr) {
		LOGF("PerVertexPrototype: capture createComputePipelines failed with driver_cache (%s), "
		     "retrying with nullptr\n",
		     vk::to_string(pipe_res).c_str());
		pipe_res = graphics.device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr,
		                                                  &value.capture_pipeline.pipeline);
	}
	EXIT_IF(pipe_res != vk::Result::eSuccess);

	LOGF("PerVertexPrototype: compiled native C++ pair VS=%llu PS=%llu\n",
	     static_cast<unsigned long long>(key.first), static_cast<unsigned long long>(key.second));
	return &variants.emplace(key, value).first->second;
}

} // namespace Libs::Graphics
