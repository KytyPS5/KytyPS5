#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"

#include "graphics/shader/recompiler/ir/ValueProgram.h"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint32_t MaxPushConstantBytes = 128;

constexpr std::array ImageBindingKinds = {
    DescriptorBindingKind::Sampled1D,
    DescriptorBindingKind::Sampled1DArray,
    DescriptorBindingKind::Sampled2D,
    DescriptorBindingKind::Sampled2DArray,
    DescriptorBindingKind::Sampled2DMsaa,
    DescriptorBindingKind::Sampled2DMsaaArray,
    DescriptorBindingKind::Sampled3D,
    DescriptorBindingKind::SampledUint1D,
    DescriptorBindingKind::SampledUint1DArray,
    DescriptorBindingKind::SampledUint2D,
    DescriptorBindingKind::SampledUint2DArray,
    DescriptorBindingKind::SampledUint2DMsaa,
    DescriptorBindingKind::SampledUint2DMsaaArray,
    DescriptorBindingKind::SampledUint3D,
    DescriptorBindingKind::Storage1D,
    DescriptorBindingKind::Storage1DArray,
    DescriptorBindingKind::Storage2D,
    DescriptorBindingKind::Storage2DArray,
    DescriptorBindingKind::Storage3D,
    DescriptorBindingKind::StorageUint1D,
    DescriptorBindingKind::StorageUint1DArray,
    DescriptorBindingKind::StorageUint2D,
    DescriptorBindingKind::StorageUint2DArray,
    DescriptorBindingKind::StorageUint3D,
};

bool ImageBinding(const ImageResource& image, DescriptorBindingKind& result) {
	using Dimension = Decoder::ImageDimension;
	using Kind      = DescriptorBindingKind;

	switch (image.kind) {
		case ResourceKind::Image:
			switch (image.dimension) {
				case Dimension::Dim1D: result = Kind::Sampled1D; return true;
				case Dimension::Dim1DArray: result = Kind::Sampled1DArray; return true;
				case Dimension::Dim2D: result = Kind::Sampled2D; return true;
				case Dimension::Dim2DArray: result = Kind::Sampled2DArray; return true;
				case Dimension::Dim2DMsaa: result = Kind::Sampled2DMsaa; return true;
				case Dimension::Dim2DMsaaArray: result = Kind::Sampled2DMsaaArray; return true;
				case Dimension::Dim3D: result = Kind::Sampled3D; return true;
				default: return false;
			}
		case ResourceKind::ImageUint:
			switch (image.dimension) {
				case Dimension::Dim1D: result = Kind::SampledUint1D; return true;
				case Dimension::Dim1DArray: result = Kind::SampledUint1DArray; return true;
				case Dimension::Dim2D: result = Kind::SampledUint2D; return true;
				case Dimension::Dim2DArray: result = Kind::SampledUint2DArray; return true;
				case Dimension::Dim2DMsaa: result = Kind::SampledUint2DMsaa; return true;
				case Dimension::Dim2DMsaaArray: result = Kind::SampledUint2DMsaaArray; return true;
				case Dimension::Dim3D: result = Kind::SampledUint3D; return true;
				default: return false;
			}
		case ResourceKind::StorageImage:
			switch (image.dimension) {
				case Dimension::Dim1D: result = Kind::Storage1D; return true;
				case Dimension::Dim1DArray: result = Kind::Storage1DArray; return true;
				case Dimension::Dim2D: result = Kind::Storage2D; return true;
				case Dimension::Dim2DArray: result = Kind::Storage2DArray; return true;
				case Dimension::Dim3D: result = Kind::Storage3D; return true;
				default: return false;
			}
		case ResourceKind::StorageImageUint:
			switch (image.dimension) {
				case Dimension::Dim1D: result = Kind::StorageUint1D; return true;
				case Dimension::Dim1DArray: result = Kind::StorageUint1DArray; return true;
				case Dimension::Dim2D: result = Kind::StorageUint2D; return true;
				case Dimension::Dim2DArray: result = Kind::StorageUint2DArray; return true;
				case Dimension::Dim3D: result = Kind::StorageUint3D; return true;
				default: return false;
			}
		default: return false;
	}
}

bool CollectUserData(const Program& program, std::vector<uint32_t>& result) {
	std::set<uint32_t> registers;
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
			registers.insert(index);
		}
	}
	result.assign(registers.begin(), registers.end());
	return true;
}

void AddBinding(BindingLayout& layout, DescriptorBindingKind kind,
                std::vector<uint32_t> resources = {}) {
	layout.descriptors.push_back(
	    {kind, static_cast<uint32_t>(layout.descriptors.size()), std::move(resources)});
}

bool UsesGds(const Program& program) {
	for (const auto* block: program.values->blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() == ValueOpcode::GetGdsResource && inst.HasUses()) {
				return true;
			}
			for (size_t index = 0; index < inst.NumArgs(); index++) {
				if (inst.Arg(index).GetType() == Type::GdsResource) {
					return true;
				}
			}
		}
	}
	return false;
}

} // namespace

bool AllocateBindings(Program& program, const BindingLayoutOptions& options, std::string* error) {
	if (!program.shader_info_complete || program.binding_layout_complete) {
		if (error != nullptr) {
			*error = !program.shader_info_complete ? "shader info is not ready"
			                                       : "binding layout already allocated";
		}
		return false;
	}
	if (options.push_constant_offset > MaxPushConstantBytes) {
		if (error != nullptr) {
			*error = "push-constant offset exceeds the Vulkan minimum guarantee";
		}
		return false;
	}
	if (options.push_constant_offset % 4u != 0) {
		if (error != nullptr) {
			*error = "push-constant offset is not dword aligned";
		}
		return false;
	}
	if (program.values == nullptr) {
		if (error != nullptr) {
			*error = "typed value program is not available";
		}
		return false;
	}

	BindingLayout next;
	next.descriptor_set       = options.descriptor_set;
	next.push_constant_offset = options.push_constant_offset;
	if (!CollectUserData(program, next.user_data_registers)) {
		if (error != nullptr) {
			*error = "typed shader contains an invalid user-data register";
		}
		return false;
	}
	next.buffer_offset_dword = static_cast<uint32_t>(next.user_data_registers.size());
	next.buffer_offset_count = static_cast<uint32_t>(program.info.buffers.size());

	if (!program.info.buffers.empty()) {
		std::vector<uint32_t> resources(program.info.buffers.size());
		for (uint32_t i = 0; i < resources.size(); i++) {
			resources[i] = i;
		}
		AddBinding(next, DescriptorBindingKind::Buffers, std::move(resources));
	}

	std::array<std::vector<uint32_t>, ImageBindingKinds.size()> image_groups;
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		DescriptorBindingKind kind;
		if (!ImageBinding(program.info.images[i], kind)) {
			if (error != nullptr) {
				*error = "shader info contains an invalid image binding class";
			}
			return false;
		}
		const auto group = std::find(ImageBindingKinds.begin(), ImageBindingKinds.end(), kind);
		if (group == ImageBindingKinds.end()) {
			if (error != nullptr) {
				*error = "shader info contains an unmapped image binding class";
			}
			return false;
		}
		image_groups[static_cast<size_t>(group - ImageBindingKinds.begin())].push_back(i);
	}
	for (uint32_t i = 0; i < image_groups.size(); i++) {
		if (!image_groups[i].empty()) {
			AddBinding(next, ImageBindingKinds[i], std::move(image_groups[i]));
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
	if (!program.info.addresses.empty()) {
		std::vector<uint32_t> resources(program.info.addresses.size());
		for (uint32_t i = 0; i < resources.size(); i++) {
			resources[i] = i;
		}
		AddBinding(next, DescriptorBindingKind::AddressMemory, std::move(resources));
	}
	if (!program.values->srt_reads.empty()) {
		AddBinding(next, DescriptorBindingKind::FlattenedSrt);
	}

	const auto available_push_dwords = (MaxPushConstantBytes - options.push_constant_offset) / 4u;
	const auto push_limit            = std::min(options.max_push_dwords, available_push_dwords);
	if (next.ShaderDataDwords() <= push_limit) {
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
