#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/renderer/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/shaderSubgroup.h"
#include "graphics/shader/recompiler/ExecMask.h"
#include "graphics/shader/recompiler/ResourceTracking.h"
#include "graphics/shader/recompiler/ScalarProvenance.h"
#include "graphics/shader/recompiler/ShaderCFG.h"
#include "graphics/shader/recompiler/ShaderDecoder.h"
#include "graphics/shader/recompiler/ShaderIR.h"
#include "graphics/shader/recompiler/ShaderInfoCollection.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/SpirvEmitter.h"
#include "graphics/shader/recompiler/SrtPatcher.h"
#include "graphics/shader/recompiler/SrtWalker.h"
#include "graphics/shader/recompiler/spirvEmitter/spirvEmitterInternal.h"
#include "graphics/shader/shader.h"
#include "libs/agc.h"
#include "spirv-tools/libspirv.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace Libs::Graphics {
namespace {

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "ShaderCfgTests: failed: %s\n", text);
		std::abort();
	}
}

void CheckSpirvBinaryValidates(const std::vector<uint32_t>& binary) {
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
	std::string          messages;

	tools.SetMessageConsumer([&messages](spv_message_level_t, const char*,
	                                     const spv_position_t& position, const char* message) {
		char buffer[1024] = {};
		std::snprintf(buffer, sizeof(buffer), "%zu:%zu: %s\n", position.line, position.column,
		              message);
		messages += buffer;
	});

	if (!tools.Validate(binary)) {
		std::fprintf(stderr, "SPIR-V binary validation failed:\n%s\n", messages.c_str());
		std::abort();
	}
}

std::string DisassembleSpirvBinary(const std::vector<uint32_t>& binary) {
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
	std::string          messages;
	std::string          source;

	tools.SetMessageConsumer([&messages](spv_message_level_t, const char*,
	                                     const spv_position_t& position, const char* message) {
		char buffer[1024] = {};
		std::snprintf(buffer, sizeof(buffer), "%zu:%zu: %s\n", position.line, position.column,
		              message);
		messages += buffer;
	});

	if (!tools.Disassemble(binary.data(), binary.size(), &source,
	                       SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES |
	                           SPV_BINARY_TO_TEXT_OPTION_INDENT)) {
		std::fprintf(stderr, "SPIR-V binary disassembly failed:\n%s\n", messages.c_str());
		std::abort();
	}
	return std::string(source.c_str());
}

uint32_t CountSourceOccurrences(const std::string& source, const char* needle) {
	uint32_t count = 0;
	uint32_t from  = 0;
	for (;;) {
		const auto found = Common::FindIndex(source, std::string(needle), from);
		if (found == Common::FIND_INVALID_INDEX) {
			return count;
		}
		count++;
		from = found + static_cast<uint32_t>(std::strlen(needle));
	}
}

bool SpirvSourceHasInstructionUsing(const std::string& source, const char* opcode,
                                    const char* name) {
	std::istringstream stream(source);
	std::string        line;
	while (std::getline(stream, line)) {
		if (Common::ContainsStr(line, opcode) && Common::ContainsStr(line, name)) {
			return true;
		}
	}
	return false;
}

bool SpirvContainsOpcode(const std::vector<uint32_t>& binary, uint32_t opcode) {
	for (uint32_t word: binary) {
		if ((word & 0xffffu) == opcode) {
			return true;
		}
	}
	return false;
}

uint32_t SpirvInstructionOpcodeCount(const std::vector<uint32_t>& binary, uint32_t opcode) {
	uint32_t count = 0;
	for (size_t i = 5; i < binary.size();) {
		const uint32_t word       = binary[i];
		const uint32_t op         = word & 0xffffu;
		const uint32_t word_count = word >> 16u;
		if (word_count == 0 || i + word_count > binary.size()) {
			return count;
		}
		if (op == opcode) {
			count++;
		}
		i += word_count;
	}
	return count;
}

bool SpirvContainsCapability(const std::vector<uint32_t>& binary, uint32_t capability) {
	for (size_t i = 5; i < binary.size();) {
		const uint32_t word       = binary[i];
		const uint32_t opcode     = word & 0xffffu;
		const uint32_t word_count = word >> 16u;
		if (word_count == 0 || i + word_count > binary.size()) {
			return false;
		}
		if (opcode == 17u && word_count >= 2u && binary[i + 1] == capability) {
			return true;
		}
		i += word_count;
	}
	return false;
}

bool SpirvContainsExecutionMode(const std::vector<uint32_t>& binary, uint32_t mode) {
	for (size_t i = 5; i < binary.size();) {
		const uint32_t word       = binary[i];
		const uint32_t opcode     = word & 0xffffu;
		const uint32_t word_count = word >> 16u;
		if (word_count == 0 || i + word_count > binary.size()) {
			return false;
		}
		if (opcode == 16u && word_count >= 3u && binary[i + 2] == mode) {
			return true;
		}
		i += word_count;
	}
	return false;
}

bool SpirvContainsExtInst(const std::vector<uint32_t>& binary, uint32_t ext_inst) {
	for (size_t i = 5; i < binary.size();) {
		const uint32_t word       = binary[i];
		const uint32_t opcode     = word & 0xffffu;
		const uint32_t word_count = word >> 16u;
		if (word_count == 0 || i + word_count > binary.size()) {
			return false;
		}
		if (opcode == 12u && word_count >= 5u && binary[i + 4] == ext_inst) {
			return true;
		}
		i += word_count;
	}
	return false;
}

uint32_t SpirvExtInstCount(const std::vector<uint32_t>& binary, uint32_t ext_inst) {
	uint32_t count = 0;
	for (size_t i = 5; i < binary.size();) {
		const uint32_t word       = binary[i];
		const uint32_t opcode     = word & 0xffffu;
		const uint32_t word_count = word >> 16u;
		if (word_count == 0 || i + word_count > binary.size()) {
			return count;
		}
		if (opcode == 12u && word_count >= 5u && binary[i + 4] == ext_inst) {
			count++;
		}
		i += word_count;
	}
	return count;
}

bool SpirvContainsTypeImage(const std::vector<uint32_t>& binary, uint32_t dim, uint32_t arrayed,
                            uint32_t sampled) {
	for (size_t i = 5; i < binary.size();) {
		const uint32_t word       = binary[i];
		const uint32_t opcode     = word & 0xffffu;
		const uint32_t word_count = word >> 16u;
		if (word_count == 0 || i + word_count > binary.size()) {
			return false;
		}
		if (opcode == 25u && word_count >= 9u && binary[i + 3] == dim && binary[i + 5] == arrayed &&
		    binary[i + 7] == sampled) {
			return true;
		}
		i += word_count;
	}
	return false;
}

uint32_t SpirvDecorationValueCount(const std::vector<uint32_t>& binary, uint32_t decoration,
                                   uint32_t value) {
	uint32_t count = 0;
	for (size_t i = 5; i < binary.size();) {
		const uint32_t word       = binary[i];
		const uint32_t opcode     = word & 0xffffu;
		const uint32_t word_count = word >> 16u;
		if (word_count == 0 || i + word_count > binary.size()) {
			return count;
		}
		if (opcode == 71u && word_count >= 4u && binary[i + 2] == decoration &&
		    binary[i + 3] == value) {
			count++;
		}
		i += word_count;
	}
	return count;
}

bool SpirvHasDecorationValue(const std::vector<uint32_t>& binary, uint32_t decoration,
                             uint32_t value) {
	return SpirvDecorationValueCount(binary, decoration, value) != 0;
}

std::vector<uint32_t> SpirvDecorationValueTargets(const std::vector<uint32_t>& binary,
                                                  uint32_t decoration, uint32_t value) {
	std::vector<uint32_t> targets;
	for (size_t i = 5; i < binary.size();) {
		const uint32_t word       = binary[i];
		const uint32_t opcode     = word & 0xffffu;
		const uint32_t word_count = word >> 16u;
		if (word_count == 0 || i + word_count > binary.size()) {
			return targets;
		}
		if (opcode == 71u && word_count >= 4u && binary[i + 2] == decoration &&
		    binary[i + 3] == value) {
			targets.push_back(binary[i + 1]);
		}
		i += word_count;
	}
	return targets;
}

bool SpirvTargetHasDecoration(const std::vector<uint32_t>& binary, uint32_t target,
                              uint32_t decoration) {
	for (size_t i = 5; i < binary.size();) {
		const uint32_t word       = binary[i];
		const uint32_t opcode     = word & 0xffffu;
		const uint32_t word_count = word >> 16u;
		if (word_count == 0 || i + word_count > binary.size()) {
			return false;
		}
		if (opcode == 71u && word_count >= 3u && binary[i + 1] == target &&
		    binary[i + 2] == decoration) {
			return true;
		}
		i += word_count;
	}
	return false;
}

bool SpirvHasDecorationValueWithDecoration(const std::vector<uint32_t>& binary,
                                           uint32_t value_decoration, uint32_t value,
                                           uint32_t decoration) {
	const auto targets = SpirvDecorationValueTargets(binary, value_decoration, value);
	return std::any_of(targets.begin(), targets.end(), [&](uint32_t target) {
		return SpirvTargetHasDecoration(binary, target, decoration);
	});
}

bool ProgramHasInput(const ShaderRecompiler::IR::Program& program,
                     ShaderRecompiler::IR::StageInputKind kind) {
	return std::any_of(program.info.inputs.begin(), program.info.inputs.end(),
	                   [kind](const auto& input) { return input.kind == kind; });
}

uint32_t ProgramInputCount(const ShaderRecompiler::IR::Program& program,
                           ShaderRecompiler::IR::StageInputKind kind) {
	return static_cast<uint32_t>(
	    std::count_if(program.info.inputs.begin(), program.info.inputs.end(),
	                  [kind](const auto& input) { return input.kind == kind; }));
}

ShaderComputeInputInfo RegressionComputeInputInfo() {
	ShaderComputeInputInfo input_info;
	input_info.threads_num[0]     = 1;
	input_info.threads_num[1]     = 1;
	input_info.threads_num[2]     = 1;
	input_info.workgroup_register = 40;
	return input_info;
}

ShaderPixelInputInfo RegressionPixelInputInfo() {
	return {};
}

void SetIdentityInterpolatorSettings(ShaderPixelInputInfo* input_info) {
	Check(input_info != nullptr, "invalid pixel input info");
	for (uint32_t i = 0; i < std::size(input_info->interpolator_settings); i++) {
		input_info->interpolator_settings[i] = i;
	}
}

void EnsureConfigInitialized() {
	static bool config_initialized = false;
	if (!config_initialized) {
		Common::ThreadsSubsystem::Instance()->Init(nullptr);
		Config::ConfigSubsystem::Instance()->Init(nullptr);
		Config::ConfigOptions options;
		options.printf_direction = Config::OutputDirection::Silent;
		Config::Load(options);
		Log::LogSubsystem::Instance()->Init(nullptr);
		ShaderInit();
		config_initialized = true;
	}
}

void TestResourceDescriptorClassification() {
	uint32_t raw_texture[8] = {};
	raw_texture[3]          = 9u << 28u;
	raw_texture[5]          = 2u << 27u;
	Check(ShaderClassifyResourceDescriptor(raw_texture) == ResourceDescriptorType::Texture,
	      "raw texture descriptor must not be classified by ResourceDescriptor byte 23 fields");

	uint32_t tagged_buffer[8] = {};
	tagged_buffer[5]          = 1u << 27u;
	Check(ShaderClassifyResourceDescriptor(tagged_buffer) == ResourceDescriptorType::Buffer,
	      "tagged ResourceDescriptor buffer was not recognized");

	uint32_t tagged_sampler[8] = {};
	tagged_sampler[5]          = 2u << 27u;
	Check(ShaderClassifyResourceDescriptor(tagged_sampler) == ResourceDescriptorType::Sampler,
	      "tagged ResourceDescriptor sampler was not recognized");

	uint32_t tagged_unused[8] = {};
	tagged_unused[5]          = 3u << 27u;
	Check(ShaderClassifyResourceDescriptor(tagged_unused) == ResourceDescriptorType::Unused,
	      "tagged ResourceDescriptor unused slot was not recognized");

	uint32_t raw_buffer[8] = {};
	Check(ShaderClassifyResourceDescriptor(raw_buffer) == ResourceDescriptorType::Buffer,
	      "raw buffer descriptor fallback was not preserved");
}

constexpr uint32_t EncodeSMovB32(uint32_t dst, uint32_t src) {
	return 0x80000000u | (0x7du << 23u) | ((dst & 0x7fu) << 16u) | (0x03u << 8u) | (src & 0xffu);
}

constexpr uint32_t EncodeSop1(uint32_t opcode, uint32_t dst, uint32_t src) {
	return 0x80000000u | (0x7du << 23u) | ((dst & 0x7fu) << 16u) | ((opcode & 0xffu) << 8u) |
	       (src & 0xffu);
}

constexpr uint32_t EncodeSop2(uint32_t opcode, uint32_t dst, uint32_t src0, uint32_t src1) {
	return 0x80000000u | ((opcode & 0x7fu) << 23u) | ((dst & 0x7fu) << 16u) |
	       ((src1 & 0xffu) << 8u) | (src0 & 0xffu);
}

constexpr uint32_t EncodeSopk(uint32_t opcode, uint32_t dst, int16_t imm) {
	return 0x80000000u | (((opcode + 0x60u) & 0x7fu) << 23u) | ((dst & 0x7fu) << 16u) |
	       static_cast<uint16_t>(imm);
}

constexpr uint32_t EncodeSopc(uint32_t opcode, uint32_t src0, uint32_t src1) {
	return 0x80000000u | (0x7eu << 23u) | ((opcode & 0x7fu) << 16u) | ((src1 & 0xffu) << 8u) |
	       (src0 & 0xffu);
}

constexpr uint32_t EncodeSopp(uint32_t opcode, uint32_t simm = 0) {
	return 0x80000000u | (0x7fu << 23u) | ((opcode & 0x7fu) << 16u) | (simm & 0xffffu);
}

void TestNativeShaderResourceDependencies() {
	const auto stages = ShaderPipelineStages(
	    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
	Check(stages == (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
	                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
	      "shader-stage dependency mapping was not exact");
	const auto shader_barrier = MakeShaderWriteDependency();
	Check(shader_barrier.srcAccessMask == VK_ACCESS_SHADER_WRITE_BIT &&
	          (shader_barrier.dstAccessMask & VK_ACCESS_SHADER_READ_BIT) != 0 &&
	          (shader_barrier.dstAccessMask & VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT) != 0,
	      "shader-write dependency does not expose draw/dispatch buffer writes");

	ShaderRecompiler::IR::Program program;
	program.info.buffers.resize(3);
	program.info.buffers[0].written = true;
	program.info.buffers[1].read    = true;
	program.info.buffers[2].written = true;
	ShaderRecompiler::IR::ResourceSnapshot resources;
	resources.buffers.resize(3);
	auto set_buffer = [&](uint32_t index, uint64_t address, uint16_t stride, uint32_t records) {
		ShaderBufferResource descriptor;
		descriptor.UpdateAddress48(address);
		descriptor.fields[1] |= static_cast<uint32_t>(stride) << 16u;
		descriptor.fields[2] = records;
		std::memcpy(resources.buffers[index].dwords.data(), descriptor.fields,
		            sizeof(descriptor.fields));
		resources.buffers[index].dword_count = 4;
	};
	set_buffer(0, 0x1000, 16, 3);
	set_buffer(1, 0x1800, 4, 7);
	set_buffer(2, 0x2000, 0, 64);
	const auto writes = CollectShaderBufferWrites(program, resources);
	Check(writes == std::vector<ShaderBufferWriteRange>({{0x1000, 48}, {0x2000, 64}}),
	      "graphics/compute write collector lost or invented a storage-buffer range");

	VulkanImage image {VulkanImageType::StorageTexture};
	image.image              = reinterpret_cast<VkImage>(uintptr_t {1});
	image.layout             = VK_IMAGE_LAYOUT_GENERAL;
	image.layers             = 3;
	const auto image_barrier = MakeStorageImageDependency(image, true, true);
	Check(image_barrier.oldLayout == VK_IMAGE_LAYOUT_GENERAL &&
	          image_barrier.newLayout == VK_IMAGE_LAYOUT_GENERAL &&
	          (image_barrier.srcAccessMask & VK_ACCESS_MEMORY_WRITE_BIT) != 0 &&
	          (image_barrier.dstAccessMask & VK_ACCESS_SHADER_READ_BIT) != 0 &&
	          (image_barrier.dstAccessMask & VK_ACCESS_SHADER_WRITE_BIT) != 0 &&
	          image_barrier.subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS &&
	          image_barrier.subresourceRange.layerCount == image.layers,
	      "GENERAL storage-image dependency lacks a complete memory barrier");

	VulkanBuffer buffer;
	buffer.buffer          = reinterpret_cast<VkBuffer>(uintptr_t {1});
	const auto gds_barrier = MakeGdsDependency(buffer);
	Check((gds_barrier.srcAccessMask & VK_ACCESS_HOST_WRITE_BIT) != 0 &&
	          (gds_barrier.srcAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT) != 0 &&
	          (gds_barrier.srcAccessMask & VK_ACCESS_SHADER_WRITE_BIT) != 0 &&
	          gds_barrier.dstAccessMask ==
	              (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT) &&
	          gds_barrier.size == VK_WHOLE_SIZE,
	      "GDS dependency does not order host/transfer/shader writes");
}

void TestNativeSubgroupPolicy() {
	GraphicContext context;
	context.subgroup_size                 = 32;
	context.min_subgroup_size             = 32;
	context.max_subgroup_size             = 32;
	context.subgroup_size_control_enabled = true;
	context.required_subgroup_size_stages = VK_SHADER_STAGE_ALL;
	ShaderRecompiler::IR::Program safe;
	safe.wave_size      = 32;
	safe.lane_mask_mode = ShaderLaneMaskMode::NativeWave;
	Check(ConfigureShaderSubgroup(context, VK_SHADER_STAGE_VERTEX_BIT, safe).mode ==
	          ShaderSubgroupMode::Natural,
	      "native wave32 policy changed");
	safe.wave_size = 64;
	Check(SelectGraphicsLaneMaskMode(context, safe.wave_size) ==
	              ShaderLaneMaskMode::PerInvocation &&
	          ConfigureShaderSubgroup(context, VK_SHADER_STAGE_VERTEX_BIT, safe).mode ==
	              ShaderSubgroupMode::Unsupported,
	      "wave64 graphics mismatch accepted native-wave mask lowering");
	safe.lane_mask_mode = ShaderLaneMaskMode::PerInvocation;
	Check(ConfigureShaderSubgroup(context, VK_SHADER_STAGE_VERTEX_BIT, safe).mode ==
	          ShaderSubgroupMode::PerInvocationGraphics,
	      "wave64 graphics mismatch did not select per-invocation masks");

	ShaderRecompiler::IR::Program cross_lane = safe;
	cross_lane.blocks.emplace_back().instructions.emplace_back().op =
	    ShaderRecompiler::IR::Opcode::ReadLaneU32;
	Check(ShaderRecompiler::Spirv::ProgramRequiresExactSubgroupSize(cross_lane) &&
	          ConfigureShaderSubgroup(context, VK_SHADER_STAGE_VERTEX_BIT, cross_lane).mode ==
	              ShaderSubgroupMode::PerInvocationGraphics,
	      "graphics mismatch did not select per-invocation masks");
	auto cross_lane_compute           = cross_lane;
	cross_lane_compute.lane_mask_mode = ShaderLaneMaskMode::NativeWave;
	Check(ConfigureShaderSubgroup(context, VK_SHADER_STAGE_COMPUTE_BIT, cross_lane_compute).mode ==
	          ShaderSubgroupMode::Unsupported,
	      "cross-lane compute mismatch bypassed the exact subgroup requirement");

	ShaderRecompiler::IR::Program zero_exec = safe;
	zero_exec.lane_mask_mode                = ShaderLaneMaskMode::NativeWave;
	zero_exec.provenance.values.resize(3);
	zero_exec.provenance.values[2].op  = ShaderRecompiler::IR::ScalarValueOp::Constant;
	zero_exec.provenance.values[2].imm = 0;
	auto& zero_bfm             = zero_exec.blocks.emplace_back().instructions.emplace_back();
	zero_bfm.op                = ShaderRecompiler::IR::Opcode::BitFieldMaskU64;
	zero_bfm.dst.kind          = ShaderRecompiler::IR::OperandKind::Register;
	zero_bfm.dst.reg           = {ShaderRecompiler::IR::RegisterFile::Exec, 0};
	zero_bfm.src_count         = 2;
	zero_bfm.scalar_sources[0] = 2;
	Check(!ShaderRecompiler::Spirv::ProgramRequiresExactSubgroupSize(zero_exec) &&
	          ConfigureShaderSubgroup(context, VK_SHADER_STAGE_COMPUTE_BIT, zero_exec).mode ==
	              ShaderSubgroupMode::FlattenedMasks,
	      "compile-time uniform-zero EXEC write did not stay on the mask-free path");

	ShaderRecompiler::IR::Program selective_exec = safe;
	auto& move_exec    = selective_exec.blocks.emplace_back().instructions.emplace_back();
	move_exec.op       = ShaderRecompiler::IR::Opcode::MoveU32;
	move_exec.dst.kind = ShaderRecompiler::IR::OperandKind::Register;
	move_exec.dst.reg  = {ShaderRecompiler::IR::RegisterFile::Exec, 0};
	Check(ShaderRecompiler::Spirv::ProgramRequiresExactSubgroupSize(selective_exec),
	      "selective EXEC write was classified mask-free");

	ShaderRecompiler::IR::Program carry = safe;
	auto& add_carry                     = carry.blocks.emplace_back().instructions.emplace_back();
	add_carry.op                        = ShaderRecompiler::IR::Opcode::IAddCarryU32;
	add_carry.dst2.kind                 = ShaderRecompiler::IR::OperandKind::Register;
	add_carry.dst2.reg                  = {ShaderRecompiler::IR::RegisterFile::Vcc, 0};
	Check(ShaderRecompiler::Spirv::ProgramRequiresExactSubgroupSize(carry),
	      "lane-varying VCC carry producer was classified mask-free");

	ShaderRecompiler::IR::Program vcc_branch   = safe;
	auto&                         branch_block = vcc_branch.blocks.emplace_back();
	branch_block.terminator.kind      = ShaderRecompiler::CFG::TerminatorKind::ConditionalBranch;
	branch_block.terminator.condition = ShaderRecompiler::CFG::BranchCondition::VccNonZero;
	Check(ShaderRecompiler::Spirv::ProgramRequiresExactSubgroupSize(vcc_branch),
	      "whole-wave VCC branch was classified mask-free");

	ShaderRecompiler::IR::Program ds_partial = safe;
	ds_partial.lane_mask_mode                = ShaderLaneMaskMode::NativeWave;
	ds_partial.blocks.emplace_back().instructions.emplace_back().op =
	    ShaderRecompiler::IR::Opcode::DsAppend;
	Check(ConfigureShaderSubgroup(context, VK_SHADER_STAGE_COMPUTE_BIT, ds_partial).mode ==
	          ShaderSubgroupMode::Unsupported,
	      "partial wave64 DS append bypassed the exact subgroup requirement");
	context.max_subgroup_size = 64;
	const auto controlled =
	    ConfigureShaderSubgroup(context, VK_SHADER_STAGE_COMPUTE_BIT, cross_lane_compute);
	Check(controlled.mode == ShaderSubgroupMode::Controlled && controlled.required_size == 64,
	      "supported controlled wave64 was not preferred over splitting");
	context.subgroup_size                 = 64;
	context.subgroup_size_control_enabled = false;
	cross_lane.wave_size                  = 32;
	cross_lane.lane_mask_mode             = ShaderLaneMaskMode::PerInvocation;
	Check(ConfigureShaderSubgroup(context, VK_SHADER_STAGE_FRAGMENT_BIT, cross_lane).mode ==
	          ShaderSubgroupMode::Unsupported,
	      "inverse graphics mismatch was accepted as one guest wave");
	cross_lane_compute.wave_size = 32;
	Check(ConfigureShaderSubgroup(context, VK_SHADER_STAGE_COMPUTE_BIT, cross_lane_compute).mode ==
	          ShaderSubgroupMode::Unsupported,
	      "inverse cross-lane compute mismatch was accepted");
}

std::array<uint32_t, 64> ImageTestUserData(Prospero::ImageType type = Prospero::ImageType::kColor2D) {
	std::array<uint32_t, 64> data {};
	for (uint32_t start = 0; start + 3u < data.size(); start += 4u) {
		data[start]      = 0x1000u + start * 0x100u;
		data[start + 2u] = UINT32_MAX;
		data[start + 3u] = Prospero::GpuEnumValue(type) << 28u;
	}
	return data;
}

void SetImageTestType(std::array<uint32_t, 64>* data, uint32_t srsrc, Prospero::ImageType type) {
	const auto type_dword = srsrc * 4u + 3u;
	Check(data != nullptr && type_dword < data->size(), "invalid image test descriptor source");
	(*data)[type_dword] = Prospero::GpuEnumValue(type) << 28u;
}

bool ReadZeroTestMemory(void*, uint64_t, uint32_t* value) {
	if (value == nullptr) {
		return false;
	}
	*value = 0;
	return true;
}

constexpr uint32_t EncodeVop2(uint32_t opcode, uint32_t dst, uint32_t src0, uint32_t src1) {
	return ((opcode & 0x3fu) << 25u) | ((dst & 0xffu) << 17u) | ((src1 & 0xffu) << 9u) |
	       (src0 & 0x1ffu);
}

constexpr uint32_t EncodeVop1(uint32_t opcode, uint32_t dst, uint32_t src0) {
	return (0x3fu << 25u) | ((dst & 0xffu) << 17u) | ((opcode & 0xffu) << 9u) | (src0 & 0x1ffu);
}

constexpr uint32_t EncodeVop1Sdwa(uint32_t src0, uint32_t dst_sel = 6, uint32_t dst_u = 0,
                                  uint32_t src0_sel = 6, uint32_t src0_sext = 0,
                                  uint32_t src0_neg = 0, uint32_t src0_abs = 0, uint32_t s0 = 0) {
	return (src0 & 0xffu) | ((dst_sel & 0x7u) << 8u) | ((dst_u & 0x3u) << 11u) |
	       ((src0_sel & 0x7u) << 16u) | ((src0_sext & 0x1u) << 19u) | ((src0_neg & 0x1u) << 20u) |
	       ((src0_abs & 0x1u) << 21u) | ((s0 & 0x1u) << 23u);
}

constexpr uint32_t EncodeVop1Dpp(uint32_t src0, uint32_t dpp_ctrl = 0, uint32_t row_mask = 0xf,
                                 uint32_t bank_mask = 0xf) {
	return (src0 & 0xffu) | ((dpp_ctrl & 0x1ffu) << 8u) | ((bank_mask & 0xfu) << 24u) |
	       ((row_mask & 0xfu) << 28u);
}

constexpr uint32_t EncodeVop2Sdwa(uint32_t src0, uint32_t dst_sel = 6, uint32_t dst_u = 0,
                                  uint32_t src0_sel = 6, uint32_t src1_sel = 6,
                                  uint32_t src0_sext = 0, uint32_t src1_sext = 0,
                                  uint32_t src0_neg = 0, uint32_t src0_abs = 0,
                                  uint32_t src1_neg = 0, uint32_t src1_abs = 0, uint32_t s0 = 0,
                                  uint32_t s1 = 0) {
	return (src0 & 0xffu) | ((dst_sel & 0x7u) << 8u) | ((dst_u & 0x3u) << 11u) |
	       ((src0_sel & 0x7u) << 16u) | ((src0_sext & 0x1u) << 19u) | ((src0_neg & 0x1u) << 20u) |
	       ((src0_abs & 0x1u) << 21u) | ((s0 & 0x1u) << 23u) | ((src1_sel & 0x7u) << 24u) |
	       ((src1_sext & 0x1u) << 27u) | ((src1_neg & 0x1u) << 28u) | ((src1_abs & 0x1u) << 29u) |
	       ((s1 & 0x1u) << 31u);
}

constexpr uint32_t EncodeVop2Dpp(uint32_t src0, uint32_t dpp_ctrl = 0, uint32_t row_mask = 0xf,
                                 uint32_t bank_mask = 0xf, uint32_t src0_neg = 0,
                                 uint32_t src0_abs = 0, uint32_t src1_neg = 0,
                                 uint32_t src1_abs = 0) {
	return (src0 & 0xffu) | ((dpp_ctrl & 0x1ffu) << 8u) | ((src0_neg & 0x1u) << 20u) |
	       ((src0_abs & 0x1u) << 21u) | ((src1_neg & 0x1u) << 22u) | ((src1_abs & 0x1u) << 23u) |
	       ((bank_mask & 0xfu) << 24u) | ((row_mask & 0xfu) << 28u);
}

constexpr uint32_t EncodeVopc(uint32_t opcode, uint32_t src0, uint32_t src1) {
	return (0x3eu << 25u) | ((opcode & 0xffu) << 17u) | ((src1 & 0xffu) << 9u) | (src0 & 0x1ffu);
}

constexpr uint32_t EncodeVopcSdwa(uint32_t src0, uint32_t sdst = 0, uint32_t sd = 0,
                                  uint32_t src0_sel = 6, uint32_t src1_sel = 6,
                                  uint32_t src0_sext = 0, uint32_t src1_sext = 0,
                                  uint32_t src0_neg = 0, uint32_t src0_abs = 0,
                                  uint32_t src1_neg = 0, uint32_t src1_abs = 0, uint32_t s0 = 0,
                                  uint32_t s1 = 0) {
	return (src0 & 0xffu) | ((sdst & 0x7fu) << 8u) | ((sd & 0x1u) << 15u) |
	       ((src0_sel & 0x7u) << 16u) | ((src0_sext & 0x1u) << 19u) | ((src0_neg & 0x1u) << 20u) |
	       ((src0_abs & 0x1u) << 21u) | ((s0 & 0x1u) << 23u) | ((src1_sel & 0x7u) << 24u) |
	       ((src1_sext & 0x1u) << 27u) | ((src1_neg & 0x1u) << 28u) | ((src1_abs & 0x1u) << 29u) |
	       ((s1 & 0x1u) << 31u);
}

constexpr uint32_t EncodeVop3Word0(uint32_t opcode, uint32_t dst, uint32_t op_sel = 0,
                                   uint32_t abs = 0, bool clamp = false) {
	return (0x35u << 26u) | ((opcode & 0x3ffu) << 16u) | (dst & 0xffu) | ((abs & 0x7u) << 8u) |
	       ((op_sel & 0xfu) << 11u) | (clamp ? (1u << 15u) : 0u);
}

constexpr uint32_t EncodeVop3Word0Sdst(uint32_t opcode, uint32_t dst, uint32_t sdst) {
	return (0x35u << 26u) | ((opcode & 0x3ffu) << 16u) | ((sdst & 0x7fu) << 8u) | (dst & 0xffu);
}

constexpr uint32_t EncodeVop3Word1(uint32_t src0, uint32_t src1, uint32_t src2) {
	return (src0 & 0x1ffu) | ((src1 & 0x1ffu) << 9u) | ((src2 & 0x1ffu) << 18u);
}

constexpr uint32_t EncodeVop3pWord0(uint32_t opcode, uint32_t dst, uint32_t op_sel = 0,
                                    uint32_t op_sel_hi = 0, uint32_t neg_hi = 0,
                                    bool clamp = false) {
	return (0x33u << 26u) | ((opcode & 0x7fu) << 16u) | (dst & 0xffu) | ((neg_hi & 0x7u) << 8u) |
	       ((op_sel & 0x7u) << 11u) | (((op_sel_hi >> 2u) & 0x1u) << 14u) |
	       (clamp ? (1u << 15u) : 0u);
}

constexpr uint32_t EncodeVop3pWord1(uint32_t src0, uint32_t src1, uint32_t src2,
                                    uint32_t op_sel_hi = 0, uint32_t neg = 0) {
	return (src0 & 0x1ffu) | ((src1 & 0x1ffu) << 9u) | ((src2 & 0x1ffu) << 18u) |
	       ((op_sel_hi & 0x3u) << 27u) | ((neg & 0x7u) << 29u);
}

constexpr uint32_t EncodeSmem0(uint32_t opcode, uint32_t dst, uint32_t sbase) {
	return (0x3du << 26u) | ((opcode & 0xffu) << 18u) | ((dst & 0x7fu) << 6u) | (sbase & 0x3fu);
}

constexpr uint32_t EncodeMubuf0(uint32_t opcode, uint32_t offset = 0, bool idxen = true,
                                bool glc = false) {
	return (0x38u << 26u) | ((opcode & 0x7fu) << 18u) | (idxen ? (1u << 13u) : 0u) |
	       (glc ? (1u << 14u) : 0u) | (offset & 0xfffu);
}

constexpr uint32_t EncodeMubuf1(uint32_t vdata, uint32_t srsrc, uint32_t vaddr,
                                uint32_t soffset = 128) {
	return ((soffset & 0xffu) << 24u) | ((srsrc & 0x1fu) << 16u) | ((vdata & 0xffu) << 8u) |
	       (vaddr & 0xffu);
}

constexpr uint32_t EncodeMtbuf0(uint32_t opcode, uint32_t dfmt, uint32_t nfmt, uint32_t offset = 0,
                                bool idxen = true, bool offen = false) {
	return (0x3au << 26u) | (offset & 0xfffu) | (offen ? (1u << 12u) : 0u) |
	       (idxen ? (1u << 13u) : 0u) | ((opcode & 0x7u) << 16u) | ((dfmt & 0xfu) << 19u) |
	       ((nfmt & 0x7u) << 23u);
}

constexpr uint32_t EncodeMtbuf1(uint32_t opcode, uint32_t vdata, uint32_t srsrc, uint32_t vaddr,
                                uint32_t soffset = 128) {
	return ((opcode >> 3u) & 1u) << 21u | ((soffset & 0xffu) << 24u) | ((srsrc & 0x1fu) << 16u) |
	       ((vdata & 0xffu) << 8u) | (vaddr & 0xffu);
}

constexpr uint32_t EncodeFlat0(uint32_t opcode, uint32_t seg, uint32_t offset = 0) {
	return (0x37u << 26u) | ((opcode & 0x7fu) << 18u) | ((seg & 0x3u) << 14u) | (offset & 0xfffu);
}

constexpr uint32_t EncodeFlat1(uint32_t vdst, uint32_t saddr, uint32_t data, uint32_t addr) {
	return ((vdst & 0xffu) << 24u) | ((saddr & 0x7fu) << 16u) | ((data & 0xffu) << 8u) |
	       (addr & 0xffu);
}

constexpr uint32_t EncodeDs0(uint32_t opcode, uint32_t offset = 0) {
	return (0x36u << 26u) | ((opcode & 0xffu) << 18u) | (offset & 0xffffu);
}

constexpr uint32_t EncodeDs1Ex(uint32_t vdst, uint32_t data1, uint32_t data0, uint32_t addr) {
	return ((vdst & 0xffu) << 24u) | ((data1 & 0xffu) << 16u) | ((data0 & 0xffu) << 8u) |
	       (addr & 0xffu);
}

constexpr uint32_t EncodeDs1(uint32_t vdst, uint32_t data0, uint32_t addr) {
	return EncodeDs1Ex(vdst, 0, data0, addr);
}

constexpr uint32_t EncodeMimg0(uint32_t opcode, uint32_t dmask, bool glc = false,
                               uint32_t dim = 1) {
	return (0x3cu << 26u) | ((opcode & 0x7fu) << 18u) | ((dmask & 0xfu) << 8u) |
	       ((dim & 0x7u) << 3u) | (glc ? (1u << 13u) : 0u) | ((opcode >> 7u) & 1u);
}

constexpr uint32_t EncodeMimg1(uint32_t vdata, uint32_t srsrc, uint32_t ssamp, uint32_t vaddr,
                               bool a16 = false) {
	return ((ssamp & 0x1fu) << 21u) | ((srsrc & 0x1fu) << 16u) | ((vdata & 0xffu) << 8u) |
	       (vaddr & 0xffu) | (a16 ? (1u << 30u) : 0u);
}

constexpr uint32_t EncodeVintrp(uint32_t opcode, uint32_t vdst, uint32_t attr, uint32_t chan,
                                uint32_t vsrc) {
	return (0x32u << 26u) | ((opcode & 0x3u) << 16u) | ((vdst & 0xffu) << 18u) |
	       ((attr & 0x3fu) << 10u) | ((chan & 0x3u) << 8u) | (vsrc & 0xffu);
}

constexpr uint32_t EncodeExp0(uint32_t target, uint32_t en, bool done = true, bool compr = false,
                              bool vm = false) {
	return (0x3eu << 26u) | ((target & 0x3fu) << 4u) | (en & 0xfu) | (compr ? (1u << 10u) : 0u) |
	       (done ? (1u << 11u) : 0u) | (vm ? (1u << 12u) : 0u);
}

constexpr uint32_t EncodeExp1(uint32_t src0, uint32_t src1, uint32_t src2, uint32_t src3) {
	return (src0 & 0xffu) | ((src1 & 0xffu) << 8u) | ((src2 & 0xffu) << 16u) |
	       ((src3 & 0xffu) << 24u);
}

void TestNewShaderRecompilerSMovB32() {
	const uint32_t shader[] = {
	    EncodeSMovB32(0, 129),                      // s_mov_b32 s0, 1
	    EncodeSMovB32(1, 255),                      // s_mov_b32 s1, 0x12345678
	    0x12345678u,           EncodeSMovB32(2, 1), // s_mov_b32 s2, s1
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(!result.spirv.empty(), "new shader recompiler produced no SPIR-V");
	Check(result.spirv.front() == 0x07230203u, "new shader recompiler did not emit SPIR-V binary");
	Check(Common::ContainsStr(result.decoded_dump, "s_mov_b32 s0, 1"),
	      "new decoder did not decode inline S_MOV_B32 operand");
	Check(Common::ContainsStr(result.decoded_dump, "s_mov_b32 s1, 0x12345678"),
	      "new decoder did not decode literal S_MOV_B32 operand");
	Check(Common::ContainsStr(result.decoded_dump, "s_mov_b32 s2, s1"),
	      "new decoder did not decode register S_MOV_B32 operand");
	Check(Common::ContainsStr(result.ir_dump, "MoveU32 s0, 0x00000001"),
	      "new IR did not lower inline S_MOV_B32");
	Check(Common::ContainsStr(result.ir_dump, "MoveU32 s1, 0x12345678"),
	      "new IR did not lower literal S_MOV_B32");
	Check(Common::ContainsStr(result.ir_dump, "MoveU32 s2, s1"),
	      "new IR did not lower register S_MOV_B32");
	Check(std::find(result.spirv.begin(), result.spirv.end(), 0x12345678u) != result.spirv.end(),
	      "new SPIR-V emitter did not encode the literal as a binary word");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerSoppMarkers() {
	const uint32_t shader[] = {
	    EncodeSopp(0x00, 3),    // s_nop 3
	    EncodeSopp(0x0c, 0),    // s_waitcnt 0
	    EncodeSopp(0x10, 0x0f), // s_sendmsg 15
	    EncodeSopp(0x16, 0x2a), // s_ttracedata 42
	    EncodeSopp(0x20, 1),    // s_inst_prefetch 1
	    EncodeSopp(0x0a, 0),    // s_barrier
	    EncodeSopp(0x01, 0),    // s_endpgm
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "s_nop 0x00000003"),
	      "new decoder did not decode SOPP s_nop");
	Check(Common::ContainsStr(result.decoded_dump, "s_waitcnt 0x00000000"),
	      "new decoder did not decode SOPP s_waitcnt");
	Check(Common::ContainsStr(result.decoded_dump, "s_sendmsg 0x0000000f"),
	      "new decoder did not decode SOPP s_sendmsg");
	Check(Common::ContainsStr(result.decoded_dump, "s_ttracedata 0x0000002a"),
	      "new decoder did not decode SOPP s_ttracedata");
	Check(Common::ContainsStr(result.decoded_dump, "s_inst_prefetch 0x00000001"),
	      "new decoder did not decode SOPP s_inst_prefetch");
	Check(Common::ContainsStr(result.decoded_dump, "s_barrier"),
	      "new decoder did not decode SOPP s_barrier");
	Check(Common::ContainsStr(result.ir_dump, "ControlNop null, 0x00000003"),
	      "SOPP s_nop did not lower to an IR marker");
	Check(Common::ContainsStr(result.ir_dump, "Waitcnt null, 0x00000000"),
	      "SOPP s_waitcnt did not lower to an IR marker");
	Check(Common::ContainsStr(result.ir_dump, "Sendmsg null, 0x0000000f"),
	      "SOPP s_sendmsg did not lower to an IR marker");
	Check(Common::ContainsStr(result.ir_dump, "TtraceData null, 0x0000002a"),
	      "SOPP s_ttracedata did not lower to an IR marker");
	Check(Common::ContainsStr(result.ir_dump, "InstPrefetch null, 0x00000001"),
	      "SOPP s_inst_prefetch did not lower to an IR marker");
	Check(Common::ContainsStr(result.ir_dump, "Barrier null"),
	      "SOPP s_barrier did not lower to an IR marker");
	Check(SpirvContainsOpcode(result.spirv, 224),
	      "SPIR-V binary does not contain OpControlBarrier");
	Check(std::find(result.spirv.begin(), result.spirv.end(), 264u) != result.spirv.end(),
	      "SPIR-V barrier does not use workgroup acquire-release memory semantics");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerSopkWaitcntMarkers() {
	const uint32_t shader[] = {
	    EncodeSopk(0x17, 125, 0xffff), // s_waitcnt_vscnt null, 0xffff
	    EncodeSopk(0x18, 125, 0),      // s_waitcnt_vmcnt null, 0
	    EncodeSopk(0x19, 125, 0),      // s_waitcnt_expcnt null, 0
	    EncodeSopk(0x1a, 125, 0),      // s_waitcnt_lgkmcnt null, 0
	    EncodeSopp(0x01, 0),           // s_endpgm
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "s_waitcnt 0"),
	      "new decoder did not decode SOPK waitcnt marker");
	Check(Common::ContainsStr(result.decoded_dump, "s_waitcnt 65535"),
	      "SOPK waitcnt marker immediate was not kept unsigned");
	Check(Common::ContainsStr(result.ir_dump, "Waitcnt null, 0x00000000"),
	      "SOPK waitcnt did not lower to an IR marker");
	Check(Common::ContainsStr(result.ir_dump, "Waitcnt null, 0x0000ffff"),
	      "SOPK waitcnt marker immediate was not lowered as 16-bit unsigned");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerRdna2ScalarOpcodes() {
	const uint32_t shader[] = {
	    EncodeSMovB32(2, 135),         // s2 = 7
	    EncodeSMovB32(106, 144),       // vcc_lo = 16
	    EncodeSop1(0x1d, 106, 128),    // s_bitset1_b32 vcc_lo, 0
	    EncodeSopk(0x13, 106, 0x1019), // s_setreg_b32 vcc_lo, 0x1019
	    EncodeSop2(0x02, 106, 2, 239), // s_add_i32 vcc_lo, s2, pops_exiting_wave_id
	    EncodeSopp(0x0e, 0),           // s_sleep 0
	    EncodeSop2(0x02, 106, 239, 2), // s_add_i32 vcc_lo, pops_exiting_wave_id, s2
	    EncodeSopp(0x01, 0),           // s_endpgm
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "s_bitset1_b32 vcc_lo, 0"),
	      "new decoder did not decode RDNA2 S_BITSET1_B32");
	Check(Common::ContainsStr(result.decoded_dump, "s_setreg_b32 vcc_lo, 0x00001019"),
	      "new decoder did not decode S_SETREG_B32");
	Check(Common::ContainsStr(result.decoded_dump, "s_add_i32 vcc_lo, s2, pops_exiting_wave_id"),
	      "new decoder did not decode pops_exiting_wave_id as RHS scalar source");
	Check(Common::ContainsStr(result.decoded_dump, "s_add_i32 vcc_lo, pops_exiting_wave_id, s2"),
	      "new decoder did not decode pops_exiting_wave_id as LHS scalar source");
	Check(Common::ContainsStr(result.decoded_dump, "s_sleep 0x00000000"),
	      "new decoder did not decode S_SLEEP");
	Check(Common::ContainsStr(result.ir_dump, "BitSetU32 vcc_lo, vcc_lo, 0x00000000"),
	      "S_BITSET1_B32 did not lower to bit-set IR using the destination as input");
	Check(Common::ContainsStr(result.ir_dump, "ScalarSignedAddOverflowI32 vcc_lo, s2, 0x00000000"),
	      "pops_exiting_wave_id RHS did not lower to a deterministic zero value");
	Check(Common::ContainsStr(result.ir_dump, "ScalarSignedAddOverflowI32 vcc_lo, 0x00000000, s2"),
	      "pops_exiting_wave_id LHS did not lower to a deterministic zero value");
	Check(Common::ContainsStr(result.ir_dump, "ControlNop null, vcc_lo"),
	      "S_SETREG_B32 did not lower to an explicit control marker");
	Check(Common::ContainsStr(result.ir_dump, "ControlNop null, 0x00000000"),
	      "S_SLEEP did not lower to an explicit control marker");
	Check(SpirvContainsOpcode(result.spirv, 196),
	      "SPIR-V binary does not contain OpShiftLeftLogical for S_BITSET1_B32");
	Check(SpirvContainsOpcode(result.spirv, 197),
	      "SPIR-V binary does not contain OpBitwiseOr for S_BITSET1_B32");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerScalarVectorAlu() {
	const uint32_t shader[] = {
	    EncodeSMovB32(0, 129),           // s0 = 1
	    EncodeSMovB32(1, 130),           // s1 = 2
	    EncodeSop2(0x00, 2, 0, 1),       // s_add_u32 s2, s0, s1
	    EncodeSop2(0x01, 3, 2, 129),     // s_sub_u32 s3, s2, 1
	    EncodeSop2(0x0e, 4, 2, 3),       // s_and_b32 s4, s2, s3
	    EncodeSop2(0x10, 5, 2, 3),       // s_or_b32 s5, s2, s3
	    EncodeSop2(0x12, 6, 2, 3),       // s_xor_b32 s6, s2, s3
	    EncodeSop2(0x1e, 7, 2, 129),     // s_lshl_b32 s7, s2, 1
	    EncodeSop2(0x20, 8, 7, 129),     // s_lshr_b32 s8, s7, 1
	    EncodeSop2(0x2e, 9, 2, 1),       // s_lshl1_add_u32 s9, s2, s1
	    EncodeSop2(0x2f, 10, 9, 1),      // s_lshl2_add_u32 s10, s9, s1
	    EncodeSop2(0x30, 11, 10, 1),     // s_lshl3_add_u32 s11, s10, s1
	    EncodeSop2(0x31, 12, 11, 1),     // s_lshl4_add_u32 s12, s11, s1
	    EncodeSop2(0x35, 13, 12, 1),     // s_mul_hi_u32 s13, s12, s1
	    EncodeSopc(0x08, 8, 1),          // s_cmp_gt_u32 s8, s1
	    EncodeSop2(0x04, 14, 13, 129),   // s_addc_u32 s14, s13, 1
	    EncodeVop2(0x03, 1, 242, 0),     // v_add_f32 v1, 1.0, v0
	    EncodeVop2(0x08, 2, 1 + 256, 1), // v_mul_f32 v2, v1, v1
	    EncodeVop2(0x25, 3, 1 + 256, 2), // v_add_nc_u32 v3, v1, v2
	    EncodeVop2(0x1b, 4, 3 + 256, 2), // v_and_b32 v4, v3, v2
	    EncodeVop2(0x0b, 6, 3 + 256, 4), // v_mul_u32_u24 v6, v3, v4
	    EncodeVop2(0x09, 8, 4 + 256, 6), // v_mul_i32_i24 v8, v4, v6
	    EncodeVop2(0x22, 7, 4 + 256, 6), // v_bcnt_u32_b32 v7, v4, v6
	    EncodeVopc(0xc4, 3 + 256, 2),    // v_cmp_gt_u32 v3, v2
	    EncodeVop2(0x01, 5, 1 + 256, 2), // v_cndmask_b32 v5, v1, v2
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "s_add_u32 s2, s0, s1"),
	      "new decoder did not decode SOP2 add");
	Check(Common::ContainsStr(result.decoded_dump, "s_addc_u32 s14, s13, 1"),
	      "new decoder did not decode old-backed S_ADD_C_U32");
	Check(Common::ContainsStr(result.decoded_dump, "s_cmp_gt_u32 s8, s1"),
	      "new decoder did not decode SOPC compare");
	Check(Common::ContainsStr(result.decoded_dump, "s_lshl1_add_u32 s9, s2, s1"),
	      "new decoder did not decode old-backed S_LSHL1_ADD_U32");
	Check(Common::ContainsStr(result.decoded_dump, "s_lshl2_add_u32 s10, s9, s1"),
	      "new decoder did not decode old-backed S_LSHL2_ADD_U32");
	Check(Common::ContainsStr(result.decoded_dump, "s_lshl3_add_u32 s11, s10, s1"),
	      "new decoder did not decode old-backed S_LSHL3_ADD_U32");
	Check(Common::ContainsStr(result.decoded_dump, "s_lshl4_add_u32 s12, s11, s1"),
	      "new decoder did not decode old-backed S_LSHL4_ADD_U32");
	Check(Common::ContainsStr(result.decoded_dump, "s_mul_hi_u32 s13, s12, s1"),
	      "new decoder did not decode old-backed S_MUL_HI_U32");
	Check(Common::ContainsStr(result.decoded_dump, "v_add_f32 v1"),
	      "new decoder did not decode VOP2 float add");
	Check(Common::ContainsStr(result.decoded_dump, "v_cndmask_b32 v5"),
	      "new decoder did not decode VOP2 conditional mask select");
	Check(Common::ContainsStr(result.decoded_dump, "v_mul_u32_u24 v6"),
	      "new decoder did not decode VOP2 24-bit multiply");
	Check(Common::ContainsStr(result.decoded_dump, "v_mul_i32_i24 v8"),
	      "new decoder did not decode VOP2 signed 24-bit multiply");
	Check(Common::ContainsStr(result.decoded_dump, "v_bcnt_u32_b32 v7"),
	      "new decoder did not decode old-backed V_BCNT_U32_B32");
	Check(Common::ContainsStr(result.ir_dump, "ScalarAddCarryU32 s2, s0, s1, 0x00000000"),
	      "SOP2 add did not lower to scalar carry-writing IR");
	Check(Common::ContainsStr(result.ir_dump, "ScalarAddCarryU32 s14, s13, 0x00000001, scc"),
	      "S_ADD_C_U32 did not lower to scalar carry IR");
	Check(Common::ContainsStr(result.ir_dump, "ScalarShiftLeftAddCarryU32 s9, s2, 0x00000001, s1"),
	      "S_LSHL1_ADD_U32 did not lower through carry-writing shift-left-add IR");
	Check(Common::ContainsStr(result.ir_dump, "ScalarShiftLeftAddCarryU32 s10, s9, 0x00000002, s1"),
	      "S_LSHL2_ADD_U32 did not lower through carry-writing shift-left-add IR");
	Check(
	    Common::ContainsStr(result.ir_dump, "ScalarShiftLeftAddCarryU32 s11, s10, 0x00000003, s1"),
	    "S_LSHL3_ADD_U32 did not lower through carry-writing shift-left-add IR");
	Check(
	    Common::ContainsStr(result.ir_dump, "ScalarShiftLeftAddCarryU32 s12, s11, 0x00000004, s1"),
	    "S_LSHL4_ADD_U32 did not lower through carry-writing shift-left-add IR");
	Check(Common::ContainsStr(result.ir_dump, "UMulHighU32 s13, s12, s1"),
	      "S_MUL_HI_U32 did not lower to unsigned high-multiply IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareGtU32"), "SOPC compare did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "FAddF32 v1"), "VOP2 float add did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseAndU32 v4"),
	      "VOP2 bitwise op did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "SelectMaskU32 v5, vcc_lo, v2, v1"),
	      "VOP2 conditional mask did not lower through lane-mask select IR");
	Check(Common::ContainsStr(result.ir_dump, "UMulU24U32 v6"),
	      "VOP2 24-bit multiply did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "IMulI24U32 v8"),
	      "VOP2 signed 24-bit multiply did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "BitCountAddU32 v7, v4, v6"),
	      "V_BCNT_U32_B32 did not lower to bit-count-add IR");
	Check(SpirvContainsOpcode(result.spirv, 128), "SPIR-V binary does not contain OpIAdd");
	Check(SpirvContainsOpcode(result.spirv, 149), "SPIR-V binary does not contain OpIAddCarry");
	Check(SpirvContainsOpcode(result.spirv, 129), "SPIR-V binary does not contain OpFAdd");
	Check(SpirvContainsOpcode(result.spirv, 132), "SPIR-V binary does not contain OpIMul");
	Check(SpirvContainsOpcode(result.spirv, 151), "SPIR-V binary does not contain OpUMulExtended");
	Check(SpirvContainsOpcode(result.spirv, 205), "SPIR-V binary does not contain OpBitCount");
	Check(SpirvContainsOpcode(result.spirv, 169), "SPIR-V binary does not contain OpSelect");
	Check(SpirvContainsOpcode(result.spirv, 199), "SPIR-V binary does not contain OpBitwiseAnd");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerVop3LaneReadDestinationEncoding() {
	// Lane-read instructions have scalar results, but their VOP3A destination is encoded in
	// VDST [7:0], not the VOP3B SDST field [14:8].
	const uint32_t shader[] = {
	    EncodeVop3Word0(0x182, 25), EncodeVop3Word1(5 + 256, 0, 0),   // v_readfirstlane_b32 s25, v5
	    EncodeVop3Word0(0x360, 26), EncodeVop3Word1(5 + 256, 130, 0), // v_readlane_b32 s26, v5, 2
	    EncodeSopp(0x01, 0),
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "v_readfirstlane_b32 s25, v5"),
	      "VOP3 V_READFIRSTLANE_B32 destination was not decoded from VDST");
	Check(Common::ContainsStr(result.decoded_dump, "v_readlane_b32 s26, v5, 2"),
	      "VOP3 V_READLANE_B32 destination was not decoded from VDST");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerMoreAluFamilies() {
	const uint32_t shader[] = {
	    EncodeSopk(0x00, 9, 7), // s_movk_i32 s9, 7
	    EncodeSopk(0x0b, 9, 3), // s_cmp_gt_u32 s9, 3
	    EncodeVop1(0x00, 0, 0), // v_nop
	    EncodeVop1(0x01, 5, 9), // v_mov_b32 v5, s9
	    EncodeVop1(0x01, 103, 249),
	    EncodeVop1Sdwa(5, 6, 0, 4), // v_mov_b32 v103, v5 word0
	    EncodeVop1(0x06, 104, 249),
	    EncodeVop1Sdwa(103, 6, 0, 4), // v_cvt_f32_u32 v104, v103 word0
	    EncodeVop1(0x0a, 105, 249),
	    EncodeVop1Sdwa(6, 5, 2, 6), // v_cvt_f16_f32 v105.hi, v6
	    EncodeVop1(0x01, 106, 250),
	    EncodeVop1Dpp(5),                // v_mov_b32 v106, v5 dpp
	    EncodeVop1(0x02, 24, 5 + 256),   // v_readfirstlane_b32 s24, v5
	    EncodeVop1(0x06, 6, 5 + 256),    // v_cvt_f32_u32 v6, v5
	    EncodeVop1(0x07, 7, 6 + 256),    // v_cvt_u32_f32 v7, v6
	    EncodeVop1(0x05, 9, 5 + 256),    // v_cvt_f32_i32 v9, v5
	    EncodeVop1(0x08, 10, 6 + 256),   // v_cvt_i32_f32 v10, v6
	    EncodeVop1(0x0a, 99, 6 + 256),   // v_cvt_f16_f32 v99, v6
	    EncodeVop1(0x0b, 100, 99 + 256), // v_cvt_f32_f16 v100, v99
	    0x7e1016f9u,
	    0x00250602u,                      // v_cvt_f32_f16 v8, abs(v2.hi)
	    EncodeVop1(0x0d, 64, 6 + 256),    // v_cvt_flr_i32_f32 v64, v6
	    EncodeVop1(0x0e, 81, 5 + 256),    // v_cvt_off_f32_i4 v81, v5
	    EncodeVop1(0x11, 65, 5 + 256),    // v_cvt_f32_ubyte0 v65, v5
	    EncodeVop1(0x12, 66, 5 + 256),    // v_cvt_f32_ubyte1 v66, v5
	    EncodeVop1(0x13, 67, 5 + 256),    // v_cvt_f32_ubyte2 v67, v5
	    EncodeVop1(0x14, 68, 5 + 256),    // v_cvt_f32_ubyte3 v68, v5
	    EncodeVop1(0x2a, 11, 6 + 256),    // v_rcp_f32 v11, v6
	    EncodeVop1(0x20, 12, 6 + 256),    // v_fract_f32 v12, v6
	    EncodeVop1(0x21, 13, 6 + 256),    // v_trunc_f32 v13, v6
	    EncodeVop1(0x22, 14, 6 + 256),    // v_ceil_f32 v14, v6
	    EncodeVop1(0x23, 15, 6 + 256),    // v_rndne_f32 v15, v6
	    EncodeVop1(0x24, 16, 6 + 256),    // v_floor_f32 v16, v6
	    EncodeVop1(0x25, 17, 6 + 256),    // v_exp_f32 v17, v6
	    EncodeVop1(0x27, 18, 6 + 256),    // v_log_f32 v18, v6
	    EncodeVop1(0x2e, 19, 6 + 256),    // v_rsq_f32 v19, v6
	    EncodeVop1(0x33, 20, 6 + 256),    // v_sqrt_f32 v20, v6
	    EncodeVop1(0x35, 21, 6 + 256),    // v_sin_f32 v21, v6
	    EncodeVop1(0x36, 22, 6 + 256),    // v_cos_f32 v22, v6
	    EncodeVop1(0x37, 27, 5 + 256),    // v_not_b32 v27, v5
	    EncodeVop1(0x38, 28, 5 + 256),    // v_bfrev_b32 v28, v5
	    EncodeVop1(0x39, 31, 5 + 256),    // v_ffbh_u32 v31, v5
	    EncodeVop1(0x3a, 32, 5 + 256),    // v_ffbl_b32 v32, v5
	    0x7e6e870cu,                      // v_movrels_b32 v55, v12
	    EncodeVop2(0x1e, 83, 5 + 256, 6), // v_xnor_b32 v83, v5, v6
	    EncodeVop2(0x23, 84, 5 + 256, 6), // v_mbcnt_lo_u32_b32 v84, v5, v6
	    EncodeVop2(0x24, 85, 5 + 256, 6), // v_mbcnt_hi_u32_b32 v85, v5, v6
	    EncodeVop2(0x1f, 89, 6 + 256, 6), // v_mac_f32 v89, v6, v6
	    EncodeVop2(0x20, 90, 6 + 256, 5),
	    0x3f800000u, // v_madmk_f32 v90, v6, 1.0, v5
	    EncodeVop2(0x21, 91, 6 + 256, 5),
	    0x40000000u,                      // v_madak_f32 v91, v6, v5, 2.0
	    EncodeVop2(0x2b, 92, 6 + 256, 6), // v_mac_f32 v92, v6, v6
	    EncodeVop2(0x2c, 93, 6 + 256, 5),
	    0x3f000000u, // v_madmk_f32 v93, v6, 0.5, v5
	    EncodeVop2(0x2d, 94, 6 + 256, 5),
	    0x40400000u,                         // v_madak_f32 v94, v6, v5, 3.0
	    EncodeVop2(0x2f, 95, 6 + 256, 6),    // v_cvt_pkrtz_f16_f32 v95, v6, v6
	    EncodeVop2(0x02, 102, 95 + 256, 95), // v_dot2c_f32_f16 v102, v95, v95
	    EncodeVop2(0x28, 97, 5 + 256, 6),    // v_addc_u32 v97, vcc, v5, v6, vcc
	    EncodeVop2(0x25, 123, 249, 6),
	    EncodeVop2Sdwa(5, 6, 0, 4, 6),
	    EncodeVop2(0x1b, 124, 249, 6),
	    EncodeVop2Sdwa(5, 6, 0, 4, 5),
	    0x025e6af9u,
	    0x16060635u, // v_cndmask_b32 v47, v53, -v53
	    0x100490f9u,
	    0x86860600u, // v_mul_f32 v2, s0, s72 (SDWA full)
	    EncodeVop2(0x35, 9, 249, 1),
	    EncodeVop2Sdwa(0, 4, 2, 4, 4),
	    0x660000f9u,
	    EncodeVop2Sdwa(0, 4, 2, 4, 4),
	    EncodeVop2(0x03, 125, 250, 6),
	    EncodeVop2Dpp(5),
	    EncodeVop3Word0(0x103, 35),
	    EncodeVop3Word1(6 + 256, 6 + 256, 0),
	    EncodeVop3Word0(0x10b, 36),
	    EncodeVop3Word1(5 + 256, 5 + 256, 0),
	    EncodeVop3Word0(0x11e, 86),
	    EncodeVop3Word1(5 + 256, 6 + 256, 0),
	    EncodeVop3Word0(0x11f, 96),
	    EncodeVop3Word1(6 + 256, 6 + 256, 0),
	    EncodeVop3Word0Sdst(0x128, 98, 30),
	    EncodeVop3Word1(5 + 256, 6 + 256, 106),
	    EncodeVop3Word0(0x123, 87),
	    EncodeVop3Word1(5 + 256, 6 + 256, 0),
	    EncodeVop3Word0(0x124, 88),
	    EncodeVop3Word1(5 + 256, 6 + 256, 0),
	    EncodeVop3Word0(0x180, 0),
	    EncodeVop3Word1(0, 0, 0),
	    EncodeVop3Word0(0x181, 23),
	    EncodeVop3Word1(5 + 256, 0, 0),
	    EncodeVop3Word0(0x182, 25),
	    EncodeVop3Word1(5 + 256, 0, 0),
	    EncodeVop3Word0(0x185, 24),
	    EncodeVop3Word1(5 + 256, 0, 0),
	    EncodeVop3Word0(0x18a, 101),
	    EncodeVop3Word1(6 + 256, 0, 0),
	    EncodeVop3Word0(0x18d, 74),
	    EncodeVop3Word1(6 + 256, 0, 0),
	    EncodeVop3Word0(0x18e, 82),
	    EncodeVop3Word1(5 + 256, 0, 0),
	    EncodeVop3Word0(0x1a0, 25),
	    EncodeVop3Word1(6 + 256, 0, 0),
	    EncodeVop3Word0(0x1aa, 26),
	    EncodeVop3Word1(6 + 256, 0, 0),
	    EncodeVop3Word0(0x1b7, 29),
	    EncodeVop3Word1(5 + 256, 0, 0),
	    EncodeVop3Word0(0x1b8, 30),
	    EncodeVop3Word1(5 + 256, 0, 0),
	    EncodeVop3Word0(0x1b9, 33),
	    EncodeVop3Word1(5 + 256, 0, 0),
	    EncodeVop3Word0(0x1ba, 34),
	    EncodeVop3Word1(5 + 256, 0, 0),
	    EncodeVopc(0x04, 6 + 256, 6), // v_cmp_gt_f32 v6, v6
	    EncodeVopc(0xc4, 7 + 256, 5), // v_cmp_gt_u32 v7, v5
	    EncodeVopc(0x14, 6 + 256, 6), // v_cmpx_gt_f32 v6, v6
	    EncodeVopc(0xd4, 7 + 256, 5), // v_cmpx_gt_u32 v7, v5
	    EncodeVopc(0x00, 6 + 256, 6), // v_cmp_f_f32 v6, v6
	    EncodeVopc(0x0f, 6 + 256, 6), // v_cmp_tru_f32 v6, v6
	    EncodeVopc(0x07, 6 + 256, 6), // v_cmp_o_f32 v6, v6
	    EncodeVopc(0x08, 6 + 256, 6), // v_cmp_u_f32 v6, v6
	    EncodeVopc(0x09, 6 + 256, 6), // v_cmp_nge_f32 v6, v6
	    EncodeVopc(0x0a, 6 + 256, 6), // v_cmp_nlg_f32 v6, v6
	    EncodeVopc(0x0b, 6 + 256, 6), // v_cmp_ngt_f32 v6, v6
	    EncodeVopc(0x0c, 6 + 256, 6), // v_cmp_nle_f32 v6, v6
	    EncodeVopc(0x0d, 6 + 256, 6), // v_cmp_neq_f32 v6, v6
	    0x7c1a02f9u,
	    0x068680f0u, // v_cmp_neq_f32 s0, 0.5, v1 (SDWA)
	    EncodeVopc(0xd1, 249, 6),
	    EncodeVopcSdwa(5, 0, 0, 4),   // v_cmpx_lt_u32 exec, v5, v6 (SDWA)
	    EncodeVopc(0x0e, 6 + 256, 6), // v_cmp_nlt_f32 v6, v6
	    EncodeVopc(0x19, 6 + 256, 6), // v_cmpx_nge_f32 v6, v6
	    EncodeVopc(0x1a, 6 + 256, 6), // v_cmpx_nlg_f32 v6, v6
	    EncodeVopc(0x1b, 6 + 256, 6), // v_cmpx_ngt_f32 v6, v6
	    EncodeVopc(0x1c, 6 + 256, 6), // v_cmpx_nle_f32 v6, v6
	    EncodeVopc(0x1d, 6 + 256, 6), // v_cmpx_neq_f32 v6, v6
	    EncodeVopc(0x1e, 6 + 256, 6), // v_cmpx_nlt_f32 v6, v6
	    EncodeVopc(0x80, 5 + 256, 5), // v_cmp_f_i32 v5, v5
	    EncodeVopc(0x87, 5 + 256, 5), // v_cmp_t_i32 v5, v5
	    EncodeVopc(0xc0, 5 + 256, 5), // v_cmp_f_u32 v5, v5
	    EncodeVopc(0xc7, 5 + 256, 5), // v_cmp_t_u32 v5, v5
	    0xd4e5006au,
	    0x0000d47eu, // v_cmp_ne_u64 vcc, exec, vcc
	    EncodeVop3Word0(0x141, 8),
	    EncodeVop3Word1(6 + 256, 6 + 256, 6 + 256),
	    0xd5410004u,
	    0x20121301u, // v_mad_f32 v4, -v1, v9, s4
	    EncodeVop3Word0(0x144, 110),
	    EncodeVop3Word1(5 + 256, 6 + 256, 7 + 256),
	    EncodeVop3Word0(0x145, 111),
	    EncodeVop3Word1(5 + 256, 6 + 256, 7 + 256),
	    EncodeVop3Word0(0x146, 112),
	    EncodeVop3Word1(5 + 256, 6 + 256, 7 + 256),
	    EncodeVop3Word0(0x147, 113),
	    EncodeVop3Word1(5 + 256, 6 + 256, 7 + 256),
	    0xd5470005u,
	    0x841a0102u, // v_cubema_f32 v5, v2, v0, -v6
	    EncodeVop3Word0(0x14b, 37),
	    EncodeVop3Word1(6 + 256, 6 + 256, 6 + 256),
	    EncodeVop3Word0(0x151, 38),
	    EncodeVop3Word1(6 + 256, 6 + 256, 6 + 256),
	    EncodeVop3Word0(0x154, 39),
	    EncodeVop3Word1(6 + 256, 6 + 256, 6 + 256),
	    EncodeVop3Word0(0x157, 40),
	    EncodeVop3Word1(6 + 256, 6 + 256, 6 + 256),
	    EncodeVop3Word0(0x152, 41),
	    EncodeVop3Word1(5 + 256, 5 + 256, 5 + 256),
	    EncodeVop3Word0(0x153, 42),
	    EncodeVop3Word1(5 + 256, 5 + 256, 5 + 256),
	    EncodeVop3Word0(0x155, 43),
	    EncodeVop3Word1(5 + 256, 5 + 256, 5 + 256),
	    EncodeVop3Word0(0x156, 44),
	    EncodeVop3Word1(5 + 256, 5 + 256, 5 + 256),
	    EncodeVop3Word0(0x158, 45),
	    EncodeVop3Word1(5 + 256, 5 + 256, 5 + 256),
	    EncodeVop3Word0(0x159, 46),
	    EncodeVop3Word1(5 + 256, 5 + 256, 5 + 256),
	    EncodeVop3Word0(0x148, 47),
	    EncodeVop3Word1(5 + 256, 129, 132),
	    EncodeVop3Word0(0x149, 48),
	    EncodeVop3Word1(5 + 256, 129, 132),
	    EncodeVop3Word0(0x14a, 49),
	    EncodeVop3Word1(5 + 256, 6 + 256, 7 + 256),
	    EncodeVop3Word0(0x14e, 50),
	    EncodeVop3Word1(5 + 256, 6 + 256, 129),
	    EncodeVop3Word0(0x36d, 51),
	    EncodeVop3Word1(5 + 256, 6 + 256, 7 + 256),
	    EncodeVop3Word0(0x169, 52),
	    EncodeVop3Word1(5 + 256, 6 + 256, 0),
	    EncodeVop3Word0(0x16a, 53),
	    EncodeVop3Word1(5 + 256, 6 + 256, 0),
	    EncodeVop3Word0(0x16c, 114),
	    EncodeVop3Word1(5 + 256, 6 + 256, 0),
	    EncodeVop3Word0(0x371, 54),
	    EncodeVop3Word1(5 + 256, 6 + 256, 7 + 256),
	    EncodeVop3Word0(0x372, 55),
	    EncodeVop3Word1(5 + 256, 6 + 256, 7 + 256),
	    EncodeVop3Word0(0x178, 56),
	    EncodeVop3Word1(5 + 256, 6 + 256, 7 + 256),
	    EncodeVop3Word0(0x346, 57),
	    EncodeVop3Word1(5 + 256, 6 + 256, 7 + 256),
	    EncodeVop3Word0(0x347, 58),
	    EncodeVop3Word1(5 + 256, 6 + 256, 7 + 256),
	    EncodeVop3Word0(0x345, 59),
	    EncodeVop3Word1(5 + 256, 6 + 256, 7 + 256),
	    EncodeVop3Word0(0x36f, 60),
	    EncodeVop3Word1(5 + 256, 6 + 256, 7 + 256),
	    EncodeVop3Word0(0x15d, 61),
	    EncodeVop3Word1(5 + 256, 6 + 256, 7 + 256),
	    EncodeVop3Word0(0x142, 62),
	    EncodeVop3Word1(5 + 256, 6 + 256, 7 + 256),
	    EncodeVop3Word0(0x143, 63),
	    EncodeVop3Word1(5 + 256, 6 + 256, 7 + 256),
	    EncodeVop3Word0(0x16b, 69),
	    EncodeVop3Word1(5 + 256, 6 + 256, 0),
	    EncodeVop3Word0(0x30f, 70),
	    EncodeVop3Word1(5 + 256, 6 + 256, 0),
	    EncodeVop3Word0(0x310, 71),
	    EncodeVop3Word1(5 + 256, 6 + 256, 0),
	    EncodeVop3Word0(0x319, 72),
	    EncodeVop3Word1(5 + 256, 6 + 256, 0),
	    0xd70a1007u,
	    0x00020affu,
	    0x0000ffffu, // v_max_i16 v7, 0xffff, v5.hi
	    0xd70c5007u,
	    0x00020882u, // v_min_i16 v7, 2, v4.hi
	    EncodeVop3Word0(0x363, 73),
	    EncodeVop3Word1(129, 132, 0),
	    EncodeVop3Word0(0x360, 26),
	    EncodeVop3Word1(5 + 256, 130, 0),
	    EncodeVop3Word0(0x361, 107),
	    EncodeVop3Word1(5 + 256, 130, 0),
	    EncodeVop3Word0(0x377, 108),
	    EncodeVop3Word1(5 + 256, 128, 128),
	    EncodeVop3Word0(0x378, 109),
	    EncodeVop3Word1(5 + 256, 128, 128),
	    EncodeVop3Word0(0x12f, 75),
	    EncodeVop3Word1(6 + 256, 6 + 256, 0),
	    0xd52f0000u,
	    0x60020300u, // v_cvt_pkrtz_f16_f32 v0, -v0, -v1
	    EncodeVop3Word0(0x362, 76),
	    EncodeVop3Word1(6 + 256, 129, 0),
	    0xd7620107u,
	    0x00018509u, // v_ldexp_f32 v7, abs(v9), -2
	    0xd762800du,
	    0x00018906u, // v_ldexp_f32 clamp v13, v6, -4
	    EncodeVop3Word0(0x364, 77),
	    EncodeVop3Word1(5 + 256, 6 + 256, 0),
	    EncodeVop3Word0(0x368, 78),
	    EncodeVop3Word1(6 + 256, 6 + 256, 0),
	    EncodeVop3Word0(0x369, 79),
	    EncodeVop3Word1(6 + 256, 6 + 256, 0),
	    EncodeVop3Word0(0x36a, 80),
	    EncodeVop3Word1(5 + 256, 7 + 256, 0),
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "s_movk_i32 s9"),
	      "new decoder did not decode SOPK mov");
	Check(Common::ContainsStr(result.decoded_dump, "s_cmp_gt_u32"),
	      "new decoder did not decode SOPK compare");
	Check(Common::ContainsStr(result.decoded_dump, "v_nop"),
	      "new decoder did not decode old-backed VOP1 no-op");
	Check(Common::ContainsStr(result.decoded_dump, "v_mov_b32 v5"),
	      "new decoder did not decode VOP1 mov");
	Check(Common::ContainsStr(result.decoded_dump, "v_movrels_b32 v55, v12"),
	      "new decoder did not decode VOP1 V_MOVRELS_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_mov_b32 v103, v5.sdwa(sel=4"),
	      "new decoder did not decode VOP1 SDWA source selector");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_f16_f32 v105.sdwa(sel=5"),
	      "new decoder did not decode VOP1 SDWA destination selector");
	Check(Common::ContainsStr(result.decoded_dump, "v_mov_b32 v106, v5.dpp"),
	      "new decoder did not decode VOP1 DPP source metadata");
	Check(!Common::ContainsStr(result.decoded_dump, "VOP1 SDWA/DPP modifiers are not implemented"),
	      "new decoder still reports blanket VOP1 SDWA/DPP unsupported reason");
	Check(Common::ContainsStr(result.decoded_dump, "v_add_nc_u32 v123, v5.sdwa(sel=4"),
	      "new decoder did not decode VOP2 SDWA source selector");
	Check(Common::ContainsStr(result.decoded_dump, "v_and_b32 v124, v5.sdwa(sel=4"),
	      "new decoder did not decode VOP2 SDWA first source selector");
	Check(Common::ContainsStr(result.decoded_dump, "v6.sdwa(sel=5"),
	      "new decoder did not decode VOP2 SDWA second source selector");
	Check(Common::ContainsStr(result.decoded_dump, "v_cndmask_b32 v47, v53,") &&
	          Common::ContainsStr(result.decoded_dump, "v53.neg"),
	      "new decoder did not decode V_CNDMASK_B32 SDWA source modifier");
	Check(Common::ContainsStr(result.decoded_dump, "v_add_f32 v125, v5.dpp"),
	      "new decoder did not decode VOP2 DPP source metadata");
	Check(!Common::ContainsStr(result.decoded_dump, "VOP2 SDWA/DPP modifiers are not implemented"),
	      "new decoder still reports blanket VOP2 SDWA/DPP unsupported reason");
	Check(Common::ContainsStr(result.decoded_dump, "v_readfirstlane_b32 s24, v5"),
	      "new decoder did not decode old-backed V_READFIRSTLANE_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_f32_u32 v6"),
	      "new decoder did not decode VOP1 conversion");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_f32_i32 v9"),
	      "new decoder did not decode VOP1 signed int-to-float conversion");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_i32_f32 v10"),
	      "new decoder did not decode VOP1 float-to-signed-int conversion");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_f16_f32 v99"),
	      "new decoder did not decode old-backed V_CVT_F16_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_f32_f16 v100"),
	      "new decoder did not decode old-backed native V_CVT_F32_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_f32_f16 v8, v2.sdwa(sel=5") &&
	          Common::ContainsStr(result.decoded_dump, "v2.sdwa(sel=5,sext=0).abs"),
	      "new decoder did not decode V_CVT_F32_F16 SDWA source selector modifier");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_flr_i32_f32 v64"),
	      "new decoder did not decode old-backed V_CVT_FLR_I32_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_off_f32_i4 v81"),
	      "new decoder did not decode old-backed V_CVT_OFF_F32_I4");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_f32_ubyte0 v65"),
	      "new decoder did not decode old-backed V_CVT_F32_UBYTE0");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_f32_ubyte1 v66"),
	      "new decoder did not decode old-backed V_CVT_F32_UBYTE1");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_f32_ubyte2 v67"),
	      "new decoder did not decode old-backed V_CVT_F32_UBYTE2");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_f32_ubyte3 v68"),
	      "new decoder did not decode old-backed V_CVT_F32_UBYTE3");
	Check(Common::ContainsStr(result.decoded_dump, "v_rcp_f32 v11"),
	      "new decoder did not decode VOP1 reciprocal");
	Check(Common::ContainsStr(result.decoded_dump, "v_fract_f32 v12"),
	      "new decoder did not decode VOP1 fract");
	Check(Common::ContainsStr(result.decoded_dump, "v_cos_f32 v22"),
	      "new decoder did not decode VOP1 cosine");
	Check(Common::ContainsStr(result.decoded_dump, "v_not_b32 v27"),
	      "new decoder did not decode VOP1 not");
	Check(Common::ContainsStr(result.decoded_dump, "v_bfrev_b32 v28"),
	      "new decoder did not decode VOP1 bit reverse");
	Check(Common::ContainsStr(result.decoded_dump, "v_ffbh_u32 v31"),
	      "new decoder did not decode VOP1 find-first-bit-high");
	Check(Common::ContainsStr(result.decoded_dump, "v_ffbl_b32 v32"),
	      "new decoder did not decode VOP1 find-first-bit-low");
	Check(Common::ContainsStr(result.decoded_dump, "v_xnor_b32 v83, v5, v6"),
	      "new decoder did not decode old-backed V_XNOR_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_mbcnt_lo_u32_b32 v84, v5, v6"),
	      "new decoder did not decode old-backed V_MBCNT_LO_U32_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_mbcnt_hi_u32_b32 v85, v5, v6"),
	      "new decoder did not decode old-backed V_MBCNT_HI_U32_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_mac_f32 v89, v6, v6"),
	      "new decoder did not decode old-backed V_MAC_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_madmk_f32 v90, v6, 0x3f800000, v5"),
	      "new decoder did not decode old-backed V_MADMK_F32 literal form");
	Check(Common::ContainsStr(result.decoded_dump, "v_madak_f32 v91, v6, v5, 0x40000000"),
	      "new decoder did not decode old-backed V_MADAK_F32 literal form");
	Check(Common::ContainsStr(result.decoded_dump, "v_mac_f32 v92, v6, v6"),
	      "new decoder did not decode old-backed alternate V_MAC_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_madmk_f32 v93, v6, 0x3f000000, v5"),
	      "new decoder did not decode old-backed alternate V_MADMK_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_madak_f32 v94, v6, v5, 0x40400000"),
	      "new decoder did not decode old-backed alternate V_MADAK_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_pkrtz_f16_f32 v95, v6, v6"),
	      "new decoder did not decode old-backed native V_CVT_PKRTZ_F16_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_dot2c_f32_f16 v102, v95, v95"),
	      "new decoder did not decode old-backed V_DOT2C_F32_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v_addc_u32 v97, vcc_lo, v5, v6, vcc_lo"),
	      "new decoder did not decode old-backed V_ADD_CO_U32 carry form");
	Check(Common::ContainsStr(result.decoded_dump, "v_add_f32 v35"),
	      "new decoder did not decode VOP3-encoded VOP2 float add");
	Check(Common::ContainsStr(result.decoded_dump, "v_mul_u32_u24 v36"),
	      "new decoder did not decode VOP3-encoded VOP2 24-bit multiply");
	Check(Common::ContainsStr(result.decoded_dump, "v_xnor_b32 v86, v5, v6"),
	      "new decoder did not decode old-backed VOP3-encoded V_XNOR_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_mac_f32 v96, v6, v6"),
	      "new decoder did not decode old-backed VOP3-encoded V_MAC_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_addc_u32 v98, s30, v5, v6, vcc_lo"),
	      "new decoder did not decode old-backed VOP3 V_ADD_CO_U32 carry form");
	Check(Common::ContainsStr(result.decoded_dump, "v_mbcnt_lo_u32_b32 v87, v5, v6"),
	      "new decoder did not decode old-backed VOP3-encoded V_MBCNT_LO_U32_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_mbcnt_hi_u32_b32 v88, v5, v6"),
	      "new decoder did not decode old-backed VOP3-encoded V_MBCNT_HI_U32_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_mov_b32 v23"),
	      "new decoder did not decode VOP3-encoded VOP1 move");
	Check(Common::ContainsStr(result.decoded_dump, "v_readfirstlane_b32 s25, v5"),
	      "new decoder did not decode old-backed VOP3-encoded V_READFIRSTLANE_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_f32_i32 v24"),
	      "new decoder did not decode VOP3-encoded VOP1 signed conversion");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_f16_f32 v101"),
	      "new decoder did not decode old-backed VOP3-encoded V_CVT_F16_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_flr_i32_f32 v74"),
	      "new decoder did not decode old-backed VOP3-encoded V_CVT_FLR_I32_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_off_f32_i4 v82"),
	      "new decoder did not decode old-backed VOP3-encoded V_CVT_OFF_F32_I4");
	Check(Common::ContainsStr(result.decoded_dump, "v_fract_f32 v25"),
	      "new decoder did not decode VOP3-encoded VOP1 fract");
	Check(Common::ContainsStr(result.decoded_dump, "v_rcp_f32 v26"),
	      "new decoder did not decode VOP3-encoded VOP1 reciprocal");
	Check(Common::ContainsStr(result.decoded_dump, "v_not_b32 v29"),
	      "new decoder did not decode VOP3-encoded VOP1 not");
	Check(Common::ContainsStr(result.decoded_dump, "v_bfrev_b32 v30"),
	      "new decoder did not decode VOP3-encoded VOP1 bit reverse");
	Check(Common::ContainsStr(result.decoded_dump, "v_ffbh_u32 v33"),
	      "new decoder did not decode VOP3-encoded VOP1 find-first-bit-high");
	Check(Common::ContainsStr(result.decoded_dump, "v_ffbl_b32 v34"),
	      "new decoder did not decode VOP3-encoded VOP1 find-first-bit-low");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_gt_f32"),
	      "new decoder did not decode VOPC float compare");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmpx_gt_f32"),
	      "new decoder did not decode VOPC float compare-and-mask");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmpx_gt_u32"),
	      "new decoder did not decode VOPC uint compare-and-mask");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_f_f32"),
	      "new decoder did not decode old-backed V_CMP_F_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_tru_f32"),
	      "new decoder did not decode old-backed V_CMP_TRU_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_o_f32"),
	      "new decoder did not decode old-backed V_CMP_O_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_u_f32"),
	      "new decoder did not decode old-backed V_CMP_U_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_nge_f32"),
	      "new decoder did not decode old-backed V_CMP_NGE_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_nlg_f32"),
	      "new decoder did not decode old-backed V_CMP_NLG_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_ngt_f32"),
	      "new decoder did not decode old-backed V_CMP_NGT_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_nle_f32"),
	      "new decoder did not decode old-backed V_CMP_NLE_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_neq_f32"),
	      "new decoder did not decode old-backed V_CMP_NEQ_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_neq_f32 s0, 0.500000, v1"),
	      "new decoder did not decode old-backed V_CMP_NEQ_F32 SDWA scalar destination");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmpx_lt_u32 exec_lo, v5.sdwa(sel=4") &&
	          Common::ContainsStr(result.ir_dump, "CompareMaskLtU32 exec_lo"),
	      "new decoder did not route V_CMPX SDWA destination to exec");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_nlt_f32"),
	      "new decoder did not decode old-backed V_CMP_NLT_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmpx_nge_f32"),
	      "new decoder did not decode old-backed V_CMPX_NGE_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmpx_nlg_f32"),
	      "new decoder did not decode old-backed V_CMPX_NLG_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmpx_ngt_f32"),
	      "new decoder did not decode old-backed V_CMPX_NGT_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmpx_nle_f32"),
	      "new decoder did not decode old-backed V_CMPX_NLE_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmpx_neq_f32"),
	      "new decoder did not decode old-backed V_CMPX_NEQ_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmpx_nlt_f32"),
	      "new decoder did not decode old-backed V_CMPX_NLT_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_f_i32"),
	      "new decoder did not decode old-backed V_CMP_F_I32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_t_i32"),
	      "new decoder did not decode old-backed V_CMP_T_I32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_f_u32"),
	      "new decoder did not decode old-backed V_CMP_F_U32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_t_u32"),
	      "new decoder did not decode old-backed V_CMP_T_U32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_ne_u64 vcc_lo, exec_lo, vcc_lo"),
	      "new decoder did not decode VOP3-encoded V_CMP_NE_U64");
	Check(Common::ContainsStr(result.ir_dump, "CompareNeU64 vcc_lo, exec_lo, vcc_lo"),
	      "V_CMP_NE_U64 did not lower to 64-bit compare IR");
	Check(Common::ContainsStr(result.decoded_dump, "v_mad_f32 v8"),
	      "new decoder did not decode VOP3 mad");
	Check(Common::ContainsStr(result.decoded_dump, "v_mad_f32 v4, v1.neg, v9, s4"),
	      "new decoder did not decode VOP3 mad source modifiers");
	Check(Common::ContainsStr(result.decoded_dump, "v_cubeid_f32 v110, v5, v6, v7"),
	      "new decoder did not decode old-backed V_CUBEID_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cubesc_f32 v111, v5, v6, v7"),
	      "new decoder did not decode old-backed V_CUBESC_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cubetc_f32 v112, v5, v6, v7"),
	      "new decoder did not decode old-backed V_CUBETC_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cubema_f32 v113, v5, v6, v7"),
	      "new decoder did not decode old-backed V_CUBEMA_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cubema_f32 v5, v2, v0, v6.neg"),
	      "new decoder did not decode V_CUBEMA_F32 source modifier");
	Check(Common::ContainsStr(result.decoded_dump, "v_fma_f32 v37"),
	      "new decoder did not decode VOP3 fma");
	Check(Common::ContainsStr(result.decoded_dump, "v_min3_f32 v38"),
	      "new decoder did not decode VOP3 min3");
	Check(Common::ContainsStr(result.decoded_dump, "v_max3_f32 v39"),
	      "new decoder did not decode VOP3 max3");
	Check(Common::ContainsStr(result.decoded_dump, "v_med3_f32 v40"),
	      "new decoder did not decode VOP3 med3");
	Check(Common::ContainsStr(result.decoded_dump, "v_min3_i32 v41"),
	      "new decoder did not decode VOP3 signed min3");
	Check(Common::ContainsStr(result.decoded_dump, "v_min3_u32 v42"),
	      "new decoder did not decode VOP3 unsigned min3");
	Check(Common::ContainsStr(result.decoded_dump, "v_max3_i32 v43"),
	      "new decoder did not decode VOP3 signed max3");
	Check(Common::ContainsStr(result.decoded_dump, "v_max3_u32 v44"),
	      "new decoder did not decode VOP3 unsigned max3");
	Check(Common::ContainsStr(result.decoded_dump, "v_med3_i32 v45"),
	      "new decoder did not decode VOP3 signed med3");
	Check(Common::ContainsStr(result.decoded_dump, "v_med3_u32 v46"),
	      "new decoder did not decode VOP3 unsigned med3");
	Check(Common::ContainsStr(result.decoded_dump, "v_bfe_u32 v47"),
	      "new decoder did not decode VOP3 unsigned bitfield extract");
	Check(Common::ContainsStr(result.decoded_dump, "v_bfe_i32 v48"),
	      "new decoder did not decode VOP3 signed bitfield extract");
	Check(Common::ContainsStr(result.decoded_dump, "v_bfi_b32 v49"),
	      "new decoder did not decode VOP3 bitfield insert-select");
	Check(Common::ContainsStr(result.decoded_dump, "v_alignbit_b32 v50"),
	      "new decoder did not decode VOP3 alignbit");
	Check(Common::ContainsStr(result.decoded_dump, "v_add3_u32 v51"),
	      "new decoder did not decode VOP3 add3");
	Check(Common::ContainsStr(result.decoded_dump, "v_mul_lo_u32 v52, v5, v6"),
	      "new decoder did not decode old-backed V_MUL_LO_U32");
	Check(Common::ContainsStr(result.decoded_dump, "v_mul_hi_u32 v53, v5, v6"),
	      "new decoder did not decode old-backed V_MUL_HI_U32");
	Check(Common::ContainsStr(result.decoded_dump, "v_mul_hi_i32 v114, v5, v6"),
	      "new decoder did not decode RDNA2 V_MUL_HI_I32");
	Check(Common::ContainsStr(result.decoded_dump, "v_and_or_b32 v54, v5, v6, v7"),
	      "new decoder did not decode old-backed V_AND_OR_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_or3_b32 v55, v5, v6, v7"),
	      "new decoder did not decode old-backed V_OR3_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_xor3_b32 v56, v5, v6, v7"),
	      "new decoder did not decode old-backed V_XOR3_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_lshl_add_u32 v57, v5, v6, v7"),
	      "new decoder did not decode old-backed V_LSHL_ADD_U32");
	Check(Common::ContainsStr(result.decoded_dump, "v_add_lshl_u32 v58, v5, v6, v7"),
	      "new decoder did not decode old-backed V_ADD_LSHL_U32");
	Check(Common::ContainsStr(result.decoded_dump, "v_xad_u32 v59, v5, v6, v7"),
	      "new decoder did not decode old-backed V_XAD_U32");
	Check(Common::ContainsStr(result.decoded_dump, "v_lshl_or_b32 v60, v5, v6, v7"),
	      "new decoder did not decode old-backed V_LSHL_OR_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_sad_u32 v61, v5, v6, v7"),
	      "new decoder did not decode old-backed V_SAD_U32");
	Check(Common::ContainsStr(result.decoded_dump, "v_mad_i32_i24 v62, v5, v6, v7"),
	      "new decoder did not decode old-backed V_MAD_I32_I24");
	Check(Common::ContainsStr(result.decoded_dump, "v_mad_u32_u24 v63, v5, v6, v7"),
	      "new decoder did not decode old-backed V_MAD_U32_U24");
	Check(Common::ContainsStr(result.decoded_dump, "v_mul_lo_i32 v69, v5, v6"),
	      "new decoder did not decode old-backed V_MUL_LO_I32");
	Check(Common::ContainsStr(result.decoded_dump, "v_add_i32 v70, s0, v5, v6"),
	      "new decoder did not decode old-backed V_ADD_I32");
	Check(Common::ContainsStr(result.decoded_dump, "v_sub_i32 v71, s0, v5, v6"),
	      "new decoder did not decode RDNA2 V_SUB_CO_U32");
	Check(Common::ContainsStr(result.decoded_dump, "v_subrev_i32 v72, s0, v5, v6"),
	      "new decoder did not decode old-backed V_SUBREV_I32");
	Check(Common::ContainsStr(
	          result.decoded_dump,
	          "v_max_i16 v7.sdwa(sel=4,sext=0), 0x0000ffff, v5.opsel(lo=1,hi=0,neghi=0)"),
	      "new decoder did not decode RDNA2 V_MAX_I16 literal/op_sel form");
	Check(Common::ContainsStr(result.decoded_dump,
	                          "v_min_i16 v7.sdwa(sel=5,sext=0), 2, v4.opsel(lo=1,hi=0,neghi=0)"),
	      "new decoder did not decode RDNA2 V_MIN_I16 op_sel form");
	Check(Common::ContainsStr(result.decoded_dump, "v_bfm_b32 v73, 1, 4"),
	      "new decoder did not decode old-backed V_BFM_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_readlane_b32 s26, v5, 2"),
	      "new decoder did not decode old-backed V_READLANE_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_writelane_b32 v107, v5, 2"),
	      "new decoder did not decode old-backed V_WRITELANE_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_permlane16_b32 v108, v5, 0, 0"),
	      "new decoder did not decode old-backed V_PERMLANE16_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_permlanex16_b32 v109, v5, 0, 0"),
	      "new decoder did not decode old-backed V_PERMLANEX16_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_pkrtz_f16_f32 v75, v6, v6"),
	      "new decoder did not decode old-backed V_CVT_PKRTZ_F16_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_pkrtz_f16_f32 v0, v0.neg, v1.neg"),
	      "new decoder did not decode V_CVT_PKRTZ_F16_F32 source modifiers");
	Check(Common::ContainsStr(result.decoded_dump, "v_ldexp_f32 v76, v6, 1"),
	      "new decoder did not decode old-backed V_LDEXP_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_ldexp_f32 v7, v9.abs, -2"),
	      "new decoder did not decode V_LDEXP_F32 source modifier");
	Check(Common::ContainsStr(result.decoded_dump, "v_ldexp_f32 v13.clamp, v6, -4"),
	      "new decoder did not decode V_LDEXP_F32 clamp modifier");
	Check(Common::ContainsStr(result.decoded_dump, "v_bcnt_u32_b32 v77, v5, v6"),
	      "new decoder did not decode old-backed VOP3 V_BCNT_U32_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_pknorm_i16_f32 v78, v6, v6"),
	      "new decoder did not decode old-backed V_CVT_PKNORM_I16_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_pknorm_u16_f32 v79, v6, v6"),
	      "new decoder did not decode old-backed V_CVT_PKNORM_U16_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_pk_u16_u32 v80, v5, v7"),
	      "new decoder did not decode old-backed V_CVT_PK_U16_U32");
	Check(Common::ContainsStr(result.decoded_dump, "v_mul_f32 v2, s0, s72"),
	      "new decoder did not decode old-backed V_MUL_F32 SDWA full-width form");
	Check(Common::ContainsStr(result.decoded_dump, "v_mul_f16 v9.sdwa(sel=4") &&
	          Common::ContainsStr(result.decoded_dump, "v0.sdwa(sel=4") &&
	          Common::ContainsStr(result.decoded_dump, "v1.sdwa(sel=4"),
	      "new decoder did not decode V_MUL_F16 SDWA low-half form");
	Check(Common::ContainsStr(result.decoded_dump, "v_sub_f16 v0.sdwa(sel=4") &&
	          Common::ContainsStr(result.decoded_dump, "v0.sdwa(sel=4"),
	      "new decoder did not decode V_SUB_F16 SDWA low-half form");
	Check(Common::ContainsStr(result.ir_dump, "ConvertU32ToF32 v6"),
	      "VOP1 uint-to-float conversion did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "MoveU32 v103, v5.sdwa(sel=4"),
	      "VOP1 SDWA source selector did not lower to IR metadata");
	Check(Common::ContainsStr(result.ir_dump, "ConvertF32ToF16 v105.sdwa(sel=5"),
	      "VOP1 SDWA destination selector did not lower to IR metadata");
	Check(Common::ContainsStr(result.ir_dump, "MoveU32 v106.dpp") &&
	          Common::ContainsStr(result.ir_dump, "v5.dpp"),
	      "VOP1 DPP source/destination did not lower to IR metadata");
	Check(Common::ContainsStr(result.ir_dump, "MoveRelSourceU32 v55, v12, m0"),
	      "V_MOVRELS_B32 did not lower to indexed VGPR-source IR");
	Check(Common::ContainsStr(result.ir_dump, "IAddU32 v123, v5.sdwa(sel=4"),
	      "VOP2 SDWA did not lower first source metadata to IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseAndU32 v124, v5.sdwa(sel=4"),
	      "VOP2 SDWA bitwise op did not lower first source metadata to IR");
	Check(Common::ContainsStr(result.ir_dump, "v6.sdwa(sel=5"),
	      "VOP2 SDWA bitwise op did not lower second source metadata to IR");
	Check(Common::ContainsStr(result.ir_dump, "SelectMaskF32Bits v47, vcc_lo, v53.neg, v53") &&
	          Common::ContainsStr(result.ir_dump, "v53.neg"),
	      "V_CNDMASK_B32 SDWA source modifier did not lower to float-bit select IR");
	Check(Common::ContainsStr(result.ir_dump, "FAddF32 v125.dpp") &&
	          Common::ContainsStr(result.ir_dump, "v5.dpp"),
	      "VOP2 DPP source/destination did not lower to IR metadata");
	Check(Common::ContainsStr(result.ir_dump, "FMulF32 v2, s0, s72"),
	      "V_MUL_F32 SDWA full-width form did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "MulF16 v9.sdwa(sel=4") &&
	          Common::ContainsStr(result.ir_dump, "v0.sdwa(sel=4") &&
	          Common::ContainsStr(result.ir_dump, "v1.sdwa(sel=4"),
	      "V_MUL_F16 SDWA low-half form did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "SubF16 v0.sdwa(sel=4") &&
	          Common::ContainsStr(result.ir_dump, "v0.sdwa(sel=4"),
	      "V_SUB_F16 SDWA low-half form did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertF32ToU32 v7"),
	      "VOP1 float-to-uint conversion did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertI32ToF32 v9"),
	      "VOP1 signed int-to-float conversion did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertF32ToI32 v10"),
	      "VOP1 float-to-signed-int conversion did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertF32ToF16 v99, v6"),
	      "V_CVT_F16_F32 did not lower to shared half-pack IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertF16ToF32 v100, v99"),
	      "V_CVT_F32_F16 did not lower to shared half-unpack IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertF16ToF32 v8, v2.sdwa(sel=5") &&
	          Common::ContainsStr(result.ir_dump, "v2.sdwa(sel=5,sext=0).abs"),
	      "V_CVT_F32_F16 SDWA source selector modifier did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "ReadFirstLaneU32 s24, v5"),
	      "V_READFIRSTLANE_B32 did not lower to subgroup IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertFloorF32ToI32 v64, v6"),
	      "V_CVT_FLR_I32_F32 did not lower to shared IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertI4ToOffsetF32 v81, v5"),
	      "V_CVT_OFF_F32_I4 did not lower to shared offset-convert IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertByteU32ToF32 v65, v5, 0x00000000"),
	      "V_CVT_F32_UBYTE0 did not lower to shared byte-convert IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertByteU32ToF32 v66, v5, 0x00000001"),
	      "V_CVT_F32_UBYTE1 did not lower to shared byte-convert IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertByteU32ToF32 v67, v5, 0x00000002"),
	      "V_CVT_F32_UBYTE2 did not lower to shared byte-convert IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertByteU32ToF32 v68, v5, 0x00000003"),
	      "V_CVT_F32_UBYTE3 did not lower to shared byte-convert IR");
	Check(Common::ContainsStr(result.ir_dump, "RcpF32 v11"), "VOP1 reciprocal did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "FractF32 v12"), "VOP1 fract did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "TruncF32 v13"), "VOP1 trunc did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "CeilF32 v14"), "VOP1 ceil did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "RoundEvenF32 v15"),
	      "VOP1 round-even did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "FloorF32 v16"), "VOP1 floor did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "Exp2F32 v17"), "VOP1 exp2 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "Log2F32 v18"), "VOP1 log2 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "InverseSqrtF32 v19"),
	      "VOP1 inverse-sqrt did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "SqrtF32 v20"), "VOP1 sqrt did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "SinF32 v21"), "VOP1 sin did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "CosF32 v22"), "VOP1 cos did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseNotU32 v27"), "VOP1 not did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "BitReverseU32 v28"),
	      "VOP1 bit reverse did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "FindMsbFromHighU32 v31"),
	      "VOP1 find-first-bit-high did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "FindLsbU32 v32"),
	      "VOP1 find-first-bit-low did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseXnorU32 v83, v5, v6"),
	      "V_XNOR_B32 did not lower to shared xnor IR");
	Check(Common::ContainsStr(result.ir_dump, "MaskedBitCountLowU32 v84, v5, v6"),
	      "V_MBCNT_LO_U32_B32 did not lower to shared masked bit-count IR");
	Check(Common::ContainsStr(result.ir_dump, "MaskedBitCountHighU32 v85, v5, v6"),
	      "V_MBCNT_HI_U32_B32 did not lower to shared masked bit-count IR");
	Check(Common::ContainsStr(result.ir_dump, "FMadF32 v89, v6, v6, v89"),
	      "V_MAC_F32 did not lower with the destination register as addend");
	Check(Common::ContainsStr(result.ir_dump, "FMadF32 v90, v6, 0x3f800000, v5"),
	      "V_MADMK_F32 did not lower with the literal in source 1");
	Check(Common::ContainsStr(result.ir_dump, "FMadF32 v91, v6, v5, 0x40000000"),
	      "V_MADAK_F32 did not lower with the literal in source 2");
	Check(Common::ContainsStr(result.ir_dump, "FMadF32 v92, v6, v6, v92"),
	      "alternate V_MAC_F32 did not lower with the destination register as addend");
	Check(Common::ContainsStr(result.ir_dump, "FMadF32 v93, v6, 0x3f000000, v5"),
	      "alternate V_MADMK_F32 did not lower with the literal in source 1");
	Check(Common::ContainsStr(result.ir_dump, "FMadF32 v94, v6, v5, 0x40400000"),
	      "alternate V_MADAK_F32 did not lower with the literal in source 2");
	Check(Common::ContainsStr(result.ir_dump, "PackF32ToF16Rtz v95, v6, v6"),
	      "native V_CVT_PKRTZ_F16_F32 did not lower to shared pack IR");
	Check(Common::ContainsStr(result.ir_dump, "Dot2AccF32F16 v102, v95, v95, v102"),
	      "V_DOT2C_F32_F16 did not lower to explicit dot-accumulate IR");
	Check(Common::ContainsStr(result.ir_dump, "IAddCarryU32 v97, vcc_lo, v5, v6, vcc_lo"),
	      "V_ADD_CO_U32 did not lower to carry-add IR");
	Check(Common::ContainsStr(result.ir_dump, "FAddF32 v35"),
	      "VOP3-encoded VOP2 float add did not lower through shared IR");
	Check(Common::ContainsStr(result.ir_dump, "UMulU24U32 v36"),
	      "VOP3-encoded VOP2 24-bit multiply did not lower through shared IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseXnorU32 v86, v5, v6"),
	      "VOP3-encoded V_XNOR_B32 did not lower through shared IR");
	Check(Common::ContainsStr(result.ir_dump, "FMadF32 v96, v6, v6, v96"),
	      "VOP3-encoded V_MAC_F32 did not lower with the destination register as addend");
	Check(Common::ContainsStr(result.ir_dump, "IAddCarryU32 v98, s30, v5, v6, vcc_lo"),
	      "VOP3-encoded V_ADD_CO_U32 did not lower to carry-add IR");
	Check(Common::ContainsStr(result.ir_dump, "MaskedBitCountLowU32 v87, v5, v6"),
	      "VOP3-encoded V_MBCNT_LO_U32_B32 did not lower through shared IR");
	Check(Common::ContainsStr(result.ir_dump, "MaskedBitCountHighU32 v88, v5, v6"),
	      "VOP3-encoded V_MBCNT_HI_U32_B32 did not lower through shared IR");
	Check(Common::ContainsStr(result.ir_dump, "MoveU32 v23"),
	      "VOP3-encoded VOP1 move did not lower through shared IR");
	Check(Common::ContainsStr(result.ir_dump, "ReadFirstLaneU32 s25, v5"),
	      "VOP3-encoded V_READFIRSTLANE_B32 did not lower through shared IR");
	Check(Common::ContainsStr(result.ir_dump, "ReadLaneU32 s26, v5, 0x00000002"),
	      "V_READLANE_B32 did not lower to subgroup lane IR");
	Check(Common::ContainsStr(result.ir_dump, "WriteLaneU32 v107, v5, 0x00000002"),
	      "V_WRITELANE_B32 did not lower to subgroup lane IR");
	Check(Common::ContainsStr(result.ir_dump, "Permlane16B32 v108, v5, 0x00000000, 0x00000000"),
	      "V_PERMLANE16_B32 did not lower to subgroup perm-lane IR");
	Check(Common::ContainsStr(result.ir_dump, "Permlanex16B32 v109, v5, 0x00000000, 0x00000000"),
	      "V_PERMLANEX16_B32 did not lower to subgroup perm-lane IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertI32ToF32 v24"),
	      "VOP3-encoded VOP1 signed conversion did not lower through shared IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertFloorF32ToI32 v74, v6"),
	      "VOP3-encoded V_CVT_FLR_I32_F32 did not lower through shared IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertI4ToOffsetF32 v82, v5"),
	      "VOP3-encoded V_CVT_OFF_F32_I4 did not lower through shared IR");
	Check(Common::ContainsStr(result.ir_dump, "FractF32 v25"),
	      "VOP3-encoded VOP1 fract did not lower through shared IR");
	Check(Common::ContainsStr(result.ir_dump, "RcpF32 v26"),
	      "VOP3-encoded VOP1 reciprocal did not lower through shared IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseNotU32 v29"),
	      "VOP3-encoded VOP1 not did not lower through shared IR");
	Check(Common::ContainsStr(result.ir_dump, "BitReverseU32 v30"),
	      "VOP3-encoded VOP1 bit reverse did not lower through shared IR");
	Check(Common::ContainsStr(result.ir_dump, "FindMsbFromHighU32 v33"),
	      "VOP3-encoded VOP1 find-first-bit-high did not lower through shared IR");
	Check(Common::ContainsStr(result.ir_dump, "FindLsbU32 v34"),
	      "VOP3-encoded VOP1 find-first-bit-low did not lower through shared IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareGtF32"),
	      "VOPC float compare did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareMaskGtF32 exec_lo"),
	      "VOPC float compare-and-mask did not lower to exec mask IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareMaskGtU32 exec_lo"),
	      "VOPC uint compare-and-mask did not lower to exec mask IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareFalse vcc_lo, v6, v6"),
	      "VOPC false compare did not lower to shared IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareTrue vcc_lo, v6, v6"),
	      "VOPC true compare did not lower to shared IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareOrderedF32 vcc_lo, v6, v6"),
	      "VOPC ordered compare did not lower to shared IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareUnorderedF32 vcc_lo, v6, v6"),
	      "VOPC unordered compare did not lower to shared IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareUnordLtF32 vcc_lo, v6, v6"),
	      "VOPC unordered-less compare did not lower to shared IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareUnordEqF32 vcc_lo, v6, v6"),
	      "VOPC unordered-equal compare did not lower to shared IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareUnordLeF32 vcc_lo, v6, v6"),
	      "VOPC unordered-less-equal compare did not lower to shared IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareUnordGtF32 vcc_lo, v6, v6"),
	      "VOPC unordered-greater compare did not lower to shared IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareUnordNeF32 vcc_lo, v6, v6"),
	      "VOPC unordered-not-equal compare did not lower to shared IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareUnordNeF32 s0, 0x3f000000, v1"),
	      "VOPC SDWA unordered-not-equal compare did not lower to scalar-destination IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareUnordGeF32 vcc_lo, v6, v6"),
	      "VOPC unordered-greater-equal compare did not lower to shared IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareMaskUnordLtF32 exec_lo, v6, v6"),
	      "VOPC unordered-less compare-and-mask did not lower to shared IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareMaskUnordEqF32 exec_lo, v6, v6"),
	      "VOPC unordered-equal compare-and-mask did not lower to shared IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareMaskUnordLeF32 exec_lo, v6, v6"),
	      "VOPC unordered-less-equal compare-and-mask did not lower to shared IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareMaskUnordGtF32 exec_lo, v6, v6"),
	      "VOPC unordered-greater compare-and-mask did not lower to shared IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareMaskUnordNeF32 exec_lo, v6, v6"),
	      "VOPC unordered-not-equal compare-and-mask did not lower to shared IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareMaskUnordGeF32 exec_lo, v6, v6"),
	      "VOPC unordered-greater-equal compare-and-mask did not lower to shared IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareFalse vcc_lo, v5, v5"),
	      "VOPC integer false compare did not lower to shared IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareTrue vcc_lo, v5, v5"),
	      "VOPC integer true compare did not lower to shared IR");
	Check(Common::ContainsStr(result.ir_dump, "FMadF32 v8"), "VOP3 mad did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "FMadF32 v4, v1.neg, v9, s4"),
	      "VOP3 mad source modifiers did not lower to IR metadata");
	Check(Common::ContainsStr(result.ir_dump, "CubeIdF32 v110, v5, v6, v7"),
	      "V_CUBEID_F32 did not lower to cube IR");
	Check(Common::ContainsStr(result.ir_dump, "CubeScF32 v111, v5, v6, v7"),
	      "V_CUBESC_F32 did not lower to cube IR");
	Check(Common::ContainsStr(result.ir_dump, "CubeTcF32 v112, v5, v6, v7"),
	      "V_CUBETC_F32 did not lower to cube IR");
	Check(Common::ContainsStr(result.ir_dump, "CubeMaF32 v113, v5, v6, v7"),
	      "V_CUBEMA_F32 did not lower to cube IR");
	Check(Common::ContainsStr(result.ir_dump, "CubeMaF32 v5, v2, v0, v6.neg"),
	      "V_CUBEMA_F32 source modifier did not lower to cube IR");
	Check(Common::ContainsStr(result.ir_dump, "FMadF32 v37"), "VOP3 fma did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "FMin3F32 v38"), "VOP3 min3 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "FMax3F32 v39"), "VOP3 max3 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "FMed3F32 v40"), "VOP3 med3 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "IMin3I32 v41"),
	      "VOP3 signed min3 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "UMin3U32 v42"),
	      "VOP3 unsigned min3 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "IMax3I32 v43"),
	      "VOP3 signed max3 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "UMax3U32 v44"),
	      "VOP3 unsigned max3 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "IMed3I32 v45"),
	      "VOP3 signed med3 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "UMed3U32 v46"),
	      "VOP3 unsigned med3 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "BitFieldExtract3U32 v47"),
	      "VOP3 unsigned bitfield extract did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "BitFieldExtract3I32 v48"),
	      "VOP3 signed bitfield extract did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "BitFieldInsertSelectU32 v49"),
	      "VOP3 bitfield insert-select did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AlignBitU32 v50"),
	      "VOP3 alignbit did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "IAdd3U32 v51"), "VOP3 add3 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "IMulU32 v52, v5, v6"),
	      "V_MUL_LO_U32 did not lower to multiply IR");
	Check(Common::ContainsStr(result.ir_dump, "UMulHighU32 v53, v5, v6"),
	      "V_MUL_HI_U32 did not lower to high-multiply IR");
	Check(Common::ContainsStr(result.ir_dump, "SMulHighI32 v114, v5, v6"),
	      "V_MUL_HI_I32 did not lower to signed high-multiply IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseAndOrU32 v54, v5, v6, v7"),
	      "V_AND_OR_B32 did not lower to ternary bitwise IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseOr3U32 v55, v5, v6, v7"),
	      "V_OR3_B32 did not lower to ternary bitwise IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseXor3U32 v56, v5, v6, v7"),
	      "V_XOR3_B32 did not lower to ternary bitwise IR");
	Check(Common::ContainsStr(result.ir_dump, "ShiftLeftAddU32 v57, v5, v6, v7"),
	      "V_LSHL_ADD_U32 did not lower to shared shift-left-add IR");
	Check(Common::ContainsStr(result.ir_dump, "AddShiftLeftU32 v58, v5, v6, v7"),
	      "V_ADD_LSHL_U32 did not lower to add-shift-left IR");
	Check(Common::ContainsStr(result.ir_dump, "XorAddU32 v59, v5, v6, v7"),
	      "V_XAD_U32 did not lower to xor-add IR");
	Check(Common::ContainsStr(result.ir_dump, "ShiftLeftOrU32 v60, v5, v6, v7"),
	      "V_LSHL_OR_B32 did not lower to shift-left-or IR");
	Check(Common::ContainsStr(result.ir_dump, "SadU32 v61, v5, v6, v7"),
	      "V_SAD_U32 did not lower to sad IR");
	Check(Common::ContainsStr(result.ir_dump, "IMadI24U32 v62, v5, v6, v7"),
	      "V_MAD_I32_I24 did not lower to signed 24-bit mad IR");
	Check(Common::ContainsStr(result.ir_dump, "UMadU24U32 v63, v5, v6, v7"),
	      "V_MAD_U32_U24 did not lower to unsigned 24-bit mad IR");
	Check(Common::ContainsStr(result.ir_dump, "IMulU32 v69, v5, v6"),
	      "V_MUL_LO_I32 did not lower to multiply IR");
	Check(Common::ContainsStr(result.ir_dump, "IAddCarryU32 v70, s0, v5, v6, 0x00000000"),
	      "V_ADD_I32 did not lower to carry-out add IR");
	Check(Common::ContainsStr(result.ir_dump, "ISubBorrowU32 v71, s0, v5, v6"),
	      "V_SUB_I32 did not lower to borrow-out subtract IR");
	Check(Common::ContainsStr(result.ir_dump, "ISubBorrowU32 v72, s0, v6, v5"),
	      "V_SUBREV_I32 did not reverse source order in borrow-out IR");
	Check(Common::ContainsStr(
	          result.ir_dump,
	          "IMaxI16 v7.sdwa(sel=4,sext=0), 0x0000ffff, v5.opsel(lo=1,hi=0,neghi=0)"),
	      "V_MAX_I16 did not lower to signed halfword max IR");
	Check(Common::ContainsStr(
	          result.ir_dump,
	          "IMinI16 v7.sdwa(sel=5,sext=0), 0x00000002, v4.opsel(lo=1,hi=0,neghi=0)"),
	      "V_MIN_I16 did not lower to signed halfword min IR");
	Check(Common::ContainsStr(result.ir_dump, "BitFieldMaskU32 v73, 0x00000001, 0x00000004"),
	      "V_BFM_B32 did not lower to shared bitfield-mask IR");
	Check(Common::ContainsStr(result.ir_dump, "PackF32ToF16Rtz v75, v6, v6"),
	      "V_CVT_PKRTZ_F16_F32 did not lower to shared pack IR");
	Check(Common::ContainsStr(result.ir_dump, "PackF32ToF16Rtz v0, v0.neg, v1.neg"),
	      "V_CVT_PKRTZ_F16_F32 source modifiers did not lower to shared pack IR");
	Check(Common::ContainsStr(result.ir_dump, "LdexpF32 v76, v6, 0x00000001"),
	      "V_LDEXP_F32 did not lower to ldexp IR");
	Check(Common::ContainsStr(result.ir_dump, "LdexpF32 v7, v9.abs, 0xfffffffe"),
	      "V_LDEXP_F32 source modifier did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "LdexpF32 v13.clamp, v6, 0xfffffffc"),
	      "V_LDEXP_F32 clamp modifier did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "BitCountAddU32 v77, v5, v6"),
	      "VOP3 V_BCNT_U32_B32 did not lower to bit-count-add IR");
	Check(Common::ContainsStr(result.ir_dump, "PackSnorm2x16F32 v78, v6, v6"),
	      "V_CVT_PKNORM_I16_F32 did not lower to shared pack IR");
	Check(Common::ContainsStr(result.ir_dump, "PackUnorm2x16F32 v79, v6, v6"),
	      "V_CVT_PKNORM_U16_F32 did not lower to shared pack IR");
	Check(Common::ContainsStr(result.ir_dump, "PackU16U32 v80, v5, v7"),
	      "V_CVT_PK_U16_U32 did not lower to shared pack IR");
	Check(SpirvContainsOpcode(result.spirv, 112), "SPIR-V binary does not contain OpConvertUToF");
	Check(SpirvContainsOpcode(result.spirv, 109), "SPIR-V binary does not contain OpConvertFToU");
	Check(SpirvContainsOpcode(result.spirv, 111), "SPIR-V binary does not contain OpConvertSToF");
	Check(SpirvContainsOpcode(result.spirv, 110), "SPIR-V binary does not contain OpConvertFToS");
	Check(SpirvContainsOpcode(result.spirv, 136), "SPIR-V binary does not contain OpFDiv");
	Check(SpirvContainsOpcode(result.spirv, 127), "SPIR-V binary does not contain OpFNegate");
	Check(SpirvContainsOpcode(result.spirv, 12), "SPIR-V binary does not contain OpExtInst");
	Check(SpirvContainsExtInst(result.spirv, 58),
	      "SPIR-V binary does not contain GLSL.std.450 PackHalf2x16");
	Check(SpirvContainsExtInst(result.spirv, 62),
	      "SPIR-V binary does not contain GLSL.std.450 UnpackHalf2x16");
	Check(SpirvContainsExtInst(result.spirv, 50),
	      "SPIR-V binary does not contain GLSL.std.450 Fma");
	Check(SpirvContainsExtInst(result.spirv, 43),
	      "SPIR-V binary does not contain GLSL.std.450 FClamp");
	Check(SpirvContainsExtInst(result.spirv, 53),
	      "SPIR-V binary does not contain GLSL.std.450 Ldexp");
	Check(SpirvContainsOpcode(result.spirv, 128), "SPIR-V binary does not contain OpIAdd");
	Check(SpirvContainsOpcode(result.spirv, 149), "SPIR-V binary does not contain OpIAddCarry");
	Check(SpirvContainsOpcode(result.spirv, 130), "SPIR-V binary does not contain OpISub");
	Check(SpirvContainsOpcode(result.spirv, 132), "SPIR-V binary does not contain OpIMul");
	Check(SpirvContainsOpcode(result.spirv, 171), "SPIR-V binary does not contain OpINotEqual");
	Check(SpirvContainsOpcode(result.spirv, 200), "SPIR-V binary does not contain OpNot");
	Check(SpirvContainsOpcode(result.spirv, 204), "SPIR-V binary does not contain OpBitReverse");
	Check(SpirvContainsOpcode(result.spirv, 205), "SPIR-V binary does not contain OpBitCount");
	Check(SpirvContainsOpcode(result.spirv, 166), "SPIR-V binary does not contain OpLogicalOr");
	Check(SpirvContainsOpcode(result.spirv, 167), "SPIR-V binary does not contain OpLogicalAnd");
	Check(SpirvContainsOpcode(result.spirv, 186),
	      "SPIR-V binary does not contain OpFOrdGreaterThan");
	Check(SpirvContainsOpcode(result.spirv, 181), "SPIR-V binary does not contain OpFUnordEqual");
	Check(SpirvContainsOpcode(result.spirv, 183),
	      "SPIR-V binary does not contain OpFUnordNotEqual");
	Check(SpirvContainsOpcode(result.spirv, 185),
	      "SPIR-V binary does not contain OpFUnordLessThan");
	Check(SpirvContainsOpcode(result.spirv, 187),
	      "SPIR-V binary does not contain OpFUnordGreaterThan");
	Check(SpirvContainsOpcode(result.spirv, 189),
	      "SPIR-V binary does not contain OpFUnordLessThanEqual");
	Check(SpirvContainsOpcode(result.spirv, 191),
	      "SPIR-V binary does not contain OpFUnordGreaterThanEqual");
	Check(SpirvContainsOpcode(result.spirv, 190),
	      "SPIR-V binary does not contain OpFOrdGreaterThanEqual");
	Check(SpirvContainsOpcode(result.spirv, 184), "SPIR-V binary does not contain OpFOrdLessThan");
	Check(SpirvContainsOpcode(result.spirv, 173), "SPIR-V binary does not contain OpSGreaterThan");
	Check(SpirvContainsOpcode(result.spirv, 174),
	      "SPIR-V binary does not contain OpUGreaterThanEqual");
	Check(SpirvContainsOpcode(result.spirv, 177), "SPIR-V binary does not contain OpSLessThan");
	Check(SpirvContainsOpcode(result.spirv, 202),
	      "SPIR-V binary does not contain OpBitFieldSExtract");
	Check(SpirvContainsOpcode(result.spirv, 203),
	      "SPIR-V binary does not contain OpBitFieldUExtract");
	Check(SpirvContainsOpcode(result.spirv, 194),
	      "SPIR-V binary does not contain OpShiftRightLogical");
	Check(SpirvContainsOpcode(result.spirv, 61),
	      "SPIR-V binary does not contain OpLoad for cmpx old exec mask");
	Check(SpirvContainsOpcode(result.spirv, 133), "SPIR-V binary does not contain OpFMul");
	Check(SpirvContainsOpcode(result.spirv, 129), "SPIR-V binary does not contain OpFAdd");
	Check(SpirvContainsOpcode(result.spirv, 151), "SPIR-V binary does not contain OpUMulExtended");
	Check(SpirvContainsOpcode(result.spirv, 152), "SPIR-V binary does not contain OpSMulExtended");
	Check(SpirvContainsOpcode(result.spirv, 196),
	      "SPIR-V binary does not contain OpShiftLeftLogical");
	Check(SpirvContainsOpcode(result.spirv, 197), "SPIR-V binary does not contain OpBitwiseOr");
	Check(SpirvContainsOpcode(result.spirv, 198), "SPIR-V binary does not contain OpBitwiseXor");
	Check(SpirvContainsOpcode(result.spirv, 199), "SPIR-V binary does not contain OpBitwiseAnd");
	Check(SpirvContainsOpcode(result.spirv, 80),
	      "SPIR-V binary does not contain OpCompositeConstruct");
	Check(SpirvContainsOpcode(result.spirv, 81),
	      "SPIR-V binary does not contain OpCompositeExtract");
	Check(SpirvContainsOpcode(result.spirv, 339),
	      "SPIR-V binary does not contain OpGroupNonUniformBallot");
	Check(SpirvContainsOpcode(result.spirv, 343),
	      "SPIR-V binary does not contain OpGroupNonUniformBallotFindLSB");
	Check(SpirvContainsOpcode(result.spirv, 345),
	      "SPIR-V binary does not contain OpGroupNonUniformShuffle");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerExpandedAluBatch() {
	const uint32_t shader[] = {
	    EncodeSopk(0x00, 9, 7),          // s_movk_i32 s9, 7
	    EncodeSopk(0x0f, 9, 2),          // s_add_i32 s9, s9, 2
	    EncodeSopk(0x10, 9, 3),          // s_mulk_i32 s9, s9, 3
	    EncodeSop2(0x07, 10, 9, 128),    // s_min_u32 s10, s9, 0
	    EncodeSop2(0x09, 11, 10, 130),   // s_max_u32 s11, s10, 2
	    EncodeSop2(0x26, 12, 11, 130),   // s_mul_i32 s12, s11, 2
	    EncodeVop2(0x05, 1, 242, 0),     // v_subrev_f32 v1, 1.0, v0
	    EncodeVop2(0x0f, 2, 1 + 256, 0), // v_min_f32 v2, v1, v0
	    EncodeVop2(0x10, 3, 1 + 256, 2), // v_max_f32 v3, v1, v2
	    EncodeVop2(0x27, 4, 130, 3),     // v_subrev_nc_u32 v4, 2, v3
	    EncodeVop2(0x16, 5, 129, 4),     // v_lshrrev_b32 v5, 1, v4
	    EncodeVop2(0x1a, 6, 129, 5),     // v_lshlrev_b32 v6, 1, v5
	    EncodeVop2(0x13, 7, 6 + 256, 5), // v_min_u32 v7, v6, v5
	    EncodeVop2(0x14, 8, 7 + 256, 6), // v_max_u32 v8, v7, v6
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "s_min_u32"),
	      "new decoder did not decode S_MIN_U32");
	Check(Common::ContainsStr(result.decoded_dump, "s_mulk_i32"),
	      "new decoder did not decode S_MULK_I32");
	Check(Common::ContainsStr(result.decoded_dump, "v_subrev_f32"),
	      "new decoder did not decode V_SUBREV_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_lshlrev_b32"),
	      "new decoder did not decode V_LSHLREV_B32");
	Check(Common::ContainsStr(result.ir_dump, "IMulU32 s9, s9, 0x00000003"),
	      "SOPK multiply did not lower to self-multiply IR");
	Check(Common::ContainsStr(result.ir_dump, "UMinU32 s10"), "S_MIN_U32 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "UMaxU32 s11"), "S_MAX_U32 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "FSubF32 v1, v0, 0x3f800000"),
	      "V_SUBREV_F32 did not reverse source order in IR");
	Check(Common::ContainsStr(result.ir_dump, "FMinF32 v2"), "V_MIN_F32 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "FMaxF32 v3"), "V_MAX_F32 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "ISubU32 v4, v3, 0x00000002"),
	      "V_SUBREV_NC_U32 did not reverse source order in IR");
	Check(Common::ContainsStr(result.ir_dump, "ShiftLeftLogicalU32 v6, v5, 0x00000001"),
	      "V_LSHLREV_B32 did not reverse source order in IR");
	Check(Common::ContainsStr(result.ir_dump, "UMinU32 v7"), "V_MIN_U32 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "UMaxU32 v8"), "V_MAX_U32 did not lower to IR");
	Check(SpirvContainsOpcode(result.spirv, 132), "SPIR-V binary does not contain OpIMul");
	Check(SpirvContainsOpcode(result.spirv, 169), "SPIR-V binary does not contain OpSelect");
	Check(SpirvContainsOpcode(result.spirv, 172), "SPIR-V binary does not contain OpUGreaterThan");
	Check(SpirvContainsOpcode(result.spirv, 176), "SPIR-V binary does not contain OpULessThan");
	Check(SpirvContainsOpcode(result.spirv, 184), "SPIR-V binary does not contain OpFOrdLessThan");
	Check(SpirvContainsOpcode(result.spirv, 186),
	      "SPIR-V binary does not contain OpFOrdGreaterThan");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerVop3pPackedF16() {
	const uint32_t shader[] = {
	    EncodeVop3pWord0(0x0f, 114, 0, 7),
	    EncodeVop3pWord1(5 + 256, 6 + 256, 0, 7), // v_pk_add_f16 v114, v5, v6
	    EncodeVop3pWord0(0x10, 115, 0, 7),
	    EncodeVop3pWord1(5 + 256, 6 + 256, 0, 7), // v_pk_mul_f16 v115, v5, v6
	    EncodeVop3pWord0(0x11, 116, 0, 7),
	    EncodeVop3pWord1(5 + 256, 6 + 256, 0, 7), // v_pk_min_f16 v116, v5, v6
	    EncodeVop3pWord0(0x12, 117, 0, 7),
	    EncodeVop3pWord1(5 + 256, 6 + 256, 0, 7), // v_pk_max_f16 v117, v5, v6
	    EncodeVop3pWord0(0x0e, 118, 0, 7, 2),
	    EncodeVop3pWord1(5 + 256, 6 + 256, 7 + 256, 7, 1),
	    EncodeVop3pWord0(0x20, 119),
	    EncodeVop3pWord1(6 + 256, 6 + 256, 6 + 256), // v_fma_f32 via VOP3P
	    0xcc204044u,
	    0x1a022110u, // v_fma_mix_f32 v68, v16.lo, v16.lo, 0.lo
	    EncodeVop3pWord0(0x21, 120, 0, 7),
	    EncodeVop3pWord1(5 + 256, 6 + 256, 7 + 256, 7), // v_mad_mixlo_f16
	    EncodeVop3pWord0(0x22, 121, 0, 7, 1),
	    EncodeVop3pWord1(5 + 256, 6 + 256, 7 + 256, 7, 2), // v_mad_mixhi_f16
	    EncodeVop3Word0(0x34b, 122, 0x0f),
	    EncodeVop3Word1(5 + 256, 6 + 256, 7 + 256), // native v_fma_f16
	    EncodeVop3pWord0(0x22, 126, 0, 1, 0, true),
	    EncodeVop3pWord1(242, 243, 3 + 256, 1), // clamped v_mad_mixhi_f16
	    EncodeVop3Word0(0x34b, 127, 0x8, 0, true),
	    EncodeVop3Word1(240, 4 + 256, 241), // clamped native high-half v_fma_f16
	    0xd711001au,
	    0x0002170au, // v_pack_b32_f16 v26, v10, v11
	    0xd711182bu,
	    0x0002170au, // v_pack_b32_f16 v43, v10.hi, v11.hi
	    0x78765714u, // v_pk_fmac_f16 from boot shader 0x0000001c00f5c000
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "v_pk_add_f16 v114"),
	      "new decoder did not decode old-backed V_PK_ADD_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v_pk_mul_f16 v115"),
	      "new decoder did not decode old-backed V_PK_MUL_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v_pk_min_f16 v116"),
	      "new decoder did not decode old-backed V_PK_MIN_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v_pk_max_f16 v117"),
	      "new decoder did not decode old-backed V_PK_MAX_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v_pk_fma_f16 v118"),
	      "new decoder did not decode old-backed V_PK_FMA_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v_fma_f32 v119"),
	      "new decoder did not decode old-backed VOP3P V_FMA_F32");
	Check(Common::ContainsStr(result.decoded_dump, "v_fma_f32 v68, v16.opsel(lo=0,hi=1,neghi=0)"),
	      "new decoder did not decode VOP3P V_FMA_MIX_F32 source selectors");
	Check(Common::ContainsStr(result.decoded_dump, "v_mad_mixlo_f16 v120"),
	      "new decoder did not decode old-backed V_MAD_MIXLO_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v_mad_mixhi_f16 v121"),
	      "new decoder did not decode old-backed V_MAD_MIXHI_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v_fma_f16 v122"),
	      "new decoder did not decode native VOP3 V_FMA_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v_mad_mixhi_f16 v126.sdwa(sel=5"),
	      "new decoder did not decode clamped V_MAD_MIXHI_F16 high-half destination");
	Check(Common::ContainsStr(result.decoded_dump, "v_fma_f16 v127.sdwa(sel=5"),
	      "new decoder did not decode clamped native high-half V_FMA_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v_pack_b32_f16 v26"),
	      "new decoder did not decode native V_PACK_B32_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v_pack_b32_f16 v43") &&
	          Common::ContainsStr(result.decoded_dump, "v10.opsel(lo=1"),
	      "new decoder did not decode V_PACK_B32_F16 source lane selectors");
	Check(Common::ContainsStr(result.decoded_dump, "v_pk_fmac_f16 v59"),
	      "new decoder did not decode VOP2 V_PK_FMAC_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v6.opsel(lo=0,hi=1,neghi=1)"),
	      "VOP3P high-lane source modifier was not dumped");
	Check(Common::ContainsStr(result.decoded_dump, "v120.sdwa(sel=4"),
	      "V_MAD_MIXLO_F16 did not expose low-half destination merge");
	Check(Common::ContainsStr(result.decoded_dump, "v121.sdwa(sel=5"),
	      "V_MAD_MIXHI_F16 did not expose high-half destination merge");
	Check(Common::ContainsStr(result.ir_dump, "PackedAddF16 v114"),
	      "V_PK_ADD_F16 did not lower to packed f16 IR");
	Check(Common::ContainsStr(result.ir_dump, "PackedMulF16 v115"),
	      "V_PK_MUL_F16 did not lower to packed f16 IR");
	Check(Common::ContainsStr(result.ir_dump, "PackedMinF16 v116"),
	      "V_PK_MIN_F16 did not lower to packed f16 IR");
	Check(Common::ContainsStr(result.ir_dump, "PackedMaxF16 v117"),
	      "V_PK_MAX_F16 did not lower to packed f16 IR");
	Check(Common::ContainsStr(result.ir_dump, "PackedFmaF16 v118"),
	      "V_PK_FMA_F16 did not lower to packed f16 IR");
	Check(Common::ContainsStr(result.ir_dump, "FMadF32 v119"),
	      "VOP3P V_FMA_F32 did not lower through shared fma IR");
	Check(Common::ContainsStr(result.ir_dump, "FMadF32 v68, v16.opsel(lo=0,hi=1,neghi=0)"),
	      "VOP3P V_FMA_MIX_F32 source selectors did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "MadMixF16 v120.sdwa(sel=4"),
	      "V_MAD_MIXLO_F16 did not lower to shared mad-mix IR");
	Check(Common::ContainsStr(result.ir_dump, "MadMixF16 v121.sdwa(sel=5"),
	      "V_MAD_MIXHI_F16 did not lower to shared mad-mix IR");
	Check(Common::ContainsStr(result.ir_dump, "FmaF16 v122"),
	      "native VOP3 V_FMA_F16 did not lower to native f16 fma IR");
	Check(Common::ContainsStr(result.ir_dump, "MadMixF16 v126.sdwa(sel=5"),
	      "clamped V_MAD_MIXHI_F16 did not lower to shared mad-mix IR");
	Check(Common::ContainsStr(result.ir_dump, "FmaF16 v127.sdwa(sel=5"),
	      "clamped native V_FMA_F16 did not lower to native f16 fma IR");
	Check(Common::ContainsStr(result.ir_dump, "PackB32F16 v26"),
	      "V_PACK_B32_F16 did not lower to packed-bit IR");
	Check(Common::ContainsStr(result.ir_dump, "PackB32F16 v43") &&
	          Common::ContainsStr(result.ir_dump, "v10.opsel(lo=1"),
	      "V_PACK_B32_F16 source selectors did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "PackedFmaF16 v59, v20, v43, v59"),
	      "V_PK_FMAC_F16 did not lower using destination as packed FMA accumulator");
	Check(SpirvContainsExtInst(result.spirv, 62),
	      "SPIR-V binary does not contain GLSL.std.450 UnpackHalf2x16 for VOP3P");
	Check(SpirvContainsExtInst(result.spirv, 58),
	      "SPIR-V binary does not contain GLSL.std.450 PackHalf2x16 for VOP3P");
	Check(SpirvContainsExtInst(result.spirv, 50),
	      "SPIR-V binary does not contain GLSL.std.450 Fma for VOP3P");
	Check(SpirvContainsExtInst(result.spirv, 43),
	      "SPIR-V binary does not contain GLSL.std.450 FClamp for clamped f16 ops");
	Check(!SpirvContainsExtInst(result.spirv, 37),
	      "SPIR-V binary still contains GLSL.std.450 FMin for VOP3P packed min");
	Check(!SpirvContainsExtInst(result.spirv, 40),
	      "SPIR-V binary still contains GLSL.std.450 FMax for VOP3P packed max");
	Check(SpirvContainsOpcode(result.spirv, 169),
	      "SPIR-V binary does not contain OpSelect for VOP3P packed min/max");
	Check(SpirvContainsOpcode(result.spirv, 184),
	      "SPIR-V binary does not contain OpFOrdLessThan for VOP3P packed min");
	Check(SpirvContainsOpcode(result.spirv, 190),
	      "SPIR-V binary does not contain OpFOrdGreaterThanEqual for VOP3P packed max");
	Check(SpirvContainsOpcode(result.spirv, 197),
	      "SPIR-V binary does not contain OpBitwiseOr for VOP3P packed min/max");
	Check(SpirvContainsOpcode(result.spirv, 199),
	      "SPIR-V binary does not contain OpBitwiseAnd for VOP3P packed min/max");
	Check(SpirvContainsOpcode(result.spirv, 129), "SPIR-V binary does not contain OpFAdd");
	Check(SpirvContainsOpcode(result.spirv, 133), "SPIR-V binary does not contain OpFMul");
	Check(SpirvContainsOpcode(result.spirv, 81),
	      "SPIR-V binary does not contain OpCompositeExtract");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerStagedShaderOps() {
	const uint32_t shader[] = {
	    EncodeSMovB32(0, 132),            // s0 = 4
	    EncodeSMovB32(1, 129),            // s1 = 1
	    EncodeSop2(0x05, 2, 0, 1),        // s_subb_u32 s2, s0, s1
	    EncodeSop1(0x1b, 3, 1),           // s_bitset0_b32 s3, s1
	    EncodeVop2(0x36, 70, 5 + 256, 6), // v_fmac_f16 v70, v5, v6
	    EncodeVop2(0x37, 71, 7 + 256, 8),
	    0x3c003c00u, // v_fmamk_f16
	    EncodeVop2(0x38, 72, 9 + 256, 10),
	    0x40004000u, // v_fmaak_f16
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "s_subb_u32 s2, s0, s1"),
	      "new decoder did not decode RDNA2 S_SUBB_U32");
	Check(Common::ContainsStr(result.decoded_dump, "s_bitset0_b32 s3, s1"),
	      "new decoder did not decode RDNA2 S_BITSET0_B32");
	Check(Common::ContainsStr(result.decoded_dump, "v_fmac_f16 v70.sdwa(sel=4"),
	      "new decoder did not decode RDNA2 V_FMAC_F16");
	Check(Common::ContainsStr(result.decoded_dump,
	                          "v_fmamk_f16 v71.sdwa(sel=4,sext=0), v7, 0x3c003c00, v8"),
	      "new decoder did not consume V_FMAMK_F16 literal as the multiply source");
	Check(Common::ContainsStr(result.decoded_dump,
	                          "v_fmaak_f16 v72.sdwa(sel=4,sext=0), v9, v10, 0x40004000"),
	      "new decoder did not consume V_FMAAK_F16 literal as the add source");
	Check(Common::ContainsStr(result.ir_dump, "ScalarSubBorrowCarryU32 s2, s0, s1, scc"),
	      "S_SUBB_U32 did not lower to scalar subtract-with-borrow IR");
	Check(Common::ContainsStr(result.ir_dump, "BitClearU32 s3, s3, s1"),
	      "S_BITSET0_B32 did not lower to bit-clear IR using the destination as input");
	Check(Common::ContainsStr(result.ir_dump,
	                          "FmaF16 v70.sdwa(sel=4,sext=0), v5, v6, v70.sdwa(sel=4,sext=0)"),
	      "V_FMAC_F16 did not lower using the destination as the FMA accumulator");
	Check(Common::ContainsStr(result.ir_dump, "FmaF16 v71.sdwa(sel=4,sext=0), v7, 0x3c003c00, v8"),
	      "V_FMAMK_F16 did not lower with the literal in source 1");
	Check(Common::ContainsStr(result.ir_dump, "FmaF16 v72.sdwa(sel=4,sext=0), v9, v10, 0x40004000"),
	      "V_FMAAK_F16 did not lower with the literal in source 2");
	Check(SpirvContainsOpcode(result.spirv, 130),
	      "SPIR-V binary does not contain OpISub for S_SUBB_U32");
	Check(SpirvContainsOpcode(result.spirv, 166),
	      "SPIR-V binary does not contain OpLogicalOr for S_SUBB_U32 borrow-out");
	Check(SpirvContainsOpcode(result.spirv, 172),
	      "SPIR-V binary does not contain OpUGreaterThan for S_SUBB_U32 borrow-out");
	Check(SpirvContainsOpcode(result.spirv, 196),
	      "SPIR-V binary does not contain OpShiftLeftLogical for S_BITSET0_B32");
	Check(SpirvContainsOpcode(result.spirv, 199),
	      "SPIR-V binary does not contain OpBitwiseAnd for S_BITSET0_B32");
	Check(SpirvContainsOpcode(result.spirv, 200),
	      "SPIR-V binary does not contain OpNot for S_BITSET0_B32");
	Check(SpirvContainsExtInst(result.spirv, 50),
	      "SPIR-V binary does not contain GLSL.std.450 Fma for VOP2 F16 FMA ops");
	Check(SpirvContainsExtInst(result.spirv, 58),
	      "SPIR-V binary does not contain GLSL.std.450 PackHalf2x16 for VOP2 F16 FMA ops");
	Check(SpirvContainsExtInst(result.spirv, 62),
	      "SPIR-V binary does not contain GLSL.std.450 UnpackHalf2x16 for VOP2 F16 FMA ops");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerBootF16UnaryOpcodes() {
	const uint32_t shader[] = {
	    EncodeVop1(0x55, 4, 249),
	    EncodeVop1Sdwa(3, 5, 2, 6),
	    EncodeVop1(0x5b, 5, 4 + 256),
	    0x7e12b8f9u,
	    0x0006150bu,
	    0x7e12b90cu,
	    EncodeVop1(0x5d, 6, 5 + 256),
	    0x7e0e02f9u,
	    0x008616c1u,
	    0x7e1002f9u,
	    0x008616c1u,
	    EncodeVop1(0x5e, 3, 3 + 256),
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "v_sqrt_f16 v4.sdwa(sel=5"),
	      "new decoder did not decode V_SQRT_F16 SDWA high-half destination");
	Check(Common::ContainsStr(result.decoded_dump, "v_rndne_f16 v3"),
	      "new decoder did not decode V_RNDNE_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v_floor_f16 v5"),
	      "new decoder did not decode V_FLOOR_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v_ceil_f16 v9.sdwa(sel=5"),
	      "new decoder did not decode boot V_CEIL_F16 SDWA high-half destination");
	Check(Common::ContainsStr(result.decoded_dump, "v_ceil_f16 v9"),
	      "new decoder did not decode boot V_CEIL_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v_trunc_f16 v6"),
	      "new decoder did not decode V_TRUNC_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v_mov_b32 v7, -1") &&
	          Common::ContainsStr(result.decoded_dump, "v_mov_b32 v8, -1"),
	      "new decoder did not accept full-width V_MOV_B32 SDWA with DST_U=PRESERVE");
	Check(!Common::ContainsStr(result.decoded_dump,
	                           "VOP1 SDWA destination selector is not supported"),
	      "full-width V_MOV_B32 SDWA with DST_U=PRESERVE was rejected");
	Check(!Common::ContainsStr(result.decoded_dump, "unsupported family=VOP2 opcode=0x00"),
	      "VOP1 SDWA extension word was decoded as a phantom VOP2 instruction");
	Check(Common::ContainsStr(result.ir_dump, "SqrtF16 v4.sdwa(sel=5"),
	      "V_SQRT_F16 did not lower to f16 sqrt IR");
	Check(Common::ContainsStr(result.ir_dump, "FloorF16 v5"),
	      "V_FLOOR_F16 did not lower to f16 floor IR");
	Check(Common::ContainsStr(result.ir_dump, "CeilF16 v9.sdwa(sel=5"),
	      "boot V_CEIL_F16 SDWA did not lower to f16 ceil IR");
	Check(Common::ContainsStr(result.ir_dump, "CeilF16 v9"),
	      "V_CEIL_F16 did not lower to f16 ceil IR");
	Check(Common::ContainsStr(result.ir_dump, "TruncF16 v6"),
	      "V_TRUNC_F16 did not lower to f16 trunc IR");
	Check(Common::ContainsStr(result.ir_dump, "MoveU32 v7, 0xffffffff") &&
	          Common::ContainsStr(result.ir_dump, "MoveU32 v8, 0xffffffff"),
	      "full-width V_MOV_B32 SDWA with DST_U=PRESERVE did not lower to move IR");
	Check(Common::ContainsStr(result.ir_dump, "RoundEvenF16 v3"),
	      "V_RNDNE_F16 did not lower to f16 round-even IR");
	Check(SpirvContainsExtInst(result.spirv, 31),
	      "SPIR-V binary does not contain GLSL.std.450 Sqrt for V_SQRT_F16");
	Check(SpirvContainsExtInst(result.spirv, 8),
	      "SPIR-V binary does not contain GLSL.std.450 Floor for V_FLOOR_F16");
	Check(SpirvContainsExtInst(result.spirv, 9),
	      "SPIR-V binary does not contain GLSL.std.450 Ceil for V_CEIL_F16");
	Check(SpirvContainsExtInst(result.spirv, 3),
	      "SPIR-V binary does not contain GLSL.std.450 Trunc for V_TRUNC_F16");
	Check(SpirvContainsExtInst(result.spirv, 2),
	      "SPIR-V binary does not contain GLSL.std.450 RoundEven for V_RNDNE_F16");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerBootB16PackedAndSdwaOpcodes() {
	const uint32_t shader[] = {
	    0xd7070001u,
	    0x00020081u, // v_lshrrev_b16 v1.lo, 1, v0.lo
	    0xd7071008u,
	    0x0002128fu, // v_lshrrev_b16 with selected high source lane
	    0xd7144005u,
	    0x00020481u, // v_lshlrev_b16 v5.hi, 1, v2.lo
	    0xd7034001u,
	    0x0002066au, // v_add_nc_u16 v1.hi, v106.lo, v3.lo
	    0xd70e4011u,
	    0x00020e80u, // v_sub_nc_i16 v17.hi, 0, v7.lo
	    0xcc03400eu,
	    0x10020e80u, // v_pk_sub_i16 from boot shader 0x0000001980eb0000
	    0xcc0a4104u,
	    0x300202ffu,
	    0x00007fffu, // v_pk_add_u16 with literal
	    0x360e04f9u,
	    0x04861482u, // v_and_b32 SDWA partial low-half destination
	    0x381212f9u,
	    0x04041508u, // v_or_b32 SDWA partial high-half destination
	    0xcc10c00cu,
	    0x18021d0du, // clamped v_pk_mul_f16
	    0x7e08a0f9u,
	    0x00061500u,                    // v_cvt_f16_u16 v4.hi, v0 from boot shader
	    EncodeVop1(0x50, 16, 10 + 256), // v_cvt_f16_u16 v16, v10
	    0x7e1ca307u,                    // v_cvt_f16_i16 v14, v7
	    0x7e1ca2f9u,
	    0x00051511u,                   // v_cvt_f16_i16 v14.hi, v17.hi
	    0x7e08a704u,                   // v_cvt_i16_f16 v4, v4 from boot shader
	    EncodeVop1(0x52, 15, 4 + 256), // v_cvt_u16_f16 v15, v4
	    0x4a0a08f9u,
	    0x0c860688u, // v_add_nc_u32 v5, 8, sign-extended v4.lo
	    0x4a0c0cf9u,
	    0x0d860688u, // v_add_nc_u32 v6, 8, sign-extended v6.hi
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "v_lshrrev_b16 v1.sdwa(sel=4"),
	      "new decoder did not decode low-half V_LSHRREV_B16");
	Check(Common::ContainsStr(result.decoded_dump, "v_lshlrev_b16 v5.sdwa(sel=5"),
	      "new decoder did not decode high-half V_LSHLREV_B16");
	Check(Common::ContainsStr(result.decoded_dump, "v_add_nc_u16 v1.sdwa(sel=5"),
	      "new decoder did not decode V_ADD_NC_U16 op_sel destination");
	Check(Common::ContainsStr(result.decoded_dump, "v_sub_nc_i16 v17.sdwa(sel=5"),
	      "new decoder did not decode V_SUB_NC_I16 op_sel destination");
	Check(Common::ContainsStr(result.decoded_dump, "v_pk_sub_i16 v14"),
	      "new decoder did not decode V_PK_SUB_I16");
	Check(Common::ContainsStr(result.decoded_dump, "v_pk_add_u16 v4"),
	      "new decoder did not decode V_PK_ADD_U16");
	Check(Common::ContainsStr(result.decoded_dump, "v_pk_add_u16 v4, 0x00007fff"),
	      "V_PK_ADD_U16 literal was not consumed as a source operand");
	Check(Common::ContainsStr(result.decoded_dump, "v_and_b32 v7.sdwa(sel=4"),
	      "new decoder did not decode partial-destination V_AND_B32 SDWA");
	Check(Common::ContainsStr(result.decoded_dump, "v_or_b32 v9.sdwa(sel=5"),
	      "new decoder did not decode partial-destination V_OR_B32 SDWA");
	Check(Common::ContainsStr(result.decoded_dump, "v_pk_mul_f16 v12"),
	      "new decoder did not decode clamped V_PK_MUL_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_f16_u16 v4.sdwa(sel=5"),
	      "new decoder did not decode/consume SDWA V_CVT_F16_U16");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_f16_u16 v16"),
	      "new decoder did not decode plain V_CVT_F16_U16");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_f16_i16 v14"),
	      "new decoder did not decode plain V_CVT_F16_I16");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_f16_i16 v14.sdwa(sel=5"),
	      "new decoder did not decode V_CVT_F16_I16 SDWA high-half destination");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_i16_f16 v4"),
	      "new decoder did not decode plain V_CVT_I16_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v_cvt_u16_f16 v15"),
	      "new decoder still rejects plain V_CVT_U16_F16");
	Check(Common::ContainsStr(result.decoded_dump, "v_add_nc_u32 v5") &&
	          Common::ContainsStr(result.decoded_dump, "v4.sdwa(sel=4,sext=1"),
	      "new decoder did not decode V_ADD_NC_U32 SDWA sign-extended low word");
	Check(Common::ContainsStr(result.decoded_dump, "v_add_nc_u32 v6") &&
	          Common::ContainsStr(result.decoded_dump, "v6.sdwa(sel=5,sext=1"),
	      "new decoder did not decode V_ADD_NC_U32 SDWA sign-extended high word");
	Check(!Common::ContainsStr(result.decoded_dump, "unsupported family=VOP2 opcode=0x00"),
	      "literal/SDWA extension words were decoded as phantom VOP2 instructions");
	Check(!Common::ContainsStr(result.decoded_dump,
	                           "VOP2 SDWA destination selector is not supported"),
	      "partial bitwise SDWA destination is still rejected");
	Check(Common::ContainsStr(result.ir_dump, "ShiftRightLogicalU16"),
	      "V_LSHRREV_B16 did not lower to 16-bit logical right shift IR");
	Check(Common::ContainsStr(result.ir_dump, "ShiftLeftLogicalU16"),
	      "V_LSHLREV_B16 did not lower to 16-bit logical left shift IR");
	Check(Common::ContainsStr(result.ir_dump, "IAddU16 v1.sdwa(sel=5"),
	      "V_ADD_NC_U16 did not lower to 16-bit add IR");
	Check(Common::ContainsStr(result.ir_dump, "ISubI16 v17.sdwa(sel=5"),
	      "V_SUB_NC_I16 did not lower to 16-bit subtract IR");
	Check(Common::ContainsStr(result.ir_dump, "PackedSubI16 v14"),
	      "V_PK_SUB_I16 did not lower to packed I16 subtract IR");
	Check(Common::ContainsStr(result.ir_dump, "PackedAddU16 v4"),
	      "V_PK_ADD_U16 did not lower to packed U16 add IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertI16ToF16 v14"),
	      "V_CVT_F16_I16 did not lower to signed I16-to-F16 conversion IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertU16ToF16 v4.sdwa(sel=5"),
	      "V_CVT_F16_U16 SDWA did not lower to unsigned U16-to-F16 conversion IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertU16ToF16 v16"),
	      "V_CVT_F16_U16 plain form did not lower to unsigned U16-to-F16 conversion IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertF16ToI16 v4"),
	      "V_CVT_I16_F16 did not lower to signed F16-to-I16 conversion IR");
	Check(Common::ContainsStr(result.ir_dump, "ConvertF16ToU16 v15"),
	      "V_CVT_U16_F16 did not lower to unsigned F16-to-U16 conversion IR");
	Check(Common::ContainsStr(result.ir_dump, "IAddU32 v5") &&
	          Common::ContainsStr(result.ir_dump, "v4.sdwa(sel=4,sext=1"),
	      "V_ADD_NC_U32 SDWA sign-extended low word did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "IAddU32 v6") &&
	          Common::ContainsStr(result.ir_dump, "v6.sdwa(sel=5,sext=1"),
	      "V_ADD_NC_U32 SDWA sign-extended high word did not lower to IR");
	Check(SpirvContainsOpcode(result.spirv, 128),
	      "SPIR-V binary does not contain OpIAdd for U16 operations");
	Check(SpirvContainsOpcode(result.spirv, 202),
	      "SPIR-V binary does not contain OpBitFieldSExtract for SDWA sign extension");
	Check(SpirvContainsOpcode(result.spirv, 130),
	      "SPIR-V binary does not contain OpISub for I16 operations");
	Check(SpirvContainsOpcode(result.spirv, 194),
	      "SPIR-V binary does not contain OpShiftRightLogical for B16 shift");
	Check(SpirvContainsOpcode(result.spirv, 196),
	      "SPIR-V binary does not contain OpShiftLeftLogical for B16 shift");
	Check(SpirvContainsOpcode(result.spirv, 197),
	      "SPIR-V binary does not contain OpBitwiseOr for packed U16 result");
	Check(SpirvContainsOpcode(result.spirv, 124),
	      "SPIR-V binary does not contain OpConvertSToF for V_CVT_F16_I16");
	Check(SpirvContainsOpcode(result.spirv, 109),
	      "SPIR-V binary does not contain OpConvertFToU for V_CVT_U16_F16");
	Check(SpirvContainsOpcode(result.spirv, 110),
	      "SPIR-V binary does not contain OpConvertFToS for V_CVT_I16_F16");
	Check(SpirvContainsOpcode(result.spirv, 112),
	      "SPIR-V binary does not contain OpConvertUToF for V_CVT_F16_U16");
	Check(SpirvContainsExtInst(result.spirv, 43),
	      "SPIR-V binary does not contain GLSL.std.450 FClamp for clamped V_PK_MUL_F16");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerScalarB64Alu() {
	const uint32_t shader[] = {
	    EncodeSMovB32(0, 129),          // s0 = 1
	    EncodeSMovB32(1, 130),          // s1 = 2
	    EncodeSop1(0x07, 41, 0),        // s_not_b32 s41, s0
	    EncodeSop1(0x04, 2, 0),         // s_mov_b64 s[2:3], s[0:1]
	    EncodeSop1(0x08, 4, 2),         // s_not_b64 s[4:5], s[2:3]
	    EncodeSop2(0x0f, 6, 2, 4),      // s_and_b64 s[6:7], s[2:3], s[4:5]
	    EncodeSop2(0x11, 8, 6, 2),      // s_or_b64 s[8:9], s[6:7], s[2:3]
	    EncodeSop2(0x13, 10, 8, 4),     // s_xor_b64 s[10:11], s[8:9], s[4:5]
	    EncodeSop2(0x14, 36, 0, 1),     // s_andn2_b32 s36, s0, s1
	    EncodeSop2(0x16, 37, 0, 1),     // s_orn2_b32 s37, s0, s1
	    EncodeSop2(0x18, 38, 0, 1),     // s_nand_b32 s38, s0, s1
	    EncodeSop2(0x1a, 39, 0, 1),     // s_nor_b32 s39, s0, s1
	    EncodeSop2(0x1c, 40, 0, 1),     // s_xnor_b32 s40, s0, s1
	    EncodeSopc(0x06, 0, 0),         // s_cmp_eq_u32 s0, s0
	    EncodeSop2(0x0b, 12, 10, 2),    // s_cselect_b64 s[12:13], s[10:11], s[2:3]
	    EncodeSop2(0x15, 14, 10, 4),    // s_andn2_b64 s[14:15], s[10:11], s[4:5]
	    EncodeSop2(0x17, 16, 14, 6),    // s_orn2_b64 s[16:17], s[14:15], s[6:7]
	    EncodeSop2(0x19, 18, 16, 8),    // s_nand_b64 s[18:19], s[16:17], s[8:9]
	    EncodeSop2(0x1b, 20, 18, 10),   // s_nor_b64 s[20:21], s[18:19], s[10:11]
	    EncodeSop2(0x1d, 22, 20, 12),   // s_xnor_b64 s[22:23], s[20:21], s[12:13]
	    EncodeSop2(0x1f, 24, 22, 1),    // s_lshl_b64 s[24:25], s[22:23], s1
	    EncodeSop2(0x21, 26, 24, 0),    // s_lshr_b64 s[26:27], s[24:25], s0
	    EncodeSop2(0x25, 28, 132, 130), // s_bfm_b64 s[28:29], 4, 2
	    EncodeSop1(0x10, 30, 26),       // s_bcnt1_i32_b64 s30, s[26:27]
	    EncodeSop1(0x16, 106, 26),      // s_flbit_i32_b64 vcc_lo, s[26:27]
	    EncodeSop1(0x3b, 32, 30),       // s_bitreplicate_b64_b32 s[32:33], s30
	    EncodeSop2(0x29, 34, 32, 255),  // s_bfe_u64 s[34:35], s[32:33], 0x00040002
	    0x00040002u,
	    EncodeSop1(0x0a, 126, 126), // s_wqm_b64 exec, exec
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "s_mov_b64 s2, s0"),
	      "new decoder did not decode old-backed S_MOV_B64");
	Check(Common::ContainsStr(result.decoded_dump, "s_not_b32 s41, s0"),
	      "new decoder did not decode RDNA2 S_NOT_B32");
	Check(Common::ContainsStr(result.decoded_dump, "s_not_b64 s4, s2"),
	      "new decoder did not decode old-backed S_NOT_B64");
	Check(Common::ContainsStr(result.decoded_dump, "s_and_b64 s6, s2, s4"),
	      "new decoder did not decode old-backed S_AND_B64");
	Check(Common::ContainsStr(result.decoded_dump, "s_or_b64 s8, s6, s2"),
	      "new decoder did not decode old-backed S_OR_B64");
	Check(Common::ContainsStr(result.decoded_dump, "s_xor_b64 s10, s8, s4"),
	      "new decoder did not decode old-backed S_XOR_B64");
	Check(Common::ContainsStr(result.decoded_dump, "s_andn2_b32 s36, s0, s1"),
	      "new decoder did not decode RDNA2 S_ANDN2_B32");
	Check(Common::ContainsStr(result.decoded_dump, "s_orn2_b32 s37, s0, s1"),
	      "new decoder did not decode RDNA2 S_ORN2_B32");
	Check(Common::ContainsStr(result.decoded_dump, "s_nand_b32 s38, s0, s1"),
	      "new decoder did not decode RDNA2 S_NAND_B32");
	Check(Common::ContainsStr(result.decoded_dump, "s_nor_b32 s39, s0, s1"),
	      "new decoder did not decode RDNA2 S_NOR_B32");
	Check(Common::ContainsStr(result.decoded_dump, "s_xnor_b32 s40, s0, s1"),
	      "new decoder did not decode RDNA2 S_XNOR_B32");
	Check(Common::ContainsStr(result.decoded_dump, "s_cselect_b64 s12, s10, s2"),
	      "new decoder did not decode old-backed S_CSELECT_B64");
	Check(Common::ContainsStr(result.decoded_dump, "s_andn2_b64 s14, s10, s4"),
	      "new decoder did not decode old-backed S_ANDN2_B64");
	Check(Common::ContainsStr(result.decoded_dump, "s_orn2_b64 s16, s14, s6"),
	      "new decoder did not decode old-backed S_ORN2_B64");
	Check(Common::ContainsStr(result.decoded_dump, "s_nand_b64 s18, s16, s8"),
	      "new decoder did not decode old-backed S_NAND_B64");
	Check(Common::ContainsStr(result.decoded_dump, "s_nor_b64 s20, s18, s10"),
	      "new decoder did not decode old-backed S_NOR_B64");
	Check(Common::ContainsStr(result.decoded_dump, "s_xnor_b64 s22, s20, s12"),
	      "new decoder did not decode old-backed S_XNOR_B64");
	Check(Common::ContainsStr(result.decoded_dump, "s_lshl_b64 s24, s22, s1"),
	      "new decoder did not decode old-backed S_LSHL_B64");
	Check(Common::ContainsStr(result.decoded_dump, "s_lshr_b64 s26, s24, s0"),
	      "new decoder did not decode old-backed S_LSHR_B64");
	Check(Common::ContainsStr(result.decoded_dump, "s_bfm_b64 s28, 4, 2"),
	      "new decoder did not decode old-backed S_BFM_B64");
	Check(Common::ContainsStr(result.decoded_dump, "s_bcnt1_i32_b64 s30, s26"),
	      "new decoder did not decode old-backed S_BCNT1_I32_B64");
	Check(Common::ContainsStr(result.decoded_dump, "s_flbit_i32_b64 vcc_lo, s26"),
	      "new decoder did not decode RDNA2 S_FLBIT_I32_B64");
	Check(Common::ContainsStr(result.decoded_dump, "s_bitreplicate_b64_b32 s32, s30"),
	      "new decoder did not decode old-backed S_BITREPLICATE_B64_B32");
	Check(Common::ContainsStr(result.decoded_dump, "s_bfe_u64 s34, s32, 0x00040002"),
	      "new decoder did not decode old-backed S_BFE_U64");
	Check(Common::ContainsStr(result.decoded_dump, "s_wqm_b64 exec_lo, exec_lo"),
	      "new decoder did not decode old-backed S_WQM_B64");
	Check(Common::ContainsStr(result.ir_dump, "MoveU64 s2, s0"),
	      "S_MOV_B64 did not lower to paired-dword IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseNotU32 s41, s0"),
	      "S_NOT_B32 did not lower to scalar bitwise-not IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseNotU64 s4, s2"),
	      "S_NOT_B64 did not lower to paired-dword IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseAndU64 s6, s2, s4"),
	      "S_AND_B64 did not lower to paired-dword IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseOrU64 s8, s6, s2"),
	      "S_OR_B64 did not lower to paired-dword IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseXorU64 s10, s8, s4"),
	      "S_XOR_B64 did not lower to paired-dword IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseAndNotU32 s36, s0, s1"),
	      "S_ANDN2_B32 did not lower to scalar bitwise-and-not IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseOrNotU32 s37, s0, s1"),
	      "S_ORN2_B32 did not lower to scalar bitwise-or-not IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseNandU32 s38, s0, s1"),
	      "S_NAND_B32 did not lower to scalar bitwise-nand IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseNorU32 s39, s0, s1"),
	      "S_NOR_B32 did not lower to scalar bitwise-nor IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseXnorU32 s40, s0, s1"),
	      "S_XNOR_B32 did not lower to scalar bitwise-xnor IR");
	Check(Common::ContainsStr(result.ir_dump, "SelectU64 s12"),
	      "S_CSELECT_B64 did not lower to paired-dword select IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseAndNotU64 s14, s10, s4"),
	      "S_ANDN2_B64 did not lower to paired-dword IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseOrNotU64 s16, s14, s6"),
	      "S_ORN2_B64 did not lower to paired-dword IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseNandU64 s18, s16, s8"),
	      "S_NAND_B64 did not lower to paired-dword IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseNorU64 s20, s18, s10"),
	      "S_NOR_B64 did not lower to paired-dword IR");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseXnorU64 s22, s20, s12"),
	      "S_XNOR_B64 did not lower to paired-dword IR");
	Check(Common::ContainsStr(result.ir_dump, "ShiftLeftLogicalU64 s24, s22, s1"),
	      "S_LSHL_B64 did not lower to paired-dword IR");
	Check(Common::ContainsStr(result.ir_dump, "ShiftRightLogicalU64 s26, s24, s0"),
	      "S_LSHR_B64 did not lower to paired-dword IR");
	Check(Common::ContainsStr(result.ir_dump, "BitFieldMaskU64 s28"),
	      "S_BFM_B64 did not lower to paired-dword IR");
	Check(Common::ContainsStr(result.ir_dump, "BitCountU64 s30, s26"),
	      "S_BCNT1_I32_B64 did not lower to paired-source bit count IR");
	Check(Common::ContainsStr(result.ir_dump, "FindMsbFromHighU64 vcc_lo, s26"),
	      "S_FLBIT_I32_B64 did not lower to paired-source leading-zero IR");
	Check(Common::ContainsStr(result.ir_dump, "BitReplicateB64B32 s32, s30"),
	      "S_BITREPLICATE_B64_B32 did not lower to paired-dword IR");
	Check(Common::ContainsStr(result.ir_dump, "BitFieldExtractU64 s34, s32, 0x00040002"),
	      "S_BFE_U64 did not lower to paired-dword IR");
	Check(Common::ContainsStr(result.ir_dump, "WqmB64 exec_lo, exec_lo"),
	      "S_WQM_B64 did not lower to whole-quad-mask IR");
	Check(SpirvContainsOpcode(result.spirv, 61),
	      "SPIR-V binary does not contain OpLoad for scalar B64 ops");
	Check(SpirvContainsOpcode(result.spirv, 62),
	      "SPIR-V binary does not contain OpStore for scalar B64 ops");
	Check(SpirvContainsOpcode(result.spirv, 197),
	      "SPIR-V binary does not contain OpBitwiseOr for scalar B64 ops");
	Check(SpirvContainsOpcode(result.spirv, 198),
	      "SPIR-V binary does not contain OpBitwiseXor for scalar B64 ops");
	Check(SpirvContainsOpcode(result.spirv, 199),
	      "SPIR-V binary does not contain OpBitwiseAnd for scalar B64 ops");
	Check(SpirvContainsOpcode(result.spirv, 200),
	      "SPIR-V binary does not contain OpNot for scalar B64 ops");
	Check(SpirvContainsOpcode(result.spirv, 169),
	      "SPIR-V binary does not contain OpSelect for scalar B64 select");
	Check(SpirvContainsOpcode(result.spirv, 171),
	      "SPIR-V binary does not contain OpINotEqual for scalar B64 whole-quad mask");
	Check(SpirvContainsOpcode(result.spirv, 196),
	      "SPIR-V binary does not contain OpShiftLeftLogical for scalar B64 shifts");
	Check(SpirvContainsOpcode(result.spirv, 194),
	      "SPIR-V binary does not contain OpShiftRightLogical for scalar B64 shifts");
	Check(SpirvContainsOpcode(result.spirv, 201),
	      "SPIR-V binary does not contain OpBitFieldInsert for scalar B64 mask");
	Check(SpirvContainsOpcode(result.spirv, 203),
	      "SPIR-V binary does not contain OpBitFieldUExtract for scalar B64 extract");
	Check(SpirvContainsOpcode(result.spirv, 205),
	      "SPIR-V binary does not contain OpBitCount for scalar B64 bit count");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerSignedCompareAlu() {
	const uint32_t shader[] = {
	    EncodeSMovB32(0, 193),   // s0 = -1
	    EncodeSMovB32(1, 129),   // s1 = 1
	    EncodeSopc(0x02, 0, 1),  // s_cmp_gt_i32 s0, s1
	    EncodeSopk(0x07, 0, -2), // s_cmp_lt_i32 s0, -2
	    EncodeVopc(0x84, 0, 1),  // v_cmp_gt_i32 s0, v1
	    EncodeVopc(0x81, 0, 1),  // v_cmp_lt_i32 s0, v1
	    0x7d130ef9u,
	    0x86068413u, // v_cmp_lt_i16 vcc_lo.sdwa, v19.word0, v7.word1
	    0x7d1d02f9u,
	    0x86068213u,            // v_cmp_ge_i16 vcc_lo.sdwa, v19.word0, v1.word1
	    EncodeVopc(0xa9, 0, 1), // v_cmp_lt_u16 s0, v1
	    EncodeVopc(0x94, 0, 1), // v_cmpx_gt_i32 s0, v1
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "s_cmp_gt_i32"),
	      "new decoder did not decode SOPC signed compare");
	Check(Common::ContainsStr(result.decoded_dump, "s_cmp_lt_i32"),
	      "new decoder did not decode SOPK signed compare");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_gt_i32"),
	      "new decoder did not decode VOPC signed greater-than compare");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_lt_i32"),
	      "new decoder did not decode VOPC signed less-than compare");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_lt_i16"),
	      "new decoder did not decode VOPC signed halfword less-than compare");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_ge_i16"),
	      "new decoder did not decode VOPC signed halfword greater-or-equal compare");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_lt_u16"),
	      "new decoder did not decode VOPC unsigned halfword less-than compare");
	Check(Common::ContainsStr(result.decoded_dump, "v_cmpx_gt_i32"),
	      "new decoder did not decode VOPC signed compare-and-mask");
	Check(Common::ContainsStr(result.ir_dump, "CompareGtI32"),
	      "signed greater-than compare did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareLtI32"),
	      "signed less-than compare did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareLtI16"),
	      "signed halfword less-than compare did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareGeI16"),
	      "signed halfword greater-or-equal compare did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareLtU16"),
	      "unsigned halfword less-than compare did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareMaskGtI32 exec_lo"),
	      "signed compare-and-mask did not lower to exec mask IR");
	Check(SpirvContainsOpcode(result.spirv, 173), "SPIR-V binary does not contain OpSGreaterThan");
	Check(SpirvContainsOpcode(result.spirv, 177), "SPIR-V binary does not contain OpSLessThan");
	Check(SpirvContainsOpcode(result.spirv, 202),
	      "SPIR-V binary does not contain OpBitFieldSExtract for I16 compare");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerSignedMinShiftAlu() {
	const uint32_t shader[] = {
	    EncodeSMovB32(0, 193),           // s0 = -1
	    EncodeSMovB32(1, 130),           // s1 = 2
	    EncodeSop2(0x06, 2, 0, 1),       // s_min_i32 s2, s0, s1
	    EncodeSop2(0x08, 3, 0, 1),       // s_max_i32 s3, s0, s1
	    EncodeSop2(0x22, 4, 0, 129),     // s_ashr_i32 s4, s0, 1
	    EncodeVop2(0x11, 1, 0, 0),       // v_min_i32 v1, s0, v0
	    EncodeVop2(0x12, 2, 1, 1),       // v_max_i32 v2, s1, v1
	    EncodeVop2(0x17, 3, 1 + 256, 1), // v_ashr_i32 v3, v1, v1
	    EncodeVop2(0x18, 4, 129, 3),     // v_ashrrev_i32 v4, 1, v3
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "s_min_i32"),
	      "new decoder did not decode S_MIN_I32");
	Check(Common::ContainsStr(result.decoded_dump, "s_ashr_i32"),
	      "new decoder did not decode S_ASHR_I32");
	Check(Common::ContainsStr(result.decoded_dump, "v_min_i32"),
	      "new decoder did not decode V_MIN_I32");
	Check(Common::ContainsStr(result.decoded_dump, "v_ashrrev_i32"),
	      "new decoder did not decode V_ASHRREV_I32");
	Check(Common::ContainsStr(result.ir_dump, "IMinI32 s2"), "S_MIN_I32 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "IMaxI32 s3"), "S_MAX_I32 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "ShiftRightArithmeticI32 s4"),
	      "S_ASHR_I32 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "IMinI32 v1"), "V_MIN_I32 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "IMaxI32 v2"), "V_MAX_I32 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "ShiftRightArithmeticI32 v4, v3, 0x00000001"),
	      "V_ASHRREV_I32 did not reverse source order in IR");
	Check(SpirvContainsOpcode(result.spirv, 173), "SPIR-V binary does not contain OpSGreaterThan");
	Check(SpirvContainsOpcode(result.spirv, 177), "SPIR-V binary does not contain OpSLessThan");
	Check(SpirvContainsOpcode(result.spirv, 195),
	      "SPIR-V binary does not contain OpShiftRightArithmetic");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerScalarBitfieldAlu() {
	const uint32_t shader[] = {
	    EncodeSMovB32(0, 193),         // s0 = -1
	    EncodeSMovB32(1, 129),         // s1 = 1
	    EncodeSopc(0x02, 1, 0),        // s_cmp_gt_i32 s1, s0
	    EncodeSop2(0x0a, 2, 1, 0),     // s_cselect_b32 s2, s1, s0
	    EncodeSop1(0x34, 3, 0),        // s_abs_i32 s3, s0
	    EncodeSop1(0x0b, 4, 1),        // s_brev_b32 s4, s1
	    EncodeSop2(0x24, 5, 132, 129), // s_bfm_b32 s5, 4, 1
	    EncodeSop2(0x27, 6, 4, 255),   // s_bfe_u32 s6, s4, literal
	    0x00080004u,
	    EncodeSop2(0x32, 7, 0, 1), // s_pack_ll_b32_b16 s7, s0, s1
	    EncodeSop2(0x33, 8, 0, 1), // s_pack_lh_b32_b16 s8, s0, s1
	    EncodeSop2(0x34, 9, 0, 1), // s_pack_hh_b32_b16 s9, s0, s1
	    EncodeSopc(0x0c, 0, 1),    // s_bitcmp0_b32 s0, s1
	    EncodeSopc(0x0d, 1, 0),    // s_bitcmp1_b32 s1, s0
	    EncodeSopc(0x13, 0, 2),    // s_cmp_lg_u64 s[0:1], s[2:3]
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "s_cselect_b32"),
	      "new decoder did not decode S_CSELECT_B32");
	Check(Common::ContainsStr(result.decoded_dump, "s_abs_i32"),
	      "new decoder did not decode S_ABS_I32");
	Check(Common::ContainsStr(result.decoded_dump, "s_brev_b32"),
	      "new decoder did not decode S_BREV_B32");
	Check(Common::ContainsStr(result.decoded_dump, "s_bfm_b32"),
	      "new decoder did not decode S_BFM_B32");
	Check(Common::ContainsStr(result.decoded_dump, "s_bfe_u32"),
	      "new decoder did not decode S_BFE_U32");
	Check(Common::ContainsStr(result.decoded_dump, "s_pack_hh_b32_b16"),
	      "new decoder did not decode S_PACK_HH_B32_B16");
	Check(Common::ContainsStr(result.decoded_dump, "s_bitcmp0_b32"),
	      "new decoder did not decode S_BITCMP0_B32");
	Check(Common::ContainsStr(result.decoded_dump, "s_bitcmp1_b32"),
	      "new decoder did not decode S_BITCMP1_B32");
	Check(Common::ContainsStr(result.decoded_dump, "s_cmp_lg_u64"),
	      "new decoder did not decode S_CMP_LG_U64");
	Check(Common::ContainsStr(result.ir_dump, "SelectU32 s2"), "S_CSELECT_B32 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AbsI32 s3"), "S_ABS_I32 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "BitReverseU32 s4"),
	      "S_BREV_B32 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "BitFieldMaskU32 s5"),
	      "S_BFM_B32 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "BitFieldExtractU32 s6"),
	      "S_BFE_U32 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "PackLowLowU16 s7"),
	      "S_PACK_LL_B32_B16 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "PackLowHighU16 s8"),
	      "S_PACK_LH_B32_B16 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "PackHighHighU16 s9"),
	      "S_PACK_HH_B32_B16 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "BitCompare0B32"),
	      "S_BITCMP0_B32 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "BitCompare1B32"),
	      "S_BITCMP1_B32 did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "CompareNeU64"), "S_CMP_LG_U64 did not lower to IR");
	Check(SpirvContainsOpcode(result.spirv, 126), "SPIR-V binary does not contain OpSNegate");
	Check(SpirvContainsOpcode(result.spirv, 169), "SPIR-V binary does not contain OpSelect");
	Check(SpirvContainsOpcode(result.spirv, 201),
	      "SPIR-V binary does not contain OpBitFieldInsert");
	Check(SpirvContainsOpcode(result.spirv, 203),
	      "SPIR-V binary does not contain OpBitFieldUExtract");
	Check(SpirvContainsOpcode(result.spirv, 171), "SPIR-V binary does not contain OpINotEqual");
	Check(SpirvContainsOpcode(result.spirv, 166), "SPIR-V binary does not contain OpLogicalOr");
	Check(SpirvContainsOpcode(result.spirv, 204), "SPIR-V binary does not contain OpBitReverse");
	CheckSpirvBinaryValidates(result.spirv);
}

void CheckNewDecoderUnsupported(const uint32_t* shader, uint32_t words, const char* family,
                                const char* opcode_name) {
	ShaderRecompiler::Decoder::Program program;
	std::string                        error;
	const std::span                    code {shader, words};
	Check(ShaderRecompiler::Decoder::DecodeProgram(code, &program, &error), error.c_str());
	Check(program.instructions.size() >= 2, "decoder did not return instruction plus endpgm");
	const auto text = ShaderRecompiler::Decoder::ProgramToString(program);
	Check(Common::ContainsStr(text, family),
	      "decoder unsupported text did not include opcode family");
	Check(Common::ContainsStr(text, opcode_name),
	      "decoder unsupported text did not include opcode name");

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;
	ShaderRecompiler::CompileResult result;
	Check(!ShaderRecompiler::TryRecompile(code, options, &result, &error),
	      "unsupported opcode unexpectedly lowered without implementation");
	Check(Common::ContainsStr(error, "no IR lowering") ||
	          Common::ContainsStr(error, "unsupported decoded"),
	      "unsupported lowering error was not explicit");
}

void TestNewShaderRecompilerMemoryFamilyLowering() {
	const uint32_t shader[] = {
	    EncodeSmem0(0x00, 0, 4),
	    0u, // s_load_dword s0
	    EncodeSmem0(0x08, 1, 4),
	    0u, // s_buffer_load_dword s1
	    EncodeMubuf0(0x0c, 4),
	    EncodeMubuf1(0, 0, 1), // buffer_load_dword v0
	    EncodeMubuf0(0x1c, 8),
	    EncodeMubuf1(0, 0, 1), // buffer_store_dword v0
	    EncodeDs0(0x0d),
	    EncodeDs1(0, 0, 1), // ds_write_b32 v0, v1
	    EncodeDs0(0x36),
	    EncodeDs1(2, 0, 1), // ds_read_b32 v2, v1
	    EncodeMimg0(0x0e, 0x1),
	    EncodeMimg1(5, 0, 0, 1), // image_get_resinfo v5
	    EncodeMimg0(0x00, 0xf),
	    EncodeMimg1(4, 0, 0, 1), // image_load v4
	    EncodeMimg0(0x20, 0xf),
	    EncodeMimg1(3, 0, 0, 1), // image_sample v3
	    0xbf810000u,
	};

	auto                             user_data = ImageTestUserData();
	ShaderRecompiler::CompileOptions options;
	options.stage       = ShaderType::Compute;
	options.dump_ir     = true;
	options.user_data   = user_data.data();
	options.read_memory = ReadZeroTestMemory;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	const auto compiled = ShaderRecompiler::TryRecompile(shader, options, &result, &error);
	Check(compiled, error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "s_load_dword"),
	      "new decoder did not decode SMEM dword load");
	Check(Common::ContainsStr(result.decoded_dump, "s_buffer_load_dword"),
	      "new decoder did not decode SMEM scalar-buffer dword load");
	Check(Common::ContainsStr(result.decoded_dump, "s_buffer_load_dword s1, s8"),
	      "SMEM scalar-buffer SBASE was not decoded as an SGPR-pair index");
	Check(Common::ContainsStr(result.decoded_dump, "buffer_load_dword"),
	      "new decoder did not decode MUBUF dword load");
	Check(Common::ContainsStr(result.decoded_dump, "buffer_store_dword"),
	      "new decoder did not decode MUBUF dword store");
	Check(Common::ContainsStr(result.decoded_dump, "ds_read_b32"),
	      "new decoder did not decode DS dword read");
	Check(Common::ContainsStr(result.decoded_dump, "image_get_resinfo"),
	      "new decoder did not decode MIMG resinfo query");
	Check(Common::ContainsStr(result.decoded_dump, "image_load"),
	      "new decoder did not decode MIMG load");
	Check(Common::ContainsStr(result.decoded_dump, "image_sample"),
	      "new decoder did not decode MIMG sample");
	Check(!Common::ContainsStr(result.decoded_dump, "lowering is not implemented"),
	      "implemented memory decode still reports unsupported lowering");
	Check(Common::ContainsStr(result.ir_dump, "SLoadDword s0"), "SMEM load did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "SBufferLoadDword s1"),
	      "SMEM scalar-buffer load did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "BufferLoadDword v0"),
	      "MUBUF load did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "BufferStoreDword null, v0"),
	      "MUBUF store did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "DsWriteB32 null, v0"),
	      "DS write did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "DsReadB32 v2"), "DS read did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "ImageGetResinfo v5"),
	      "MIMG resinfo query did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "ImageLoad v4"), "MIMG load did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "ImageSample v3"), "MIMG sample did not lower to IR");
	Check(SpirvContainsOpcode(result.spirv, 65), "SPIR-V binary does not contain OpAccessChain");
	Check(SpirvContainsOpcode(result.spirv, 61), "SPIR-V binary does not contain OpLoad");
	Check(SpirvContainsOpcode(result.spirv, 62), "SPIR-V binary does not contain OpStore");
	Check(SpirvContainsOpcode(result.spirv, 95), "SPIR-V binary does not contain OpImageFetch");
	Check(SpirvContainsOpcode(result.spirv, 100), "SPIR-V binary does not contain OpImage");
	Check(SpirvContainsOpcode(result.spirv, 103),
	      "SPIR-V binary does not contain OpImageQuerySizeLod");
	Check(SpirvContainsOpcode(result.spirv, 88),
	      "SPIR-V binary does not contain OpImageSampleExplicitLod");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerImageQueryLowering() {
	const uint32_t shader[] = {
	    EncodeMimg0(0x60, 0x3),
	    EncodeMimg1(6, 0, 0, 1), // image_get_lod
	    0xbf810000u,
	};

	auto                             user_data = ImageTestUserData();
	ShaderRecompiler::CompileOptions options;
	options.stage     = ShaderType::Compute;
	options.dump_ir   = true;
	options.user_data = user_data.data();

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "image_get_lod"),
	      "new decoder did not decode MIMG image get-lod query");
	Check(Common::ContainsStr(result.decoded_dump, "dmask=0x3"),
	      "image_get_lod decode did not preserve dmask metadata");
	Check(Common::ContainsStr(result.ir_dump, "ImageGetLod v6"),
	      "image_get_lod did not lower to explicit query IR");
	Check(Common::ContainsStr(result.ir_dump, "data_dwords=2"),
	      "image_get_lod did not preserve two-component result metadata");
	Check(Common::ContainsStr(result.ir_dump, "image_addr=2"),
	      "image_get_lod did not preserve address component metadata");
	Check(SpirvContainsOpcode(result.spirv, 105), "SPIR-V binary does not contain OpImageQueryLod");
	Check(SpirvContainsOpcode(result.spirv, 80),
	      "SPIR-V binary does not contain coordinate composite for image_get_lod");
	Check(SpirvContainsOpcode(result.spirv, 81),
	      "SPIR-V binary does not contain dmask extraction for image_get_lod");
	Check(SpirvContainsOpcode(result.spirv, 124),
	      "SPIR-V binary does not contain result bitcast for image_get_lod");
	Check(std::find(result.spirv.begin(), result.spirv.end(), 5288u) != result.spirv.end(),
	      "SPIR-V binary does not request compute derivative group capability");
	Check(std::find(result.spirv.begin(), result.spirv.end(), 5289u) != result.spirv.end(),
	      "SPIR-V binary does not request compute derivative group execution mode");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerImageSampleVariants() {
	const uint32_t shader[] = {
	    EncodeMimg0(0x24, 0xf),
	    EncodeMimg1(8, 0, 0, 1), // image_sample_l
	    EncodeMimg0(0x25, 0xf),
	    EncodeMimg1(12, 0, 0, 1), // image_sample_b
	    EncodeMimg0(0x27, 0xf),
	    EncodeMimg1(16, 0, 0, 1), // image_sample_lz
	    EncodeMimg0(0x28, 0xf),
	    EncodeMimg1(20, 0, 0, 1), // image_sample_c
	    EncodeMimg0(0x30, 0xf),
	    EncodeMimg1(24, 0, 0, 1), // image_sample_o
	    EncodeMimg0(0x22, 0xf),
	    EncodeMimg1(28, 0, 0, 1), // image_sample_d
	    EncodeMimg0(0x2f, 0x1),
	    EncodeMimg1(32, 0, 0, 1), // image_sample_c_lz
	    EncodeMimg0(0x38, 0x1),
	    EncodeMimg1(36, 0, 0, 1), // image_sample_c_o
	    EncodeMimg0(0x2a, 0x1),
	    EncodeMimg1(40, 0, 0, 1), // image_sample_c_d
	    EncodeMimg0(0x20, 0x7),
	    EncodeMimg1(48, 0, 0, 1, true), // image_sample with A16 bit
	    0xbf810000u,
	};

	auto                             user_data = ImageTestUserData();
	ShaderRecompiler::CompileOptions options;
	options.stage     = ShaderType::Compute;
	options.dump_ir   = true;
	options.user_data = user_data.data();

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "image_sample_l"),
	      "new decoder did not decode IMAGE_SAMPLE_L through shared MIMG path");
	Check(Common::ContainsStr(result.decoded_dump, "image_sample_b"),
	      "new decoder did not decode IMAGE_SAMPLE_B through shared MIMG path");
	Check(Common::ContainsStr(result.decoded_dump, "image_sample_lz"),
	      "new decoder did not decode IMAGE_SAMPLE_LZ through shared MIMG path");
	Check(Common::ContainsStr(result.decoded_dump, "image_sample_c"),
	      "new decoder did not decode IMAGE_SAMPLE_C through shared MIMG path");
	Check(Common::ContainsStr(result.decoded_dump, "image_sample_o"),
	      "new decoder did not decode IMAGE_SAMPLE_O through shared MIMG path");
	Check(Common::ContainsStr(result.decoded_dump, "image_sample_d"),
	      "new decoder did not decode IMAGE_SAMPLE_D through shared MIMG path");
	Check(Common::ContainsStr(result.decoded_dump, "image_sample_c_lz"),
	      "new decoder did not decode IMAGE_SAMPLE_C_LZ through shared MIMG path");
	Check(Common::ContainsStr(result.decoded_dump, "image_sample_c_o"),
	      "new decoder did not decode IMAGE_SAMPLE_C_O through shared MIMG path");
	Check(Common::ContainsStr(result.decoded_dump, "image_sample_c_d"),
	      "new decoder did not decode IMAGE_SAMPLE_C_D through shared MIMG path");
	Check(Common::ContainsStr(result.decoded_dump, "sample_flags=a16"),
	      "IMAGE_SAMPLE with MIMG A16 bit did not expose A16 sample flag");
	Check(Common::ContainsStr(result.decoded_dump, "sample_flags=lod"),
	      "IMAGE_SAMPLE_L did not expose lod sample flag");
	Check(Common::ContainsStr(result.decoded_dump, "sample_flags=bias"),
	      "IMAGE_SAMPLE_B did not expose bias sample flag");
	Check(Common::ContainsStr(result.decoded_dump, "sample_flags=level_zero"),
	      "IMAGE_SAMPLE_LZ did not expose level-zero sample flag");
	Check(Common::ContainsStr(result.decoded_dump, "sample_flags=compare"),
	      "IMAGE_SAMPLE_C did not expose compare sample flag");
	Check(Common::ContainsStr(result.decoded_dump, "sample_flags=offset addr_components=3"),
	      "IMAGE_SAMPLE_O did not expose offset sample flag/address width");
	Check(Common::ContainsStr(result.decoded_dump, "sample_flags=derivative addr_components=6"),
	      "IMAGE_SAMPLE_D did not expose derivative sample flag/address width");
	Check(Common::ContainsStr(result.decoded_dump, "sample_flags=compare|level_zero"),
	      "IMAGE_SAMPLE_C_LZ did not expose compare+level-zero flags");
	Check(Common::ContainsStr(result.decoded_dump, "sample_flags=compare|offset addr_components=4"),
	      "IMAGE_SAMPLE_C_O did not expose compare+offset flags/address width");
	Check(Common::ContainsStr(result.decoded_dump,
	                          "sample_flags=derivative|compare addr_components=7"),
	      "IMAGE_SAMPLE_C_D did not expose compare+derivative address width");
	Check(Common::ContainsStr(result.ir_dump, "ImageSample v8"),
	      "IMAGE_SAMPLE_L did not lower to shared IR ImageSample");
	Check(Common::ContainsStr(result.ir_dump, "ImageSample v28"),
	      "IMAGE_SAMPLE_D did not lower to shared IR ImageSample");
	Check(Common::ContainsStr(result.ir_dump, "image_flags=0x4"),
	      "derivative sample flag did not survive into IR memory metadata");
	Check(Common::ContainsStr(result.ir_dump, "image_addr=6"),
	      "derivative sample address width did not survive into IR memory metadata");
	Check(Common::ContainsStr(result.ir_dump, "image_addr=7"),
	      "compare+derivative sample address width did not survive into IR memory metadata");
	Check(Common::ContainsStr(result.ir_dump, "image_flags=0x80"),
	      "MIMG A16 bit did not survive into IR memory metadata");
	Check(SpirvContainsOpcode(result.spirv, 88),
	      "SPIR-V binary does not contain shared OpImageSampleExplicitLod emission");
	Check(SpirvContainsOpcode(result.spirv, 90),
	      "SPIR-V binary does not contain shared OpImageSampleDrefExplicitLod emission");
	Check(SpirvContainsOpcode(result.spirv, 202),
	      "SPIR-V binary does not contain packed offset extraction");
	Check(SpirvContainsOpcode(result.spirv, 80),
	      "SPIR-V binary does not contain coordinate/gradient composite construction");
	Check(SpirvContainsExtInst(result.spirv, 62),
	      "SPIR-V binary does not unpack A16 image address halves");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerImageSampleA16SamplerCoords() {
	constexpr uint32_t MimgDim3D = 2;

	const uint32_t shader[] = {
	    EncodeMimg0(0x20, 0x7, false, MimgDim3D),
	    EncodeMimg1(8, 0, 0, 4, true), // image_sample, A16 bit, 3D xyz coords
	    0xbf810000u,
	};

	auto                             user_data = ImageTestUserData(Prospero::ImageType::kColor3D);
	ShaderRecompiler::CompileOptions options;
	options.stage     = ShaderType::Compute;
	options.dump_ir   = true;
	options.user_data = user_data.data();

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	const auto compiled = ShaderRecompiler::TryRecompile(shader, options, &result, &error);
	Check(compiled, error.c_str());
	Check(
	    Common::ContainsStr(result.decoded_dump, "image_dim=3d sample_flags=a16 addr_components=3"),
	    "3D IMAGE_SAMPLE with MIMG A16 bit did not decode as three A16 sampler coords");
	Check(Common::ContainsStr(result.ir_dump, "ImageSample v8"),
	      "3D A16 IMAGE_SAMPLE did not lower to sample IR");
	Check(Common::ContainsStr(result.ir_dump, "image_flags=0x80"),
	      "3D A16 IMAGE_SAMPLE did not preserve A16 memory metadata");
	Check(Common::ContainsStr(result.ir_dump, "image_dim=3d"),
	      "3D A16 IMAGE_SAMPLE did not preserve image dimension metadata");
	Check(Common::ContainsStr(result.ir_dump, "image_addr=3"),
	      "3D A16 IMAGE_SAMPLE did not preserve three logical address components");
	Check(SpirvExtInstCount(result.spirv, 62) == 3,
	      "sampler A16 xyz coordinates should be converted from three packed f16 components");
	Check(SpirvContainsOpcode(result.spirv, 194),
	      "sampler A16 high-half coordinate extraction is missing");
	Check(SpirvContainsOpcode(result.spirv, 199),
	      "sampler A16 low-half coordinate masking is missing");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerImageSampleOpcodeAliases() {
	const uint32_t shader[] = {
	    0xf0800109u,
	    0x00c00602u, // observed image_sample_a v6, v2, s0, s24
	    EncodeMimg0(0xa5, 0xf),
	    EncodeMimg1(12, 0, 0, 4), // image_sample_b_a
	    EncodeMimg0(0xa8, 0x1),
	    EncodeMimg1(16, 0, 0, 4), // image_sample_c_a
	    EncodeMimg0(0xad, 0x1),
	    EncodeMimg1(20, 0, 0, 4), // image_sample_c_b_a
	    0xbf810000u,
	};

	auto                             user_data = ImageTestUserData();
	ShaderRecompiler::CompileOptions options;
	options.stage     = ShaderType::Compute;
	options.dump_ir   = true;
	options.user_data = user_data.data();

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "image_sample_a"),
	      "MIMG opcode 0xa0 should decode as image_sample_a alias");
	Check(Common::ContainsStr(result.decoded_dump, "image_sample_b_a"),
	      "MIMG opcode 0xa5 should decode as image_sample_b_a alias");
	Check(Common::ContainsStr(result.decoded_dump, "image_sample_c_a"),
	      "MIMG opcode 0xa8 should decode as image_sample_c_a alias");
	Check(Common::ContainsStr(result.decoded_dump, "image_sample_c_b_a"),
	      "MIMG opcode 0xad should decode as image_sample_c_b_a alias");
	Check(Common::ContainsStr(result.decoded_dump, "sample_flags=none"),
	      "opcode 0xa0 alias should use normal 32-bit sample coordinates");
	Check(Common::ContainsStr(result.decoded_dump, "sample_flags=bias|compare"),
	      "compare+bias opcode alias did not expose expected sample flags");
	Check(!Common::ContainsStr(result.decoded_dump, "a16"),
	      "A16 must come from MIMG bit 62, not from the opcode alias");
	Check(!SpirvContainsExtInst(result.spirv, 62),
	      "opcode aliases without bit 62 must not unpack sampled f16 address halves");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerImageSampleA16ExceptionComponents() {
	auto compile = [](uint32_t opcode, uint32_t dmask, uint32_t vdata) {
		const uint32_t shader[] = {
		    EncodeMimg0(opcode, dmask),
		    EncodeMimg1(vdata, 0, 0, 4, true),
		    0xbf810000u,
		};

		auto                             user_data = ImageTestUserData();
		ShaderRecompiler::CompileOptions options;
		options.stage     = ShaderType::Compute;
		options.dump_ir   = true;
		options.user_data = user_data.data();

		ShaderRecompiler::CompileResult result;
		std::string                     error;
		Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
		CheckSpirvBinaryValidates(result.spirv);
		return result;
	};

	{
		const auto result = compile(0x28, 0x1, 12); // image_sample_c with A16 bit
		Check(
		    Common::ContainsStr(result.decoded_dump, "sample_flags=compare|a16 addr_components=3"),
		    "A16 IMAGE_SAMPLE_C did not preserve compare+A16 metadata");
		Check(Common::ContainsStr(result.ir_dump, "image_flags=0x88"),
		      "A16 IMAGE_SAMPLE_C did not preserve compare+A16 IR flags");
		Check(SpirvInstructionOpcodeCount(result.spirv, 90) == 1,
		      "A16 IMAGE_SAMPLE_C should emit one dref sample");
		Check(SpirvExtInstCount(result.spirv, 62) == 2,
		      "PCF reference must remain 32-bit while only xy sampler coords use A16");
	}

	{
		const auto result = compile(0x30, 0xf, 16); // image_sample_o with A16 bit
		Check(Common::ContainsStr(result.decoded_dump, "sample_flags=offset|a16 addr_components=3"),
		      "A16 IMAGE_SAMPLE_O did not preserve offset+A16 metadata");
		Check(Common::ContainsStr(result.ir_dump, "image_flags=0x90"),
		      "A16 IMAGE_SAMPLE_O did not preserve offset+A16 IR flags");
		Check(SpirvContainsOpcode(result.spirv, 202),
		      "texel offset should still be decoded from its packed 6-bit fields");
		Check(SpirvExtInstCount(result.spirv, 62) == 2,
		      "texel offset must remain 32-bit while only xy sampler coords use A16");
	}
}

void TestNewShaderRecompilerImageSampleA16Rdna2AddressOrder() {
	namespace Decoder = Libs::Graphics::ShaderRecompiler::Decoder;
	namespace Emitter = Libs::Graphics::ShaderRecompiler::Spirv::Emitter;
	namespace IR      = Libs::Graphics::ShaderRecompiler::IR;

	auto make_inst = [](uint32_t flags) {
		IR::Instruction inst {};
		inst.src[0]                    = Emitter::MakeRegisterOperand(IR::RegisterFile::Vector, 20);
		inst.memory.image_sample_flags = flags | Decoder::ImageSampleFlagA16;
		inst.memory.image_address_components = 5;
		return inst;
	};
	auto check_vgpr = [](const IR::Operand& operand, uint32_t expected, const char* message) {
		Check(operand.kind == IR::OperandKind::Register &&
		          operand.reg.file == IR::RegisterFile::Vector && operand.reg.index == expected,
		      message);
	};

	{
		auto       inst = make_inst(Decoder::ImageSampleFlagBias | Decoder::ImageSampleFlagCompare);
		const auto layout = Emitter::MakeImageSampleLayout(inst, Emitter::ImageViewKind::Dim2D);
		Check(layout.bias == 0u, "RDNA2 sample_c_b A16 bias must precede PCF reference");
		Check(layout.dref == 1u, "RDNA2 sample_c_b A16 PCF reference must follow bias");
		Check(layout.coord == 2u, "RDNA2 sample_c_b A16 body must follow bias and PCF");
		Check(Emitter::ImageAddressHalfComponent(inst, layout.bias) == 0u,
		      "A16 bias should use the first packed half");
		Check(Emitter::ImageAddressHalfComponent(inst, layout.dref) == 2u,
		      "32-bit PCF reference must align to the next VGPR after A16 bias");
		Check(Emitter::ImageAddressHalfComponent(inst, layout.coord) == 4u,
		      "A16 coordinates must start after the aligned PCF reference");
		check_vgpr(Emitter::ImageAddressOperand(inst, inst.src[0], layout.bias), 20,
		           "A16 sample_c_b bias should read v20");
		check_vgpr(Emitter::ImageAddressOperand(inst, inst.src[0], layout.dref), 21,
		           "A16 sample_c_b PCF reference should read v21");
		check_vgpr(Emitter::ImageAddressOperand(inst, inst.src[0], layout.coord), 22,
		           "A16 sample_c_b x/y coordinates should start in v22");
	}

	{
		auto       inst = make_inst(Decoder::ImageSampleFlagOffset | Decoder::ImageSampleFlagBias |
		                            Decoder::ImageSampleFlagCompare);
		const auto layout = Emitter::MakeImageSampleLayout(inst, Emitter::ImageViewKind::Dim2D);
		Check(layout.offset == 0u && layout.bias == 1u && layout.dref == 2u && layout.coord == 3u,
		      "RDNA2 sample_c_b_o order must be offset, bias, PCF, body");
		Check(Emitter::ImageAddressHalfComponent(inst, layout.offset) == 0u,
		      "texel offset should stay a packed 32-bit VGPR");
		Check(Emitter::ImageAddressHalfComponent(inst, layout.bias) == 2u,
		      "A16 bias should follow the 32-bit texel offset");
		Check(Emitter::ImageAddressHalfComponent(inst, layout.dref) == 4u,
		      "PCF reference should align after A16 bias");
		Check(Emitter::ImageAddressHalfComponent(inst, layout.coord) == 6u,
		      "A16 coordinates should start after offset, bias, and PCF");
		check_vgpr(Emitter::ImageAddressOperand(inst, inst.src[0], layout.offset), 20,
		           "A16 sample_c_b_o offset should read v20");
		check_vgpr(Emitter::ImageAddressOperand(inst, inst.src[0], layout.bias), 21,
		           "A16 sample_c_b_o bias should read v21");
		check_vgpr(Emitter::ImageAddressOperand(inst, inst.src[0], layout.dref), 22,
		           "A16 sample_c_b_o PCF reference should read v22");
		check_vgpr(Emitter::ImageAddressOperand(inst, inst.src[0], layout.coord), 23,
		           "A16 sample_c_b_o x/y coordinates should start in v23");
	}
}

void TestNewShaderRecompilerImageLoadA16UintCoords() {
	const uint32_t shader[] = {
	    EncodeMimg0(0x00, 0x3),
	    EncodeMimg1(20, 0, 0, 4, true), // image_load, A16 bit
	    0xbf810000u,
	};

	auto                             user_data = ImageTestUserData();
	ShaderRecompiler::CompileOptions options;
	options.stage     = ShaderType::Compute;
	options.dump_ir   = true;
	options.user_data = user_data.data();

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "image_load"), "A16 IMAGE_LOAD did not decode");
	Check(Common::ContainsStr(result.ir_dump, "ImageLoad v20"),
	      "A16 IMAGE_LOAD did not lower to image-load IR");
	Check(Common::ContainsStr(result.ir_dump, "image_flags=0x80"),
	      "A16 IMAGE_LOAD did not preserve A16 memory metadata");
	Check(Common::ContainsStr(result.ir_dump, "image_addr=2"),
	      "A16 IMAGE_LOAD did not preserve logical address component count");
	Check(!SpirvContainsExtInst(result.spirv, 62),
	      "image ops without sampler use u16 A16 addresses and must not unpack f16");
	Check(SpirvContainsOpcode(result.spirv, 194),
	      "u16 A16 high-half coordinate extraction is missing");
	Check(SpirvContainsOpcode(result.spirv, 199), "u16 A16 low-half coordinate masking is missing");
	Check(SpirvContainsOpcode(result.spirv, 95), "A16 IMAGE_LOAD did not emit an image fetch");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerPixelImageSampleLodSelection() {
	constexpr uint32_t OpImageSampleImplicitLod     = 87;
	constexpr uint32_t OpImageSampleExplicitLod     = 88;
	constexpr uint32_t OpImageSampleDrefImplicitLod = 89;
	constexpr uint32_t OpImageSampleDrefExplicitLod = 90;

	auto compile = [](uint32_t opcode, uint32_t dmask) {
		const uint32_t shader[] = {
		    EncodeMimg0(opcode, dmask),
		    EncodeMimg1(0, 0, 0, 4),
		    0xbf810000u,
		};

		auto ps_info = RegressionPixelInputInfo();

		auto                             user_data = ImageTestUserData();
		ShaderRecompiler::CompileOptions options;
		options.stage            = ShaderType::Pixel;
		options.pixel_input_info = &ps_info;
		options.dump_ir          = true;
		options.user_data        = user_data.data();

		ShaderRecompiler::CompileResult result;
		std::string                     error;
		Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
		CheckSpirvBinaryValidates(result.spirv);
		return result;
	};

	{
		const auto result = compile(0x20, 0xf); // image_sample
		Check(Common::ContainsStr(result.decoded_dump, "image_sample"),
		      "plain pixel IMAGE_SAMPLE did not decode");
		Check(SpirvInstructionOpcodeCount(result.spirv, OpImageSampleImplicitLod) == 1,
		      "plain pixel IMAGE_SAMPLE must use OpImageSampleImplicitLod");
		Check(SpirvInstructionOpcodeCount(result.spirv, OpImageSampleExplicitLod) == 0,
		      "plain pixel IMAGE_SAMPLE unexpectedly used explicit lod");
	}
	{
		const auto result = compile(0x25, 0xf); // image_sample_b
		Check(Common::ContainsStr(result.decoded_dump, "sample_flags=bias"),
		      "pixel IMAGE_SAMPLE_B did not preserve bias metadata");
		Check(SpirvInstructionOpcodeCount(result.spirv, OpImageSampleImplicitLod) == 1,
		      "pixel IMAGE_SAMPLE_B must use implicit lod with a bias operand");
		Check(SpirvInstructionOpcodeCount(result.spirv, OpImageSampleExplicitLod) == 0,
		      "pixel IMAGE_SAMPLE_B unexpectedly used explicit lod");
	}
	{
		const auto result = compile(0x27, 0xf); // image_sample_lz
		Check(SpirvInstructionOpcodeCount(result.spirv, OpImageSampleExplicitLod) == 1,
		      "pixel IMAGE_SAMPLE_LZ must use explicit lod");
	}
	{
		const auto result = compile(0x28, 0x1); // image_sample_c
		Check(SpirvInstructionOpcodeCount(result.spirv, OpImageSampleDrefImplicitLod) == 1,
		      "pixel IMAGE_SAMPLE_C must use OpImageSampleDrefImplicitLod");
		Check(SpirvInstructionOpcodeCount(result.spirv, OpImageSampleDrefExplicitLod) == 0,
		      "pixel IMAGE_SAMPLE_C unexpectedly used explicit dref lod");
	}
	{
		const auto result = compile(0x2f, 0x1); // image_sample_c_lz
		Check(SpirvInstructionOpcodeCount(result.spirv, OpImageSampleDrefExplicitLod) == 1,
		      "pixel IMAGE_SAMPLE_C_LZ must use explicit dref lod");
	}
}

void TestNewShaderRecompilerImageViewDimensions() {
	constexpr uint32_t MimgDim3D      = 2;
	constexpr uint32_t MimgDim2DArray = 5;
	constexpr uint32_t SpirvDim2D     = 1;
	constexpr uint32_t SpirvDim3D     = 2;

	const uint32_t shader[] = {
	    EncodeMimg0(0x20, 0xf, false, MimgDim2DArray),
	    EncodeMimg1(0, 0, 1, 0), // image_sample 2D array
	    EncodeMimg0(0x24, 0xf, false, MimgDim2DArray),
	    EncodeMimg1(4, 1, 1, 4), // image_sample_l 2D array
	    EncodeMimg0(0x20, 0xf, false, MimgDim3D),
	    EncodeMimg1(8, 2, 1, 8), // image_sample 3D
	    EncodeMimg0(0x01, 0xf, false, MimgDim2DArray),
	    EncodeMimg1(12, 3, 0, 12), // image_load_mip 2D array
	    0xbf810000u,
	};

	auto ps_info   = RegressionPixelInputInfo();
	auto user_data = ImageTestUserData();
	SetImageTestType(&user_data, 0, Prospero::ImageType::kColor2DArray);
	SetImageTestType(&user_data, 1, Prospero::ImageType::kColor2DArray);
	SetImageTestType(&user_data, 2, Prospero::ImageType::kColor3D);
	SetImageTestType(&user_data, 3, Prospero::ImageType::kColor2DArray);

	ShaderRecompiler::CompileOptions options;
	options.stage            = ShaderType::Pixel;
	options.pixel_input_info = &ps_info;
	options.dump_ir          = true;
	options.user_data        = user_data.data();

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	const auto compiled = ShaderRecompiler::TryRecompile(shader, options, &result, &error);
	Check(compiled, error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "image_dim=2d_array"),
	      "MIMG DIM did not decode 2D-array image view");
	Check(Common::ContainsStr(result.decoded_dump, "image_dim=3d"),
	      "MIMG DIM did not decode 3D image view");
	Check(Common::ContainsStr(result.decoded_dump, "image_sample_l"),
	      "2D-array LOD sample did not decode");
	Check(Common::ContainsStr(result.decoded_dump,
	                          "image_dim=2d_array sample_flags=lod addr_components=4"),
	      "2D-array LOD sample did not include slice plus LOD address components");
	Check(Common::ContainsStr(result.ir_dump, "image_dim=2d_array"),
	      "2D-array image view did not survive into IR metadata");
	Check(Common::ContainsStr(result.ir_dump, "image_dim=3d"),
	      "3D image view did not survive into IR metadata");
	Check(SpirvContainsTypeImage(result.spirv, SpirvDim2D, 1, 1),
	      "SPIR-V binary does not contain sampled 2D-array image type");
	Check(SpirvContainsTypeImage(result.spirv, SpirvDim3D, 0, 1),
	      "SPIR-V binary does not contain sampled 3D image type");
	Check(SpirvContainsOpcode(result.spirv, 95),
	      "SPIR-V binary does not contain array image fetch");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerImageGatherVariants() {
	const uint32_t shader[] = {
	    EncodeMimg0(0x47, 0x1),
	    EncodeMimg1(60, 0, 1, 4), // image_gather4_lz
	    EncodeMimg0(0x48, 0x2),
	    EncodeMimg1(64, 0, 1, 8), // image_gather4_c
	    EncodeMimg0(0x4f, 0x1),
	    EncodeMimg1(68, 0, 1, 12), // image_gather4_c_lz
	    EncodeMimg0(0x57, 0x4),
	    EncodeMimg1(72, 0, 1, 16), // image_gather4_lz_o
	    EncodeMimg0(0x58, 0x1),
	    EncodeMimg1(76, 0, 1, 20), // image_gather4_c_o
	    EncodeMimg0(0x5f, 0x1),
	    EncodeMimg1(80, 0, 1, 24), // image_gather4_c_lz_o
	    0xbf810000u,
	};

	auto                             user_data = ImageTestUserData();
	ShaderRecompiler::CompileOptions options;
	options.stage     = ShaderType::Compute;
	options.dump_ir   = true;
	options.user_data = user_data.data();

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "image_gather4_lz"),
	      "new decoder did not decode IMAGE_GATHER4_LZ");
	Check(Common::ContainsStr(result.decoded_dump, "image_gather4_lz_o"),
	      "new decoder did not decode IMAGE_GATHER4_LZ_O");
	Check(Common::ContainsStr(result.decoded_dump, "image_gather4_c"),
	      "new decoder did not decode IMAGE_GATHER4_C");
	Check(Common::ContainsStr(result.decoded_dump, "image_gather4_c_lz"),
	      "new decoder did not decode IMAGE_GATHER4_C_LZ");
	Check(Common::ContainsStr(result.decoded_dump, "image_gather4_c_o"),
	      "new decoder did not decode IMAGE_GATHER4_C_O");
	Check(Common::ContainsStr(result.decoded_dump, "image_gather4_c_lz_o"),
	      "new decoder did not decode IMAGE_GATHER4_C_LZ_O");
	Check(Common::ContainsStr(result.decoded_dump, "dmask=0x4"),
	      "IMAGE_GATHER4_LZ_O did not preserve gather component dmask");
	Check(Common::ContainsStr(result.decoded_dump,
	                          "sample_flags=offset|level_zero addr_components=3"),
	      "IMAGE_GATHER4_LZ_O did not expose shared offset sample metadata");
	Check(Common::ContainsStr(result.decoded_dump, "sample_flags=compare addr_components=3"),
	      "IMAGE_GATHER4_C did not expose compare sample metadata");
	Check(Common::ContainsStr(result.decoded_dump,
	                          "sample_flags=compare|level_zero addr_components=3"),
	      "IMAGE_GATHER4_C_LZ did not expose compare+level-zero sample metadata");
	Check(Common::ContainsStr(result.decoded_dump, "sample_flags=compare|offset addr_components=4"),
	      "IMAGE_GATHER4_C_O did not expose compare+offset sample metadata");
	Check(Common::ContainsStr(result.decoded_dump,
	                          "sample_flags=compare|offset|level_zero addr_components=4"),
	      "IMAGE_GATHER4_C_LZ_O did not expose compare+offset+level-zero sample metadata");
	Check(Common::ContainsStr(result.ir_dump, "ImageGather4 v60"),
	      "IMAGE_GATHER4_LZ did not lower to shared IR ImageGather4");
	Check(Common::ContainsStr(result.ir_dump, "ImageGather4 v64"),
	      "IMAGE_GATHER4_C did not lower to shared IR ImageGather4");
	Check(Common::ContainsStr(result.ir_dump, "ImageGather4 v68"),
	      "IMAGE_GATHER4_C_LZ did not lower to shared IR ImageGather4");
	Check(Common::ContainsStr(result.ir_dump, "ImageGather4 v72"),
	      "IMAGE_GATHER4_LZ_O did not lower to shared IR ImageGather4");
	Check(Common::ContainsStr(result.ir_dump, "ImageGather4 v76"),
	      "IMAGE_GATHER4_C_O did not lower to shared IR ImageGather4");
	Check(Common::ContainsStr(result.ir_dump, "ImageGather4 v80"),
	      "IMAGE_GATHER4_C_LZ_O did not lower to shared IR ImageGather4");
	Check(Common::ContainsStr(result.ir_dump, "data_dwords=4"),
	      "IMAGE_GATHER4 did not preserve four-component result metadata");
	Check(Common::ContainsStr(result.ir_dump, "image_flags=0x30"),
	      "IMAGE_GATHER4_LZ_O flags did not survive into IR memory metadata");
	Check(Common::ContainsStr(result.ir_dump, "image_flags=0x8"),
	      "IMAGE_GATHER4_C flags did not survive into IR memory metadata");
	Check(Common::ContainsStr(result.ir_dump, "image_flags=0x28"),
	      "IMAGE_GATHER4_C_LZ flags did not survive into IR memory metadata");
	Check(Common::ContainsStr(result.ir_dump, "image_flags=0x18"),
	      "IMAGE_GATHER4_C_O flags did not survive into IR memory metadata");
	Check(Common::ContainsStr(result.ir_dump, "image_flags=0x38"),
	      "IMAGE_GATHER4_C_LZ_O flags did not survive into IR memory metadata");
	Check(SpirvContainsCapability(result.spirv, 25),
	      "SPIR-V binary does not request ImageGatherExtended");
	Check(SpirvContainsOpcode(result.spirv, 96), "SPIR-V binary does not contain OpImageGather");
	Check(SpirvContainsOpcode(result.spirv, 97),
	      "SPIR-V binary does not contain OpImageDrefGather");
	Check(SpirvContainsOpcode(result.spirv, 202),
	      "SPIR-V binary does not contain packed gather offset extraction");
	Check(SpirvContainsOpcode(result.spirv, 81),
	      "SPIR-V binary does not contain gather result extraction");
	Check(SpirvContainsOpcode(result.spirv, 124),
	      "SPIR-V binary does not contain gather result bitcast");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerImageLoadVariants() {
	const uint32_t shader[] = {
	    EncodeMimg0(0x00, 0x3),
	    EncodeMimg1(8, 0, 0, 1), // image_load dmask xy
	    EncodeMimg0(0x01, 0xf),
	    EncodeMimg1(12, 1, 0, 4), // image_load_mip dmask xyzw
	    0xbf810000u,
	};

	auto                             user_data = ImageTestUserData();
	ShaderRecompiler::CompileOptions options;
	options.stage     = ShaderType::Compute;
	options.dump_ir   = true;
	options.user_data = user_data.data();

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "image_load"),
	      "new decoder did not decode MIMG image load");
	Check(Common::ContainsStr(result.decoded_dump, "image_load_mip"),
	      "new decoder did not decode MIMG image load mip");
	Check(Common::ContainsStr(result.decoded_dump, "dmask=0x3"),
	      "image_load decode did not preserve partial dmask");
	Check(Common::ContainsStr(result.decoded_dump, "dmask=0xf"),
	      "image_load_mip decode did not preserve full dmask");
	Check(Common::ContainsStr(result.ir_dump, "ImageLoad v8"),
	      "image_load did not lower through shared image-load IR");
	Check(Common::ContainsStr(result.ir_dump, "ImageLoad v12"),
	      "image_load_mip did not lower through shared image-load IR");
	Check(Common::ContainsStr(result.ir_dump, "data_dwords=2"),
	      "image_load dmask xy did not preserve two-component result metadata");
	Check(Common::ContainsStr(result.ir_dump, "data_dwords=4"),
	      "image_load_mip dmask xyzw did not preserve four-component result metadata");
	Check(Common::ContainsStr(result.ir_dump, "image_addr=3 image_mip=1"),
	      "image_load_mip did not preserve mip address component metadata");
	Check(SpirvContainsOpcode(result.spirv, 95), "SPIR-V binary does not contain OpImageFetch");
	Check(SpirvContainsOpcode(result.spirv, 81),
	      "SPIR-V binary does not contain OpCompositeExtract for image-load dmask");
	Check(SpirvContainsOpcode(result.spirv, 124),
	      "SPIR-V binary does not contain OpBitcast for image-load result bits");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerImageStoreLowering() {
	const uint32_t shader[] = {
	    EncodeMimg0(0x08, 0xf),
	    EncodeMimg1(20, 0, 0, 4), // image_store
	    EncodeMimg0(0x09, 0x3),
	    EncodeMimg1(24, 1, 0, 8), // image_store_mip
	    0xbf810000u,
	};

	auto                             user_data = ImageTestUserData();
	ShaderRecompiler::CompileOptions options;
	options.stage     = ShaderType::Compute;
	options.dump_ir   = true;
	options.user_data = user_data.data();

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "image_store"),
	      "new decoder did not decode MIMG image store");
	Check(Common::ContainsStr(result.decoded_dump, "image_store_mip"),
	      "new decoder did not decode MIMG image store mip");
	Check(Common::ContainsStr(result.decoded_dump, "dmask=0xf"),
	      "image store decode did not preserve full dmask");
	Check(Common::ContainsStr(result.decoded_dump, "dmask=0x3"),
	      "image store mip decode did not preserve partial dmask");
	Check(Common::ContainsStr(result.ir_dump, "ImageStore null, v20, v4"),
	      "image_store did not lower to shared image-store IR");
	Check(Common::ContainsStr(result.ir_dump, "ImageStore null, v24, v8"),
	      "image_store_mip did not lower to shared image-store IR");
	Check(Common::ContainsStr(result.ir_dump, "storage_image"),
	      "image store IR did not use storage-image resource metadata");
	Check(Common::ContainsStr(result.ir_dump, "data_dwords=4"),
	      "full-dmask image store did not preserve data component count");
	Check(Common::ContainsStr(result.ir_dump, "image_addr=3 image_mip=1"),
	      "mip image store did not preserve mip address component count");
	Check(SpirvContainsOpcode(result.spirv, 99), "SPIR-V binary does not contain OpImageWrite");
	Check(SpirvContainsOpcode(result.spirv, 80),
	      "SPIR-V binary does not contain image texel/coordinate composites");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerStorageImage3DDescriptorVariant() {
	constexpr uint32_t ImageTypeColor3D   = 10;
	constexpr uint32_t ImageFormatRgba16f = 71;
	constexpr uint32_t SpirvDim3D         = 2;

	constexpr uint32_t MimgDim3D = 2;

	const uint32_t shader[] = {
	    EncodeMimg0(0x08, 0xf, false, MimgDim3D),
	    EncodeMimg1(20, 5, 0, 4), // image_store 3D
	    0xbf810000u,
	};

	ShaderComputeInputInfo   input_info = RegressionComputeInputInfo();
	std::array<uint32_t, 64> user_data {};
	user_data[20] = 0x1000u;
	user_data[21] = ImageFormatRgba16f << 20u;
	user_data[22] = 255u | (255u << 14u);
	user_data[23] = ImageTypeColor3D << 28u;

	ShaderRecompiler::CompileOptions options;
	options.stage              = ShaderType::Compute;
	options.dump_ir            = true;
	options.compute_input_info = &input_info;
	options.user_data          = user_data.data();

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "image_dim=3d"),
	      "MIMG store did not decode the RDNA2 3D instruction dimension");
	const auto has_3d_store = std::any_of(
	    result.program.blocks.begin(), result.program.blocks.end(), [](const auto& block) {
		    return std::any_of(block.instructions.begin(), block.instructions.end(),
		                       [](const auto& inst) {
			                       return inst.op == ShaderRecompiler::IR::Opcode::ImageStore &&
			                              inst.memory.image_dimension ==
			                                  ShaderRecompiler::Decoder::ImageDimension::Dim3D &&
			                              inst.memory.image_address_components == 3;
		                       });
	    });
	Check(has_3d_store, "3D storage image store did not preserve three address components");
	Check(SpirvContainsTypeImage(result.spirv, SpirvDim3D, 0, 2),
	      "SPIR-V binary does not contain storage 3D image type");
	CheckSpirvBinaryValidates(result.spirv);

	const auto source = DisassembleSpirvBinary(result.spirv);
	Check(SpirvSourceHasInstructionUsing(source, "OpAccessChain", "textures2D_L_3D"),
	      "storage image store did not access the 3D storage descriptor binding");
}

void TestNewShaderRecompilerStorageImage2DDescriptorOverridesMimg3D() {
	constexpr uint32_t ImageTypeColor2D   = 9;
	constexpr uint32_t ImageFormatRgba16f = 71;
	constexpr uint32_t MimgDim3D          = 2;
	constexpr uint32_t SpirvDim2D         = 1;

	const uint32_t shader[] = {
	    EncodeMimg0(0x08, 0xf, false, MimgDim3D),
	    EncodeMimg1(20, 5, 0, 4),
	    0xbf810000u,
	};

	ShaderComputeInputInfo   input_info = RegressionComputeInputInfo();
	std::array<uint32_t, 64> user_data {};
	user_data[20] = 0x1000u;
	user_data[21] = ImageFormatRgba16f << 20u;
	user_data[22] = 255u | (255u << 14u);
	user_data[23] = ImageTypeColor2D << 28u;

	ShaderRecompiler::CompileOptions options;
	options.stage              = ShaderType::Compute;
	options.dump_ir            = true;
	options.compute_input_info = &input_info;
	options.user_data          = user_data.data();

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "image_dim=3d"),
	      "test MIMG store should decode as a 3D instruction");
	Check(SpirvContainsTypeImage(result.spirv, SpirvDim2D, 0, 2),
	      "SPIR-V binary does not contain storage 2D image type");
	CheckSpirvBinaryValidates(result.spirv);

	const auto source = DisassembleSpirvBinary(result.spirv);
	Check(SpirvSourceHasInstructionUsing(source, "OpAccessChain", "textures2D_L"),
	      "2D descriptor storage image store did not access the base storage binding");
	Check(!SpirvSourceHasInstructionUsing(source, "OpAccessChain", "textures2D_L_A"),
	      "2D descriptor storage image store unexpectedly used the array storage binding");
	Check(!SpirvSourceHasInstructionUsing(source, "OpAccessChain", "textures2D_L_3D"),
	      "2D descriptor storage image store unexpectedly used the 3D storage binding");
}

void TestNewShaderRecompilerImageAtomicLowering() {
	const uint32_t shader[] = {
	    EncodeMimg0(0x11, 0x1, true),
	    EncodeMimg1(52, 0, 0, 1), // image_atomic_add
	    EncodeMimg0(0x15, 0x1, true),
	    EncodeMimg1(53, 0, 0, 1), // image_atomic_umin
	    EncodeMimg0(0x17, 0x1, true),
	    EncodeMimg1(57, 0, 0, 1), // image_atomic_umax
	    EncodeMimg0(0x18, 0x1, true),
	    EncodeMimg1(54, 0, 0, 1), // image_atomic_and
	    EncodeMimg0(0x19, 0x1, true),
	    EncodeMimg1(55, 0, 0, 1), // image_atomic_or
	    EncodeMimg0(0x1a, 0x1, true),
	    EncodeMimg1(56, 0, 0, 1), // image_atomic_xor
	    0xbf810000u,
	};

	auto                             user_data = ImageTestUserData();
	ShaderRecompiler::CompileOptions options;
	options.stage     = ShaderType::Compute;
	options.dump_ir   = true;
	options.user_data = user_data.data();

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "image_atomic_add"),
	      "new decoder did not decode MIMG image atomic add");
	Check(Common::ContainsStr(result.decoded_dump, "image_atomic_umin"),
	      "new decoder did not decode MIMG image atomic umin");
	Check(Common::ContainsStr(result.decoded_dump, "image_atomic_umax"),
	      "new decoder did not decode MIMG image atomic umax");
	Check(Common::ContainsStr(result.decoded_dump, "image_atomic_and"),
	      "new decoder did not decode MIMG image atomic and");
	Check(Common::ContainsStr(result.decoded_dump, "image_atomic_or"),
	      "new decoder did not decode MIMG image atomic or");
	Check(Common::ContainsStr(result.decoded_dump, "image_atomic_xor"),
	      "new decoder did not decode MIMG image atomic xor");
	Check(Common::ContainsStr(result.decoded_dump, "dmask=0x1"),
	      "image atomic decode did not preserve dmask metadata");
	Check(Common::ContainsStr(result.ir_dump, "AtomicAddU32 v52, v52, v1"),
	      "image_atomic_add did not lower through shared atomic IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicUMinU32 v53, v53, v1"),
	      "image_atomic_umin did not lower through shared atomic IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicUMaxU32 v57, v57, v1"),
	      "image_atomic_umax did not lower through shared atomic IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicAndU32 v54, v54, v1"),
	      "image_atomic_and did not lower through shared atomic IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicOrU32 v55, v55, v1"),
	      "image_atomic_or did not lower through shared atomic IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicXorU32 v56, v56, v1"),
	      "image_atomic_xor did not lower through shared atomic IR");
	Check(Common::ContainsStr(result.ir_dump, "storage_image_uint"),
	      "image atomic IR did not use uint storage-image metadata");
	Check(SpirvContainsOpcode(result.spirv, 60),
	      "SPIR-V binary does not contain OpImageTexelPointer");
	Check(SpirvContainsOpcode(result.spirv, 234), "SPIR-V binary does not contain OpAtomicIAdd");
	Check(SpirvContainsOpcode(result.spirv, 237), "SPIR-V binary does not contain OpAtomicUMin");
	Check(SpirvContainsOpcode(result.spirv, 239), "SPIR-V binary does not contain OpAtomicUMax");
	Check(SpirvContainsOpcode(result.spirv, 240), "SPIR-V binary does not contain OpAtomicAnd");
	Check(SpirvContainsOpcode(result.spirv, 241), "SPIR-V binary does not contain OpAtomicOr");
	Check(SpirvContainsOpcode(result.spirv, 242), "SPIR-V binary does not contain OpAtomicXor");
	Check(SpirvContainsOpcode(result.spirv, 225),
	      "image atomic SPIR-V binary does not contain OpMemoryBarrier");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerVintrpLowering() {
	const uint32_t shader[] = {
	    EncodeVintrp(0, 10, 1, 2, 4), // v_interp_p1_f32 v10, v4, attr1.z
	    EncodeVintrp(1, 11, 1, 2, 4), // v_interp_p2_f32 v11, v4, attr1.z
	    EncodeVintrp(2, 12, 0, 3, 2), // v_interp_mov_f32 v12, p0, attr0.w
	    0xbf810000u,
	};

	ShaderPixelInputInfo ps_info {};
	ps_info.input_num = 2;
	SetIdentityInterpolatorSettings(&ps_info);

	ShaderRecompiler::CompileOptions options;
	options.stage            = ShaderType::Pixel;
	options.pixel_input_info = &ps_info;
	options.dump_ir          = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "v_interp_p1_f32"),
	      "new decoder did not decode VINTRP P1");
	Check(Common::ContainsStr(result.decoded_dump, "v_interp_p2_f32"),
	      "new decoder did not decode VINTRP P2");
	Check(Common::ContainsStr(result.decoded_dump, "v_interp_mov_f32"),
	      "new decoder did not decode VINTRP MOV");
	Check(Common::ContainsStr(result.ir_dump, "ControlNop"),
	      "VINTRP P1 did not lower to an explicit no-op marker");
	Check(Common::ContainsStr(result.ir_dump, "LoadInputF32 v11"),
	      "VINTRP P2 did not lower to input-load IR");
	Check(Common::ContainsStr(result.ir_dump, "input_attr=1 input_chan=2"),
	      "VINTRP P2 did not preserve attr/channel metadata");
	Check(Common::ContainsStr(result.ir_dump, "LoadInputF32 v12"),
	      "VINTRP MOV did not lower to input-load IR");
	Check(Common::ContainsStr(result.ir_dump, "input_attr=0 input_chan=3"),
	      "VINTRP MOV did not preserve attr/channel metadata");
	Check(ProgramInputCount(result.program, ShaderRecompiler::IR::StageInputKind::Parameter) == 2,
	      "VINTRP pixel shader did not reflect parameter inputs");
	Check(SpirvContainsOpcode(result.spirv, 61), "SPIR-V binary does not contain OpLoad");
	Check(SpirvContainsOpcode(result.spirv, 62), "SPIR-V binary does not contain OpStore");
	Check(SpirvContainsOpcode(result.spirv, 81),
	      "SPIR-V binary does not contain input component extraction");
	Check(SpirvContainsOpcode(result.spirv, 124), "SPIR-V binary does not contain input bitcast");
	CheckSpirvBinaryValidates(result.spirv);

	const uint32_t remapped_shader[] = {
	    EncodeVintrp(1, 11, 2, 0, 4), // v_interp_p2_f32 v11, v4, attr2.x
	    0xbf810000u,
	};
	ShaderPixelInputInfo remapped_ps_info {};
	remapped_ps_info.input_num = 3;
	SetIdentityInterpolatorSettings(&remapped_ps_info);
	remapped_ps_info.interpolator_settings[2] = 3;

	options.pixel_input_info = &remapped_ps_info;

	ShaderRecompiler::CompileResult remapped_result;
	Check(ShaderRecompiler::TryRecompile(remapped_shader, options, &remapped_result, &error),
	      error.c_str());
	Check(Common::ContainsStr(remapped_result.ir_dump, "input_attr=2 input_chan=0"),
	      "remapped VINTRP did not preserve raw pixel attribute metadata");
	Check(SpirvHasDecorationValue(remapped_result.spirv, 30u, 3u),
	      "remapped VINTRP did not use SPI_PS_INPUT_CNTL input offset as Location");
	Check(!SpirvHasDecorationValue(remapped_result.spirv, 30u, 2u),
	      "remapped VINTRP still emitted the raw attribute as Location");
	CheckSpirvBinaryValidates(remapped_result.spirv);

	const uint32_t duplicate_location_shader[] = {
	    EncodeVintrp(0, 6, 1, 0, 4),  // v_interp_p1_f32 v6, v4, attr1.x
	    EncodeVintrp(0, 10, 0, 0, 4), // v_interp_p1_f32 v10, v4, attr0.x
	    EncodeVintrp(1, 6, 1, 0, 4),  // v_interp_p2_f32 v6, v4, attr1.x
	    EncodeVintrp(1, 10, 0, 0, 4), // v_interp_p2_f32 v10, v4, attr0.x
	    0xbf810000u,
	};
	ShaderPixelInputInfo duplicate_ps_info {};
	duplicate_ps_info.input_num = 2;

	options.pixel_input_info = &duplicate_ps_info;

	ShaderRecompiler::CompileResult duplicate_result;
	Check(ShaderRecompiler::TryRecompile(duplicate_location_shader, options, &duplicate_result,
	                                     &error),
	      error.c_str());
	Check(ProgramInputCount(duplicate_result.program,
	                        ShaderRecompiler::IR::StageInputKind::Parameter) == 2,
	      "duplicate-location VINTRP shader did not reflect both raw parameter attrs");
	Check(SpirvDecorationValueCount(duplicate_result.spirv, 30u, 0u) == 1,
	      "duplicate-location VINTRP emitted more than one Location 0 input");
	Check(SpirvDecorationValueCount(duplicate_result.spirv, 30u, 1u) == 1,
	      "duplicate-location VINTRP did not fall back attr1 to Location 1");
	CheckSpirvBinaryValidates(duplicate_result.spirv);

	const uint32_t flat_shader[] = {
	    EncodeVintrp(2, 12, 0, 0, 2), // v_interp_mov_f32 v12, p0, attr0.x
	    0xbf810000u,
	};
	ShaderPixelInputInfo flat_ps_info {};
	flat_ps_info.input_num = 1;
	SetIdentityInterpolatorSettings(&flat_ps_info);
	flat_ps_info.interpolator_settings[0] = 0x00000400u;

	options.pixel_input_info = &flat_ps_info;

	ShaderRecompiler::CompileResult flat_result;
	Check(ShaderRecompiler::TryRecompile(flat_shader, options, &flat_result, &error),
	      error.c_str());
	Check(SpirvHasDecorationValueWithDecoration(flat_result.spirv, 30u, 0u, 14u),
	      "flat VINTRP input did not emit a Flat decoration");
	Check(!SpirvHasDecorationValueWithDecoration(flat_result.spirv, 30u, 0u, 13u),
	      "flat VINTRP input should not also emit NoPerspective");
	Check(SpirvHasDecorationValue(flat_result.spirv, 30u, 0u),
	      "flat VINTRP input did not preserve Location 0");
	CheckSpirvBinaryValidates(flat_result.spirv);

	ShaderPixelInputInfo no_perspective_ps_info {};
	no_perspective_ps_info.input_num         = 1;
	no_perspective_ps_info.ps_no_perspective = true;
	SetIdentityInterpolatorSettings(&no_perspective_ps_info);

	options.pixel_input_info = &no_perspective_ps_info;

	ShaderRecompiler::CompileResult no_perspective_result;
	Check(ShaderRecompiler::TryRecompile(flat_shader, options, &no_perspective_result, &error),
	      error.c_str());
	Check(SpirvHasDecorationValueWithDecoration(no_perspective_result.spirv, 30u, 0u, 13u),
	      "no-perspective VINTRP input did not emit a NoPerspective decoration");
	Check(!SpirvHasDecorationValueWithDecoration(no_perspective_result.spirv, 30u, 0u, 14u),
	      "non-flat no-perspective VINTRP input should not emit Flat");
	CheckSpirvBinaryValidates(no_perspective_result.spirv);
}

void TestGraphicsCreateInterpolantMapping() {
	ShaderRegister regs[32] = {};

	Check(Gen5::GraphicsCreateInterpolantMapping(regs, nullptr, nullptr) == 0,
	      "null pixel shader interpolant mapping failed");
	for (uint32_t i = 0; i < 32u; i++) {
		Check(regs[i].offset == Pm4::SPI_PS_INPUT_CNTL_0 + i,
		      "identity interpolant register offset was unexpected");
		Check(regs[i].value == i, "identity interpolant register value was unexpected");
	}

	ShaderSemantic gs_semantics[3]   = {};
	gs_semantics[0].semantic         = 7;
	gs_semantics[0].hardware_mapping = 5;
	gs_semantics[1].semantic         = 9;
	gs_semantics[1].hardware_mapping = 12;
	gs_semantics[2].semantic         = 10;
	gs_semantics[2].hardware_mapping = 4;
	gs_semantics[2].is_f16           = 1;

	Shader gs {};
	gs.output_semantics     = gs_semantics;
	gs.num_output_semantics = static_cast<uint16_t>(std::size(gs_semantics));

	ShaderSemantic ps_semantics[4]   = {};
	ps_semantics[0].semantic         = 7;
	ps_semantics[1].semantic         = 8;
	ps_semantics[1].default_value    = 2;
	ps_semantics[2].semantic         = 9;
	ps_semantics[2].is_flat_shaded   = 1;
	ps_semantics[2].is_custom        = 1;
	ps_semantics[2].default_value    = 1;
	ps_semantics[3].semantic         = 10;
	ps_semantics[3].is_f16           = 1;
	ps_semantics[3].default_value    = 3;
	ps_semantics[3].default_value_hi = 2;

	Shader ps {};
	ps.input_semantics     = ps_semantics;
	ps.num_input_semantics = static_cast<uint32_t>(std::size(ps_semantics));

	Check(Gen5::GraphicsCreateInterpolantMapping(regs, &gs, &ps) == 0,
	      "shader interpolant mapping failed");
	Check(regs[0].value == 0x00000005u,
	      "matched interpolant mapping did not use GS hardware mapping");
	Check(regs[1].value == 0x00000220u, "missing interpolant mapping did not use PS default value");
	Check(regs[2].value == 0x0000052cu, "flat/custom interpolant mapping bits were unexpected");
	Check(regs[3].value == 0x01580304u, "f16 interpolant mapping bits were unexpected");
	Check(regs[4].offset == Pm4::SPI_PS_INPUT_CNTL_0 + 4u && regs[4].value == 4u,
	      "interpolant identity tail was not filled");
}

void TestNewShaderRecompilerWideMemoryLowering() {
	const uint32_t shader[] = {
	    EncodeSmem0(0x02, 4, 4),
	    0u, // s_load_dwordx4 s[4:7]
	    EncodeSmem0(0x0a, 8, 4),
	    0u, // s_buffer_load_dwordx4 s[8:11]
	    EncodeSmem0(0x09, 106, 4),
	    0u, // s_buffer_load_dwordx2 vcc_lo
	    EncodeMubuf0(0x0d, 16),
	    EncodeMubuf1(20, 0, 1), // buffer_load_dwordx2 v[20:21]
	    EncodeMubuf0(0x0f, 32),
	    EncodeMubuf1(24, 0, 1), // buffer_load_dwordx3 v[24:26]
	    EncodeMubuf0(0x0e, 48),
	    EncodeMubuf1(28, 0, 1), // buffer_load_dwordx4 v[28:31]
	    EncodeMubuf0(0x1d, 64),
	    EncodeMubuf1(32, 0, 1), // buffer_store_dwordx2 v[32:33]
	    EncodeMubuf0(0x1f, 80),
	    EncodeMubuf1(36, 0, 1), // buffer_store_dwordx3 v[36:38]
	    EncodeMubuf0(0x1e, 96),
	    EncodeMubuf1(40, 0, 1), // buffer_store_dwordx4 v[40:43]
	    EncodeMubuf0(0x08, 2),
	    EncodeMubuf1(44, 0, 1), // buffer_load_ubyte v44
	    EncodeMubuf0(0x0a, 4),
	    EncodeMubuf1(45, 0, 1), // buffer_load_ushort v45
	    EncodeFlat0(0x0c, 0, 4),
	    EncodeFlat1(50, 0x7d, 0, 1), // flat_load_dword
	    EncodeFlat0(0x0d, 1, 8),
	    EncodeFlat1(52, 0x7d, 0, 1), // scratch_load_dwordx2
	    EncodeFlat0(0x0e, 2, 12),
	    EncodeFlat1(56, 0x7d, 0, 1), // global_load_dwordx4
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage            = ShaderType::Compute;
	options.dump_ir          = true;
	options.flat_memory_base = 0;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "s_load_dwordx4"),
	      "new decoder did not decode SMEM x4 load");
	Check(Common::ContainsStr(result.decoded_dump, "s_buffer_load_dwordx4"),
	      "new decoder did not decode scalar-buffer x4 load");
	Check(Common::ContainsStr(result.decoded_dump, "s_buffer_load_dwordx2 vcc_lo"),
	      "new decoder did not decode scalar-buffer x2 load into VCC");
	Check(Common::ContainsStr(result.decoded_dump, "buffer_load_dwordx2"),
	      "new decoder did not decode buffer x2 load");
	Check(Common::ContainsStr(result.decoded_dump, "buffer_load_dwordx3"),
	      "new decoder did not decode buffer x3 load");
	Check(Common::ContainsStr(result.decoded_dump, "buffer_load_dwordx4"),
	      "new decoder did not decode buffer x4 load");
	Check(Common::ContainsStr(result.decoded_dump, "buffer_store_dwordx4"),
	      "new decoder did not decode buffer x4 store");
	Check(Common::ContainsStr(result.decoded_dump, "buffer_load_ubyte"),
	      "new decoder did not decode buffer ubyte load");
	Check(Common::ContainsStr(result.decoded_dump, "buffer_load_ushort"),
	      "new decoder did not decode buffer ushort load");
	Check(Common::ContainsStr(result.decoded_dump, "flat_load_dword"),
	      "new decoder did not decode flat dword load");
	Check(Common::ContainsStr(result.decoded_dump, "segment=1"),
	      "new decoder did not preserve scratch segment metadata");
	Check(Common::ContainsStr(result.decoded_dump, "segment=2"),
	      "new decoder did not preserve global segment metadata");
	Check(Common::ContainsStr(result.decoded_dump, "dwords=4 bits=32"),
	      "new decoder did not expose width metadata for wide memory ops");
	Check(Common::ContainsStr(result.ir_dump, "SLoadDword s4"),
	      "s_load_dwordx4 did not expand to first scalar dword IR load");
	Check(Common::ContainsStr(result.ir_dump, "SLoadDword s7"),
	      "s_load_dwordx4 did not expand to last scalar dword IR load");
	Check(Common::ContainsStr(result.ir_dump, "SBufferLoadDword s8"),
	      "s_buffer_load_dwordx4 did not expand to first scalar-buffer IR load");
	Check(Common::ContainsStr(result.ir_dump, "SBufferLoadDword s11"),
	      "s_buffer_load_dwordx4 did not expand to last scalar-buffer IR load");
	Check(Common::ContainsStr(result.ir_dump, "SBufferLoadDword vcc_lo"),
	      "s_buffer_load_dwordx2 did not expand to VCC low dword");
	Check(Common::ContainsStr(result.ir_dump, "SBufferLoadDword vcc_hi"),
	      "s_buffer_load_dwordx2 did not expand to VCC high dword");
	Check(Common::ContainsStr(result.ir_dump, "BufferLoadDword v20"),
	      "buffer_load_dwordx2 did not expand to dword IR loads");
	Check(Common::ContainsStr(result.ir_dump, "BufferLoadDword v31"),
	      "buffer_load_dwordx4 did not expand to the last dword IR load");
	Check(Common::ContainsStr(result.ir_dump, "BufferStoreDword null, v40"),
	      "buffer_store_dwordx4 did not expand to first dword IR store");
	Check(Common::ContainsStr(result.ir_dump, "BufferStoreDword null, v43"),
	      "buffer_store_dwordx4 did not expand to last dword IR store");
	Check(Common::ContainsStr(result.ir_dump, "BufferLoadUbyte v44"),
	      "buffer_load_ubyte did not lower to sub-dword IR load");
	Check(Common::ContainsStr(result.ir_dump, "BufferLoadUshort v45"),
	      "buffer_load_ushort did not lower to sub-dword IR load");
	Check(Common::ContainsStr(result.ir_dump, "FlatLoadDword v50"),
	      "flat_load_dword did not lower to flat dword IR load");
	Check(Common::ContainsStr(result.ir_dump, "scratch"),
	      "scratch segment load did not lower with scratch metadata");
	Check(Common::ContainsStr(result.ir_dump, "global"),
	      "global segment load did not lower with global metadata");
	Check(SpirvContainsOpcode(result.spirv, 65), "SPIR-V binary does not contain OpAccessChain");
	Check(SpirvContainsOpcode(result.spirv, 61), "SPIR-V binary does not contain OpLoad");
	Check(SpirvContainsOpcode(result.spirv, 62), "SPIR-V binary does not contain OpStore");
	Check(SpirvContainsOpcode(result.spirv, 194),
	      "SPIR-V binary does not contain OpShiftRightLogical");
	Check(SpirvContainsOpcode(result.spirv, 196),
	      "SPIR-V binary does not contain OpShiftLeftLogical");
	Check(SpirvContainsOpcode(result.spirv, 199), "SPIR-V binary does not contain OpBitwiseAnd");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerBufferSignedLoadLowering() {
	const uint32_t shader[] = {
	    EncodeMubuf0(0x09, 3), EncodeMubuf1(46, 0, 1), // buffer_load_sbyte
	    EncodeMubuf0(0x0b, 6), EncodeMubuf1(47, 0, 1), // buffer_load_sshort
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "buffer_load_sbyte"),
	      "new decoder did not decode buffer signed byte load");
	Check(Common::ContainsStr(result.decoded_dump, "buffer_load_sshort"),
	      "new decoder did not decode buffer signed short load");
	Check(Common::ContainsStr(result.decoded_dump, "bits=8"),
	      "signed byte buffer load did not preserve bit width metadata");
	Check(Common::ContainsStr(result.decoded_dump, "bits=16"),
	      "signed short buffer load did not preserve bit width metadata");
	Check(Common::ContainsStr(result.decoded_dump, "signed=1"),
	      "signed buffer loads did not preserve signed metadata");
	Check(Common::ContainsStr(result.ir_dump, "BufferLoadSbyte v46"),
	      "buffer_load_sbyte did not lower to signed byte IR load");
	Check(Common::ContainsStr(result.ir_dump, "BufferLoadSshort v47"),
	      "buffer_load_sshort did not lower to signed short IR load");
	Check(SpirvContainsOpcode(result.spirv, 61), "SPIR-V binary does not contain OpLoad");
	Check(SpirvContainsOpcode(result.spirv, 195),
	      "SPIR-V binary does not contain OpShiftRightArithmetic for sign extension");
	Check(SpirvContainsOpcode(result.spirv, 196),
	      "SPIR-V binary does not contain OpShiftLeftLogical for sign extension");
	Check(SpirvContainsOpcode(result.spirv, 199), "SPIR-V binary does not contain OpBitwiseAnd");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerBufferSubDwordStoreLowering() {
	const uint32_t shader[] = {
	    EncodeMubuf0(0x18, 2), EncodeMubuf1(48, 0, 1), // buffer_store_byte
	    EncodeMubuf0(0x1a, 4), EncodeMubuf1(49, 0, 1), // buffer_store_short
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "buffer_store_byte"),
	      "new decoder did not decode buffer byte store");
	Check(Common::ContainsStr(result.decoded_dump, "buffer_store_short"),
	      "new decoder did not decode buffer short store");
	Check(Common::ContainsStr(result.decoded_dump, "bits=8"),
	      "buffer byte store did not preserve bit width metadata");
	Check(Common::ContainsStr(result.decoded_dump, "bits=16"),
	      "buffer short store did not preserve bit width metadata");
	Check(Common::ContainsStr(result.ir_dump, "BufferStoreByte null, v48"),
	      "buffer_store_byte did not lower to byte store IR");
	Check(Common::ContainsStr(result.ir_dump, "BufferStoreShort null, v49"),
	      "buffer_store_short did not lower to short store IR");
	Check(SpirvContainsOpcode(result.spirv, 61),
	      "SPIR-V binary does not contain OpLoad for sub-dword store RMW");
	Check(SpirvContainsOpcode(result.spirv, 62),
	      "SPIR-V binary does not contain OpStore for sub-dword store RMW");
	Check(SpirvContainsOpcode(result.spirv, 197), "SPIR-V binary does not contain OpBitwiseOr");
	Check(SpirvContainsOpcode(result.spirv, 199), "SPIR-V binary does not contain OpBitwiseAnd");
	Check(SpirvContainsOpcode(result.spirv, 200), "SPIR-V binary does not contain OpNot");
	Check(SpirvContainsOpcode(result.spirv, 196),
	      "SPIR-V binary does not contain OpShiftLeftLogical");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerMubufFormatLowering() {
	const uint32_t shader[] = {
	    EncodeMubuf0(0x00, 4),
	    EncodeMubuf1(84, 0, 1), // buffer_load_format_x
	    EncodeMubuf0(0x01, 8),
	    EncodeMubuf1(88, 0, 1), // buffer_load_format_xy
	    EncodeMubuf0(0x02, 12),
	    EncodeMubuf1(92, 0, 1), // buffer_load_format_xyz
	    EncodeMubuf0(0x03, 16),
	    EncodeMubuf1(96, 0, 1), // buffer_load_format_xyzw
	    EncodeMubuf0(0x04, 20),
	    EncodeMubuf1(100, 0, 1), // buffer_store_format_x
	    EncodeMubuf0(0x05, 24),
	    EncodeMubuf1(104, 0, 1), // buffer_store_format_xy
	    EncodeMubuf0(0x06, 28),
	    EncodeMubuf1(108, 0, 1), // buffer_store_format_xyz
	    EncodeMubuf0(0x07, 32),
	    EncodeMubuf1(112, 0, 1), // buffer_store_format_xyzw
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "buffer_load_format_x"),
	      "new decoder did not decode MUBUF format-x load");
	Check(Common::ContainsStr(result.decoded_dump, "buffer_load_format_xyzw"),
	      "new decoder did not decode MUBUF format-xyzw load");
	Check(Common::ContainsStr(result.decoded_dump, "buffer_store_format_x"),
	      "new decoder did not decode MUBUF format-x store");
	Check(Common::ContainsStr(result.decoded_dump, "buffer_store_format_xyzw"),
	      "new decoder did not decode MUBUF format-xyzw store");
	Check(Common::ContainsStr(result.decoded_dump, "typed=0 formatted=1"),
	      "MUBUF format decode did not preserve formatted non-typed metadata");
	Check(Common::ContainsStr(result.ir_dump, "BufferLoadDword v84"),
	      "MUBUF format-x load did not lower through shared buffer load IR");
	Check(Common::ContainsStr(result.ir_dump, "BufferLoadDword v99"),
	      "MUBUF format-xyzw load did not expand to the last shared dword load");
	Check(Common::ContainsStr(result.ir_dump, "BufferStoreDword null, v100"),
	      "MUBUF format-x store did not lower through shared buffer store IR");
	Check(Common::ContainsStr(result.ir_dump, "BufferStoreDword null, v115"),
	      "MUBUF format-xyzw store did not expand to the last shared dword store");
	Check(Common::ContainsStr(result.ir_dump, "typed=0 formatted=1"),
	      "MUBUF format metadata did not survive into IR");
	Check(SpirvContainsOpcode(result.spirv, 65), "SPIR-V binary does not contain OpAccessChain");
	Check(SpirvContainsOpcode(result.spirv, 61), "SPIR-V binary does not contain OpLoad");
	Check(SpirvContainsOpcode(result.spirv, 62), "SPIR-V binary does not contain OpStore");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerFormattedStoreUsesRuntimeArrayLengthOnly() {
	const uint32_t shader[] = {
	    EncodeMubuf0(0x04),
	    EncodeMubuf1(0, 0, 1), // buffer_store_format_x
	    0xbf810000u,
	};

	std::array<uint32_t, 64> user_data {};
	user_data[1] = 4u << 16u;
	user_data[2] = 5u;
	user_data[3] = Prospero::GpuEnumValue(Prospero::BufferFormat::k32UInt) << 12u;

	ShaderRecompiler::CompileOptions options;
	options.stage     = ShaderType::Compute;
	options.dump_ir   = true;
	options.user_data = user_data.data();

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(result.resources.buffers.size() == 1 && result.resources.buffers[0].dwords[2] == 5u,
	      "formatted store test did not preserve descriptor NumRecords");
	Check(Common::ContainsStr(result.decoded_dump, "buffer_store_format_x"),
	      "formatted store regression did not decode buffer_store_format_x");
	Check(Common::ContainsStr(result.ir_dump, "typed=0 formatted=1"),
	      "formatted store regression did not preserve formatted metadata");
	CheckSpirvBinaryValidates(result.spirv);

	const auto source = DisassembleSpirvBinary(result.spirv);
	Check(Common::ContainsStr(source, "OpArrayLength"),
	      "formatted store SPIR-V lacks runtime storage-buffer bounds check");
	Check(!SpirvSourceHasInstructionUsing(source, "OpULessThan", "%uint_5"),
	      "formatted store SPIR-V baked descriptor NumRecords into a store guard");
}

void TestNewShaderRecompilerTypedBufferLowering() {
	const uint32_t shader[] = {
	    EncodeMtbuf0(0x00, 14, 7, 4),
	    EncodeMtbuf1(0x00, 60, 0, 1), // tbuffer_load_format_x
	    EncodeMtbuf0(0x01, 13, 7, 8),
	    EncodeMtbuf1(0x01, 61, 0, 1), // tbuffer_load_format_xy
	    EncodeMtbuf0(0x02, 13, 4, 12),
	    EncodeMtbuf1(0x02, 64, 0, 1), // tbuffer_load_format_xyz
	    EncodeMtbuf0(0x03, 14, 7, 16, true, true),
	    EncodeMtbuf1(0x03, 68, 0, 1), // tbuffer_load_format_xyzw
	    EncodeMtbuf0(0x04, 14, 7, 20),
	    EncodeMtbuf1(0x04, 72, 0, 1), // tbuffer_store_format_x
	    EncodeMtbuf0(0x05, 13, 7, 24),
	    EncodeMtbuf1(0x05, 76, 0, 1), // tbuffer_store_format_xy
	    EncodeMtbuf0(0x07, 14, 7, 28),
	    EncodeMtbuf1(0x07, 80, 0, 1), // tbuffer_store_format_xyzw
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "tbuffer_load_format_x"),
	      "new decoder did not decode typed buffer format-x load");
	Check(Common::ContainsStr(result.decoded_dump, "tbuffer_load_format_xy"),
	      "new decoder did not decode typed buffer format-xy load");
	Check(Common::ContainsStr(result.decoded_dump, "tbuffer_load_format_xyz"),
	      "new decoder did not decode typed buffer format-xyz load");
	Check(Common::ContainsStr(result.decoded_dump, "tbuffer_load_format_xyzw"),
	      "new decoder did not decode typed buffer format-xyzw load");
	Check(Common::ContainsStr(result.decoded_dump, "tbuffer_store_format_x"),
	      "new decoder did not decode typed buffer format-x store");
	Check(Common::ContainsStr(result.decoded_dump, "tbuffer_store_format_xy"),
	      "new decoder did not decode typed buffer format-xy store");
	Check(Common::ContainsStr(result.decoded_dump, "tbuffer_store_format_xyzw"),
	      "new decoder did not decode typed buffer format-xyzw store");
	Check(Common::ContainsStr(result.decoded_dump, "dfmt=14 nfmt=7"),
	      "MTBUF decode did not expose dfmt/nfmt metadata");
	Check(Common::ContainsStr(result.decoded_dump, "typed=1"),
	      "MTBUF decode did not preserve typed metadata");
	Check(Common::ContainsStr(result.decoded_dump, "offen=1"),
	      "MTBUF decode did not preserve offen metadata");
	Check(Common::ContainsStr(result.ir_dump, "BufferLoadDword v60"),
	      "typed buffer x load did not lower through shared buffer load IR");
	Check(Common::ContainsStr(result.ir_dump, "BufferLoadDword v71"),
	      "typed buffer xyzw load did not expand to last shared dword load");
	Check(Common::ContainsStr(result.ir_dump, "BufferStoreDword null, v72"),
	      "typed buffer x store did not lower through shared buffer store IR");
	Check(Common::ContainsStr(result.ir_dump, "BufferStoreDword null, v83"),
	      "typed buffer xyzw store did not expand to last shared dword store");
	Check(Common::ContainsStr(result.ir_dump, "typed=1"),
	      "typed buffer metadata did not survive into IR");
	Check(Common::ContainsStr(result.ir_dump, "dfmt=13 nfmt=7"),
	      "typed buffer format metadata did not survive into IR");
	Check(SpirvContainsOpcode(result.spirv, 65), "SPIR-V binary does not contain OpAccessChain");
	Check(SpirvContainsOpcode(result.spirv, 61), "SPIR-V binary does not contain OpLoad");
	Check(SpirvContainsOpcode(result.spirv, 62), "SPIR-V binary does not contain OpStore");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerFlatOldBackedLowering() {
	const uint32_t shader[] = {
	    EncodeFlat0(0x08, 0, 4),
	    EncodeFlat1(9, 0x7d, 0, 1), // flat_load_ubyte
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage            = ShaderType::Compute;
	options.dump_ir          = true;
	options.flat_memory_base = 0;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "flat_load_ubyte"),
	      "new decoder did not decode old-backed FLAT ubyte load");
	Check(Common::ContainsStr(result.ir_dump, "FlatLoadUbyte v9"),
	      "old-backed FLAT ubyte load did not lower to IR");
	Check(SpirvContainsOpcode(result.spirv, 65), "SPIR-V binary does not contain OpAccessChain");
	Check(SpirvContainsOpcode(result.spirv, 61), "SPIR-V binary does not contain OpLoad");
	Check(SpirvContainsOpcode(result.spirv, 194),
	      "SPIR-V binary does not contain OpShiftRightLogical");
	Check(SpirvContainsOpcode(result.spirv, 196),
	      "SPIR-V binary does not contain OpShiftLeftLogical");
	Check(SpirvContainsOpcode(result.spirv, 199), "SPIR-V binary does not contain OpBitwiseAnd");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerUnbasedFlatRequiresTranslator() {
	const uint32_t shader[] = {
	    EncodeFlat0(0x0c, 0, 0),
	    EncodeFlat1(0, 0x7d, 0, 1),
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage = ShaderType::Compute;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(!ShaderRecompiler::TryRecompile(shader, options, &result, &error) &&
	          Common::ContainsStr(error, "requires runtime guest-address translation"),
	      "unbased FLAT compiled without an explicit translator");
}

void TestNewShaderRecompilerFlatSignedLoadLowering() {
	const uint32_t shader[] = {
	    EncodeFlat0(0x09, 0, 4),
	    EncodeFlat1(10, 0x7d, 0, 1), // flat_load_sbyte
	    EncodeFlat0(0x0b, 1, 8),
	    EncodeFlat1(11, 0x7d, 0, 1), // scratch_load_sshort
	    EncodeFlat0(0x09, 2, 12),
	    EncodeFlat1(12, 0x7d, 0, 1), // global_load_sbyte
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage            = ShaderType::Compute;
	options.dump_ir          = true;
	options.flat_memory_base = 0;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "flat_load_sbyte"),
	      "new decoder did not decode flat signed byte load");
	Check(Common::ContainsStr(result.decoded_dump, "flat_load_sshort"),
	      "new decoder did not decode flat signed short load");
	Check(Common::ContainsStr(result.decoded_dump, "bits=8"),
	      "signed byte flat load did not preserve bit width metadata");
	Check(Common::ContainsStr(result.decoded_dump, "bits=16"),
	      "signed short flat load did not preserve bit width metadata");
	Check(Common::ContainsStr(result.decoded_dump, "signed=1"),
	      "signed flat loads did not preserve signed metadata");
	Check(Common::ContainsStr(result.ir_dump, "FlatLoadSbyte v10"),
	      "flat_load_sbyte did not lower to signed byte IR load");
	Check(Common::ContainsStr(result.ir_dump, "FlatLoadSshort v11"),
	      "flat_load_sshort did not lower to signed short IR load");
	Check(Common::ContainsStr(result.ir_dump, "scratch"),
	      "signed scratch load did not preserve scratch metadata");
	Check(Common::ContainsStr(result.ir_dump, "global"),
	      "signed global load did not preserve global metadata");
	Check(SpirvContainsOpcode(result.spirv, 61), "SPIR-V binary does not contain OpLoad");
	Check(SpirvContainsOpcode(result.spirv, 195),
	      "SPIR-V binary does not contain OpShiftRightArithmetic for sign extension");
	Check(SpirvContainsOpcode(result.spirv, 196),
	      "SPIR-V binary does not contain OpShiftLeftLogical for sign extension");
	Check(SpirvContainsOpcode(result.spirv, 199), "SPIR-V binary does not contain OpBitwiseAnd");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerFlatStoreLowering() {
	const uint32_t shader[] = {
	    EncodeFlat0(0x18, 0, 2),
	    EncodeFlat1(0, 0x7d, 72, 1), // flat_store_byte
	    EncodeFlat0(0x1a, 1, 6),
	    EncodeFlat1(0, 0x7d, 73, 1), // scratch_store_short
	    EncodeFlat0(0x1c, 0, 4),
	    EncodeFlat1(0, 0x7d, 60, 1), // flat_store_dword
	    EncodeFlat0(0x1d, 1, 8),
	    EncodeFlat1(0, 0x7d, 61, 1), // scratch_store_dwordx2
	    EncodeFlat0(0x1f, 2, 12),
	    EncodeFlat1(0, 0x7d, 64, 1), // global_store_dwordx3
	    EncodeFlat0(0x1e, 2, 16),
	    EncodeFlat1(0, 0x7d, 68, 1), // global_store_dwordx4
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage            = ShaderType::Compute;
	options.dump_ir          = true;
	options.flat_memory_base = 0;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "flat_store_byte"),
	      "new decoder did not decode flat byte store");
	Check(Common::ContainsStr(result.decoded_dump, "flat_store_short"),
	      "new decoder did not decode flat short store");
	Check(Common::ContainsStr(result.decoded_dump, "flat_store_dword"),
	      "new decoder did not decode flat dword store");
	Check(Common::ContainsStr(result.decoded_dump, "flat_store_dwordx2"),
	      "new decoder did not decode flat dwordx2 store");
	Check(Common::ContainsStr(result.decoded_dump, "flat_store_dwordx3"),
	      "new decoder did not decode flat dwordx3 store");
	Check(Common::ContainsStr(result.decoded_dump, "flat_store_dwordx4"),
	      "new decoder did not decode flat dwordx4 store");
	Check(Common::ContainsStr(result.decoded_dump, "segment=1"),
	      "flat store decode did not preserve scratch segment metadata");
	Check(Common::ContainsStr(result.decoded_dump, "segment=2"),
	      "flat store decode did not preserve global segment metadata");
	Check(Common::ContainsStr(result.decoded_dump, "dwords=1 bits=8"),
	      "flat byte store decode did not expose byte-width metadata");
	Check(Common::ContainsStr(result.decoded_dump, "dwords=1 bits=16"),
	      "flat short store decode did not expose short-width metadata");
	Check(Common::ContainsStr(result.decoded_dump, "dwords=4 bits=32"),
	      "flat store decode did not expose wide width metadata");
	Check(Common::ContainsStr(result.ir_dump, "FlatStoreByte null, v72"),
	      "flat_store_byte did not lower through shared flat store IR");
	Check(Common::ContainsStr(result.ir_dump, "FlatStoreShort null, v73"),
	      "flat_store_short did not lower through shared flat store IR");
	Check(Common::ContainsStr(result.ir_dump, "FlatStoreDword null, v60"),
	      "flat_store_dword did not lower through shared flat store IR");
	Check(Common::ContainsStr(result.ir_dump, "FlatStoreDword null, v62"),
	      "scratch_store_dwordx2 did not expand to the last dword store");
	Check(Common::ContainsStr(result.ir_dump, "FlatStoreDword null, v66"),
	      "global_store_dwordx3 did not expand to the last dword store");
	Check(Common::ContainsStr(result.ir_dump, "FlatStoreDword null, v71"),
	      "global_store_dwordx4 did not expand to the last dword store");
	Check(Common::ContainsStr(result.ir_dump, "flat"),
	      "flat store IR did not preserve flat resource metadata");
	Check(Common::ContainsStr(result.ir_dump, "scratch"),
	      "flat store IR did not preserve scratch resource metadata");
	Check(Common::ContainsStr(result.ir_dump, "global"),
	      "flat store IR did not preserve global resource metadata");
	Check(SpirvContainsOpcode(result.spirv, 61),
	      "SPIR-V binary does not contain OpLoad for sub-dword flat store merge");
	Check(SpirvContainsOpcode(result.spirv, 65), "SPIR-V binary does not contain OpAccessChain");
	Check(SpirvContainsOpcode(result.spirv, 62), "SPIR-V binary does not contain OpStore");
	Check(SpirvContainsOpcode(result.spirv, 197),
	      "SPIR-V binary does not contain OpBitwiseOr for sub-dword flat store merge");
	Check(SpirvContainsOpcode(result.spirv, 200),
	      "SPIR-V binary does not contain OpNot for sub-dword flat store mask");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerAtomicLowering() {
	const uint32_t shader[] = {
	    EncodeMubuf0(0x30, 4, true, true),
	    EncodeMubuf1(0, 0, 1), // buffer_atomic_swap
	    EncodeMubuf0(0x32, 8, true, true),
	    EncodeMubuf1(1, 0, 1), // buffer_atomic_add
	    EncodeMubuf0(0x33, 12, true, true),
	    EncodeMubuf1(7, 0, 1), // buffer_atomic_sub
	    EncodeMubuf0(0x35, 16, true, true),
	    EncodeMubuf1(8, 0, 1), // buffer_atomic_smin
	    EncodeMubuf0(0x36, 20, true, true),
	    EncodeMubuf1(2, 0, 1), // buffer_atomic_umin
	    EncodeMubuf0(0x37, 24, true, true),
	    EncodeMubuf1(9, 0, 1), // buffer_atomic_smax
	    EncodeMubuf0(0x38, 28, true, true),
	    EncodeMubuf1(3, 0, 1), // buffer_atomic_umax
	    EncodeMubuf0(0x39, 32, true, true),
	    EncodeMubuf1(4, 0, 1), // buffer_atomic_and
	    EncodeMubuf0(0x3a, 36, true, true),
	    EncodeMubuf1(5, 0, 1), // buffer_atomic_or
	    EncodeMubuf0(0x3b, 40, true, true),
	    EncodeMubuf1(6, 0, 1), // buffer_atomic_xor
	    EncodeDs0(0x00),
	    EncodeDs1(0, 2, 1), // ds_add_u32
	    EncodeDs0(0x01),
	    EncodeDs1(0, 10, 1), // ds_sub_u32
	    EncodeDs0(0x05),
	    EncodeDs1(0, 11, 1), // ds_min_i32
	    EncodeDs0(0x06),
	    EncodeDs1(0, 12, 1), // ds_max_i32
	    EncodeDs0(0x09),
	    EncodeDs1(0, 13, 1), // ds_and_b32
	    EncodeDs0(0x0b),
	    EncodeDs1(0, 14, 1), // ds_xor_b32
	    EncodeDs0(0x20),
	    EncodeDs1(15, 2, 1), // ds_add_rtn_u32
	    EncodeDs0(0x21),
	    EncodeDs1(16, 10, 1), // ds_sub_rtn_u32
	    EncodeDs0(0x25),
	    EncodeDs1(17, 11, 1), // ds_min_rtn_i32
	    EncodeDs0(0x26),
	    EncodeDs1(18, 12, 1), // ds_max_rtn_i32
	    EncodeDs0(0x27),
	    EncodeDs1(19, 2, 1), // ds_min_rtn_u32
	    EncodeDs0(0x28),
	    EncodeDs1(20, 2, 1), // ds_max_rtn_u32
	    EncodeDs0(0x29),
	    EncodeDs1(21, 13, 1), // ds_and_rtn_b32
	    EncodeDs0(0x2a),
	    EncodeDs1(22, 2, 1), // ds_or_rtn_b32
	    EncodeDs0(0x2b),
	    EncodeDs1(23, 14, 1), // ds_xor_rtn_b32
	    EncodeDs0(0x2d),
	    EncodeDs1(24, 15, 1), // ds_wrxchg_rtn_b32
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "buffer_atomic_swap"),
	      "new decoder did not decode buffer atomic swap");
	Check(Common::ContainsStr(result.decoded_dump, "buffer_atomic_add"),
	      "new decoder did not decode buffer atomic add");
	Check(Common::ContainsStr(result.decoded_dump, "buffer_atomic_sub"),
	      "new decoder did not decode buffer atomic sub");
	Check(Common::ContainsStr(result.decoded_dump, "buffer_atomic_smin"),
	      "new decoder did not decode buffer atomic signed min");
	Check(Common::ContainsStr(result.decoded_dump, "buffer_atomic_smax"),
	      "new decoder did not decode buffer atomic signed max");
	Check(Common::ContainsStr(result.decoded_dump, "buffer_atomic_xor"),
	      "new decoder did not decode buffer atomic xor");
	Check(Common::ContainsStr(result.decoded_dump, "ds_add_u32"),
	      "new decoder did not decode DS atomic add");
	Check(Common::ContainsStr(result.decoded_dump, "ds_sub_u32"),
	      "new decoder did not decode DS atomic sub");
	Check(Common::ContainsStr(result.decoded_dump, "ds_min_i32"),
	      "new decoder did not decode DS signed min");
	Check(Common::ContainsStr(result.decoded_dump, "ds_xor_rtn_b32"),
	      "new decoder did not decode DS xor-return");
	Check(Common::ContainsStr(result.decoded_dump, "ds_wrxchg_rtn_b32"),
	      "new decoder did not decode DS write-exchange-return");
	Check(Common::ContainsStr(result.decoded_dump, "ds_add_rtn_u32"),
	      "new decoder did not decode DS atomic add-return");
	Check(Common::ContainsStr(result.ir_dump, "AtomicSwapU32 v0"),
	      "buffer atomic swap did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicAddU32 v1"),
	      "buffer atomic add did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicSubU32 v7"),
	      "buffer atomic sub did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicSMinI32 v8"),
	      "buffer atomic signed min did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicUMinU32 v2"),
	      "buffer atomic umin did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicSMaxI32 v9"),
	      "buffer atomic signed max did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicUMaxU32 v3"),
	      "buffer atomic umax did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicAndU32 v4"),
	      "buffer atomic and did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicOrU32 v5"),
	      "buffer atomic or did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicXorU32 v6"),
	      "buffer atomic xor did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicAddU32 null, v2"),
	      "DS no-return atomic add did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicSubU32 null, v10"),
	      "DS no-return atomic sub did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicSMinI32 null, v11"),
	      "DS no-return signed min did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicSMaxI32 null, v12"),
	      "DS no-return signed max did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicAndU32 null, v13"),
	      "DS no-return and did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicXorU32 null, v14"),
	      "DS no-return xor did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicAddU32 v15"),
	      "DS add-return did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicSubU32 v16"),
	      "DS sub-return did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicSMinI32 v17"),
	      "DS signed min-return did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicSMaxI32 v18"),
	      "DS signed max-return did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicUMinU32 v19"),
	      "DS unsigned min-return did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicUMaxU32 v20"),
	      "DS unsigned max-return did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicAndU32 v21"),
	      "DS and-return did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicOrU32 v22"),
	      "DS or-return did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicXorU32 v23"),
	      "DS xor-return did not lower to IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicSwapU32 v24"),
	      "DS write-exchange-return did not lower to shared atomic swap IR");
	Check(SpirvContainsOpcode(result.spirv, 229),
	      "SPIR-V binary does not contain OpAtomicExchange");
	Check(SpirvContainsOpcode(result.spirv, 234), "SPIR-V binary does not contain OpAtomicIAdd");
	Check(SpirvContainsOpcode(result.spirv, 235), "SPIR-V binary does not contain OpAtomicISub");
	Check(SpirvContainsOpcode(result.spirv, 236), "SPIR-V binary does not contain OpAtomicSMin");
	Check(SpirvContainsOpcode(result.spirv, 237), "SPIR-V binary does not contain OpAtomicUMin");
	Check(SpirvContainsOpcode(result.spirv, 238), "SPIR-V binary does not contain OpAtomicSMax");
	Check(SpirvContainsOpcode(result.spirv, 239), "SPIR-V binary does not contain OpAtomicUMax");
	Check(SpirvContainsOpcode(result.spirv, 240), "SPIR-V binary does not contain OpAtomicAnd");
	Check(SpirvContainsOpcode(result.spirv, 241), "SPIR-V binary does not contain OpAtomicOr");
	Check(SpirvContainsOpcode(result.spirv, 242), "SPIR-V binary does not contain OpAtomicXor");
	Check(SpirvContainsOpcode(result.spirv, 225),
	      "buffer atomic SPIR-V binary does not contain OpMemoryBarrier");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerDsReadWrite2Lowering() {
	const uint32_t shader[] = {
	    EncodeDs0(0x0e, (3u << 8u) | 1u),
	    EncodeDs1Ex(0, 61, 60, 1),
	    EncodeDs0(0x37, (4u << 8u) | 2u),
	    EncodeDs1Ex(70, 0, 0, 1),
	    EncodeDs0(0x38, (5u << 8u) | 1u),
	    EncodeDs1Ex(72, 0, 0, 1),
	    EncodeDs0(0x77, (6u << 8u) | 2u),
	    EncodeDs1Ex(80, 0, 0, 1),
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "ds_write2_b32"),
	      "new decoder did not decode old-backed DS write2");
	Check(Common::ContainsStr(result.decoded_dump, "ds_read2_b32"),
	      "new decoder did not decode old-backed DS read2");
	Check(Common::ContainsStr(result.decoded_dump, "ds_read2_b64"),
	      "new decoder did not decode old-backed DS read2 b64");
	Check(Common::ContainsStr(result.decoded_dump, "offset=4"),
	      "DS write2 decode did not scale offset0 to bytes");
	Check(Common::ContainsStr(result.decoded_dump, "offset2=12"),
	      "DS write2 decode did not scale offset1 to bytes");
	Check(Common::ContainsStr(result.decoded_dump, "offset=8"),
	      "DS read2 decode did not scale offset0 to bytes");
	Check(Common::ContainsStr(result.decoded_dump, "offset2=16"),
	      "DS read2 decode did not scale offset1 to bytes");
	Check(Common::ContainsStr(result.decoded_dump, "offset=256"),
	      "DS read2 st64 decode did not scale offset0 to bytes");
	Check(Common::ContainsStr(result.decoded_dump, "offset2=1280"),
	      "DS read2 st64 decode did not scale offset1 to bytes");
	Check(Common::ContainsStr(result.decoded_dump, "offset=16"),
	      "DS read2 b64 decode did not scale offset0 to bytes");
	Check(Common::ContainsStr(result.decoded_dump, "offset2=48"),
	      "DS read2 b64 decode did not scale offset1 to bytes");
	Check(Common::ContainsStr(result.decoded_dump, "dwords=2 bits=32"),
	      "DS read/write2 decode did not preserve two-dword metadata");
	Check(Common::ContainsStr(result.decoded_dump, "dwords=4 bits=32"),
	      "DS read2 b64 decode did not preserve four-dword metadata");
	Check(Common::ContainsStr(result.ir_dump, "DsWriteB32 null, v60"),
	      "DS write2 did not lower first dword through shared LDS store IR");
	Check(Common::ContainsStr(result.ir_dump, "DsWriteB32 null, v61"),
	      "DS write2 did not lower second dword through shared LDS store IR");
	Check(Common::ContainsStr(result.ir_dump, "DsReadB32 v70"),
	      "DS read2 did not lower first dword through shared LDS load IR");
	Check(Common::ContainsStr(result.ir_dump, "DsReadB32 v71"),
	      "DS read2 did not lower second dword through shared LDS load IR");
	Check(Common::ContainsStr(result.ir_dump, "DsReadB32 v72"),
	      "DS read2 st64 did not lower first dword through shared LDS load IR");
	Check(Common::ContainsStr(result.ir_dump, "DsReadB32 v73"),
	      "DS read2 st64 did not lower second dword through shared LDS load IR");
	Check(Common::ContainsStr(result.ir_dump, "DsReadB32 v80"),
	      "DS read2 b64 did not lower first dword through shared LDS load IR");
	Check(Common::ContainsStr(result.ir_dump, "DsReadB32 v83"),
	      "DS read2 b64 did not lower fourth dword through shared LDS load IR");
	Check(SpirvContainsOpcode(result.spirv, 61), "SPIR-V binary does not contain OpLoad");
	Check(SpirvContainsOpcode(result.spirv, 62), "SPIR-V binary does not contain OpStore");
	Check(SpirvContainsOpcode(result.spirv, 65), "SPIR-V binary does not contain OpAccessChain");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerDsSubDwordLowering() {
	const uint32_t shader[] = {
	    EncodeDs0(0x1e, 1), EncodeDs1(0, 40, 1), // ds_write_b8 v40, v1
	    EncodeDs0(0x1f, 2), EncodeDs1(0, 41, 1), // ds_write_b16 v41, v1
	    EncodeDs0(0x39, 3), EncodeDs1(42, 0, 1), // ds_read_i8 v42, v1
	    EncodeDs0(0x3a, 4), EncodeDs1(43, 0, 1), // ds_read_u8 v43, v1
	    EncodeDs0(0x3b, 6), EncodeDs1(44, 0, 1), // ds_read_i16 v44, v1
	    EncodeDs0(0x3c, 8), EncodeDs1(45, 0, 1), // ds_read_u16 v45, v1
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "ds_write_b8"),
	      "new decoder did not decode DS byte write");
	Check(Common::ContainsStr(result.decoded_dump, "ds_write_b16"),
	      "new decoder did not decode DS short write");
	Check(Common::ContainsStr(result.decoded_dump, "ds_read_i8"),
	      "new decoder did not decode DS signed byte read");
	Check(Common::ContainsStr(result.decoded_dump, "ds_read_u8"),
	      "new decoder did not decode DS unsigned byte read");
	Check(Common::ContainsStr(result.decoded_dump, "ds_read_i16"),
	      "new decoder did not decode DS signed short read");
	Check(Common::ContainsStr(result.decoded_dump, "ds_read_u16"),
	      "new decoder did not decode DS unsigned short read");
	Check(Common::ContainsStr(result.decoded_dump, "dwords=1 bits=8"),
	      "DS byte decode did not preserve byte-width metadata");
	Check(Common::ContainsStr(result.decoded_dump, "dwords=1 bits=16"),
	      "DS short decode did not preserve short-width metadata");
	Check(Common::ContainsStr(result.ir_dump, "DsWriteByte null, v40"),
	      "DS byte write did not lower to explicit IR");
	Check(Common::ContainsStr(result.ir_dump, "DsWriteShort null, v41"),
	      "DS short write did not lower to explicit IR");
	Check(Common::ContainsStr(result.ir_dump, "DsReadSbyte v42"),
	      "DS signed byte read did not lower to explicit IR");
	Check(Common::ContainsStr(result.ir_dump, "DsReadUbyte v43"),
	      "DS unsigned byte read did not lower to explicit IR");
	Check(Common::ContainsStr(result.ir_dump, "DsReadSshort v44"),
	      "DS signed short read did not lower to explicit IR");
	Check(Common::ContainsStr(result.ir_dump, "DsReadUshort v45"),
	      "DS unsigned short read did not lower to explicit IR");
	Check(SpirvContainsOpcode(result.spirv, 61), "SPIR-V binary does not contain OpLoad");
	Check(SpirvContainsOpcode(result.spirv, 62), "SPIR-V binary does not contain OpStore");
	Check(SpirvContainsOpcode(result.spirv, 65), "SPIR-V binary does not contain OpAccessChain");
	Check(SpirvContainsOpcode(result.spirv, 194),
	      "SPIR-V binary does not contain OpShiftRightLogical");
	Check(SpirvContainsOpcode(result.spirv, 195),
	      "SPIR-V binary does not contain OpShiftRightArithmetic");
	Check(SpirvContainsOpcode(result.spirv, 196),
	      "SPIR-V binary does not contain OpShiftLeftLogical");
	Check(SpirvContainsOpcode(result.spirv, 199), "SPIR-V binary does not contain OpBitwiseAnd");
	Check(SpirvContainsOpcode(result.spirv, 200), "SPIR-V binary does not contain OpNot");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerDsWideAndAtomicLowering() {
	const uint32_t shader[] = {
	    EncodeDs0(0x4d, 4),  EncodeDs1(0, 10, 1), // ds_write_b64 v[10:11], v1
	    EncodeDs0(0xde, 8),  EncodeDs1(0, 12, 1), // ds_write_b96 v[12:14], v1
	    EncodeDs0(0xdf, 12), EncodeDs1(0, 16, 1), // ds_write_b128 v[16:19], v1
	    EncodeDs0(0x76, 16), EncodeDs1(20, 0, 1), // ds_read_b64 v[20:21], v1
	    EncodeDs0(0xfe, 20), EncodeDs1(24, 0, 1), // ds_read_b96 v[24:26], v1
	    EncodeDs0(0xff, 24), EncodeDs1(28, 0, 1), // ds_read_b128 v[28:31], v1
	    EncodeDs0(0x07, 28), EncodeDs1(0, 32, 1), // ds_min_u32 v32, v1
	    EncodeDs0(0x08, 32), EncodeDs1(0, 33, 1), // ds_max_u32 v33, v1
	    EncodeDs0(0x0a, 36), EncodeDs1(0, 34, 1), // ds_or_b32 v34, v1
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "ds_write_b64"),
	      "new decoder did not decode DS b64 write");
	Check(Common::ContainsStr(result.decoded_dump, "ds_write_b96"),
	      "new decoder did not decode DS b96 write");
	Check(Common::ContainsStr(result.decoded_dump, "ds_write_b128"),
	      "new decoder did not decode DS b128 write");
	Check(Common::ContainsStr(result.decoded_dump, "ds_read_b64"),
	      "new decoder did not decode DS b64 read");
	Check(Common::ContainsStr(result.decoded_dump, "ds_read_b96"),
	      "new decoder did not decode DS b96 read");
	Check(Common::ContainsStr(result.decoded_dump, "ds_read_b128"),
	      "new decoder did not decode DS b128 read");
	Check(Common::ContainsStr(result.decoded_dump, "ds_min_u32"),
	      "new decoder did not decode DS min atomic");
	Check(Common::ContainsStr(result.decoded_dump, "ds_max_u32"),
	      "new decoder did not decode DS max atomic");
	Check(Common::ContainsStr(result.decoded_dump, "ds_or_b32"),
	      "new decoder did not decode DS or atomic");
	Check(Common::ContainsStr(result.decoded_dump, "dwords=4 bits=32"),
	      "new decoder did not expose DS wide width metadata");
	Check(Common::ContainsStr(result.ir_dump, "DsWriteB32 null, v10"),
	      "DS b64 write did not expand to first dword store");
	Check(Common::ContainsStr(result.ir_dump, "DsWriteB32 null, v19"),
	      "DS b128 write did not expand to last dword store");
	Check(Common::ContainsStr(result.ir_dump, "DsReadB32 v20"),
	      "DS b64 read did not expand to first dword load");
	Check(Common::ContainsStr(result.ir_dump, "DsReadB32 v31"),
	      "DS b128 read did not expand to last dword load");
	Check(Common::ContainsStr(result.ir_dump, "AtomicUMinU32 null, v32"),
	      "DS min atomic did not lower to shared atomic IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicUMaxU32 null, v33"),
	      "DS max atomic did not lower to shared atomic IR");
	Check(Common::ContainsStr(result.ir_dump, "AtomicOrU32 null, v34"),
	      "DS or atomic did not lower to shared atomic IR");
	Check(SpirvContainsOpcode(result.spirv, 61), "SPIR-V binary does not contain OpLoad");
	Check(SpirvContainsOpcode(result.spirv, 62), "SPIR-V binary does not contain OpStore");
	Check(SpirvContainsOpcode(result.spirv, 237), "SPIR-V binary does not contain OpAtomicUMin");
	Check(SpirvContainsOpcode(result.spirv, 239), "SPIR-V binary does not contain OpAtomicUMax");
	Check(SpirvContainsOpcode(result.spirv, 241), "SPIR-V binary does not contain OpAtomicOr");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerDsSwizzleLowering() {
	const uint32_t shader[] = {
	    EncodeDs0(0x35, 0x001f),
	    EncodeDs1(8, 0, 5), // ds_swizzle_b32 v8, v5
	    EncodeDs0(0x35, 0x801b),
	    EncodeDs1(9, 0, 6), // ds_swizzle_b32 v9, v6
	    0xd8d4c480u,
	    0x45000045u, // ds_swizzle_b32 rotate mode from boot shader
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "ds_swizzle_b32"),
	      "new decoder did not decode DS swizzle");
	Check(Common::ContainsStr(result.decoded_dump, "offset=31"),
	      "new decoder did not expose DS swizzle control");
	Check(Common::ContainsStr(result.decoded_dump, "offset=32795"),
	      "new decoder did not expose DS swizzle quad control");
	Check(Common::ContainsStr(result.decoded_dump, "offset=50304"),
	      "new decoder did not expose DS swizzle rotate control");
	Check(Common::ContainsStr(result.ir_dump, "DsSwizzleB32 v8, v5, 0x0000001f"),
	      "DS swizzle did not lower to explicit IR");
	Check(Common::ContainsStr(result.ir_dump, "DsSwizzleB32 v9, v6, 0x0000801b"),
	      "DS quad swizzle did not lower to explicit IR");
	Check(Common::ContainsStr(result.ir_dump, "DsSwizzleB32 v69, v69, 0x0000c480"),
	      "DS rotate swizzle did not lower to explicit IR");
	Check(SpirvContainsOpcode(result.spirv, 345),
	      "SPIR-V binary does not contain OpGroupNonUniformShuffle");
	Check(SpirvContainsOpcode(result.spirv, 128),
	      "SPIR-V binary does not contain OpIAdd for DS rotate swizzle");
	Check(SpirvContainsOpcode(result.spirv, 61), "SPIR-V binary does not contain OpLoad");
	Check(SpirvContainsOpcode(result.spirv, 62), "SPIR-V binary does not contain OpStore");
	Check(SpirvContainsOpcode(result.spirv, 196),
	      "SPIR-V binary does not contain OpShiftLeftLogical");
	Check(SpirvContainsOpcode(result.spirv, 198), "SPIR-V binary does not contain OpBitwiseOr");
	Check(SpirvContainsOpcode(result.spirv, 199), "SPIR-V binary does not contain OpBitwiseAnd");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerDsAddtidLowering() {
	const uint32_t shader[] = {
	    EncodeSMovB32(124, 132), // m0 = 4
	    EncodeDs0(0xb0, 8),
	    EncodeDs1(0, 7, 0), // ds_write_addtid_b32 v7
	    EncodeDs0(0xb1, 12),
	    EncodeDs1(8, 0, 0), // ds_read_addtid_b32 v8
	    0xbf810000u,
	};

	ShaderComputeInputInfo input_info = RegressionComputeInputInfo();
	input_info.thread_ids_num         = 1;

	ShaderRecompiler::CompileOptions options;
	options.stage              = ShaderType::Compute;
	options.dump_ir            = true;
	options.compute_input_info = &input_info;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "ds_write_addtid_b32"),
	      "new decoder did not decode DS write addtid");
	Check(Common::ContainsStr(result.decoded_dump, "ds_read_addtid_b32"),
	      "new decoder did not decode DS read addtid");
	Check(Common::ContainsStr(result.ir_dump, "DsWriteAddtidB32 null, v7, m0"),
	      "DS write addtid did not lower to explicit IR");
	Check(Common::ContainsStr(result.ir_dump, "DsReadAddtidB32 v8, m0"),
	      "DS read addtid did not lower to explicit IR");
	Check(
	    ProgramHasInput(result.program, ShaderRecompiler::IR::StageInputKind::LocalInvocationIndex),
	    "DS addtid did not request LocalInvocationIndex input");
	Check(SpirvContainsOpcode(result.spirv, 61), "SPIR-V binary does not contain OpLoad");
	Check(SpirvContainsOpcode(result.spirv, 62), "SPIR-V binary does not contain OpStore");
	Check(SpirvContainsOpcode(result.spirv, 65), "SPIR-V binary does not contain OpAccessChain");
	Check(SpirvContainsOpcode(result.spirv, 128), "SPIR-V binary does not contain OpIAdd");
	Check(SpirvContainsOpcode(result.spirv, 196),
	      "SPIR-V binary does not contain OpShiftLeftLogical");
	Check(SpirvContainsOpcode(result.spirv, 199), "SPIR-V binary does not contain OpBitwiseAnd");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerDsFloatMinMaxLowering() {
	const uint32_t shader[] = {
	    EncodeDs0(0x12, 4), EncodeDs1Ex(0, 9, 7, 1),  // ds_min_f32 v7, v9, v1
	    EncodeDs0(0x13, 8), EncodeDs1Ex(0, 10, 8, 1), // ds_max_f32 v8, v10, v1
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "ds_min_f32"),
	      "new decoder did not decode DS float min");
	Check(Common::ContainsStr(result.decoded_dump, "ds_max_f32"),
	      "new decoder did not decode DS float max");
	Check(Common::ContainsStr(result.ir_dump, "DsMinF32 null, v7, v1"),
	      "DS float min did not lower to explicit IR");
	Check(Common::ContainsStr(result.ir_dump, "DsMaxF32 null, v8, v1"),
	      "DS float max did not lower to explicit IR");
	Check(Common::ContainsStr(result.ir_dump, "v9"),
	      "DS float min did not retain DATA1 compare operand");
	Check(Common::ContainsStr(result.ir_dump, "v10"),
	      "DS float max did not retain DATA1 compare operand");
	Check(SpirvContainsOpcode(result.spirv, 12), "SPIR-V binary does not contain OpExtInst");
	Check(SpirvContainsOpcode(result.spirv, 61), "SPIR-V binary does not contain OpLoad");
	Check(SpirvContainsOpcode(result.spirv, 62), "SPIR-V binary does not contain OpStore");
	Check(SpirvContainsOpcode(result.spirv, 65), "SPIR-V binary does not contain OpAccessChain");
	Check(SpirvContainsOpcode(result.spirv, 124), "SPIR-V binary does not contain OpBitcast");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerCfgStraightLine() {
	const uint32_t shader[] = {
	    EncodeSMovB32(0, 129),
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.ir_dump, "CFG:"), "CFG dump was not emitted");
	Check(Common::ContainsStr(result.ir_dump, "block_0"), "straight-line CFG block missing");
	Check(Common::ContainsStr(result.ir_dump, "successors=["), "CFG successors were not dumped");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerCfgIfElse() {
	const uint32_t shader[] = {
	    EncodeSopc(0x06, 0, 0), // s_cmp_eq_u32 s0, s0
	    EncodeSopp(0x05, 2),    // s_cbranch_scc1 else
	    EncodeSMovB32(1, 129),  // then
	    EncodeSopp(0x02, 1),    // s_branch merge
	    EncodeSMovB32(1, 130),  // else
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.ir_dump, "condition=scc1"),
	      "if/else branch condition missing");
	Check(SpirvContainsOpcode(result.spirv, 247), "if/else SPIR-V lacks OpSelectionMerge");
	Check(SpirvContainsOpcode(result.spirv, 250), "if/else SPIR-V lacks OpBranchConditional");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerCfgTerminalExitMergePS() {
	const uint32_t shader[] = {
	    EncodeSopp(0x04, 2),   // s_cbranch_scc0 end
	    EncodeSMovB32(0, 129), // fallthrough work
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Pixel;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.ir_dump, "mode=structured"),
	      "terminal PS branch should stay on structured path");
	Check(!Common::ContainsStr(result.ir_dump, "conditional block 0 has no structured merge"),
	      "known terminal PS branch shape still reports missing merge");
	Check(SpirvContainsOpcode(result.spirv, 247),
	      "terminal PS branch SPIR-V lacks OpSelectionMerge");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerCfgPostEndTargetMergePS() {
	const uint32_t shader[] = {
	    EncodeSopp(0x04, 2),                          // s_cbranch_scc0 label_000c
	    EncodeSMovB32(0, 129),                        // fallthrough work
	    0xbf810000u,           EncodeSMovB32(1, 129), // branch target after first s_endpgm
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Pixel;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	const bool ok = ShaderRecompiler::TryRecompile(shader, options, &result, &error);
	if (!ok) {
		std::fprintf(stderr, "PostEndTargetMergePS compile error: %s\n", error.c_str());
	}
	Check(ok, "post-end target PS shader failed to compile");
	Check(Common::ContainsStr(result.decoded_dump, "0x0000000c: s_mov_b32"),
	      "post-end branch target was not decoded");
	Check(Common::ContainsStr(result.ir_dump, "mode=structured"),
	      "post-end terminal branch should stay on structured path");
	Check(Common::ContainsStr(result.ir_dump, "pc=0x0000000c"),
	      "post-end branch target did not reach IR blocks");
	Check(!Common::ContainsStr(result.ir_dump, "conditional block 0 has no structured merge"),
	      "known post-end PS branch shape still reports missing merge");
	Check(SpirvContainsOpcode(result.spirv, 247),
	      "post-end terminal branch SPIR-V lacks OpSelectionMerge");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerCfgLoopBreakContinue() {
	const uint32_t shader[] = {
	    EncodeSMovB32(0, 128),       // s0 = 0
	    EncodeSopc(0x0a, 0, 129),    // s_cmp_lt_u32 s0, 1
	    EncodeSopp(0x04, 2),         // break when scc == 0
	    EncodeSop2(0x00, 0, 0, 129), // s_add_u32 s0, s0, 1
	    EncodeSopp(0x02, 0xfffcu),   // continue/backedge
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.ir_dump, "backedge"), "loop backedge was not detected");
	Check(Common::ContainsStr(result.ir_dump, "loop_header=1"), "loop header was not marked");
	Check(Common::ContainsStr(result.ir_dump, "continue="),
	      "loop continue block was not identified");
	Check(SpirvContainsOpcode(result.spirv, 246), "loop SPIR-V lacks OpLoopMerge");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerCfgLoopHeaderDynamicScalarBufferLoadStructured() {
	const uint32_t shader[] = {
	    EncodeSMovB32(0, 128), // preheader: s0 = 0
	    EncodeSmem0(0x08, 8, 4),
	    0u,                          // loop: s_buffer_load_dword s8, s[4:7]
	    EncodeSop2(0x00, 0, 0, 129), // s_add_u32 s0, s0, 1
	    EncodeSopc(0x0a, 0, 130),    // s_cmp_lt_u32 s0, 2
	    EncodeSopp(0x05, 0xfffbu),   // s_cbranch_scc1 loop
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(!ShaderRecompiler::TryRecompile(shader, options, &result, &error) &&
	          Common::ContainsStr(error, "unsupported GPU selection"),
	      "self-modifying scalar-buffer descriptor should fail explicitly");
}

void TestNewShaderRecompilerCfgLoopHeaderBufferLoadDispatcher() {
	const uint32_t shader[] = {
	    EncodeSMovB32(0, 128), // preheader: s0 = 0
	    EncodeMubuf0(0x0c),
	    EncodeMubuf1(0, 0, 1),       // loop: buffer_load_dword v0
	    EncodeSop2(0x00, 0, 0, 129), // s_add_u32 s0, s0, 1
	    EncodeSopc(0x0a, 0, 130),    // s_cmp_lt_u32 s0, 2
	    EncodeSopp(0x05, 0xfffbu),   // s_cbranch_scc1 loop
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(!ShaderRecompiler::TryRecompile(shader, options, &result, &error) &&
	          Common::ContainsStr(error, "unsupported GPU selection"),
	      "self-modifying vector-buffer descriptor should fail explicitly");
}

void TestNewShaderRecompilerCfgLoopHeaderDsAppendConsumeDispatcher() {
	const uint32_t shader[] = {
	    EncodeSMovB32(124, 129), // m0 = one counter
	    EncodeDs0(0x3e),         // loop: ds_append
	    EncodeDs1(0, 0, 0),
	    EncodeDs0(0x3d), // ds_consume
	    EncodeDs1(1, 0, 0),
	    EncodeSop2(0x00, 2, 2, 129), // s_add_u32 s2, s2, 1
	    EncodeSopc(0x0a, 2, 130),    // s_cmp_lt_u32 s2, 2
	    EncodeSopp(0x05, 0xfff9u),   // s_cbranch_scc1 loop
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.ir_dump, "mode=dispatcher"),
	      "DS append/consume loop header did not select dispatcher fallback");
	Check(SpirvContainsOpcode(result.spirv, 246), "DS dispatcher SPIR-V lacks OpLoopMerge");
	Check(SpirvContainsOpcode(result.spirv, 251), "DS dispatcher SPIR-V lacks OpSwitch");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerCfgSharedOuterAndLoopMerge() {
	const uint32_t shader[] = {
	    EncodeSopc(0x06, 0, 0),      // s_cmp_eq_u32 s0, s0
	    EncodeSopp(0x05, 5),         // outer early exit -> end
	    EncodeSMovB32(2, 128),       // s2 = 0
	    EncodeSopc(0x0a, 2, 129),    // loop: s_cmp_lt_u32 s2, 1
	    EncodeSopp(0x04, 2),         // loop exit -> same real end block
	    EncodeSop2(0x00, 2, 2, 129), // s_add_u32 s2, s2, 1
	    EncodeSopp(0x02, 0xfffcu),   // continue/backedge
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.ir_dump, "mode=structured"),
	      "shared outer/loop merge should stay on structured path");
	Check(!Common::ContainsStr(result.ir_dump, "duplicate structured merge block"),
	      "shared outer/loop merge was not split before structurization");
	Check(SpirvContainsOpcode(result.spirv, 246),
	      "shared outer/loop merge SPIR-V lacks OpLoopMerge");
	Check(SpirvContainsOpcode(result.spirv, 247),
	      "shared outer/loop merge SPIR-V lacks OpSelectionMerge");
	Check(!SpirvContainsOpcode(result.spirv, 251),
	      "shared outer/loop merge unexpectedly used dispatcher OpSwitch");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerCfgLoopSharedContinueSelectionMerges() {
	const uint32_t shader[] = {
	    EncodeSMovB32(0, 128),       // s0 = 0
	    EncodeSopc(0x0a, 0, 130),    // loop: s_cmp_lt_u32 s0, 2
	    EncodeSopp(0x04, 9),         // loop exit -> end
	    EncodeSopc(0x06, 1, 1),      // s_cmp_eq_u32 s1, s1
	    EncodeSopp(0x05, 5),         // first early continue -> continue block
	    EncodeSopc(0x06, 2, 2),      // s_cmp_eq_u32 s2, s2
	    EncodeSopp(0x05, 3),         // second early continue -> continue block
	    EncodeSopc(0x06, 3, 3),      // s_cmp_eq_u32 s3, s3
	    EncodeSopp(0x05, 1),         // third early continue -> continue block
	    EncodeSMovB32(4, 129),       // fallthrough work
	    EncodeSop2(0x00, 0, 0, 129), // continue: s_add_u32 s0, s0, 1
	    EncodeSopp(0x02, 0xfff5u),   // backedge -> loop header
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.ir_dump, "mode=structured"),
	      "shared loop continue selections should stay on structured path");
	Check(!Common::ContainsStr(result.ir_dump, "duplicate structured merge block"),
	      "shared loop continue selections were not split before structurization");
	Check(SpirvContainsOpcode(result.spirv, 246),
	      "shared loop continue selections SPIR-V lacks OpLoopMerge");
	Check(SpirvContainsOpcode(result.spirv, 247),
	      "shared loop continue selections SPIR-V lacks OpSelectionMerge");
	Check(!SpirvContainsOpcode(result.spirv, 251),
	      "shared loop continue selections unexpectedly used dispatcher OpSwitch");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerCfgDuplicateMergeStructuredSplit() {
	const uint32_t shader[] = {
	    EncodeSopc(0x06, 0, 0), // s_cmp_eq_u32 s0, s0
	    EncodeSopp(0x05, 2),    // first early exit -> end
	    EncodeSopp(0x05, 1),    // second early exit -> same end merge
	    EncodeSMovB32(0, 129),  // fallthrough work
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.ir_dump, "mode=structured"),
	      "duplicate merge CFG did not stay on structured path");
	Check(!Common::ContainsStr(result.ir_dump, "duplicate structured merge block"),
	      "duplicate merge CFG was not split before structurization");
	Check(SpirvContainsOpcode(result.spirv, 247), "duplicate merge SPIR-V lacks OpSelectionMerge");
	Check(!SpirvContainsOpcode(result.spirv, 251),
	      "duplicate merge CFG unexpectedly used dispatcher OpSwitch");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerCfgIrreducibleDispatcher() {
	const uint32_t shader[] = {
	    EncodeSopp(0x05, 2),       // entry -> B, fallthrough A
	    EncodeSopp(0x02, 0),       // A -> C
	    EncodeSopp(0x05, 0xfffeu), // C -> A, fallthrough B
	    EncodeSopp(0x02, 0xfffeu), // B -> C
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.ir_dump, "irreducible CFG"),
	      "irreducible CFG reason was not retained");
	Check(Common::ContainsStr(result.ir_dump, "mode=dispatcher"),
	      "irreducible CFG did not select dispatcher fallback");
	Check(SpirvContainsOpcode(result.spirv, 246), "dispatcher SPIR-V lacks OpLoopMerge");
	Check(SpirvContainsOpcode(result.spirv, 251), "dispatcher SPIR-V lacks OpSwitch");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerExecMaskHelpers() {
	using namespace ShaderRecompiler::Exec;

	State wave32;
	wave32.wave_size = 32;
	WriteExec(wave32, FullMask(64));
	Check(ReadExec(wave32).low == 0xffffffffu && ReadExec(wave32).high == 0u,
	      "wave32 EXEC high half was not masked off");
	AndExec(wave32, {0x0fu, 0xffffffffu});
	Check(ReadExec(wave32).low == 0x0fu && ReadExec(wave32).high == 0u,
	      "wave32 AndExec did not preserve wave size");
	InvertExec(wave32);
	Check(ReadExec(wave32).low == 0xfffffff0u && ReadExec(wave32).high == 0u,
	      "wave32 InvertExec did not preserve wave size");
	WriteExec(wave32, {0u, 0xffffffffu});
	Check(IsExecZero(wave32), "wave32 IsExecZero used inactive high lanes");

	State wave64;
	wave64.wave_size = 64;
	WriteExec(wave64, FullMask(64));
	Check(ReadExec(wave64).low == 0xffffffffu && ReadExec(wave64).high == 0xffffffffu,
	      "wave64 EXEC full mask was wrong");
	XorExec(wave64, {0xffffffffu, 0u});
	Check(ReadExec(wave64).low == 0u && ReadExec(wave64).high == 0xffffffffu,
	      "wave64 XorExec did not update both halves correctly");
	wave64.vcc = {0u, 1u};
	Check(!IsVccZero(wave64), "wave64 VCC high half was ignored");
}

void TestComputeShaderInputWaveSize() {
	const auto decode_wave_size = [](uint32_t rsrc1) {
		const bool wave32 = (((rsrc1 >> Pm4::COMPUTE_PGM_RSRC1_W32_EN_SHIFT) &
		                      Pm4::COMPUTE_PGM_RSRC1_W32_EN_MASK) != 0u);
		return wave32 ? 32u : 64u;
	};

	constexpr uint32_t observed_wave32_rsrc1_a = 0x402c0146u;
	constexpr uint32_t observed_wave32_rsrc1_b = 0x402c00c1u;
	Check(decode_wave_size(observed_wave32_rsrc1_a) == 32u,
	      "COMPUTE_PGM_RSRC1 W32_EN bit did not match observed PS5 value A");
	Check(decode_wave_size(observed_wave32_rsrc1_b) == 32u,
	      "COMPUTE_PGM_RSRC1 W32_EN bit did not match observed PS5 value B");

	constexpr uint32_t w32_en_bit = 1u << Pm4::COMPUTE_PGM_RSRC1_W32_EN_SHIFT;
	Check(decode_wave_size(observed_wave32_rsrc1_a & ~w32_en_bit) == 64u,
	      "cleared COMPUTE_PGM_RSRC1 W32_EN bit did not decode as wave64");
}

void TestNewShaderRecompilerWave32MasksExecHighStores() {
	const uint32_t shader[] = {
	    EncodeSop1(0x04, 126, 193), // s_mov_b64 exec, -1
	    EncodeSopp(0x01),
	};

	ShaderRecompiler::CompileOptions options;
	options.stage     = ShaderType::Compute;
	options.dump_ir   = true;
	options.wave_size = 32;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "s_mov_b64 exec_lo, -1"),
	      "wave32 EXEC mask regression did not decode s_mov_b64 exec, -1");
	CheckSpirvBinaryValidates(result.spirv);

	const auto source = DisassembleSpirvBinary(result.spirv);
	Check(CountSourceOccurrences(source, "OpStore %exec_hi %uint_0") >= 2u,
	      "wave32 EXEC high half was not clamped to zero in SPIR-V stores");
	Check(!Common::ContainsStr(source, "OpStore %exec_hi %uint_4294967295"),
	      "wave32 EXEC high half was stored as a full 64-lane mask");
}

void TestNewShaderRecompilerWave32VccHighScalarStores() {
	const uint32_t shader[] = {
	    EncodeSmem0(0x09, 106, 4),
	    0u, // s_buffer_load_dwordx2 vcc_lo
	    EncodeSopp(0x01),
	};

	ShaderRecompiler::CompileOptions options;
	options.stage     = ShaderType::Compute;
	options.dump_ir   = true;
	options.wave_size = 32;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.ir_dump, "SBufferLoadDword vcc_hi"),
	      "wave32 VCC scalar regression did not lower the high VCC dword load");
	CheckSpirvBinaryValidates(result.spirv);

	const auto source = DisassembleSpirvBinary(result.spirv);
	Check(CountSourceOccurrences(source, "OpStore %vcc_hi") >= 2u,
	      "wave32 VCC high scalar load did not store to vcc_hi");
	Check(CountSourceOccurrences(source, "OpStore %vcc_hi %uint_0") == 1u,
	      "wave32 VCC high scalar load was clamped to zero instead of preserving data");
}

void TestNewShaderRecompilerCompareMaskIsFullWaveBallot() {
	const uint32_t shader[] = {
	    EncodeVopc(0xc1, 0 + 256, 1),  // v_cmp_lt_u32 vcc, v0, v1
	    EncodeSop2(0x0f, 2, 126, 106), // s_and_b64 s[2:3], exec, vcc
	    EncodeSop1(0x04, 126, 2),      // s_mov_b64 exec, s[2:3]
	    EncodeSopp(0x01),
	};

	ShaderRecompiler::CompileOptions options;
	options.stage     = ShaderType::Compute;
	options.dump_ir   = true;
	options.wave_size = 32;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "v_cmp_lt_u32"),
	      "compare-mask ballot regression did not decode V_CMP_LT_U32");
	Check(Common::ContainsStr(result.ir_dump, "CompareLtU32 vcc_lo"),
	      "compare-mask ballot regression did not lower compare to VCC");
	Check(Common::ContainsStr(result.ir_dump, "BitwiseAndU64 s2, exec_lo, vcc_lo"),
	      "compare-mask ballot regression did not consume VCC through scalar mask ALU");
	CheckSpirvBinaryValidates(result.spirv);

	const auto source = DisassembleSpirvBinary(result.spirv);
	Check(Common::ContainsStr(source, "OpGroupNonUniformBallot"),
	      "compare-mask result was not materialized with a subgroup ballot");
	Check(Common::ContainsStr(source, "OpStore %vcc_lo"),
	      "compare-mask ballot regression did not store VCC low mask");
	Check(Common::ContainsStr(source, "OpStore %vcc_hi %uint_0"),
	      "wave32 compare-mask VCC high half was not cleared");
}

void TestNewShaderRecompilerBufferLoadsGuardedByExec() {
	const uint32_t shader[] = {
	    EncodeMubuf0(0x0c),
	    EncodeMubuf1(0, 0, 1), // buffer_load_dword v0
	    EncodeSopp(0x01),
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "buffer_load_dword"),
	      "buffer load guard regression did not decode MUBUF load");
	CheckSpirvBinaryValidates(result.spirv);

	const auto source       = DisassembleSpirvBinary(result.spirv);
	const auto exec_branch  = Common::FindIndex(source, std::string("OpBranchConditional"), 0);
	const auto array_length = Common::FindIndex(source, std::string("OpArrayLength"), 0);
	const auto bounds_branch =
	    Common::FindIndex(source, std::string("OpBranchConditional"), array_length);
	const auto element_access =
	    Common::FindIndex(source, std::string("OpAccessChain %_ptr_StorageBuffer_uint"), 0);
	Check(exec_branch != Common::FIND_INVALID_INDEX, "buffer load SPIR-V lacks EXEC guard branch");
	Check(array_length != Common::FIND_INVALID_INDEX,
	      "buffer load SPIR-V lacks storage buffer array-length bounds check");
	Check(bounds_branch != Common::FIND_INVALID_INDEX,
	      "buffer load SPIR-V lacks storage buffer bounds branch");
	Check(element_access != Common::FIND_INVALID_INDEX,
	      "buffer load SPIR-V lacks storage element access");
	Check(exec_branch < array_length, "buffer load bounds check was emitted outside EXEC guard");
	Check(bounds_branch < element_access,
	      "buffer load storage element pointer was formed before bounds guard");
}

void TestNewShaderRecompilerBufferAtomicsGuardedByBounds() {
	const uint32_t shader[] = {
	    EncodeMubuf0(0x32, 0, true, true),
	    EncodeMubuf1(0, 0, 1), // buffer_atomic_add
	    EncodeSopp(0x01),
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "buffer_atomic_add"),
	      "buffer atomic bounds regression did not decode MUBUF atomic");
	CheckSpirvBinaryValidates(result.spirv);

	const auto source       = DisassembleSpirvBinary(result.spirv);
	const auto array_length = Common::FindIndex(source, std::string("OpArrayLength"), 0);
	const auto bounds_branch =
	    Common::FindIndex(source, std::string("OpBranchConditional"), array_length);
	const auto atomic         = Common::FindIndex(source, std::string("OpAtomicIAdd"), 0);
	const auto memory_barrier = Common::FindIndex(source, std::string("OpMemoryBarrier"), atomic);
	Check(array_length != Common::FIND_INVALID_INDEX,
	      "buffer atomic SPIR-V lacks storage buffer array-length bounds check");
	Check(bounds_branch != Common::FIND_INVALID_INDEX,
	      "buffer atomic SPIR-V lacks storage buffer bounds branch");
	Check(atomic != Common::FIND_INVALID_INDEX, "buffer atomic SPIR-V lacks atomic operation");
	Check(memory_barrier != Common::FIND_INVALID_INDEX,
	      "buffer atomic SPIR-V lacks memory barrier after atomic operation");
	Check(bounds_branch < atomic, "buffer atomic was emitted before bounds guard");
	Check(atomic < memory_barrier, "buffer atomic memory barrier was emitted before atomic");
}

void TestNewShaderRecompilerBranchConditionForms() {
	struct Case {
		uint32_t    opcode;
		const char* condition;
	};
	const Case cases[] = {
	    {0x04, "condition=scc0"},
	    {0x08, "condition=execz"},
	    {0x07, "condition=vccnz"},
	};

	for (const auto& c: cases) {
		const uint32_t shader[] = {
		    EncodeSopp(c.opcode, 1),
		    EncodeSMovB32(0, 129),
		    0xbf810000u,
		};

		ShaderRecompiler::CompileOptions options;
		options.stage   = ShaderType::Compute;
		options.dump_ir = true;

		ShaderRecompiler::CompileResult result;
		std::string                     error;
		Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
		Check(Common::ContainsStr(result.ir_dump, c.condition),
		      "branch condition was not preserved");
		Check(SpirvContainsOpcode(result.spirv, 250),
		      "branch condition SPIR-V lacks OpBranchConditional");
		CheckSpirvBinaryValidates(result.spirv);
		if (c.opcode == 0x04) {
			const auto source = DisassembleSpirvBinary(result.spirv);
			Check(!Common::ContainsStr(source, "OpGroupNonUniformBallot"),
			      "structured SCC branch retained a host-subgroup reduction");
		}
	}
}

void TestNewShaderRecompilerSetpcBranch() {
	const uint32_t shader[] = {
	    EncodeSop1(0x1f, 4, 0),      // s_getpc_b64 s[4:5]
	    EncodeSop2(0x00, 4, 4, 140), // s_add_u32 s4, s4, 12
	    EncodeSop1(0x20, 0, 4),      // s_setpc_b64 s[4:5]
	    EncodeSMovB32(0, 129),       0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "s_setpc_b64"), "S_SETPC_B64 was not decoded");
	Check(Common::ContainsStr(result.ir_dump, "successors=["),
	      "S_SETPC_B64 did not participate in CFG");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerSetpcJumpTable() {
	const uint32_t shader[] = {
	    EncodeSop2(0x07, 0, 0, 129), // s_min_u32 s0, s0, 1
	    EncodeSop2(0x1e, 0, 0, 131), // s_lshl_b32 s0, s0, 3
	    EncodeSop1(0x1f, 8, 0),      // s_getpc_b64 s[8:9]
	    EncodeSop2(0x00, 8, 8, 255), // s_add_u32 s8, s8, literal
	    0x00000038u,
	    EncodeSop2(0x04, 9, 9, 128), // s_addc_u32 s9, s9, 0
	    EncodeSmem0(0x01, 4, 4),
	    0u,                          // s_load_dwordx2 s[4:5], s[8:9], s0
	    EncodeSopp(0x0c, 0),         // s_waitcnt 0
	    EncodeSop1(0x1f, 10, 0),     // s_getpc_b64 s[10:11]
	    EncodeSop2(0x00, 10, 10, 4), // s_add_u32 s10, s10, s4
	    EncodeSop2(0x04, 11, 11, 5), // s_addc_u32 s11, s11, s5
	    EncodeSop1(0x20, 0, 10),     // s_setpc_b64 s[10:11]
	    EncodeSMovB32(1, 129),       // case 0
	    EncodeSopp(0x02, 1),
	    EncodeSMovB32(2, 129), // case 1
	    EncodeSopp(0x01, 0),
	    0x0000000cu,
	    0x00000000u, // table: target case 0 relative to pc 0x28
	    0x00000014u,
	    0x00000000u, // table: target case 1 relative to pc 0x28
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Compute;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.ir_dump, "mode=dispatcher"),
	      "S_SETPC_B64 jump table did not select dispatcher fallback");
	Check(Common::ContainsStr(result.ir_dump, "indirect_targets=2"),
	      "S_SETPC_B64 jump table did not retain both targets");
	Check(Common::ContainsStr(result.ir_dump, "selector_values=2"),
	      "S_SETPC_B64 jump table did not retain selector mapping");
	Check(!Common::ContainsStr(result.ir_dump, "SLoadDword"),
	      "S_SETPC_B64 jump table load was reflected as a raw scalar buffer load");
	Check(SpirvContainsOpcode(result.spirv, 251), "dispatcher SPIR-V lacks OpSwitch");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerExpVertexOutputs() {
	const uint32_t shader[] = {
	    EncodeExp0(0x0c, 0xf), EncodeExp1(0, 1, 2, 3), // POS0
	    EncodeExp0(0x20, 0xf), EncodeExp1(4, 5, 6, 7), // PARAM0
	    EncodeExp0(0x14, 0x1), EncodeExp1(8, 0, 0, 0), // PRIM
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Vertex;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.decoded_dump, "target=0x0c"), "POS export was not decoded");
	Check(Common::ContainsStr(result.decoded_dump, "target=0x20"), "PARAM export was not decoded");
	Check(Common::ContainsStr(result.decoded_dump, "target=0x14"), "PRIM export was not decoded");
	Check(Common::ContainsStr(result.ir_dump, "position"), "POS export did not reach IR");
	Check(Common::ContainsStr(result.ir_dump, "parameter"), "PARAM export did not reach IR");
	Check(Common::ContainsStr(result.ir_dump, "primitive"), "PRIM export did not reach IR");
	Check(SpirvContainsOpcode(result.spirv, 62), "vertex export SPIR-V lacks OpStore");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerVertexSystemVgprs() {
	using StageInputKind = ShaderRecompiler::IR::StageInputKind;

	const uint32_t shader[] = {
	    EncodeVop1(0x01, 0, 5 + 256), // v_mov_b32 v0, v5
	    EncodeVop1(0x01, 1, 8 + 256), // v_mov_b32 v1, v8
	    EncodeExp0(0x0c, 0xf),
	    EncodeExp1(0, 1, 2, 3), // POS0
	    EncodeExp0(0x20, 0x3),
	    EncodeExp1(0, 1, 0, 0), // PARAM0
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Vertex;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(ProgramHasInput(result.program, StageInputKind::VertexIndex),
	      "vertex shader missing VertexIndex input");
	Check(ProgramHasInput(result.program, StageInputKind::InstanceIndex),
	      "vertex shader missing InstanceIndex input");
	Check(Common::ContainsStr(result.ir_dump, "MoveU32 v0, v5"),
	      "vertex shader did not keep v5 as a guest VGPR source");
	Check(Common::ContainsStr(result.ir_dump, "MoveU32 v1, v8"),
	      "vertex shader did not keep v8 as a guest VGPR source");
	CheckSpirvBinaryValidates(result.spirv);

	const auto source = DisassembleSpirvBinary(result.spirv);
	Check(CountSourceOccurrences(source, "OpLoad %int %gl_VertexIndex") == 1u,
	      "vertex SPIR-V does not load gl_VertexIndex");
	Check(CountSourceOccurrences(source, "OpLoad %int %gl_InstanceIndex") == 1u,
	      "vertex SPIR-V does not load gl_InstanceIndex");
	Check(CountSourceOccurrences(source, "OpStore %v5") >= 2u,
	      "vertex SPIR-V does not seed guest v5 from gl_VertexIndex");
	Check(CountSourceOccurrences(source, "OpStore %v8") >= 2u,
	      "vertex SPIR-V does not seed guest v8 from gl_InstanceIndex");
}

void TestNewShaderRecompilerVertexExportUsesLaneExecMask() {
	const uint32_t shader[] = {
	    EncodeSop1(0x04, 126, 129), // s_mov_b64 exec, 1
	    EncodeExp0(0x0c, 0xf),
	    EncodeExp1(0, 1, 2, 3), // POS0
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Vertex;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	CheckSpirvBinaryValidates(result.spirv);

	const auto source = DisassembleSpirvBinary(result.spirv);
	Check(Common::ContainsStr(source, "gl_SubgroupInvocationID"),
	      "vertex export EXEC guard did not test the current lane");
	Check(Common::ContainsStr(source, "OpBranchConditional"),
	      "vertex export EXEC guard did not branch on the lane-active mask");

	options.lane_mask_mode = ShaderLaneMaskMode::PerInvocation;
	ShaderRecompiler::CompileResult local_result;
	Check(ShaderRecompiler::TryRecompile(shader, options, &local_result, &error), error.c_str());
	Check(local_result.program.lane_mask_mode == ShaderLaneMaskMode::PerInvocation,
	      "per-invocation mask mode was not preserved in the immutable program");
	CheckSpirvBinaryValidates(local_result.spirv);
	const auto local_source = DisassembleSpirvBinary(local_result.spirv);
	Check(!Common::ContainsStr(local_source, "OpLoad %uint %gl_SubgroupInvocationID"),
	      "per-invocation EXEC still depends on the native subgroup lane");
	Check(Common::ContainsStr(local_source, "OpBranchConditional"),
	      "per-invocation vertex export lost its EXEC guard");
}

void TestNewShaderRecompilerPerInvocationMasks() {
	const uint32_t local_shader[] = {
	    EncodeVopc(0xc1, 0 + 256, 1),    // v_cmp_lt_u32 vcc, v0, v1
	    EncodeSop2(0x0f, 2, 126, 106),   // s_and_b64 s[2:3], exec, vcc
	    EncodeSop1(0x24, 4, 126),        // s_and_saveexec_b64 s[4:5], exec
	    EncodeSop2(0x25, 126, 132, 128), // s_bfm_b64 exec, 4, 0
	    EncodeSop1(0x08, 126, 126),      // s_not_b64 exec, exec
	    EncodeSop1(0x08, 126, 126),      // s_not_b64 exec, exec
	    EncodeExp0(0x0c, 0xf),
	    EncodeExp1(0, 1, 2, 3), // POS0
	    EncodeSopp(0x01),
	};

	ShaderRecompiler::CompileOptions options;
	options.stage          = ShaderType::Vertex;
	options.lane_mask_mode = ShaderLaneMaskMode::PerInvocation;
	options.dump_ir        = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(local_shader, options, &result, &error), error.c_str());
	CheckSpirvBinaryValidates(result.spirv);
	const auto source = DisassembleSpirvBinary(result.spirv);
	Check(!Common::ContainsStr(source, "OpGroupNonUniformBallot"),
	      "per-invocation VCC producer still materialized a shared subgroup mask");
	Check(!Common::ContainsStr(source, "OpLoad %uint %gl_SubgroupInvocationID"),
	      "per-invocation BFM EXEC prefix still selected native subgroup lanes");
	Check(Common::ContainsStr(source, "OpLogicalAnd") &&
	          Common::ContainsStr(source, "OpLogicalNot"),
	      "per-invocation wide mask ALU did not use Boolean operations");
	Check(Common::ContainsStr(source, "OpStore %vcc_lo") &&
	          Common::ContainsStr(source, "OpStore %vcc_hi %uint_0"),
	      "per-invocation comparison did not store a local Boolean VCC pair");
	Check(Common::ContainsStr(result.ir_dump, "SaveexecB64 s4, exec_lo"),
	      "per-invocation SAVEEXEC regression did not reach IR");

	const uint32_t wqm_shader[] = {
	    EncodeSop1(0x0a, 2, 126), // s_wqm_b64 s[2:3], exec
	    EncodeExp0(0x0c, 0xf),
	    EncodeExp1(0, 1, 2, 3), // POS0
	    EncodeSopp(0x01),
	};
	Check(ShaderRecompiler::TryRecompile(wqm_shader, options, &result, &error), error.c_str());
	CheckSpirvBinaryValidates(result.spirv);
	const auto wqm_source = DisassembleSpirvBinary(result.spirv);
	Check(Common::ContainsStr(wqm_source, "OpCapability GroupNonUniformBallot") &&
	          Common::ContainsStr(wqm_source, "OpGroupNonUniformBallot"),
	      "per-invocation scalar WQM omitted its subgroup ballot capability");

	const uint32_t cross_lane_shader[] = {
	    EncodeSop2(0x25, 126, 132, 128), // s_bfm_b64 exec, 4, 0
	    EncodeVop1(0x01, 0, 250),
	    EncodeVop1Dpp(1), // v_mov_b32 v0, v1 dpp
	    EncodeExp0(0x0c, 0xf),
	    EncodeExp1(0, 1, 2, 3), // POS0
	    EncodeSopp(0x01),
	};
	Check(ShaderRecompiler::TryRecompile(cross_lane_shader, options, &result, &error),
	      error.c_str());
	CheckSpirvBinaryValidates(result.spirv);
	Check(Common::ContainsStr(DisassembleSpirvBinary(result.spirv), "OpGroupNonUniformBallot"),
	      "per-invocation cross-lane EXEC was not reconstructed as a subgroup ballot");
}

void TestNewShaderRecompilerExpPixelOutputs() {
	const uint32_t shader[] = {
	    EncodeExp0(0x00, 0xf),
	    EncodeExp1(0, 1, 2, 3), // MRT0
	    EncodeExp0(0x08, 0x1),
	    EncodeExp1(4, 0, 0, 0), // MRTZ depth
	    EncodeExp0(0x09, 0x0),
	    EncodeExp1(0, 0, 0, 0), // NULL
	    EncodeExp0(0x00, 0xf, true, true),
	    EncodeExp1(5, 6, 0, 0), // compressed MRT0
	    0xbf810000u,
	};

	ShaderRecompiler::CompileOptions options;
	options.stage   = ShaderType::Pixel;
	options.dump_ir = true;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(Common::ContainsStr(result.ir_dump, "mrt"), "MRT export did not reach IR");
	Check(Common::ContainsStr(result.ir_dump, "mrtz"), "MRTZ export did not reach IR");
	Check(Common::ContainsStr(result.ir_dump, "null"), "NULL export did not reach IR");
	Check(Common::ContainsStr(result.ir_dump, "compr=1"), "compressed MRT export did not reach IR");
	Check(SpirvContainsOpcode(result.spirv, 62), "pixel export SPIR-V lacks OpStore");
	Check(SpirvContainsExtInst(result.spirv, 62),
	      "compressed pixel export SPIR-V lacks GLSL.std.450 UnpackHalf2x16");
	Check(SpirvContainsOpcode(result.spirv, 81),
	      "compressed pixel export SPIR-V lacks OpCompositeExtract");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerEarlyZDisabledWhenPixelKillEnabled() {
	constexpr uint32_t ExecutionModeEarlyFragmentTests = 9;

	const uint32_t shader[] = {
	    EncodeExp0(0x00, 0xf, true, false, true),
	    EncodeExp1(0, 1, 2, 3),
	    0xbf810000u,
	};

	ShaderPixelInputInfo ps_info;
	ps_info.ps_early_z           = true;
	ps_info.ps_pixel_kill_enable = true;

	ShaderRecompiler::CompileOptions options;
	options.stage            = ShaderType::Pixel;
	options.pixel_input_info = &ps_info;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, options, &result, &error), error.c_str());
	Check(SpirvContainsOpcode(result.spirv, 252),
	      "pixel valid-mask export should still lower to OpKill");
	Check(!SpirvContainsExecutionMode(result.spirv, ExecutionModeEarlyFragmentTests),
	      "pixel shaders that may kill fragments must not request EarlyFragmentTests");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerNativeBindingPlan() {
	using BindingKind = ShaderRecompiler::IR::DescriptorBindingKind;

	const uint32_t shader[] = {
	    EncodeSmem0(0x08, 0, 4),
	    0u, // s_buffer_load_dword s0
	    EncodeMubuf0(0x1c, 8),
	    EncodeMubuf1(0, 1, 1), // buffer_store_dword
	    EncodeMimg0(0x20, 0xf),
	    EncodeMimg1(8, 2, 3, 1), // image_sample
	    EncodeMimg0(0x08, 0xf),
	    EncodeMimg1(12, 5, 0, 1), // image_store
	    EncodeMimg0(0x11, 0x1),
	    EncodeMimg1(16, 6, 0, 1), // image_atomic_add
	    0xbf810000u,
	};

	auto                             user_data = ImageTestUserData();
	ShaderRecompiler::CompileOptions options;
	options.stage     = ShaderType::Compute;
	options.dump_ir   = true;
	options.user_data = user_data.data();

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	const auto compiled = ShaderRecompiler::TryRecompile(shader, options, &result, &error);
	Check(compiled, error.c_str());
	const auto* buffers =
	    ShaderRecompiler::IR::FindBinding(result.program.bindings, BindingKind::Buffers);
	const auto* sampled =
	    ShaderRecompiler::IR::FindBinding(result.program.bindings, BindingKind::Sampled2D);
	const auto* storage =
	    ShaderRecompiler::IR::FindBinding(result.program.bindings, BindingKind::Storage2D);
	const auto* uint_storage =
	    ShaderRecompiler::IR::FindBinding(result.program.bindings, BindingKind::StorageUint2D);
	const auto* samplers =
	    ShaderRecompiler::IR::FindBinding(result.program.bindings, BindingKind::Samplers);
	Check(buffers != nullptr && buffers->resources.size() == 2,
	      "native binding plan did not allocate scalar/vector buffers");
	Check(sampled != nullptr && sampled->resources.size() == 1,
	      "native binding plan did not allocate the sampled image");
	Check(storage != nullptr && storage->resources.size() == 1,
	      "native binding plan did not allocate the float storage image");
	Check(uint_storage != nullptr && uint_storage->resources.size() == 1,
	      "native binding plan did not allocate the uint storage image");
	Check(samplers != nullptr && samplers->resources.size() == 1,
	      "native binding plan did not allocate the sampler");
	Check(SpirvContainsOpcode(result.spirv, 86),
	      "SPIR-V binary does not combine separate image/sampler descriptors");
	Check(SpirvHasDecorationValue(result.spirv, 33u, buffers->binding),
	      "SPIR-V lacks storage-buffer Binding decoration");
	Check(SpirvHasDecorationValue(result.spirv, 33u, sampled->binding),
	      "SPIR-V lacks sampled-image Binding decoration");
	Check(SpirvHasDecorationValue(result.spirv, 33u, samplers->binding),
	      "SPIR-V lacks sampler Binding decoration");
	Check(SpirvHasDecorationValue(result.spirv, 34u, result.program.bindings.descriptor_set),
	      "SPIR-V lacks DescriptorSet decoration");
	CheckSpirvBinaryValidates(result.spirv);

	auto malformed = result.program;
	auto malformed_buffers =
	    std::find_if(malformed.bindings.descriptors.begin(), malformed.bindings.descriptors.end(),
	                 [](const auto& binding) { return binding.kind == BindingKind::Buffers; });
	Check(malformed_buffers != malformed.bindings.descriptors.end(),
	      "native validation fixture lacks a buffer group");
	malformed_buffers->resources.clear();
	std::vector<uint32_t> rejected_spirv  = {0xdeadbeefu};
	const auto            rejected_before = rejected_spirv;
	std::string           rejected_error;
	Check(!ShaderRecompiler::Spirv::EmitProgram(malformed, result.resources, nullptr, nullptr,
	                                            nullptr, &rejected_spirv, &rejected_error) &&
	          rejected_spirv == rejected_before &&
	          rejected_error.find("topology") != std::string::npos,
	      "malformed native binding plan was not rejected transactionally");
	const auto FindBufferInstruction = [](auto* program) {
		for (auto& block: program->blocks) {
			const auto found = std::find_if(
			    block.instructions.begin(), block.instructions.end(), [](const auto& inst) {
				    return inst.memory.kind == ShaderRecompiler::IR::ResourceKind::Buffer;
			    });
			if (found != block.instructions.end()) {
				return &*found;
			}
		}
		return static_cast<ShaderRecompiler::IR::Instruction*>(nullptr);
	};

	auto  malformed_inst = result.program;
	auto* buffer_inst    = FindBufferInstruction(&malformed_inst);
	Check(buffer_inst != nullptr, "native validation fixture lacks a buffer instruction");
	buffer_inst->op          = ShaderRecompiler::IR::Opcode::FlatLoadDword;
	buffer_inst->memory.kind = ShaderRecompiler::IR::ResourceKind::Flat;
	buffer_inst->src_count   = 1;
	rejected_spirv           = rejected_before;
	rejected_error.clear();
	Check(!ShaderRecompiler::Spirv::EmitProgram(malformed_inst, result.resources, nullptr, nullptr,
	                                            nullptr, &rejected_spirv, &rejected_error) &&
	          rejected_spirv == rejected_before &&
	          rejected_error.find("contract") != std::string::npos,
	      "malformed native instruction was not rejected before fatal emitter paths");

	auto missing_buffer_operand = result.program;
	buffer_inst                 = FindBufferInstruction(&missing_buffer_operand);
	Check(buffer_inst != nullptr, "native validation fixture lacks a buffer-arity candidate");
	buffer_inst->src_count--;
	rejected_spirv = rejected_before;
	rejected_error.clear();
	Check(!ShaderRecompiler::Spirv::EmitProgram(missing_buffer_operand, result.resources, nullptr,
	                                            nullptr, nullptr, &rejected_spirv,
	                                            &rejected_error) &&
	          rejected_spirv == rejected_before &&
	          rejected_error.find("contract") != std::string::npos,
	      "buffer instruction with a missing address operand was accepted");

	auto extra_buffer_operand = result.program;
	buffer_inst               = FindBufferInstruction(&extra_buffer_operand);
	Check(buffer_inst != nullptr, "native validation fixture lacks an extra-operand candidate");
	buffer_inst->src_count++;
	rejected_spirv = rejected_before;
	rejected_error.clear();
	Check(!ShaderRecompiler::Spirv::EmitProgram(extra_buffer_operand, result.resources, nullptr,
	                                            nullptr, nullptr, &rejected_spirv,
	                                            &rejected_error) &&
	          rejected_spirv == rejected_before &&
	          rejected_error.find("contract") != std::string::npos,
	      "buffer instruction with an extra address operand was accepted");

	auto  malformed_atomic = result.program;
	auto* atomic_inst      = FindBufferInstruction(&malformed_atomic);
	Check(atomic_inst != nullptr, "native validation fixture lacks an atomic candidate");
	atomic_inst->op          = ShaderRecompiler::IR::Opcode::AtomicAddU32;
	atomic_inst->memory.kind = ShaderRecompiler::IR::ResourceKind::ScalarBuffer;
	atomic_inst->src_count   = 2;
	rejected_spirv           = rejected_before;
	rejected_error.clear();
	Check(
	    !ShaderRecompiler::Spirv::EmitProgram(malformed_atomic, result.resources, nullptr, nullptr,
	                                          nullptr, &rejected_spirv, &rejected_error) &&
	        rejected_spirv == rejected_before && rejected_error.find("atomic") != std::string::npos,
	    "malformed atomic instruction reached a fatal descriptor path");

	auto overwide_atomic = result.program;
	atomic_inst          = FindBufferInstruction(&overwide_atomic);
	Check(atomic_inst != nullptr, "native validation fixture lacks an overwide atomic candidate");
	atomic_inst->op        = ShaderRecompiler::IR::Opcode::AtomicAddU32;
	atomic_inst->src_count = 5;
	rejected_spirv         = rejected_before;
	rejected_error.clear();
	Check(!ShaderRecompiler::Spirv::EmitProgram(overwide_atomic, result.resources, nullptr, nullptr,
	                                            nullptr, &rejected_spirv, &rejected_error) &&
	          rejected_spirv == rejected_before &&
	          rejected_error.find("fixed IR storage") != std::string::npos,
	      "overwide atomic operands reached fixed emitter storage");

	auto  overwide_move = result.program;
	auto& move_inst     = overwide_move.blocks.front().instructions.front();
	move_inst.op        = ShaderRecompiler::IR::Opcode::MoveU32;
	move_inst.memory    = {};
	move_inst.src_count = 5;
	rejected_spirv      = rejected_before;
	rejected_error.clear();
	Check(!ShaderRecompiler::Spirv::EmitProgram(overwide_move, result.resources, nullptr, nullptr,
	                                            nullptr, &rejected_spirv, &rejected_error) &&
	          rejected_spirv == rejected_before &&
	          rejected_error.find("fixed IR storage") != std::string::npos,
	      "overwide non-memory operands reached fixed emitter storage");

	auto  malformed_image = result.program;
	auto* image_inst      = static_cast<ShaderRecompiler::IR::Instruction*>(nullptr);
	for (auto& block: malformed_image.blocks) {
		const auto found = std::find_if(
		    block.instructions.begin(), block.instructions.end(),
		    [](const auto& inst) { return inst.op == ShaderRecompiler::IR::Opcode::ImageSample; });
		if (found != block.instructions.end()) {
			image_inst = &*found;
			break;
		}
	}
	Check(image_inst != nullptr, "native validation fixture lacks a sampled image");
	image_inst->src_count = 0;
	rejected_spirv        = rejected_before;
	rejected_error.clear();
	Check(!ShaderRecompiler::Spirv::EmitProgram(malformed_image, result.resources, nullptr, nullptr,
	                                            nullptr, &rejected_spirv, &rejected_error) &&
	          rejected_spirv == rejected_before &&
	          rejected_error.find("sampled image") != std::string::npos,
	      "malformed image operands reached fixed emitter storage");

	auto     stale_resources = result.resources;
	uint32_t float_storage   = UINT32_MAX;
	for (uint32_t i = 0; i < result.program.info.images.size(); i++) {
		if (result.program.info.images[i].kind ==
		    ShaderRecompiler::IR::ResourceKind::StorageImage) {
			float_storage = i;
			break;
		}
	}
	Check(float_storage != UINT32_MAX, "native validation fixture lacks float storage image");
	stale_resources.images[float_storage].dwords[1] = 20u << 20u;
	rejected_spirv                                  = rejected_before;
	rejected_error.clear();
	Check(!ShaderRecompiler::Spirv::EmitProgram(result.program, stale_resources, nullptr, nullptr,
	                                            nullptr, &rejected_spirv, &rejected_error) &&
	          rejected_spirv == rejected_before &&
	          rejected_error.find("specialized format") != std::string::npos,
	      "stale runtime image format re-selected an absent descriptor group");

	auto stale_dimension = result.resources;
	stale_dimension.images[float_storage].dwords[3] &= 0x0fffffffu;
	rejected_spirv = rejected_before;
	rejected_error.clear();
	Check(!ShaderRecompiler::Spirv::EmitProgram(result.program, stale_dimension, nullptr, nullptr,
	                                            nullptr, &rejected_spirv, &rejected_error) &&
	          rejected_spirv == rejected_before &&
	          rejected_error.find("specialized dimension") != std::string::npos,
	      "unsupported runtime image type bypassed specialization validation");
}

bool LowerProvenanceOnly(const uint32_t* code, uint32_t words, ShaderRecompiler::IR::Program* ir,
                         std::string* error) {
	ShaderRecompiler::Decoder::Program decoded;
	if (!ShaderRecompiler::Decoder::DecodeProgram(std::span {code, words}, &decoded, error)) {
		return false;
	}
	ShaderRecompiler::CFG::Graph cfg;
	if (!ShaderRecompiler::CFG::BuildGraph(decoded, &cfg, error) ||
	    !ShaderRecompiler::IR::LowerProgram(decoded, cfg, ShaderType::Compute, 64, ir, error)) {
		return false;
	}
	return ShaderRecompiler::IR::BuildScalarProvenance(ir, error) &&
	       ShaderRecompiler::IR::BuildSrtPlan(ir, error);
}

bool ReadSrtHostDword(void*, uint64_t address, uint32_t* value) {
	if (address == 0 || value == nullptr) {
		return false;
	}
	std::memcpy(value, reinterpret_cast<const void*>(address), sizeof(*value));
	return true;
}

void TestScalarProvenanceRealWideMoveLowering() {
	const uint32_t shader[] = {
	    EncodeSop1(0x04, 0, 20),                        // s_mov_b64 s[0:1], s[20:21]
	    EncodeSop1(0x04, 2, 22),                        // s_mov_b64 s[2:3], s[22:23]
	    EncodeMubuf0(0x0c),      EncodeMubuf1(0, 0, 1), // buffer_load_dword via copied s[0:3]
	    EncodeSopp(0x01),
	};

	std::string                   error;
	ShaderRecompiler::IR::Program ir;
	const auto                    lowered =
	    LowerProvenanceOnly(shader, static_cast<uint32_t>(std::size(shader)), &ir, &error);
	Check(lowered, error.c_str());

	const ShaderRecompiler::IR::Instruction* use = nullptr;
	for (const auto& block: ir.blocks) {
		for (const auto& inst: block.instructions) {
			if (inst.op == ShaderRecompiler::IR::Opcode::BufferLoadDword) {
				use = &inst;
			}
		}
	}
	Check(use != nullptr, "real wide-move shader did not lower a buffer use");
	const auto* source = ShaderRecompiler::IR::GetDescriptorSource(ir, use->memory.resource_source);
	Check(source != nullptr &&
	          ShaderRecompiler::IR::DescriptorSourceResolved(ir, use->memory.resource_source),
	      "real wide-move descriptor source was unresolved");
	for (uint32_t i = 0; i < 4; i++) {
		const auto& value = ir.provenance.values[source->dwords[i]];
		Check(value.op == ShaderRecompiler::IR::ScalarValueOp::UserData && value.imm == 20 + i,
		      "real s_mov_b64 lowering did not copy both descriptor pairs");
	}
}

void TestScalarProvenanceRealCarryAndScalarLoads() {
	const uint32_t carry_shader[] = {
	    EncodeSop1(0x1f, 0, 0),      // s_getpc_b64 s[0:1]
	    EncodeSop2(0x00, 0, 0, 129), // s_add_u32 s0, s0, 1
	    EncodeSop2(0x04, 1, 1, 128), // s_addc_u32 s1, s1, 0
	    EncodeMubuf0(0x0c),
	    EncodeMubuf1(0, 0, 1), // buffer_load_dword via computed s[0:3]
	    EncodeSopp(0x01),
	};
	std::string                   error;
	ShaderRecompiler::IR::Program carry_ir;
	const auto                    carry_lowered = LowerProvenanceOnly(
	    carry_shader, static_cast<uint32_t>(std::size(carry_shader)), &carry_ir, &error);
	Check(carry_lowered, error.c_str());
	const ShaderRecompiler::IR::DescriptorValue* carry_source = nullptr;
	for (const auto& block: carry_ir.blocks) {
		for (const auto& inst: block.instructions) {
			if (inst.op == ShaderRecompiler::IR::Opcode::BufferLoadDword) {
				carry_source = ShaderRecompiler::IR::GetDescriptorSource(
				    carry_ir, inst.memory.resource_source);
			}
		}
	}
	Check(carry_source != nullptr, "real add/addc descriptor source was not attached");
	const auto& high = carry_ir.provenance.values[carry_source->dwords[1]];
	Check(high.op == ShaderRecompiler::IR::ScalarValueOp::AddCarry &&
	          carry_ir.provenance.values[high.args[2]].op ==
	              ShaderRecompiler::IR::ScalarValueOp::Carry,
	      "real s_add_u32/s_addc_u32 lowering lost SCC carry provenance");

	const uint32_t load_shader[] = {
	    EncodeSmem0(0x02, 0, 4),
	    0u, // s_load_dwordx4 s[0:3]
	    EncodeMubuf0(0x0c),
	    EncodeMubuf1(0, 0, 1),
	    EncodeSmem0(0x0a, 4, 4),
	    0u, // s_buffer_load_dwordx4 s[4:7]
	    EncodeMubuf0(0x0c),
	    EncodeMubuf1(0, 1, 1),
	    EncodeSopp(0x01),
	};
	ShaderRecompiler::IR::Program load_ir;
	const auto                    load_lowered = LowerProvenanceOnly(
	    load_shader, static_cast<uint32_t>(std::size(load_shader)), &load_ir, &error);
	Check(load_lowered, error.c_str());
	uint32_t uses = 0;
	for (const auto& block: load_ir.blocks) {
		for (const auto& inst: block.instructions) {
			if (inst.op != ShaderRecompiler::IR::Opcode::BufferLoadDword) {
				continue;
			}
			const auto* source =
			    ShaderRecompiler::IR::GetDescriptorSource(load_ir, inst.memory.resource_source);
			Check(source != nullptr, "real scalar-load descriptor source was not attached");
			const auto expected = uses++ == 0
			                          ? ShaderRecompiler::IR::ScalarValueOp::ReadConst
			                          : ShaderRecompiler::IR::ScalarValueOp::ReadConstBuffer;
			for (uint32_t i = 0; i < 4; i++) {
				Check(load_ir.provenance.values[source->dwords[i]].op == expected,
				      "real scalar load used the wrong provenance operation");
			}
		}
	}
	Check(uses == 2, "real scalar-load provenance test did not find both buffer uses");

	const uint32_t inline_sampler_shader[] = {
	    EncodeSop2(0x25, 12, 128 + 12, 128 + 44), // s_bfm_b64 s[12:13], 12, 44
	    EncodeSop1(0x04, 14, 255),
	    0x09500000u, // s_mov_b64 s[14:15], 0x09500000
	    EncodeMimg0(0xa0, 0xf),
	    EncodeMimg1(2, 1, 3, 6), // image_sample_a v2, v6, s[4:11], s[12:15]
	    EncodeSopp(0x01),
	};
	ShaderRecompiler::IR::Program inline_sampler_ir;
	error.clear();
	Check(LowerProvenanceOnly(inline_sampler_shader,
	                          static_cast<uint32_t>(std::size(inline_sampler_shader)),
	                          &inline_sampler_ir, &error),
	      error.c_str());
	uint32_t sampler_source = 0;
	for (const auto& block: inline_sampler_ir.blocks) {
		for (const auto& inst: block.instructions) {
			if (inst.op == ShaderRecompiler::IR::Opcode::ImageSample) {
				sampler_source = inst.memory.sampler_source;
			}
		}
	}
	ShaderRecompiler::IR::DescriptorValue sampler;
	ShaderRecompiler::IR::SrtRuntime      runtime;
	Check(sampler_source >= 2 &&
	          ShaderRecompiler::IR::DescriptorSourceResolved(inline_sampler_ir, sampler_source) &&
	          ShaderRecompiler::IR::EvaluateDescriptorSource(inline_sampler_ir, sampler_source,
	                                                         0x10, runtime, &sampler, &error) &&
	          sampler.dwords[0] == 0 && sampler.dwords[1] == 0x00fff000u &&
	          sampler.dwords[2] == 0x09500000u && sampler.dwords[3] == 0,
	      "real inline sampler construction was unresolved or evaluated incorrectly");

	auto                             user_data = ImageTestUserData();
	ShaderRecompiler::CompileOptions options;
	options.stage     = ShaderType::Pixel;
	options.user_data = user_data.data();
	ShaderRecompiler::CompileResult result;
	error.clear();
	Check(ShaderRecompiler::TryRecompile(inline_sampler_shader, options, &result, &error),
	      error.c_str());
}

void TestSrtWalkerRealSmemLowering() {
	const uint32_t shader[] = {
	    EncodeSMovB32(124, 130), // m0 = 2
	    EncodeSmem0(0x02, 0, 4),
	    (124u << 25u) | 2u, // s_load_dwordx4 s[0:3], s[8:9], m0 offset + immediate 2
	    EncodeMubuf0(0x0c),      EncodeMubuf1(0, 0, 1), EncodeSopp(0x01),
	};
	std::string                   error;
	ShaderRecompiler::IR::Program ir;
	const auto                    lowered =
	    LowerProvenanceOnly(shader, static_cast<uint32_t>(std::size(shader)), &ir, &error);
	Check(lowered, error.c_str());
	Check(ir.srt.reads.size() == 4 && ir.srt.dynamic_reads.empty(),
	      "real SMEM lowering did not build four compact SRT reads");

	const std::array<uint32_t, 4> table     = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
	std::array<uint32_t, 16>      user_data = {};
	const auto                    address   = reinterpret_cast<uint64_t>(table.data());
	user_data[8]                            = static_cast<uint32_t>(address);
	user_data[9]                            = static_cast<uint32_t>(address >> 32u);
	std::vector<uint32_t>                  flat;
	const ShaderRecompiler::IR::SrtRuntime runtime {user_data, 0, ReadSrtHostDword, nullptr};
	const auto walked = ShaderRecompiler::IR::WalkSrt(ir, runtime, &flat, &error);
	Check(walked, error.c_str());
	Check(flat.size() == table.size() && std::equal(flat.begin(), flat.end(), table.begin()),
	      "real SMEM SRT walk did not apply component-level alignment");
	Check(ShaderRecompiler::IR::PatchSrtReads(&ir, &error), error.c_str());
	uint32_t patched = 0;
	for (const auto& block: ir.blocks) {
		for (const auto& inst: block.instructions) {
			if (inst.op == ShaderRecompiler::IR::Opcode::LoadSrtDword) {
				Check(inst.src[0].imm == patched++, "real SMEM patch used the wrong flat offset");
			}
		}
	}
	Check(patched == 4, "real SMEM patch did not rewrite every component");
}

void TestSrtWalkerRealSBufferLowering() {
	const uint32_t shader[] = {
	    EncodeSMovB32(124, 130), // m0 = 2
	    EncodeSmem0(0x0a, 0, 4),
	    (124u << 25u) | 2u, // s_buffer_load_dwordx4 s[0:3], s[8:11], m0 + 2
	    EncodeMubuf0(0x0c),      EncodeMubuf1(0, 0, 1), EncodeSopp(0x01),
	};
	std::string                   error;
	ShaderRecompiler::IR::Program ir;
	const auto                    lowered =
	    LowerProvenanceOnly(shader, static_cast<uint32_t>(std::size(shader)), &ir, &error);
	Check(lowered, error.c_str());
	Check(ir.srt.reads.size() == 4 && ir.srt.dynamic_reads.empty(),
	      "real S_BUFFER_LOAD lowering did not build four compact reads");

	const std::array<uint32_t, 5> table     = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
	                                           0x55555555u};
	std::array<uint32_t, 16>      user_data = {};
	const auto                    address   = reinterpret_cast<uint64_t>(table.data());
	user_data[8]                            = static_cast<uint32_t>(address);
	user_data[9]                            = static_cast<uint32_t>(address >> 32u);
	user_data[10]                           = sizeof(table);
	std::vector<uint32_t>                  flat;
	const ShaderRecompiler::IR::SrtRuntime runtime {user_data, 0, ReadSrtHostDword, nullptr};
	const auto walked = ShaderRecompiler::IR::WalkSrt(ir, runtime, &flat, &error);
	Check(walked, error.c_str());
	Check(flat.size() == 4 && std::equal(flat.begin(), flat.end(), table.begin() + 1),
	      "real S_BUFFER_LOAD walk used the wrong final alignment");

	user_data[10]                  = 4 * sizeof(uint32_t);
	const auto flat_before_failure = flat;
	const auto bounds_walked       = ShaderRecompiler::IR::WalkSrt(ir, runtime, &flat, &error);
	Check(!bounds_walked && error.find("exceeds size=16") != std::string::npos,
	      "real S_BUFFER_LOAD walk ignored descriptor bounds");
	Check(flat == flat_before_failure,
	      "failed real S_BUFFER_LOAD walk changed the prior flat snapshot");
	Check(ShaderRecompiler::IR::PatchSrtReads(&ir, &error), error.c_str());
	uint32_t patched = 0;
	for (const auto& block: ir.blocks) {
		for (const auto& inst: block.instructions) {
			if (inst.op == ShaderRecompiler::IR::Opcode::LoadSrtDword) {
				Check(inst.src[0].imm == patched++,
				      "real S_BUFFER_LOAD patch used the wrong flat offset");
			}
		}
	}
	Check(patched == 4, "real S_BUFFER_LOAD patch did not rewrite every component");

	const uint32_t negative_shader[] = {
	    EncodeSmem0(0x0a, 0, 4),
	    (125u << 25u) | 0x1ffffeu, // illegal negative S_BUFFER immediate
	    EncodeMubuf0(0x0c),        EncodeMubuf1(0, 0, 1), EncodeSopp(0x01),
	};
	ShaderRecompiler::IR::Program negative_ir;
	const auto                    negative_lowered = LowerProvenanceOnly(
	    negative_shader, static_cast<uint32_t>(std::size(negative_shader)), &negative_ir, &error);
	Check(negative_lowered, error.c_str());
	user_data[10] = sizeof(table);
	Check(!ShaderRecompiler::IR::WalkSrt(negative_ir, runtime, &flat, &error) &&
	          error.find("negative immediate") != std::string::npos,
	      "real S_BUFFER_LOAD walk accepted a negative immediate");
}

void TestScalarMemoryLoadsSnapshotOverlappingOperands() {
	const auto CheckOverlap = [](uint32_t opcode) {
		const uint32_t shader[] = {
		    EncodeSmem0(opcode, 0, 0),
		    125u << 25u, // load s[0:3] through overlapping s[0:*], null SOFFSET
		    EncodeMubuf0(0x0c),
		    EncodeMubuf1(0, 0, 1),
		    EncodeSopp(0x01),
		};
		std::string                   error;
		ShaderRecompiler::IR::Program ir;
		Check(LowerProvenanceOnly(shader, static_cast<uint32_t>(std::size(shader)), &ir, &error),
		      error.c_str());
		Check(ir.srt.reads.size() == 4 && ir.srt.dynamic_reads.empty(),
		      opcode == 0x02
		          ? "overlapping S_LOAD operands were evaluated after a component write"
		          : "overlapping S_BUFFER_LOAD operands were evaluated after a component write");
		Check(ShaderRecompiler::IR::PatchSrtReads(&ir, &error), error.c_str());
		uint32_t patched = 0;
		for (const auto& block: ir.blocks) {
			for (const auto& inst: block.instructions) {
				if (inst.op == ShaderRecompiler::IR::Opcode::LoadSrtDword) {
					Check(inst.src[0].imm == patched++,
					      "overlapping scalar-memory patch used the wrong flat offset");
				}
			}
		}
		Check(patched == 4, "overlapping scalar-memory load was not fully patched");
	};
	CheckOverlap(0x02); // s_load_dwordx4
	CheckOverlap(0x0a); // s_buffer_load_dwordx4
}

void TestScalarMemoryLoadCrossesIntoVcc() {
	const uint32_t shader[] = {
	    EncodeSmem0(0x02, 104, 4),
	    125u << 25u, // s_load_dwordx4 s[104:105], vcc_lo, vcc_hi
	    EncodeMubuf0(0x0c),
	    EncodeMubuf1(0, 26, 1),
	    EncodeSopp(0x01),
	};
	std::string                   error;
	ShaderRecompiler::IR::Program ir;
	Check(LowerProvenanceOnly(shader, static_cast<uint32_t>(std::size(shader)), &ir, &error),
	      error.c_str());
	Check(ir.srt.reads.size() == 4 && ir.srt.dynamic_reads.empty(),
	      "wide SMEM destination crossing into VCC lost scalar provenance");
	Check(ShaderRecompiler::IR::PatchSrtReads(&ir, &error), error.c_str());
	uint32_t patched = 0;
	for (const auto& block: ir.blocks) {
		for (const auto& inst: block.instructions) {
			if (inst.op == ShaderRecompiler::IR::Opcode::LoadSrtDword) {
				Check(inst.src[0].imm == patched++,
				      "wide SMEM destination crossing into VCC used the wrong flat offset");
			}
		}
	}
	Check(patched == 4, "wide SMEM destination crossing into VCC was not fully patched");
}

void TestResourceTrackingRealDensePatching() {
	const uint32_t shader[] = {
	    EncodeMubuf0(0x0c),     EncodeMubuf1(0, 0, 1),    // buffer load s[0:3]
	    EncodeMubuf0(0x0c),     EncodeMubuf1(1, 0, 1),    // same buffer
	    EncodeMubuf0(0x1c),     EncodeMubuf1(2, 1, 1),    // buffer store s[4:7]
	    EncodeMimg0(0x20, 0xf), EncodeMimg1(8, 0, 2, 1),  // sampled image s[0:7], sampler s[8:11]
	    EncodeMimg0(0x08, 0xf), EncodeMimg1(12, 0, 0, 1), // storage view of the same image source
	    EncodeSopp(0x01),
	};
	std::string                   error;
	ShaderRecompiler::IR::Program ir;
	const auto                    lowered =
	    LowerProvenanceOnly(shader, static_cast<uint32_t>(std::size(shader)), &ir, &error);
	Check(lowered, error.c_str());
	Check(ShaderRecompiler::IR::PatchSrtReads(&ir, &error), error.c_str());
	const auto tracked = ShaderRecompiler::IR::TrackResources(&ir, &error);
	Check(tracked, error.c_str());
	Check(ir.info.buffers.size() == 2 && ir.info.images.size() == 2 && ir.info.samplers.size() == 1,
	      "real resource tracking produced the wrong dense list sizes");

	uint32_t buffer_use = 0;
	uint32_t image_use  = 0;
	for (const auto& block: ir.blocks) {
		for (const auto& inst: block.instructions) {
			switch (inst.memory.kind) {
				case ShaderRecompiler::IR::ResourceKind::Buffer: {
					const uint32_t expected = buffer_use++ < 2 ? 0 : 1;
					Check(inst.memory.resource == expected,
					      "real buffer operand was not patched densely");
					break;
				}
				case ShaderRecompiler::IR::ResourceKind::Image:
				case ShaderRecompiler::IR::ResourceKind::StorageImage:
					Check(inst.memory.resource == image_use++,
					      "real image operand was not patched by view class");
					if (inst.op == ShaderRecompiler::IR::Opcode::ImageSample) {
						Check(inst.memory.sampler == 0,
						      "real sampler operand was not patched densely");
					}
					break;
				default: break;
			}
		}
	}
	Check(buffer_use == 3 && image_use == 2 && ir.info.buffers[0].read &&
	          ir.info.buffers[1].written,
	      "real tracked resource access facts were incomplete");
}

void TestLowerProgramResetsAnalysisState() {
	const uint32_t first_shader[] = {EncodeMubuf0(0x0c), EncodeMubuf1(0, 0, 1), EncodeSopp(0x01)};
	std::string    error;
	ShaderRecompiler::IR::Program ir;
	Check(LowerProvenanceOnly(first_shader, static_cast<uint32_t>(std::size(first_shader)), &ir,
	                          &error),
	      error.c_str());
	Check(ShaderRecompiler::IR::PatchSrtReads(&ir, &error), error.c_str());
	Check(ShaderRecompiler::IR::TrackResources(&ir, &error), error.c_str());
	ShaderComputeInputInfo compute;
	Check(ShaderRecompiler::IR::CollectShaderInfo(&ir, {.compute = &compute}, &error),
	      error.c_str());
	Check(ir.resource_tracking_complete && ir.shader_info_complete && !ir.info.buffers.empty(),
	      "analysis-reset fixture did not reach completed state");
	ir.shader_hash = 0xdeadbeef;

	const uint32_t second_shader[] = {EncodeSopp(0x01)};
	Check(LowerProvenanceOnly(second_shader, static_cast<uint32_t>(std::size(second_shader)), &ir,
	                          &error),
	      error.c_str());
	Check(!ir.resource_tracking_complete && !ir.shader_info_complete && ir.srt_plan_complete &&
	          ir.shader_hash == 0 && ir.info.buffers.empty() && ir.info.images.empty() &&
	          ir.info.samplers.empty() && ir.info.sampled_pairs.empty() && ir.info.inputs.empty() &&
	          ir.info.outputs.empty(),
	      "LowerProgram reused stale provenance/resource/interface state");
}

void TestNewShaderRecompilerStageInputInfo() {
	using StageInputKind = ShaderRecompiler::IR::StageInputKind;

	const uint32_t shader[] = {0xbf810000u};

	ShaderComputeInputInfo cs_info {};
	cs_info.group_id[0]                = true;
	cs_info.thread_ids_num             = 3;
	cs_info.dispatch_thread_dimensions = true;

	ShaderRecompiler::CompileOptions cs_options;
	cs_options.stage              = ShaderType::Compute;
	cs_options.compute_input_info = &cs_info;

	ShaderRecompiler::CompileResult cs_result;
	std::string                     error;
	Check(ShaderRecompiler::TryRecompile(shader, cs_options, &cs_result, &error), error.c_str());
	Check(ProgramHasInput(cs_result.program, StageInputKind::WorkgroupId),
	      "compute WorkgroupId input missing from reflection");
	Check(ProgramHasInput(cs_result.program, StageInputKind::LocalInvocationId),
	      "compute LocalInvocationId input missing from reflection");
	Check(ProgramHasInput(cs_result.program, StageInputKind::LocalInvocationIndex),
	      "compute LocalInvocationIndex input missing from reflection");
	Check(ProgramHasInput(cs_result.program, StageInputKind::GlobalInvocationId),
	      "compute GlobalInvocationId input missing from reflection");
	Check(SpirvHasDecorationValue(cs_result.spirv, 11u, 26u),
	      "SPIR-V lacks WorkgroupId BuiltIn decoration");
	Check(SpirvHasDecorationValue(cs_result.spirv, 11u, 27u),
	      "SPIR-V lacks LocalInvocationId BuiltIn decoration");
	Check(SpirvHasDecorationValue(cs_result.spirv, 11u, 28u),
	      "SPIR-V lacks GlobalInvocationId BuiltIn decoration");
	Check(SpirvHasDecorationValue(cs_result.spirv, 11u, 29u),
	      "SPIR-V lacks LocalInvocationIndex BuiltIn decoration");
	CheckSpirvBinaryValidates(cs_result.spirv);

	ShaderRecompiler::CompileOptions vs_options;
	vs_options.stage = ShaderType::Vertex;

	ShaderRecompiler::CompileResult vs_result;
	Check(ShaderRecompiler::TryRecompile(shader, vs_options, &vs_result, &error), error.c_str());
	Check(ProgramHasInput(vs_result.program, StageInputKind::VertexIndex),
	      "vertex VertexIndex input missing from reflection");
	Check(ProgramHasInput(vs_result.program, StageInputKind::InstanceIndex),
	      "vertex InstanceIndex input missing from reflection");
	Check(SpirvHasDecorationValue(vs_result.spirv, 11u, 42u),
	      "SPIR-V lacks VertexIndex BuiltIn decoration");
	Check(SpirvHasDecorationValue(vs_result.spirv, 11u, 43u),
	      "SPIR-V lacks InstanceIndex BuiltIn decoration");
	CheckSpirvBinaryValidates(vs_result.spirv);

	ShaderPixelInputInfo ps_info {};
	ps_info.input_num = 2;
	ps_info.ps_pos_x  = true;
	ps_info.ps_pos_y  = true;
	ps_info.ps_pos_z  = true;
	SetIdentityInterpolatorSettings(&ps_info);

	ShaderRecompiler::CompileOptions ps_options;
	ps_options.stage            = ShaderType::Pixel;
	ps_options.pixel_input_info = &ps_info;

	ShaderRecompiler::CompileResult ps_result;
	Check(ShaderRecompiler::TryRecompile(shader, ps_options, &ps_result, &error), error.c_str());
	Check(ProgramHasInput(ps_result.program, StageInputKind::FragCoord),
	      "pixel FragCoord input missing from reflection");
	Check(ProgramInputCount(ps_result.program, StageInputKind::Parameter) == 2,
	      "pixel interpolant inputs missing from reflection");
	Check(SpirvHasDecorationValue(ps_result.spirv, 11u, 15u),
	      "SPIR-V lacks FragCoord BuiltIn decoration");
	Check(SpirvHasDecorationValue(ps_result.spirv, 30u, 0u),
	      "SPIR-V lacks interpolant Location 0 decoration");
	Check(SpirvHasDecorationValue(ps_result.spirv, 30u, 1u),
	      "SPIR-V lacks interpolant Location 1 decoration");
	CheckSpirvBinaryValidates(ps_result.spirv);

	ShaderPixelInputInfo ps_pos_y_info {};
	ps_pos_y_info.input_num            = 1;
	ps_pos_y_info.ps_system_input_base = 2;
	ps_pos_y_info.ps_pos_y             = true;
	SetIdentityInterpolatorSettings(&ps_pos_y_info);
	ps_options.pixel_input_info = &ps_pos_y_info;

	ShaderRecompiler::CompileResult ps_pos_y_result;
	Check(ShaderRecompiler::TryRecompile(shader, ps_options, &ps_pos_y_result, &error),
	      error.c_str());
	Check(ProgramHasInput(ps_pos_y_result.program, StageInputKind::FragCoord),
	      "pixel POS_Y-only FragCoord input missing from reflection");
	Check(SpirvHasDecorationValue(ps_pos_y_result.spirv, 11u, 15u),
	      "SPIR-V lacks POS_Y-only FragCoord BuiltIn decoration");
	CheckSpirvBinaryValidates(ps_pos_y_result.spirv);
}

void TestNewShaderRecompilerPixelPipelineEntry() {
	const uint32_t shader[] = {0xbf810000u};

	EnsureConfigInitialized();

	HW::PixelShaderInfo regs {};
	regs.ps_regs.data_addr = reinterpret_cast<uint64_t>(shader);
	regs.ps_regs.chksum    = 0x1234567800000001ull;
	ShaderMappedData mapped {};
	mapped.code_size_bytes = sizeof(shader);
	ShaderMapUserData(regs.ps_regs.data_addr, mapped);

	HW::ShaderRegisters   sh {};
	ShaderPixelInputInfo  input_info {};
	std::vector<uint32_t> spirv;
	Check(ShaderCompileSpirvPS(&regs, &sh, ShaderLaneMaskMode::NativeWave, &input_info, &spirv),
	      "new pixel shader recompiler wrapper did not produce SPIR-V");
	Check(!spirv.empty(), "new pixel shader recompiler wrapper returned empty SPIR-V");
	Check(spirv.front() == 0x07230203u,
	      "new pixel shader recompiler wrapper did not emit SPIR-V binary");
	CheckSpirvBinaryValidates(spirv);

	const uint32_t vcc_load_shader[] = {
	    EncodeSop1(0x24, 0, 106), // s_and_saveexec_b64 s[0:1], vcc
	    EncodeSMovB32(106, 27),   EncodeSMovB32(107, 28), EncodeSmem0(0x02, 44, 53),
	    (0x7du << 25u) | 160u,    EncodeMubuf0(0x0c),     EncodeMubuf1(0, 11, 1),
	    EncodeSopp(0x01),
	};
	std::array<uint32_t, 44> table {};
	std::array<uint32_t, 1>  buffer {};
	const auto               table_address  = reinterpret_cast<uint64_t>(table.data());
	const auto               buffer_address = reinterpret_cast<uint64_t>(buffer.data());
	table[40]                               = static_cast<uint32_t>(buffer_address);
	table[41]                               = static_cast<uint32_t>(buffer_address >> 32u);
	table[42]                               = 1;

	HW::PixelShaderInfo vcc_regs {};
	vcc_regs.ps_regs.data_addr       = reinterpret_cast<uint64_t>(vcc_load_shader);
	vcc_regs.ps_regs.chksum          = 0x6306606500000001ull;
	vcc_regs.ps_regs.rsrc2.user_sgpr = 29;
	vcc_regs.ps_user_sgpr.value[27]  = static_cast<uint32_t>(table_address);
	vcc_regs.ps_user_sgpr.value[28]  = static_cast<uint32_t>(table_address >> 32u);
	ShaderMappedData vcc_mapped {};
	vcc_mapped.code_size_bytes = sizeof(vcc_load_shader);
	ShaderMapUserData(vcc_regs.ps_regs.data_addr, vcc_mapped);

	ShaderPixelInputInfo  vcc_input {};
	std::vector<uint32_t> vcc_spirv;
	Check(ShaderCompileSpirvPS(&vcc_regs, &sh, ShaderLaneMaskMode::NativeWave, &vcc_input,
	                           &vcc_spirv),
	      "VCC raw-load pointer was lost");
	CheckSpirvBinaryValidates(vcc_spirv);
}

void TestPixelProgramCacheDescriptorSetIdentity() {
	const uint32_t      shader_01[]   = {0xbf810000u};
	const uint32_t      shader_10[]   = {0xbf810000u};
	const uint32_t      shader_mask[] = {0xbf810000u};
	HW::ShaderRegisters sh {};

	auto check_transition = [&](const uint32_t* shader, uint64_t checksum,
	                            bool first_has_vs_descriptors, bool second_has_vs_descriptors) {
		HW::PixelShaderInfo regs {};
		regs.ps_regs.data_addr = reinterpret_cast<uint64_t>(shader);
		regs.ps_regs.chksum    = checksum;
		ShaderMappedData mapped {};
		mapped.code_size_bytes = sizeof(uint32_t);
		ShaderMapUserData(regs.ps_regs.data_addr, mapped);

		auto compile = [&](bool has_vs_descriptors) {
			auto vs_program = std::make_shared<ShaderRecompiler::IR::Program>();
			if (has_vs_descriptors) {
				vs_program->bindings.descriptors.emplace_back();
			}
			ShaderVertexInputInfo vs_info {};
			vs_info.stage.program = std::move(vs_program);
			ShaderPixelInputInfo      ps_info {};
			std::span<const uint32_t> spirv;
			Check(ShaderCompileInfoPS(&regs, &sh, ShaderLaneMaskMode::NativeWave, &vs_info,
			                          &ps_info, &spirv),
			      "pixel program-cache transition failed to compile");
			const auto expected_set = has_vs_descriptors ? 1u : 0u;
			Check(ps_info.descriptor_set == expected_set && ps_info.stage.program != nullptr &&
			          ps_info.stage.program->bindings.descriptor_set == expected_set,
			      "pixel program cache reused SPIR-V with the paired VS descriptor set");
		};

		compile(first_has_vs_descriptors);
		compile(second_has_vs_descriptors);
	};

	check_transition(shader_01, 0x91a27e6300000001ull, false, true);
	check_transition(shader_10, 0x91a27e6300000002ull, true, false);

	HW::PixelShaderInfo mask_regs {};
	mask_regs.ps_regs.data_addr = reinterpret_cast<uint64_t>(shader_mask);
	mask_regs.ps_regs.chksum    = 0x91a27e6300000003ull;
	ShaderMappedData mapped {};
	mapped.code_size_bytes = sizeof(shader_mask);
	ShaderMapUserData(mask_regs.ps_regs.data_addr, mapped);
	auto compile_mask_mode = [&](ShaderLaneMaskMode mode) {
		ShaderVertexInputInfo vs_info {};
		vs_info.stage.program = std::make_shared<ShaderRecompiler::IR::Program>();
		ShaderPixelInputInfo      ps_info {};
		std::span<const uint32_t> spirv;
		Check(ShaderCompileInfoPS(&mask_regs, &sh, mode, &vs_info, &ps_info, &spirv),
		      "pixel lane-mask cache transition failed to compile");
		Check(ps_info.stage.program != nullptr && ps_info.stage.program->lane_mask_mode == mode,
		      "pixel program cache reused a different lane-mask lowering");
	};
	compile_mask_mode(ShaderLaneMaskMode::NativeWave);
	compile_mask_mode(ShaderLaneMaskMode::PerInvocation);
}

void TestNewShaderRecompilerUnsupportedMemoryDecode() {
	const uint32_t mubuf_unknown[] = {EncodeMubuf0(0x7b), EncodeMubuf1(0, 0, 1), 0xbf810000u};
	CheckNewDecoderUnsupported(mubuf_unknown, static_cast<uint32_t>(std::size(mubuf_unknown)),
	                           "MUBUF", "opcode=0x7b");

	const uint32_t flat_unknown[] = {EncodeFlat0(0x7e, 0, 4), EncodeFlat1(9, 0x7d, 0, 1),
	                                 0xbf810000u};
	CheckNewDecoderUnsupported(flat_unknown, static_cast<uint32_t>(std::size(flat_unknown)), "FLAT",
	                           "opcode=0x7e");

	const uint32_t mtbuf_unknown[] = {EncodeMtbuf0(0x08, 14, 7, 4), EncodeMtbuf1(0x08, 9, 0, 1),
	                                  0xbf810000u};
	CheckNewDecoderUnsupported(mtbuf_unknown, static_cast<uint32_t>(std::size(mtbuf_unknown)),
	                           "MTBUF", "opcode=0x08");
}

void TestNewShaderRecompilerFlatUserPointerProvenance() {
	const uint32_t shader[] = {
	    EncodeVop2(0x25, 0, 0, 2), // v_add_nc_u32 v0, s0, v2
	    EncodeVop1(0x01, 1, 1),    // v_mov_b32 v1, s1
	    EncodeFlat0(0x0c, 0, 0),
	    EncodeFlat1(3, 0x7d, 0, 0), // flat_load_dword v3, v[0:1]
	    0xbf810000u,
	};
	const uint32_t user_data[] = {0x34567000u, 0x00000012u};

	ShaderRecompiler::CompileOptions options;
	options.stage           = ShaderType::Compute;
	options.user_data       = user_data;
	options.user_data_count = static_cast<uint32_t>(std::size(user_data));

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	const bool compiled = ShaderRecompiler::TryRecompile(shader, options, &result, &error);
	Check(compiled, error.c_str());
	Check(result.program.info.addresses.size() == 1 &&
	          result.program.info.addresses[0].source !=
	              ShaderRecompiler::IR::ScalarProvenance::Unknown,
	      "FLAT user pointer provenance was not attached");
	Check(result.resources.addresses.size() == 1 &&
	          result.resources.addresses[0].guest_base == 0x0000001234567000ull &&
	          result.resources.addresses[0].binding_base == 0x0000001234560000ull &&
	          result.program.info.addresses[0].specialized_base == 0x0000001234560000ull,
	      "FLAT user pointer did not materialize its guest base");
	CheckSpirvBinaryValidates(result.spirv);
}

void TestNewShaderRecompilerFlatAddressProvenanceBoundaries() {
	const uint32_t segmented_shader[] = {
	    EncodeVop2(0x25, 0, 0, 2),
	    EncodeVop1(0x01, 1, 1),
	    EncodeFlat0(0x0c, 2, 0),
	    EncodeFlat1(3, 0x7d, 0, 0), // global_load_dword v3, v[0:1]
	    EncodeFlat0(0x0c, 1, 0),
	    EncodeFlat1(4, 0x7d, 0, 0), // scratch_load_dword v4, v[0:1]
	    0xbf810000u,
	};
	const uint32_t user_data[] = {0x34567000u, 0u, 0x1000u};

	ShaderRecompiler::CompileOptions options;
	options.stage            = ShaderType::Compute;
	options.user_data        = user_data;
	options.user_data_count  = static_cast<uint32_t>(std::size(user_data));
	options.flat_memory_base = 0;
	ShaderRecompiler::CompileResult result;
	std::string                     error;
	const bool                      compiled =
	    ShaderRecompiler::TryRecompile(segmented_shader, options, &result, &error);
	Check(compiled, error.c_str());
	Check(result.program.info.addresses.size() == 2,
	      "segmented address resources were not tracked independently");
	for (const auto& address: result.program.info.addresses) {
		Check(address.source == ShaderRecompiler::IR::ScalarProvenance::Unknown,
		      "GLOBAL/SCRATCH null-SADDR incorrectly inherited FLAT VGPR provenance");
	}
}

} // namespace
} // namespace Libs::Graphics

int main() {
	using namespace Libs::Graphics;

	EnsureConfigInitialized();

	TestResourceDescriptorClassification();
	TestNativeShaderResourceDependencies();
	TestNativeSubgroupPolicy();
	TestNewShaderRecompilerSMovB32();
	TestNewShaderRecompilerSoppMarkers();
	TestNewShaderRecompilerSopkWaitcntMarkers();
	TestNewShaderRecompilerRdna2ScalarOpcodes();
	TestNewShaderRecompilerScalarVectorAlu();
	TestNewShaderRecompilerVop3LaneReadDestinationEncoding();
	TestNewShaderRecompilerMoreAluFamilies();
	TestNewShaderRecompilerExpandedAluBatch();
	TestNewShaderRecompilerVop3pPackedF16();
	TestNewShaderRecompilerStagedShaderOps();
	TestNewShaderRecompilerBootF16UnaryOpcodes();
	TestNewShaderRecompilerBootB16PackedAndSdwaOpcodes();
	TestNewShaderRecompilerScalarB64Alu();
	TestNewShaderRecompilerSignedCompareAlu();
	TestNewShaderRecompilerSignedMinShiftAlu();
	TestNewShaderRecompilerScalarBitfieldAlu();
	TestNewShaderRecompilerMemoryFamilyLowering();
	TestNewShaderRecompilerImageQueryLowering();
	TestNewShaderRecompilerImageSampleVariants();
	TestNewShaderRecompilerImageSampleA16SamplerCoords();
	TestNewShaderRecompilerImageSampleOpcodeAliases();
	TestNewShaderRecompilerImageSampleA16ExceptionComponents();
	TestNewShaderRecompilerImageSampleA16Rdna2AddressOrder();
	TestNewShaderRecompilerPixelImageSampleLodSelection();
	TestNewShaderRecompilerImageViewDimensions();
	TestNewShaderRecompilerImageGatherVariants();
	TestNewShaderRecompilerImageLoadA16UintCoords();
	TestNewShaderRecompilerImageLoadVariants();
	TestNewShaderRecompilerImageStoreLowering();
	TestNewShaderRecompilerStorageImage3DDescriptorVariant();
	TestNewShaderRecompilerStorageImage2DDescriptorOverridesMimg3D();
	TestNewShaderRecompilerImageAtomicLowering();
	TestNewShaderRecompilerVintrpLowering();
	TestNewShaderRecompilerWideMemoryLowering();
	TestNewShaderRecompilerBufferSignedLoadLowering();
	TestNewShaderRecompilerBufferSubDwordStoreLowering();
	TestNewShaderRecompilerMubufFormatLowering();
	TestNewShaderRecompilerFormattedStoreUsesRuntimeArrayLengthOnly();
	TestNewShaderRecompilerTypedBufferLowering();
	TestNewShaderRecompilerFlatOldBackedLowering();
	TestNewShaderRecompilerUnbasedFlatRequiresTranslator();
	TestNewShaderRecompilerFlatUserPointerProvenance();
	TestNewShaderRecompilerFlatAddressProvenanceBoundaries();
	TestNewShaderRecompilerFlatSignedLoadLowering();
	TestNewShaderRecompilerFlatStoreLowering();
	TestNewShaderRecompilerAtomicLowering();
	TestNewShaderRecompilerDsReadWrite2Lowering();
	TestNewShaderRecompilerDsSubDwordLowering();
	TestNewShaderRecompilerDsWideAndAtomicLowering();
	TestNewShaderRecompilerDsSwizzleLowering();
	TestNewShaderRecompilerDsAddtidLowering();
	TestNewShaderRecompilerDsFloatMinMaxLowering();
	TestNewShaderRecompilerCfgStraightLine();
	TestNewShaderRecompilerCfgIfElse();
	TestNewShaderRecompilerCfgTerminalExitMergePS();
	TestNewShaderRecompilerCfgPostEndTargetMergePS();
	TestNewShaderRecompilerCfgLoopBreakContinue();
	TestNewShaderRecompilerCfgLoopHeaderDynamicScalarBufferLoadStructured();
	TestNewShaderRecompilerCfgLoopHeaderBufferLoadDispatcher();
	TestNewShaderRecompilerCfgLoopHeaderDsAppendConsumeDispatcher();
	TestNewShaderRecompilerCfgSharedOuterAndLoopMerge();
	TestNewShaderRecompilerCfgLoopSharedContinueSelectionMerges();
	TestNewShaderRecompilerCfgDuplicateMergeStructuredSplit();
	TestNewShaderRecompilerCfgIrreducibleDispatcher();
	TestNewShaderRecompilerExecMaskHelpers();
	TestComputeShaderInputWaveSize();
	TestNewShaderRecompilerWave32MasksExecHighStores();
	TestNewShaderRecompilerWave32VccHighScalarStores();
	TestNewShaderRecompilerCompareMaskIsFullWaveBallot();
	TestNewShaderRecompilerBufferLoadsGuardedByExec();
	TestNewShaderRecompilerBufferAtomicsGuardedByBounds();
	TestNewShaderRecompilerBranchConditionForms();
	TestNewShaderRecompilerSetpcBranch();
	TestNewShaderRecompilerSetpcJumpTable();
	TestNewShaderRecompilerExpVertexOutputs();
	TestNewShaderRecompilerVertexSystemVgprs();
	TestNewShaderRecompilerVertexExportUsesLaneExecMask();
	TestNewShaderRecompilerPerInvocationMasks();
	TestNewShaderRecompilerExpPixelOutputs();
	TestNewShaderRecompilerEarlyZDisabledWhenPixelKillEnabled();
	TestScalarProvenanceRealWideMoveLowering();
	TestScalarProvenanceRealCarryAndScalarLoads();
	TestSrtWalkerRealSmemLowering();
	TestSrtWalkerRealSBufferLowering();
	TestScalarMemoryLoadsSnapshotOverlappingOperands();
	TestScalarMemoryLoadCrossesIntoVcc();
	TestResourceTrackingRealDensePatching();
	TestLowerProgramResetsAnalysisState();
	TestNewShaderRecompilerNativeBindingPlan();
	TestNewShaderRecompilerStageInputInfo();
	TestGraphicsCreateInterpolantMapping();
	TestNewShaderRecompilerPixelPipelineEntry();
	TestPixelProgramCacheDescriptorSetIdentity();
	TestNewShaderRecompilerUnsupportedMemoryDecode();

	return 0;
}
