#include "graphics/shader/recompiler/ir/passes/ResourceTracking.h"

#include "graphics/shader/recompiler/ir/ValueProgram.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <algorithm>
#include <fmt/format.h>
#include <span>
#include <unordered_set>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint32_t SamplerBorderClampMask = (1u << 2u) | (1u << 5u) | (1u << 8u);

const char* StageName(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return "vertex";
		case ShaderType::Pixel: return "pixel";
		case ShaderType::Fetch: return "fetch";
		case ShaderType::Compute: return "compute";
		default: return "unknown";
	}
}

bool IsBufferAtomic(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::BufferAtomicSwap32:
		case ValueOpcode::BufferAtomicIAdd32:
		case ValueOpcode::BufferAtomicISub32:
		case ValueOpcode::BufferAtomicSMin32:
		case ValueOpcode::BufferAtomicUMin32:
		case ValueOpcode::BufferAtomicSMax32:
		case ValueOpcode::BufferAtomicUMax32:
		case ValueOpcode::BufferAtomicAnd32:
		case ValueOpcode::BufferAtomicOr32:
		case ValueOpcode::BufferAtomicXor32:
		case ValueOpcode::BufferAtomicFMin32:
		case ValueOpcode::BufferAtomicFMax32: return true;
		default: return false;
	}
}

bool IsBufferStore(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::StoreBufferU8:
		case ValueOpcode::StoreBufferU16:
		case ValueOpcode::StoreBufferU32: return true;
		default: return IsBufferAtomic(op);
	}
}

bool IsBuffer(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::ReadConstBuffer:
		case ValueOpcode::LoadBufferU8:
		case ValueOpcode::LoadBufferU16:
		case ValueOpcode::LoadBufferU32:
		case ValueOpcode::StoreBufferU8:
		case ValueOpcode::StoreBufferU16:
		case ValueOpcode::StoreBufferU32: return true;
		default: return IsBufferAtomic(op);
	}
}

bool IsAddressStore(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::StoreAddressU8:
		case ValueOpcode::StoreAddressU16:
		case ValueOpcode::StoreAddressU32: return true;
		default: return false;
	}
}

bool IsAddress(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::LoadAddressU8:
		case ValueOpcode::LoadAddressU16:
		case ValueOpcode::LoadAddressU32:
		case ValueOpcode::StoreAddressU8:
		case ValueOpcode::StoreAddressU16:
		case ValueOpcode::StoreAddressU32: return true;
		default: return false;
	}
}

bool IsImageAtomic(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::ImageAtomicIAdd32:
		case ValueOpcode::ImageAtomicUMin32:
		case ValueOpcode::ImageAtomicUMax32:
		case ValueOpcode::ImageAtomicAnd32:
		case ValueOpcode::ImageAtomicOr32:
		case ValueOpcode::ImageAtomicXor32: return true;
		default: return false;
	}
}

bool IsImageStore(ValueOpcode op) {
	return op == ValueOpcode::ImageWrite || IsImageAtomic(op);
}

bool IsImage(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::ImageQueryDimensions:
		case ValueOpcode::ImageQueryLod:
		case ValueOpcode::ImageRead:
		case ValueOpcode::ImageWrite:
		case ValueOpcode::ImageSampleRaw:
		case ValueOpcode::ImageGatherRaw: return true;
		default: return IsImageAtomic(op);
	}
}

bool NeedsSampler(ValueOpcode op) {
	return op == ValueOpcode::ImageQueryLod || op == ValueOpcode::ImageSampleRaw ||
	       op == ValueOpcode::ImageGatherRaw;
}

uint32_t ByteExtent(const MemoryInfo& memory) {
	const auto bytes = std::max((memory.data_bits + 7u) / 8u, 1u);
	const auto count = std::max(memory.data_dwords, 1u);
	const auto end   = static_cast<uint64_t>(memory.offset) + static_cast<uint64_t>(bytes) * count;
	return end > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(end);
}

ImageMipMode MipMode(const MemoryInfo& memory) {
	const bool storage =
	    memory.kind == ResourceKind::StorageImage || memory.kind == ResourceKind::StorageImageUint;
	return storage && memory.image_has_mip ? ImageMipMode::DynamicStorage : ImageMipMode::None;
}

class Tracker {
public:
	Tracker(Program& program, ValueProgram& values)
	    : m_program(program), m_values(values), m_info(program.info) {
		m_info.buffers.clear();
		m_info.addresses.clear();
		m_info.images.clear();
		m_info.samplers.clear();
		m_info.sampled_pairs.clear();
	}

	bool Run(std::string* error) {
		if (m_program.resource_tracking_complete) {
			return Fail(0, error, "resources already tracked");
		}
		if (!m_program.srt_plan_complete) {
			return Fail(0, error, "SRT plan is not ready");
		}
		PlanIndirectImages();
		for (auto* block: m_values.blocks) {
			for (auto& inst: *block) {
				if (!Collect(inst, error)) {
					return false;
				}
			}
		}
		LinkImageAliases();
		for (const auto& patch: m_handle_patches) {
			patch.handle->SetFlags<uint32_t>(patch.resource);
		}
		for (const auto& patch: m_memory_patches) {
			auto& memory    = m_values.memory_info[patch.index];
			memory.resource = patch.resource;
			if (patch.has_sampler) {
				memory.sampler = patch.sampler;
			}
		}
		for (const auto& plan: m_indirect_images) {
			plan.handle->SetArg(0, plan.key);
			for (uint32_t dword = 0; dword < 4u; dword++) {
				plan.handle->SetArg(dword + 1u, plan.roots[dword + 4u]);
			}
			for (uint32_t dword = 5u; dword < plan.roots.size(); dword++) {
				plan.handle->SetArg(dword, plan.key);
			}
			for (const auto index: plan.memory) {
				m_values.memory_info[index].planning_only = true;
			}
		}
		std::erase_if(m_values.dynamic_reads, [&](Value value) {
			const auto* inst = value.Resolve().TryInstruction();
			return std::ranges::any_of(m_indirect_images, [&](const IndirectImagePlan& plan) {
				return std::ranges::find(plan.reads, inst) != plan.reads.end();
			});
		});
		m_values.descriptor_sources          = std::move(m_sources);
		m_program.info                       = std::move(m_info);
		m_program.resource_tracking_complete = true;
		return true;
	}

private:
	struct HandlePatch {
		Inst*    handle   = nullptr;
		uint32_t resource = 0;
	};

	struct MemoryPatch {
		uint32_t index       = 0;
		uint32_t resource    = 0;
		uint32_t sampler     = 0;
		bool     has_sampler = false;
	};

	struct IndirectImagePlan {
		Inst*                      handle = nullptr;
		uint32_t                   source = 0;
		Value                      key;
		std::array<Value, 8>       roots {};
		std::array<uint32_t, 8>    memory {};
		std::array<const Inst*, 8> reads {};
	};

	bool Fail(uint32_t pc, std::string* error, const std::string& reason) const {
		if (error != nullptr) {
			*error = fmt::format("shader resource tracking: hash=0x{:016x} stage={} pc=0x{:08x} {}",
			                     m_program.shader_hash, StageName(m_program.stage), pc, reason);
		}
		return false;
	}

	struct AddressPart {
		Value value;
		bool  rooted = false;
	};

	AddressPart FindAddressPart(Value value, Value active,
	                            std::unordered_set<const Inst*>& visiting) const {
		value = value.Resolve();
		if (value.IsImmediate()) {
			return {value, false};
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || !visiting.insert(inst).second) {
			return {};
		}
		const auto finish = [&](AddressPart part) {
			visiting.erase(inst);
			return part;
		};
		std::string reason;
		if (ValidateRuntimeValue(m_program, value, reason)) {
			return finish({value, true});
		}
		const auto merge = [&](AddressPart left, AddressPart right, bool require_both) {
			if (left.value.IsEmpty() || right.value.IsEmpty()) {
				return require_both ? AddressPart {} : (left.value.IsEmpty() ? right : left);
			}
			return EquivalentValue(m_values, left.value, right.value)
			           ? AddressPart {left.value, left.rooted || right.rooted}
			           : AddressPart {};
		};
		switch (inst->GetOpcode()) {
			case ValueOpcode::SelectU32:
				if (EquivalentValue(m_values, inst->Arg(0), active)) {
					return finish(FindAddressPart(inst->Arg(1), active, visiting));
				}
				return finish(merge(FindAddressPart(inst->Arg(1), active, visiting),
				                    FindAddressPart(inst->Arg(2), active, visiting), true));
			case ValueOpcode::Phi: {
				const auto invariant = ResolveInvariantPhi(m_values, value);
				return finish(invariant.IsEmpty() ? AddressPart {}
				                                  : FindAddressPart(invariant, active, visiting));
			}
			case ValueOpcode::IAdd32:
			case ValueOpcode::ISub32: {
				const auto left  = FindAddressPart(inst->Arg(0), active, visiting);
				const auto right = FindAddressPart(inst->Arg(1), active, visiting);
				if (left.rooted == right.rooted ||
				    (inst->GetOpcode() == ValueOpcode::ISub32 && right.rooted)) {
					return finish({});
				}
				return finish(left.rooted ? left : right);
			}
			case ValueOpcode::CompositeExtractU32x2: {
				const auto* source = inst->Arg(0).ResolveInstruction();
				if (source == nullptr || source->GetOpcode() != ValueOpcode::IAddCarry32) {
					return finish({});
				}
				const auto left  = FindAddressPart(source->Arg(0), active, visiting);
				const auto right = FindAddressPart(source->Arg(1), active, visiting);
				if (left.rooted == right.rooted) {
					return finish({});
				}
				return finish(left.rooted ? left : right);
			}
			default: return finish({});
		}
	}

	bool MakeFlatAddressSource(const Inst& handle, Value active,
	                           DescriptorSource& descriptor) const {
		std::unordered_set<const Inst*> visiting;
		auto                            low = FindAddressPart(handle.Arg(0), active, visiting);
		visiting.clear();
		auto high = FindAddressPart(handle.Arg(1), active, visiting);
		if ((!low.rooted && !high.rooted) || low.value.IsEmpty() || high.value.IsEmpty()) {
			return false;
		}
		descriptor.dword_count = 2;
		descriptor.dwords[0]   = low.value;
		descriptor.dwords[1]   = high.value;
		return true;
	}

	bool MakeSource(const Inst& handle, uint32_t width, bool sampler, DescriptorSource& descriptor,
	                uint32_t pc, std::string* error) const {
		if (handle.NumArgs() != width) {
			return Fail(pc, error,
			            fmt::format("{} has {} descriptor dwords, expected {}",
			                        ValueOpcodeName(handle.GetOpcode()), handle.NumArgs(), width));
		}
		descriptor.dword_count = width;
		for (uint32_t i = 0; i < width; i++) {
			descriptor.dwords[i] = handle.Arg(i).Resolve();
		}
		const auto dword0 = descriptor.dwords[0].Resolve();
		if (sampler && dword0.IsImmediate() && dword0.GetType() == Type::U32 &&
		    (dword0.U32() & SamplerBorderClampMask) == 0) {
			// Border color and its table index are unused unless a clamp axis selects border mode.
			descriptor.dwords[3] = Value(0u);
		}
		return true;
	}

	bool ValidateSource(const DescriptorSource& descriptor, std::string& reason,
	                    uint32_t& bad_dword) const {
		for (uint32_t i = 0; i < descriptor.dword_count; i++) {
			bad_dword = i;
			if (descriptor.dwords[i].Resolve().GetType() != Type::U32) {
				reason = "is not U32";
				return false;
			}
			if (!ValidateRuntimeValue(m_program, descriptor.dwords[i], reason)) {
				return false;
			}
		}
		return true;
	}

	uint32_t InternSource(const DescriptorSource& descriptor) {
		for (uint32_t candidate = 0; candidate < m_sources.size(); candidate++) {
			const auto& current = m_sources[candidate];
			if (current.dword_count != descriptor.dword_count ||
			    current.indirect_image != descriptor.indirect_image) {
				continue;
			}
			bool same = true;
			for (uint32_t i = 0; i < descriptor.dword_count; i++) {
				same = same && EquivalentValue(m_values, current.dwords[i], descriptor.dwords[i]);
			}
			if (same) {
				return candidate;
			}
		}
		m_sources.push_back(descriptor);
		return static_cast<uint32_t>(m_sources.size() - 1);
	}

	static bool ImmediateU32(Value value, uint32_t& result) {
		value = value.Resolve();
		if (!value.IsImmediate() || value.GetType() != Type::U32) {
			return false;
		}
		result = value.U32();
		return true;
	}

	static bool UsesOnly(const Inst& value, std::span<const Inst* const> users) {
		return !value.Uses().empty() && std::ranges::all_of(value.Uses(), [&](const Use& use) {
			return std::ranges::find(users, use.user) != users.end();
		});
	}

	const MemoryInfo* ScalarReadMemory(const Inst& read, uint32_t& index) const {
		if (read.GetOpcode() != ValueOpcode::ReadConstBuffer || read.NumArgs() != 2u) {
			return nullptr;
		}
		index = read.Flags<MemoryFlags>().index;
		if (index >= m_values.memory_info.size()) {
			return nullptr;
		}
		const auto& memory = m_values.memory_info[index];
		return memory.kind == ResourceKind::ScalarBuffer && memory.data_bits == 32u &&
		               memory.data_dwords == 1u
		           ? &memory
		           : nullptr;
	}

	bool MemoryIndexBelongsTo(uint32_t index, const Inst& owner) const {
		for (const auto* block: m_values.blocks) {
			for (const auto& inst: *block) {
				const auto op = inst.GetOpcode();
				if ((!IsBuffer(op) && !IsAddress(op) && !IsImage(op)) || &inst == &owner) {
					continue;
				}
				if (inst.Flags<MemoryFlags>().index == index) {
					return false;
				}
			}
		}
		return true;
	}

	bool MakeRuntimeBufferSource(const Inst& handle, uint32_t pc, uint32_t& source,
	                             DescriptorSource& descriptor) {
		if (handle.GetOpcode() != ValueOpcode::GetBufferResource ||
		    !MakeSource(handle, 4u, false, descriptor, pc, nullptr)) {
			return false;
		}
		std::string reason;
		uint32_t    bad_dword = 0;
		if (!ValidateSource(descriptor, reason, bad_dword)) {
			return false;
		}
		source = InternSource(descriptor);
		return true;
	}

	bool MatchMaterialOffset(Value value, Value& selector, uint32_t& stride,
	                         uint32_t& offset) const {
		value           = value.Resolve();
		offset          = 0;
		auto* candidate = value.TryInstruction();
		if (candidate != nullptr && candidate->GetOpcode() == ValueOpcode::IAdd32 &&
		    candidate->NumArgs() == 2u) {
			uint32_t immediate = 0;
			if (ImmediateU32(candidate->Arg(0), immediate)) {
				value = candidate->Arg(1).Resolve();
			} else if (ImmediateU32(candidate->Arg(1), immediate)) {
				value = candidate->Arg(0).Resolve();
			} else {
				return false;
			}
			offset = immediate;
		}
		const auto* multiply = value.TryInstruction();
		if (multiply == nullptr || multiply->GetOpcode() != ValueOpcode::IMul32 ||
		    multiply->NumArgs() != 2u) {
			return false;
		}
		if (ImmediateU32(multiply->Arg(0), stride)) {
			selector = multiply->Arg(1).Resolve();
		} else if (ImmediateU32(multiply->Arg(1), stride)) {
			selector = multiply->Arg(0).Resolve();
		} else {
			return false;
		}
		const auto* selector_inst = selector.TryInstruction();
		return stride != 0u && selector_inst != nullptr &&
		       selector_inst->GetOpcode() == ValueOpcode::ReadFirstLane;
	}

	bool TryMakeIndirectImage(Inst& handle, uint32_t pc, IndirectImagePlan& plan) {
		if (handle.GetOpcode() != ValueOpcode::GetImageResource || handle.NumArgs() != 8u) {
			return false;
		}

		std::array<Inst*, 8> heap_reads {};
		Inst*                heap_handle = nullptr;
		Value                heap_offset;
		for (uint32_t dword = 0; dword < heap_reads.size(); dword++) {
			heap_reads[dword] = handle.Arg(dword).Resolve().TryInstruction();
			if (heap_reads[dword] == nullptr) {
				return false;
			}
			uint32_t    memory_index = 0;
			const auto* memory       = ScalarReadMemory(*heap_reads[dword], memory_index);
			if (memory == nullptr || memory->offset != dword * sizeof(uint32_t) ||
			    !MemoryIndexBelongsTo(memory_index, *heap_reads[dword])) {
				return false;
			}
			auto* current_handle = heap_reads[dword]->Arg(0).Resolve().TryInstruction();
			if (current_handle == nullptr ||
			    (heap_handle != nullptr && current_handle != heap_handle)) {
				return false;
			}
			heap_handle = current_handle;
			if (dword == 0u) {
				heap_offset = heap_reads[dword]->Arg(1).Resolve();
			} else if (!EquivalentValue(m_values, heap_offset, heap_reads[dword]->Arg(1))) {
				return false;
			}
			plan.memory[dword] = memory_index;
			plan.reads[dword]  = heap_reads[dword];
		}

		const auto* shift        = heap_offset.TryInstruction();
		uint32_t    shift_amount = 0;
		if (shift == nullptr || shift->GetOpcode() != ValueOpcode::ShiftLeftLogical32 ||
		    shift->NumArgs() != 2u || !ImmediateU32(shift->Arg(1), shift_amount) ||
		    shift_amount != 5u) {
			return false;
		}
		auto* material_read = shift->Arg(0).Resolve().TryInstruction();
		if (material_read == nullptr) {
			return false;
		}
		uint32_t    material_memory_index = 0;
		const auto* material_memory       = ScalarReadMemory(*material_read, material_memory_index);
		if (material_memory == nullptr || material_memory->offset != 0u ||
		    !MemoryIndexBelongsTo(material_memory_index, *material_read)) {
			return false;
		}
		auto* material_handle = material_read->Arg(0).Resolve().TryInstruction();
		if (material_handle == nullptr) {
			return false;
		}

		Value    selector;
		uint32_t selector_stride = 0;
		uint32_t selector_offset = 0;
		if (!MatchMaterialOffset(material_read->Arg(1), selector, selector_stride,
		                         selector_offset)) {
			return false;
		}

		const std::array<const Inst*, 1> material_users {shift};
		std::array<const Inst*, 8>       heap_users {};
		std::copy(heap_reads.begin(), heap_reads.end(), heap_users.begin());
		const std::array<const Inst*, 1> image_users {&handle};
		if (!UsesOnly(*material_read, material_users) || !UsesOnly(*shift, heap_users)) {
			return false;
		}
		for (const auto* read: heap_reads) {
			if (!UsesOnly(*read, image_users)) {
				return false;
			}
		}

		DescriptorSource material_source;
		DescriptorSource heap_source;
		uint32_t         material_source_index = 0;
		uint32_t         heap_source_index     = 0;
		if (!MakeRuntimeBufferSource(*material_handle, pc, material_source_index,
		                             material_source) ||
		    !MakeRuntimeBufferSource(*heap_handle, pc, heap_source_index, heap_source)) {
			return false;
		}

		DescriptorSource image_source;
		image_source.dword_count = 8u;
		std::copy(material_source.dwords.begin(), material_source.dwords.begin() + 4u,
		          image_source.dwords.begin());
		std::copy(heap_source.dwords.begin(), heap_source.dwords.begin() + 4u,
		          image_source.dwords.begin() + 4u);
		image_source.indirect_image = DescriptorSource::IndirectImage {
		    material_source_index, heap_source_index, selector_stride, selector_offset, 0u};

		plan.handle = &handle;
		plan.source = InternSource(image_source);
		plan.key    = Value(material_read);
		plan.roots  = image_source.dwords;
		return true;
	}

	const IndirectImagePlan* FindIndirectImage(const Inst& handle) const {
		const auto found =
		    std::ranges::find_if(m_indirect_images, [&](const IndirectImagePlan& plan) {
			    return plan.handle == &handle;
		    });
		return found == m_indirect_images.end() ? nullptr : &*found;
	}

	bool IsIndirectPlanningMemory(uint32_t index) const {
		return std::ranges::any_of(m_indirect_images, [&](const IndirectImagePlan& plan) {
			return std::ranges::find(plan.memory, index) != plan.memory.end();
		});
	}

	void PlanIndirectImages() {
		for (auto* block: m_values.blocks) {
			for (auto& inst: *block) {
				if (!IsImage(inst.GetOpcode()) || inst.NumArgs() == 0u) {
					continue;
				}
				auto* handle = inst.Arg(0).Resolve().TryInstruction();
				if (handle == nullptr || FindIndirectImage(*handle) != nullptr) {
					continue;
				}
				IndirectImagePlan plan;
				if (TryMakeIndirectImage(*handle, inst.Flags<MemoryFlags>().pc, plan)) {
					m_indirect_images.push_back(std::move(plan));
				}
			}
		}
	}

	bool GetHandle(Value value, ValueOpcode expected, uint32_t width, uint32_t pc, Inst*& handle,
	               uint32_t& source, std::string* error, bool sampler = false) {
		handle = value.Resolve().TryInstruction();
		if (handle == nullptr || handle->GetOpcode() != expected) {
			return Fail(pc, error,
			            fmt::format("memory operation requires {}", ValueOpcodeName(expected)));
		}
		DescriptorSource descriptor;
		if (!MakeSource(*handle, width, sampler, descriptor, pc, error)) {
			return false;
		}
		std::string reason;
		uint32_t    bad_dword = 0;
		if (!ValidateSource(descriptor, reason, bad_dword)) {
			return Fail(
			    pc, error,
			    fmt::format("{} dword {} {}", ValueOpcodeName(expected), bad_dword, reason));
		}
		source = InternSource(descriptor);
		return true;
	}

	bool GetAddressHandle(const Inst& memory_inst, Value value, uint32_t pc, Inst*& handle,
	                      uint32_t& source, bool& unbased, std::string* error) {
		handle = value.Resolve().TryInstruction();
		if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetAddressResource) {
			return Fail(pc, error, "address operation requires GetAddressResource");
		}
		if (handle->NumArgs() != 2) {
			return Fail(pc, error, "GetAddressResource must have two address dwords");
		}
		DescriptorSource descriptor;
		const auto&      memory = m_values.memory_info[memory_inst.Flags<MemoryFlags>().index];
		if (memory.address_is_full) {
			const auto active = memory_inst.Arg(memory_inst.NumArgs() - 1u);
			if (memory.kind != ResourceKind::Flat ||
			    !MakeFlatAddressSource(*handle, active, descriptor)) {
				unbased = true;
				source  = UINT32_MAX;
				return true;
			}
		} else if (!MakeSource(*handle, 2, false, descriptor, pc, error)) {
			return false;
		}
		std::string reason;
		uint32_t    bad_dword = 0;
		if (!ValidateSource(descriptor, reason, bad_dword)) {
			if (memory.address_is_full) {
				unbased = true;
				source  = UINT32_MAX;
				return true;
			}
			return Fail(
			    pc, error,
			    fmt::format("scalar memory base dword {} is unresolved: {}", bad_dword, reason));
		}
		unbased = false;
		source  = InternSource(descriptor);
		return true;
	}

	uint32_t AddBuffer(uint32_t source, const MemoryInfo& memory, ValueOpcode op, uint32_t pc) {
		for (uint32_t i = 0; i < m_info.buffers.size(); i++) {
			if (m_info.buffers[i].source == source) {
				Merge(m_info.buffers[i], memory, op, pc);
				return i;
			}
		}
		if (m_info.buffers.size() >= ShaderInfo::MaxBuffers) {
			return UINT32_MAX;
		}
		BufferResource resource;
		resource.source       = source;
		resource.first_use_pc = pc;
		Merge(resource, memory, op, pc);
		m_info.buffers.push_back(resource);
		return static_cast<uint32_t>(m_info.buffers.size() - 1);
	}

	static void Merge(BufferResource& resource, const MemoryInfo& memory, ValueOpcode op,
	                  uint32_t pc) {
		const bool atomic        = IsBufferAtomic(op);
		const bool write         = IsBufferStore(op);
		resource.first_use_pc    = std::min(resource.first_use_pc, pc);
		resource.max_byte_extent = std::max(resource.max_byte_extent, ByteExtent(memory));
		resource.read            = resource.read || !write || atomic;
		resource.written         = resource.written || write;
		resource.atomic          = resource.atomic || atomic;
		resource.formatted       = resource.formatted || memory.formatted;
		resource.scalar          = resource.scalar || op == ValueOpcode::ReadConstBuffer ||
		                           memory.kind == ResourceKind::ScalarBuffer;
	}

	uint32_t AddImage(uint32_t source, const MemoryInfo& memory, ValueOpcode op, uint32_t pc) {
		const auto mip   = MipMode(memory);
		const bool depth = (memory.image_sample_flags & Decoder::ImageSampleFlagCompare) != 0;
		for (uint32_t i = 0; i < m_info.images.size(); i++) {
			auto& image = m_info.images[i];
			if (image.source == source && image.kind == memory.kind &&
			    image.dimension == memory.image_dimension && image.mip_mode == mip &&
			    image.depth_compare == depth) {
				Merge(image, op, pc);
				return i;
			}
		}
		if (m_info.images.size() >= ShaderInfo::MaxImages) {
			return UINT32_MAX;
		}
		ImageResource image;
		image.source        = source;
		image.first_use_pc  = pc;
		image.kind          = memory.kind;
		image.dimension     = memory.image_dimension;
		image.mip_mode      = mip;
		image.depth_compare = depth;
		Merge(image, op, pc);
		m_info.images.push_back(image);
		return static_cast<uint32_t>(m_info.images.size() - 1);
	}

	static void Merge(ImageResource& image, ValueOpcode op, uint32_t pc) {
		const bool atomic  = IsImageAtomic(op);
		const bool write   = IsImageStore(op);
		image.first_use_pc = std::min(image.first_use_pc, pc);
		image.read         = image.read || !write || atomic;
		image.written      = image.written || write;
		image.atomic       = image.atomic || atomic;
	}

	uint32_t AddSampler(uint32_t source, uint32_t pc) {
		for (uint32_t i = 0; i < m_info.samplers.size(); i++) {
			if (m_info.samplers[i].source == source) {
				m_info.samplers[i].first_use_pc = std::min(m_info.samplers[i].first_use_pc, pc);
				return i;
			}
		}
		if (m_info.samplers.size() >= ShaderInfo::MaxSamplers) {
			return UINT32_MAX;
		}
		m_info.samplers.push_back({source, pc});
		return static_cast<uint32_t>(m_info.samplers.size() - 1);
	}

	uint32_t AddAddress(uint32_t source, bool unbased, const MemoryInfo& memory, ValueOpcode op,
	                    uint32_t pc) {
		auto immediate = static_cast<int32_t>(memory.offset);
		if (memory.kind == ResourceKind::ScalarAddress) {
			immediate = static_cast<int32_t>(static_cast<uint32_t>(immediate) & ~3u);
		}
		const auto min_offset = unbased ? 0 : std::min(immediate, 0);
		for (uint32_t i = 0; i < m_info.addresses.size(); i++) {
			auto& address = m_info.addresses[i];
			if (address.source == source && address.unbased == unbased &&
			    address.kind == memory.kind) {
				address.first_use_pc = std::min(address.first_use_pc, pc);
				address.min_offset   = std::min(address.min_offset, min_offset);
				address.read         = address.read || !IsAddressStore(op);
				address.written      = address.written || IsAddressStore(op);
				return i;
			}
		}
		if (m_info.addresses.size() >= ShaderInfo::MaxAddresses) {
			return UINT32_MAX;
		}
		AddressResource address;
		address.source       = source;
		address.first_use_pc = pc;
		address.kind         = memory.kind;
		address.min_offset   = min_offset;
		address.unbased      = unbased;
		address.read         = !IsAddressStore(op);
		address.written      = IsAddressStore(op);
		m_info.addresses.push_back(address);
		return static_cast<uint32_t>(m_info.addresses.size() - 1);
	}

	bool AddSampledPair(uint32_t image, uint32_t sampler, uint32_t pc, std::string* error) {
		for (auto& pair: m_info.sampled_pairs) {
			if (pair.image == image && pair.sampler == sampler) {
				pair.first_use_pc = std::min(pair.first_use_pc, pc);
				return true;
			}
		}
		if (m_info.sampled_pairs.size() >= ShaderInfo::MaxSampledPairs) {
			return Fail(pc, error, "sampled image/sampler pair limit exceeded");
		}
		m_info.sampled_pairs.push_back({image, sampler, pc});
		return true;
	}

	bool AddHandlePatch(Inst* handle, uint32_t resource, uint32_t pc, std::string* error) {
		for (const auto& patch: m_handle_patches) {
			if (patch.handle == handle) {
				return patch.resource == resource ||
				       Fail(pc, error,
				            fmt::format("{} is reused with incompatible resource classes",
				                        ValueOpcodeName(handle->GetOpcode())));
			}
		}
		m_handle_patches.push_back({handle, resource});
		return true;
	}

	bool AddMemoryPatch(uint32_t index, uint32_t resource, uint32_t sampler, bool has_sampler,
	                    uint32_t pc, std::string* error) {
		for (auto& patch: m_memory_patches) {
			if (patch.index != index) {
				continue;
			}
			if (patch.resource != resource ||
			    (has_sampler && patch.has_sampler && patch.sampler != sampler)) {
				return Fail(pc, error, "memory metadata is reused with incompatible resources");
			}
			if (has_sampler) {
				patch.sampler     = sampler;
				patch.has_sampler = true;
			}
			return true;
		}
		m_memory_patches.push_back({index, resource, sampler, has_sampler});
		return true;
	}

	bool Collect(Inst& inst, std::string* error) {
		const auto op = inst.GetOpcode();
		if (!IsBuffer(op) && !IsAddress(op) && !IsImage(op)) {
			return true;
		}
		const auto flags = inst.Flags<MemoryFlags>();
		if (flags.index >= m_values.memory_info.size()) {
			return Fail(flags.pc, error,
			            fmt::format("memory metadata index {} is out of range", flags.index));
		}
		if (inst.NumArgs() == 0) {
			return Fail(flags.pc, error, "memory operation has no resource handle");
		}
		const auto& memory = m_values.memory_info[flags.index];
		if (memory.planning_only || IsIndirectPlanningMemory(flags.index)) {
			return true;
		}
		Inst*    handle   = nullptr;
		uint32_t source   = 0;
		uint32_t resource = 0;

		if (IsBuffer(op)) {
			if (!GetHandle(inst.Arg(0), ValueOpcode::GetBufferResource, 4, flags.pc, handle, source,
			               error)) {
				return false;
			}
			resource = AddBuffer(source, memory, op, flags.pc);
			if (resource == UINT32_MAX) {
				return Fail(flags.pc, error, "buffer resource limit exceeded");
			}
			return AddHandlePatch(handle, resource, flags.pc, error) &&
			       AddMemoryPatch(flags.index, resource, 0, false, flags.pc, error);
		}
		if (IsAddress(op)) {
			bool unbased = false;
			if (!GetAddressHandle(inst, inst.Arg(0), flags.pc, handle, source, unbased, error)) {
				return false;
			}
			resource = AddAddress(source, unbased, memory, op, flags.pc);
			if (resource == UINT32_MAX) {
				return Fail(flags.pc, error, "address resource limit exceeded");
			}
			return AddHandlePatch(handle, resource, flags.pc, error) &&
			       AddMemoryPatch(flags.index, resource, 0, false, flags.pc, error);
		}

		handle               = inst.Arg(0).Resolve().TryInstruction();
		const auto* indirect = handle != nullptr ? FindIndirectImage(*handle) : nullptr;
		if (indirect != nullptr) {
			source = indirect->source;
		} else if (!GetHandle(inst.Arg(0), ValueOpcode::GetImageResource, 8, flags.pc, handle,
		                      source, error)) {
			return false;
		}
		resource = AddImage(source, memory, op, flags.pc);
		if (resource == UINT32_MAX) {
			return Fail(flags.pc, error, "image resource limit exceeded");
		}
		if (!AddHandlePatch(handle, resource, flags.pc, error)) {
			return false;
		}
		uint32_t sampler = 0;
		if (NeedsSampler(op)) {
			if (inst.NumArgs() < 2) {
				return Fail(flags.pc, error, "sampled image operation has no sampler handle");
			}
			Inst*    sampler_handle = nullptr;
			uint32_t sampler_source = 0;
			if (!GetHandle(inst.Arg(1), ValueOpcode::GetSamplerResource, 4, flags.pc,
			               sampler_handle, sampler_source, error, true)) {
				return false;
			}
			sampler = AddSampler(sampler_source, flags.pc);
			if (sampler == UINT32_MAX) {
				return Fail(flags.pc, error, "sampler resource limit exceeded");
			}
			if (!AddHandlePatch(sampler_handle, sampler, flags.pc, error) ||
			    !AddSampledPair(resource, sampler, flags.pc, error)) {
				return false;
			}
		}
		return AddMemoryPatch(flags.index, resource, sampler, NeedsSampler(op), flags.pc, error);
	}

	const DescriptorSource* Source(uint32_t source) const {
		return source < m_sources.size() ? &m_sources[source] : nullptr;
	}

	void LinkImageAliases() {
		for (auto& buffer: m_info.buffers) {
			const auto* buffer_source = Source(buffer.source);
			if (buffer_source == nullptr || buffer_source->dword_count != 4) {
				continue;
			}
			for (uint32_t image = 0; image < m_info.images.size(); image++) {
				const auto* image_source = Source(m_info.images[image].source);
				if (image_source == nullptr || image_source->dword_count != 8 ||
				    image_source->indirect_image.has_value()) {
					continue;
				}
				bool alias = true;
				for (uint32_t dword = 0; dword < 4; dword++) {
					alias = alias && EquivalentValue(m_values, buffer_source->dwords[dword],
					                                 image_source->dwords[dword]);
				}
				if (alias) {
					buffer.image_alias = image;
					break;
				}
			}
		}
	}

	Program&                       m_program;
	ValueProgram&                  m_values;
	ShaderInfo                     m_info;
	std::vector<DescriptorSource>  m_sources;
	std::vector<HandlePatch>       m_handle_patches;
	std::vector<MemoryPatch>       m_memory_patches;
	std::vector<IndirectImagePlan> m_indirect_images;
};

} // namespace

bool TrackResources(Program& program, std::string* error) {
	if (program.values == nullptr) {
		if (error != nullptr) {
			*error = "shader resource tracking requires typed IR";
		}
		return false;
	}
	return Tracker(program, *program.values).Run(error);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
