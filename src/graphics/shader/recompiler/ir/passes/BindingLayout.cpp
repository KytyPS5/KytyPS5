#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"

#include "graphics/shader/recompiler/ir/ValueProgram.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr auto     FirstImageBinding     = DescriptorBindingKind::Sampled1D;
constexpr uint32_t ImageBindingCount =
    static_cast<uint32_t>(DescriptorBindingKind::StorageAtomic3D) -
    static_cast<uint32_t>(FirstImageBinding) + 1u;

bool CollectUserData(const Program& program, std::vector<uint32_t>& result) {
	std::array<bool, NumScalarRegs> registers {};
	for (const auto* block: program.values->blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::GetUserData || !inst.HasUses()) {
				continue;
			}
			if (inst.Arg(0).GetType() != Type::ScalarReg) {
				return false;
			}
			const auto index = RegIndex(inst.Arg(0).ScalarRegister());
			if (index >= NumScalarRegs) {
				return false;
			}
			registers[index] = true;
		}
	}
	result.clear();
	for (uint32_t index = 0; index < registers.size(); index++) {
		if (registers[index]) {
			result.push_back(index);
		}
	}
	return true;
}

void AddBinding(BindingLayout& layout, DescriptorBindingKind kind,
                std::vector<uint32_t> resources = {}) {
	layout.descriptors.push_back({kind, std::move(resources)});
}

bool UsesGds(const Program& program, bool& uses_gds) {
	uses_gds = false;
	for (const auto* block: program.values->blocks) {
		for (const auto& inst: *block) {
			if (SharedAccessOf(inst.GetOpcode()) == SharedAccess::None) {
				continue;
			}
			const auto index = inst.Flags<MemoryFlags>().index;
			if (index >= program.values->memory_info.size()) {
				return false;
			}
			const auto kind = program.values->memory_info[index].kind;
			if (kind != ResourceKind::Lds && kind != ResourceKind::Gds) {
				return false;
			}
			uses_gds |= kind == ResourceKind::Gds;
		}
	}
	return true;
}

} // namespace

bool AllocateBindings(Program& program, uint32_t push_constant_offset, std::string* error) {
	if (!program.shader_info_complete || program.binding_layout_complete) {
		if (error != nullptr) {
			*error = !program.shader_info_complete ? "shader info is not ready"
			                                       : "binding layout already allocated";
		}
		return false;
	}
	if (program.values == nullptr) {
		if (error != nullptr) {
			*error = "typed value program is not available";
		}
		return false;
	}
	if (push_constant_offset > NativePushConstantSize ||
	    push_constant_offset % sizeof(uint32_t) != 0) {
		if (error != nullptr) {
			*error = "push-constant offset exceeds the Vulkan minimum guarantee or is unaligned";
		}
		return false;
	}

	BindingLayout next;
	next.push_constant_offset = push_constant_offset;
	if (!CollectUserData(program, next.user_data_registers)) {
		if (error != nullptr) {
			*error = "typed shader contains an invalid user-data register";
		}
		return false;
	}
	next.memory_offset_dword = static_cast<uint32_t>(next.user_data_registers.size());
	next.memory_offset_count =
	    static_cast<uint32_t>(program.info.buffers.size() + program.info.addresses.size());

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
			if (error != nullptr) {
				*error = "shader info contains an invalid image binding class";
			}
			return false;
		}
		const auto group = static_cast<uint32_t>(*kind) - static_cast<uint32_t>(FirstImageBinding);
		if (group >= image_groups.size()) {
			if (error != nullptr) {
				*error = "shader info contains an unmapped image binding class";
			}
			return false;
		}
		auto&      resources = image_groups[group];
		const auto dynamic   = program.info.images[i].mip_mode == ImageMipMode::DynamicStorage;
		const auto count     = dynamic ? program.info.images[i].mip_count : 1u;
		if (count == 0u || (!dynamic && program.info.images[i].mip_count != 1u)) {
			if (error != nullptr) {
				*error = "image has an invalid specialized mip descriptor count";
			}
			return false;
		}
		resources.insert(resources.end(), count, i);
	}
	for (uint32_t i = 0; i < image_groups.size(); i++) {
		if (!image_groups[i].empty()) {
			AddBinding(
			    next,
			    static_cast<DescriptorBindingKind>(static_cast<uint32_t>(FirstImageBinding) + i),
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
	bool uses_gds = false;
	if (!UsesGds(program, uses_gds)) {
		if (error != nullptr) {
			*error = "typed shader contains invalid shared-memory metadata";
		}
		return false;
	}
	if (uses_gds) {
		AddBinding(next, DescriptorBindingKind::Gds);
	}
	if (!program.info.addresses.empty()) {
		std::vector<uint32_t> resources(program.info.addresses.size());
		for (uint32_t i = 0; i < resources.size(); i++) {
			resources[i] = i;
		}
		AddBinding(next, DescriptorBindingKind::AddressMemory, std::move(resources));
	}
	const bool uses_flattened_runtime =
	    !program.values->srt_reads.empty() ||
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
	return true;
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
