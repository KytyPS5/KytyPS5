#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

[[noreturn]] void BindingFail(const char* message) {
	EXIT("shader binding layout failed: %s", message);
	std::abort();
}

std::vector<uint32_t> CollectUserData(const Program& program) {
	std::array<bool, NumScalarRegs> registers {};
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::GetUserData || !inst.HasUses()) {
				continue;
			}
			if (inst.Arg(0).GetType() != Type::ScalarReg) {
				BindingFail("typed shader contains an invalid user-data register");
			}
			const auto index = RegIndex(inst.Arg(0).ScalarRegister());
			if (index >= NumScalarRegs) {
				BindingFail("typed shader contains an invalid user-data register");
			}
			registers[index] = true;
		}
	}
	std::vector<uint32_t> result;
	for (uint32_t index = 0; index < registers.size(); index++) {
		if (registers[index]) {
			result.push_back(index);
		}
	}
	return result;
}

void AddBinding(BindingLayout& layout, DescriptorBindingKind kind,
                std::vector<uint32_t> resources = {}) {
	layout.descriptors.push_back({kind, std::move(resources)});
}

bool UsesGds(const Program& program) {
	bool uses_gds = false;
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (SharedAccessOf(inst.GetOpcode()) == SharedAccess::None) {
				continue;
			}
			const auto index = inst.Flags<MemoryFlags>().index;
			if (index >= program.memory_info.size()) {
				BindingFail("typed shader contains invalid shared-memory metadata");
			}
			const auto kind = program.memory_info[index].kind;
			if (kind != ResourceKind::Lds && kind != ResourceKind::Gds) {
				BindingFail("typed shader contains invalid shared-memory metadata");
			}
			uses_gds |= kind == ResourceKind::Gds;
		}
	}
	return uses_gds;
}

} // namespace

void AllocateBindings(Program& program, uint32_t push_constant_offset) {
	if (!program.shader_info_complete || program.binding_layout_complete) {
		EXIT("shader binding layout failed: %s", !program.shader_info_complete
		                                             ? "shader info is not ready"
		                                             : "binding layout already allocated");
	}
	if (push_constant_offset > NativePushConstantSize ||
	    push_constant_offset % sizeof(uint32_t) != 0) {
		EXIT("shader binding layout failed: push-constant offset %u exceeds the Vulkan minimum "
		     "guarantee or is unaligned",
		     push_constant_offset);
	}

	BindingLayout next;
	next.push_constant_offset = push_constant_offset;
	next.user_data_registers = CollectUserData(program);
	next.memory_offset_dword = static_cast<uint32_t>(next.user_data_registers.size());
	next.memory_offset_count = static_cast<uint32_t>(program.info.buffers.size());

	if (!program.info.buffers.empty()) {
		std::vector<uint32_t> resources(program.info.buffers.size());
		for (uint32_t i = 0; i < resources.size(); i++) {
			resources[i] = i;
		}
		AddBinding(next, DescriptorBindingKind::Buffers, std::move(resources));
	}

	std::array<std::vector<uint32_t>, ImageBindingCount> image_groups;
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		const auto kind = DescriptorBindingForImage(program.info.images[i]);
		if (!kind.has_value()) {
			EXIT("shader binding layout failed: image %u has an invalid binding class", i);
		}
		const auto group = ImageBindingIndex(*kind);
		if (group >= image_groups.size()) {
			EXIT("shader binding layout failed: image %u has an unmapped binding class", i);
		}
		auto&      resources = image_groups[group];
		const auto dynamic   = program.info.images[i].mip_mode == ImageMipMode::DynamicStorage;
		const auto count     = dynamic ? program.info.images[i].mip_count : 1u;
		if (count == 0u || (!dynamic && program.info.images[i].mip_count != 1u)) {
			EXIT("shader binding layout failed: image %u has invalid specialized mip count %u", i,
			     program.info.images[i].mip_count);
		}
		resources.insert(resources.end(), count, i);
	}
	for (uint32_t i = 0; i < image_groups.size(); i++) {
		if (!image_groups[i].empty()) {
			AddBinding(next, static_cast<DescriptorBindingKind>(FirstImageBinding + i),
			           std::move(image_groups[i]));
		}
	}

	if (!program.info.samplers.empty()) {
		std::vector<uint32_t> resources(program.info.samplers.size());
		for (uint32_t i = 0; i < resources.size(); i++) {
			resources[i] = i;
		}
		AddBinding(next, DescriptorBindingKind::Samplers, std::move(resources));
	}
	if (UsesGds(program)) {
		AddBinding(next, DescriptorBindingKind::Gds);
	}
	if (program.info.uses_dma) {
		AddBinding(next, DescriptorBindingKind::BdaPagetable);
		AddBinding(next, DescriptorBindingKind::FaultBuffer);
	}
	const bool uses_flattened_runtime =
	    !program.srt_reads.empty() ||
	    std::ranges::any_of(program.info.images, [](const ImageResource& image) {
		    return image.indirect_mapping_capacity != 0u;
	    });
	if (uses_flattened_runtime) {
		AddBinding(next, DescriptorBindingKind::FlattenedSrt);
	}

	const auto push_dwords = (NativePushConstantSize - push_constant_offset) / sizeof(uint32_t);
	if (next.ShaderDataDwords() <= push_dwords) {
		next.push_constant_size = next.ShaderDataDwords() * sizeof(uint32_t);
	} else {
		AddBinding(next, DescriptorBindingKind::UserData);
	}

	program.bindings                = std::move(next);
	program.binding_layout_complete = true;
}

const DescriptorBinding* FindBinding(const BindingLayout& layout, DescriptorBindingKind kind) {
	for (const auto& binding: layout.descriptors) {
		if (binding.kind == kind) {
			return &binding;
		}
	}
	return nullptr;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
