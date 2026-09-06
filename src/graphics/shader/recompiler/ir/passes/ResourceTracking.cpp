#include "graphics/shader/recompiler/ir/passes/ResourceTracking.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <fmt/format.h>
#include <span>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint32_t SamplerBorderClampMask    = (1u << 2u) | (1u << 5u) | (1u << 8u);
constexpr uint32_t SamplerDword3ReservedMask = 0x3ffff000u;
constexpr uint32_t DenseIndirectImageShift      = 5u;
constexpr uint32_t MaxDenseIndirectImageEntries = 4096u;

uint32_t PossibleU32Bits(Value value) {
	value = value.Resolve();
	if (value.IsImmediate()) {
		return value.GetType() == Type::U32 ? value.U32() : UINT32_MAX;
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) {
		return UINT32_MAX;
	}
	switch (inst->GetOpcode()) {
		case ValueOpcode::BitwiseAnd32:
			return PossibleU32Bits(inst->Arg(0)) & PossibleU32Bits(inst->Arg(1));
		case ValueOpcode::BitwiseOr32:
			return PossibleU32Bits(inst->Arg(0)) | PossibleU32Bits(inst->Arg(1));
		case ValueOpcode::ShiftLeftLogical32: {
			const auto shift = inst->Arg(1).Resolve();
			return shift.IsImmediate() && shift.GetType() == Type::U32
			           ? PossibleU32Bits(inst->Arg(0)) << (shift.U32() & 31u)
			           : UINT32_MAX;
		}
		default: return UINT32_MAX;
	}
}

Value CanonicalizeSampleAdjustDword3(Value value) {
	for (;;) {
		value            = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || inst->GetOpcode() != ValueOpcode::BitwiseOr32) {
			return value;
		}
		const auto left           = inst->Arg(0).Resolve();
		const auto right          = inst->Arg(1).Resolve();
		const bool left_reserved  = (PossibleU32Bits(left) & ~SamplerDword3ReservedMask) == 0;
		const bool right_reserved = (PossibleU32Bits(right) & ~SamplerDword3ReservedMask) == 0;
		if (left_reserved && right_reserved) {
			return Value(0u);
		}
		if (left_reserved) {
			value = right;
		} else if (right_reserved) {
			value = left;
		} else {
			return value;
		}
	}
}

const char* StageName(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return "vertex";
		case ShaderType::Pixel: return "pixel";
		case ShaderType::Fetch: return "fetch";
		case ShaderType::Compute: return "compute";
		default: return "unknown";
	}
}

uint32_t ByteExtent(const MemoryInfo& memory) {
	const auto bytes = std::max((memory.data_bits + 7u) / 8u, 1u);
	const auto count = std::max(memory.data_dwords, 1u);
	const auto end   = static_cast<uint64_t>(memory.offset) + static_cast<uint64_t>(bytes) * count;
	return end > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(end);
}

class Tracker {
public:
	explicit Tracker(Program& program): m_program(program), m_info(program.info) {
		m_info.buffers.clear();
		m_info.images.clear();
		m_info.samplers.clear();
		m_info.sampled_pairs.clear();
		m_info.uses_dma = false;
	}

	void Run() {
		if (m_program.resource_tracking_complete) {
			Fail(0, "resources already tracked");
		}
		if (!m_program.srt_plan_complete) {
			Fail(0, "SRT plan is not ready");
		}
		PlanIndirectImages();
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				Collect(inst);
			}
		}
		LinkImageAliases();
		for (const auto& patch: m_handle_patches) {
			patch.handle->SetFlags<uint32_t>(patch.resource);
		}
		for (const auto& patch: m_memory_patches) {
			auto& memory    = m_program.memory_info[patch.index];
			memory.resource = patch.resource;
			if (patch.has_sampler) {
				memory.sampler = patch.sampler;
			}
		}
		for (const auto& plan: m_indirect_images) {
			plan.handle->SetArg(0, plan.key);
			const uint32_t roots = plan.dense ? 2u : 4u;
			for (uint32_t dword = 0; dword < roots; dword++) {
				plan.handle->SetArg(dword + 1u, plan.roots[plan.dense ? dword : dword + 4u]);
			}
			for (uint32_t dword = roots + 1u; dword < plan.roots.size(); dword++) {
				plan.handle->SetArg(dword, plan.key);
			}
			for (const auto index: plan.memory) {
				m_program.memory_info[index].planning_only = true;
			}
		}
		std::erase_if(m_program.dynamic_reads, [&](Value value) {
			const auto* inst = value.Resolve().TryInstruction();
			return std::any_of(m_indirect_images.begin(), m_indirect_images.end(),
			                   [&](const IndirectImagePlan& plan) {
				return std::ranges::find(plan.reads, inst) != plan.reads.end();
			});
		});
		m_program.descriptor_sources         = std::move(m_sources);
		m_program.info                       = std::move(m_info);
		m_program.resource_tracking_complete = true;
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
		bool                       dense  = false;
	};

	[[noreturn]] void Fail(uint32_t pc, const std::string& reason) const {
		const auto message =
		    fmt::format("shader resource tracking: hash=0x{:016x} stage={} pc=0x{:08x} {}",
		                m_program.shader_hash, StageName(m_program.stage), pc, reason);
		EXIT("%s", message.c_str());
		std::abort();
	}

	bool CollapseNullPhi(Value value, Value& loaded, std::vector<Block*>& null_blocks) const {
		const auto* phi = value.Resolve().TryInstruction();
		if (phi == nullptr || phi->GetOpcode() != ValueOpcode::Phi) {
			return false;
		}
		Value               merged;
		std::vector<Block*> nulls;
		for (size_t index = 0; index < phi->NumArgs(); index++) {
			const auto resolved = ResolveInvariantPhi(m_program, phi->Arg(index));
			if (resolved.IsEmpty()) {
				return false;
			}
			if (resolved.IsImmediate() && resolved.GetType() == Type::U32 && resolved.U32() == 0u) {
				nulls.push_back(phi->PhiBlock(index));
				continue;
			}
			if (merged.IsEmpty()) {
				merged = resolved;
			} else if (!EquivalentValue(m_program, merged, resolved)) {
				return false;
			}
		}
		if (nulls.empty() || merged.IsEmpty()) {
			return false;
		}
		std::ranges::sort(nulls);
		loaded      = merged;
		null_blocks = std::move(nulls);
		return true;
	}

	void CollapseNullDescriptorPaths(DescriptorSource& descriptor, uint32_t width) const {
		std::array<Value, 8> collapsed {};
		std::vector<Block*>  null_blocks;
		bool                 rewritten = false;
		for (uint32_t i = 0; i < width; i++) {
			collapsed[i] = descriptor.dwords[i];
			if (!ResolveInvariantPhi(m_program, descriptor.dwords[i]).IsEmpty()) {
				continue;
			}
			Value               loaded;
			std::vector<Block*> blocks;
			if (!CollapseNullPhi(descriptor.dwords[i], loaded, blocks)) {
				return;
			}
			if (rewritten && blocks != null_blocks) {
				return;
			}
			collapsed[i] = loaded;
			null_blocks  = std::move(blocks);
			rewritten    = true;
		}
		if (rewritten) {
			std::copy_n(collapsed.begin(), width, descriptor.dwords.begin());
		}
	}

	void MakeSource(const Inst& handle, uint32_t width, bool sampler, bool sample_adjust,
	                DescriptorSource& descriptor, uint32_t pc) const {
		if (handle.NumArgs() != width) {
			Fail(pc, fmt::format("{} has {} descriptor dwords, expected {}",
			                     ValueOpcodeName(handle.GetOpcode()), handle.NumArgs(), width));
		}
		descriptor.dword_count = width;
		for (uint32_t i = 0; i < width; i++) {
			descriptor.dwords[i] = handle.Arg(i).Resolve();
		}
		CollapseNullDescriptorPaths(descriptor, width);
		if (sample_adjust) {
			descriptor.dwords[3] = CanonicalizeSampleAdjustDword3(descriptor.dwords[3]);
		}
		const auto dword0 = descriptor.dwords[0].Resolve();
		if (sampler && dword0.IsImmediate() && dword0.GetType() == Type::U32 &&
		    (dword0.U32() & SamplerBorderClampMask) == 0) {
			// Border color and its table index are unused unless a clamp axis selects border mode.
			descriptor.dwords[3] = Value(0u);
		}
	}

	bool ValidateSource(const DescriptorSource& descriptor, uint32_t& bad_dword,
	                    std::string& reason) const {
		for (uint32_t i = 0; i < descriptor.dword_count; i++) {
			bad_dword = i;
			if (descriptor.dwords[i].Resolve().GetType() != Type::U32) {
				reason = "value is not 32-bit";
				return false;
			}
			if (!ValidateRuntimeValue(m_program, descriptor.dwords[i], RuntimeValueType::Any,
			                          &reason)) {
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
				same = same && EquivalentValue(m_program, current.dwords[i], descriptor.dwords[i]);
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
		if (index >= m_program.memory_info.size()) {
			return nullptr;
		}
		const auto& memory = m_program.memory_info[index];
		return memory.kind == ResourceKind::ScalarBuffer && memory.data_bits == 32u &&
		               memory.data_dwords == 1u
		           ? &memory
		           : nullptr;
	}

	const MemoryInfo* AddressReadMemory(const Inst& read, uint32_t& index) const {
		if (read.GetOpcode() != ValueOpcode::LoadAddressU32 || read.NumArgs() != 4u) {
			return nullptr;
		}
		index = read.Flags<MemoryFlags>().index;
		if (index >= m_program.memory_info.size()) {
			return nullptr;
		}
		const auto& memory = m_program.memory_info[index];
		return memory.kind == ResourceKind::ScalarAddress && memory.data_bits == 32u &&
		               memory.data_dwords == 1u
		           ? &memory
		           : nullptr;
	}
	bool MemoryIndexBelongsTo(uint32_t index, const Inst& owner) const {
		for (const auto* block: m_program.blocks) {
			for (const auto& inst: *block) {
				const auto op = inst.GetOpcode();
				if ((BufferAccessOf(op) == BufferAccess::None &&
				     AddressOpcodeInfoOf(op).access == AddressAccess::None &&
				     ImageOpcodeInfoOf(op).access == ImageAccess::None) ||
				    &inst == &owner) {
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
		if (handle.GetOpcode() != ValueOpcode::GetBufferResource) {
			return false;
		}
		MakeSource(handle, 4u, false, false, descriptor, pc);
		uint32_t    bad_dword = 0;
		std::string reason;
		if (!ValidateSource(descriptor, bad_dword, reason)) {
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
			} else if (!EquivalentValue(m_program, heap_offset, heap_reads[dword]->Arg(1))) {
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

	bool MakeRuntimeAddressSource(const Inst& handle, uint32_t pc, uint32_t& source,
	                              DescriptorSource& descriptor) {
		if (handle.GetOpcode() != ValueOpcode::GetAddressResource) {
			return false;
		}
		MakeSource(handle, 2u, false, false, descriptor, pc);
		uint32_t    bad_dword = 0;
		std::string reason;
		if (!ValidateSource(descriptor, bad_dword, reason)) {
			return false;
		}
		source = InternSource(descriptor);
		return true;
	}

	bool MatchDenseTable(const Inst& handle, Inst*& heap_handle, const Inst*& based,
	                     std::array<Inst*, 8>& reads, std::array<uint32_t, 8>& memory_indices) {
		for (uint32_t dword = 0; dword < handle.NumArgs(); dword++) {
			auto* read = handle.Arg(dword).Resolve().TryInstruction();
			if (read == nullptr || read->GetOpcode() != ValueOpcode::LoadAddressU32 ||
			    read->NumArgs() != 4u) {
				return false;
			}
			uint32_t    memory_index = 0;
			const auto* memory       = AddressReadMemory(*read, memory_index);
			auto*       offset       = read->Arg(1).Resolve().TryInstruction();
			if (memory == nullptr || !MemoryIndexBelongsTo(memory_index, *read) ||
			    offset == nullptr) {
				return false;
			}
			uint32_t extra = 0;
			if (based == nullptr) {
				based = offset;
			} else if (offset != based) {
				uint32_t    immediate = 0;
				const Inst* inner     = nullptr;
				if (offset->GetOpcode() != ValueOpcode::IAdd32 || offset->NumArgs() != 2u) {
					return false;
				}
				if (ImmediateU32(offset->Arg(1), immediate)) {
					inner = offset->Arg(0).Resolve().TryInstruction();
				} else if (ImmediateU32(offset->Arg(0), immediate)) {
					inner = offset->Arg(1).Resolve().TryInstruction();
				} else {
					return false;
				}
				if (inner != based) {
					return false;
				}
				extra = immediate;
			}
			if (extra + memory->offset != dword * sizeof(uint32_t)) {
				return false;
			}
			auto* address = read->Arg(0).Resolve().TryInstruction();
			if (address == nullptr || address->GetOpcode() != ValueOpcode::GetAddressResource ||
			    (heap_handle != nullptr &&
			     !EquivalentValue(m_program, Value(heap_handle), Value(address)))) {
				return false;
			}
			heap_handle           = address;
			reads[dword]          = read;
			memory_indices[dword] = memory_index;
		}
		return based != nullptr && heap_handle != nullptr;
	}

	static bool IsBooleanOperator(ValueOpcode opcode) {
		return opcode == ValueOpcode::LogicalAnd || opcode == ValueOpcode::LogicalOr ||
		       opcode == ValueOpcode::LogicalXor || opcode == ValueOpcode::LogicalNot ||
		       opcode == ValueOpcode::SelectU1;
	}

	static bool WaveUniformOpcode(ValueOpcode opcode) {
		switch (opcode) {
			case ValueOpcode::Phi:
			case ValueOpcode::Identity:
			case ValueOpcode::GetUserData:
			case ValueOpcode::ReadConst:
			case ValueOpcode::ReadConstBuffer:
			case ValueOpcode::ReadFirstLane:
			case ValueOpcode::ReadLane:
			case ValueOpcode::Ballot:
			case ValueOpcode::AnyLane:
			case ValueOpcode::CompositeExtractU32x4:
			case ValueOpcode::BitFieldUExtract:
			case ValueOpcode::BitFieldSExtract:
			case ValueOpcode::SelectU1:
			case ValueOpcode::SelectU32:
			case ValueOpcode::IAdd32:
			case ValueOpcode::ISub32:
			case ValueOpcode::IMul32:
			case ValueOpcode::ShiftLeftLogical32:
			case ValueOpcode::ShiftRightLogical32:
			case ValueOpcode::ShiftRightArithmetic32:
			case ValueOpcode::BitwiseAnd32:
			case ValueOpcode::BitwiseOr32:
			case ValueOpcode::BitwiseXor32:
			case ValueOpcode::BitwiseNot32:
			case ValueOpcode::SMin32:
			case ValueOpcode::UMin32:
			case ValueOpcode::SMax32:
			case ValueOpcode::UMax32:
			case ValueOpcode::SLessThan32:
			case ValueOpcode::ULessThan32:
			case ValueOpcode::SLessThanEqual32:
			case ValueOpcode::ULessThanEqual32:
			case ValueOpcode::SGreaterThan32:
			case ValueOpcode::UGreaterThan32:
			case ValueOpcode::SGreaterThanEqual32:
			case ValueOpcode::UGreaterThanEqual32:
			case ValueOpcode::IEqual32:
			case ValueOpcode::INotEqual32:
			case ValueOpcode::LogicalAnd:
			case ValueOpcode::LogicalOr:
			case ValueOpcode::LogicalXor:
			case ValueOpcode::LogicalNot: return true;
			default: return false;
		}
	}

	static bool UniformResult(ValueOpcode opcode) {
		return opcode == ValueOpcode::GetUserData || opcode == ValueOpcode::ReadConst ||
		       opcode == ValueOpcode::ReadFirstLane || opcode == ValueOpcode::ReadLane ||
		       opcode == ValueOpcode::Ballot || opcode == ValueOpcode::AnyLane;
	}

	static bool WaveUniform(const Inst& inst, std::unordered_set<const Inst*>& visiting) {
		if (!WaveUniformOpcode(inst.GetOpcode())) {
			return false;
		}
		if (UniformResult(inst.GetOpcode()) || !visiting.insert(&inst).second) {
			return true;
		}
		for (size_t arg = 0; arg < inst.NumArgs(); arg++) {
			if (!WaveUniform(inst.Arg(arg), visiting)) {
				return false;
			}
		}
		return true;
	}

	static bool WaveUniform(Value value, std::unordered_set<const Inst*>& visiting) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			return true;
		}
		const auto* inst = value.TryInstruction();
		return inst != nullptr && WaveUniform(*inst, visiting);
	}

	static bool WaveUniform(Value value) {
		std::unordered_set<const Inst*> visiting;
		return WaveUniform(value, visiting);
	}

	static bool WaveUniform(const Inst& inst) {
		std::unordered_set<const Inst*> visiting;
		return WaveUniform(inst, visiting);
	}

	static bool PredicateImplies(Value predicate, bool negated, const Inst& fact) {
		for (const auto* top = predicate.Resolve().TryInstruction(); top != nullptr;
		     top             = predicate.Resolve().TryInstruction()) {
			if (top->GetOpcode() == ValueOpcode::LogicalNot) {
				negated   = !negated;
				predicate = top->Arg(0);
			} else if (top->GetOpcode() == ValueOpcode::AnyLane &&
			           (WaveUniform(top->Arg(0)) || WaveUniform(fact))) {
				predicate = top->Arg(0);
			} else {
				break;
			}
		}
		constexpr size_t                MaxAtoms = 12;
		std::vector<const Inst*>        atoms;
		std::vector<const Inst*>        pending;
		std::unordered_set<const Inst*> seen;
		const auto                      visit = [&](Value value) {
			const auto* inst = value.Resolve().TryInstruction();
			if (inst != nullptr && seen.insert(inst).second) {
				pending.push_back(inst);
			}
		};
		visit(predicate);
		while (!pending.empty()) {
			const auto* inst = pending.back();
			pending.pop_back();
			if (IsBooleanOperator(inst->GetOpcode())) {
				for (size_t arg = 0; arg < inst->NumArgs(); arg++) {
					visit(inst->Arg(arg));
				}
			} else if (inst->GetOpcode() == ValueOpcode::AnyLane && WaveUniform(inst->Arg(0))) {
				visit(inst->Arg(0));
			} else {
				atoms.push_back(inst);
			}
		}
		const auto fact_atom = std::ranges::find(atoms, &fact);
		if (fact_atom == atoms.end() || atoms.size() > MaxAtoms) {
			return false;
		}
		const auto fact_index = static_cast<size_t>(fact_atom - atoms.begin());
		for (uint32_t assignment = 0; assignment < (1u << atoms.size()); assignment++) {
			const auto evaluate = [&](auto& self, Value value) -> bool {
				value = value.Resolve();
				if (value.IsImmediate()) {
					return value.U1();
				}
				const auto* inst = value.TryInstruction();
				switch (inst->GetOpcode()) {
					case ValueOpcode::LogicalAnd:
						return self(self, inst->Arg(0)) && self(self, inst->Arg(1));
					case ValueOpcode::LogicalOr:
						return self(self, inst->Arg(0)) || self(self, inst->Arg(1));
					case ValueOpcode::LogicalXor:
						return self(self, inst->Arg(0)) != self(self, inst->Arg(1));
					case ValueOpcode::LogicalNot: return !self(self, inst->Arg(0));
					case ValueOpcode::SelectU1:
						return self(self, inst->Arg(0)) ? self(self, inst->Arg(1))
						                                : self(self, inst->Arg(2));
					default: {
						const auto index =
						    static_cast<size_t>(std::ranges::find(atoms, inst) - atoms.begin());
						return ((assignment >> index) & 1u) != 0u;
					}
				}
			};
			if (evaluate(evaluate, predicate) != negated &&
			    ((assignment >> fact_index) & 1u) == 0u) {
				return false;
			}
		}
		return true;
	}

	const Block* BlockById(uint32_t id) const {
		for (size_t index = 0; index < m_program.block_info.size(); index++) {
			if (m_program.block_info[index].id == id) {
				return m_program.blocks[index];
			}
		}
		return nullptr;
	}

	static bool Reaches(const Block* from, const Block* target, const Block* avoid) {
		std::vector<const Block*>        pending {from};
		std::unordered_set<const Block*> seen;
		while (!pending.empty()) {
			const auto* block = pending.back();
			pending.pop_back();
			if (block == avoid || !seen.insert(block).second) {
				continue;
			}
			if (block == target) {
				return true;
			}
			for (const auto* successor: block->ImmSuccessors()) {
				pending.push_back(successor);
			}
		}
		return false;
	}

	bool BlockRunsOnlyWhen(const Block& block, const Inst& fact) const {
		const auto& blocks = m_program.blocks;
		if (blocks.empty() || !Reaches(blocks.front(), &block, nullptr)) {
			return false;
		}
		std::unordered_map<const Block*, size_t> indices;
		for (size_t index = 0; index < blocks.size(); index++) {
			indices.emplace(blocks[index], index);
		}
		const auto target = indices.at(&block);
		std::vector<std::vector<bool>> dominators(blocks.size(),
		                                          std::vector<bool>(blocks.size(), true));
		dominators.front().assign(blocks.size(), false);
		dominators.front().front() = true;
		for (bool changed = true; changed;) {
			changed = false;
			for (size_t index = 1; index < blocks.size(); index++) {
				std::vector<bool> next(blocks.size(), true);
				for (const auto* predecessor: blocks[index]->ImmPredecessors()) {
					const auto found = indices.find(predecessor);
					if (found == indices.end()) {
						return false;
					}
					for (size_t candidate = 0; candidate < next.size(); candidate++) {
						next[candidate] = next[candidate] && dominators[found->second][candidate];
					}
				}
				next[index] = true;
				if (next != dominators[index]) {
					dominators[index] = std::move(next);
					changed           = true;
				}
			}
		}
		for (size_t index = 0; index < blocks.size(); index++) {
			const auto* branch = blocks[index];
			const auto& info   = m_program.block_info[index];
			if (branch == &block || !dominators[target][index] ||
			    info.terminator.kind != CFG::TerminatorKind::ConditionalBranch ||
			    info.condition.IsEmpty()) {
				continue;
			}
			const auto* taken     = BlockById(info.terminator.true_block);
			const auto* not_taken = BlockById(info.terminator.false_block);
			if (taken == nullptr || not_taken == nullptr || taken == not_taken) {
				continue;
			}
			const bool via_taken     = Reaches(taken, &block, branch);
			const bool via_not_taken = Reaches(not_taken, &block, branch);
			if (via_taken != via_not_taken && PredicateImplies(info.condition, via_not_taken, fact)) {
				return true;
			}
		}
		return false;
	}

	bool MatchLoopKeyBound(const Inst& key, const Inst& handle, Value& bound,
	                       bool& is_signed) const {
		if (key.GetOpcode() != ValueOpcode::Phi || key.NumArgs() != 2u ||
		    handle.Parent() == nullptr) {
			return false;
		}
		bool starts_at_zero = false;
		bool steps_by_one   = false;
		for (size_t operand = 0; operand < 2u; operand++) {
			const auto value     = key.Arg(operand).Resolve();
			uint32_t   immediate = 0;
			if (ImmediateU32(value, immediate)) {
				starts_at_zero = starts_at_zero || immediate == 0u;
				continue;
			}
			const auto* step = value.TryInstruction();
			if (step == nullptr || step->GetOpcode() != ValueOpcode::IAdd32 ||
			    step->NumArgs() != 2u) {
				continue;
			}
			for (size_t arg = 0; arg < 2u; arg++) {
				uint32_t one = 0;
				steps_by_one = steps_by_one ||
				               (step->Arg(arg).Resolve().TryInstruction() == &key &&
				                ImmediateU32(step->Arg(1u - arg), one) && one == 1u);
			}
		}
		if (!starts_at_zero || !steps_by_one) {
			return false;
		}
		for (const auto& use: key.Uses()) {
			const auto* compare = use.user;
			if (compare == nullptr || use.operand != 0u || compare->NumArgs() != 2u ||
			    (compare->GetOpcode() != ValueOpcode::SLessThan32 &&
			     compare->GetOpcode() != ValueOpcode::ULessThan32) ||
			    compare->Arg(1).Resolve().TryInstruction() == &key ||
			    !BlockRunsOnlyWhen(*handle.Parent(), *compare)) {
				continue;
			}
			bound     = compare->Arg(1).Resolve();
			is_signed = compare->GetOpcode() == ValueOpcode::SLessThan32;
			return true;
		}
		return false;
	}

	static void CollectConjuncts(Value predicate, std::vector<const Inst*>& out) {
		std::vector<Value> pending {predicate};
		while (!pending.empty()) {
			const auto value = pending.back().Resolve();
			pending.pop_back();
			const auto* inst = value.TryInstruction();
			if (inst == nullptr || std::ranges::find(out, inst) != out.end()) {
				continue;
			}
			out.push_back(inst);
			if (inst->GetOpcode() == ValueOpcode::LogicalAnd) {
				pending.push_back(inst->Arg(0));
				pending.push_back(inst->Arg(1));
			} else if (inst->GetOpcode() == ValueOpcode::AnyLane && WaveUniform(inst->Arg(0))) {
				pending.push_back(inst->Arg(0));
			} else if (inst->GetOpcode() == ValueOpcode::SelectU1) {
				const auto alternative = inst->Arg(2).Resolve();
				if (alternative.IsImmediate() && !alternative.U1()) {
					pending.push_back(inst->Arg(0));
					pending.push_back(inst->Arg(1));
				}
			}
		}
	}

	static bool ConjunctBound(std::span<const Inst* const> conjuncts, const Inst& item,
	                          uint32_t& bound) {
		for (const auto* conjunct: conjuncts) {
			const auto opcode = conjunct->GetOpcode();
			const bool less =
			    opcode == ValueOpcode::SLessThan32 || opcode == ValueOpcode::ULessThan32;
			const bool greater =
			    opcode == ValueOpcode::SGreaterThan32 || opcode == ValueOpcode::UGreaterThan32;
			if ((!less && !greater) || conjunct->NumArgs() != 2u) {
				continue;
			}
			const auto subject = less ? conjunct->Arg(0) : conjunct->Arg(1);
			const auto limit   = less ? conjunct->Arg(1) : conjunct->Arg(0);
			uint32_t   value   = 0;
			if (subject.Resolve().TryInstruction() != &item || !ImmediateU32(limit, value) ||
			    value == 0u) {
				continue;
			}
			bound = value;
			return true;
		}
		return false;
	}

	struct Affine {
		Inst*    leaf  = nullptr;
		uint32_t    scale = 0;
		uint32_t    bias  = 0;
	};

	static bool AffineOf(Value value, Value predicate, Affine& out, size_t depth = 0) {
		value              = value.Resolve();
		uint32_t immediate = 0;
		if (ImmediateU32(value, immediate)) {
			out = {nullptr, 0u, immediate};
			return true;
		}
		auto* inst = value.TryInstruction();
		if (inst == nullptr || depth > 32u) {
			return false;
		}
		switch (inst->GetOpcode()) {
			case ValueOpcode::SelectU32:
				if (inst->Arg(0).Resolve() == predicate.Resolve()) {
					return AffineOf(inst->Arg(1), predicate, out, depth + 1u);
				}
				break;
			case ValueOpcode::ShiftLeftLogical32:
				if (ImmediateU32(inst->Arg(1), immediate) && immediate < 32u &&
				    AffineOf(inst->Arg(0), predicate, out, depth + 1u)) {
					out.scale <<= immediate;
					out.bias <<= immediate;
					return true;
				}
				return false;
			case ValueOpcode::IMul32:
				for (size_t arg = 0; arg < 2u; arg++) {
					if (ImmediateU32(inst->Arg(arg), immediate)) {
						if (!AffineOf(inst->Arg(1u - arg), predicate, out, depth + 1u)) {
							return false;
						}
						out.scale *= immediate;
						out.bias *= immediate;
						return true;
					}
				}
				return false;
			case ValueOpcode::IAdd32: {
				Affine left;
				Affine right;
				if (!AffineOf(inst->Arg(0), predicate, left, depth + 1u) ||
				    !AffineOf(inst->Arg(1), predicate, right, depth + 1u) ||
				    (left.leaf != nullptr && right.leaf != nullptr && left.leaf != right.leaf)) {
					return false;
				}
				out = {left.leaf != nullptr ? left.leaf : right.leaf, left.scale + right.scale,
				       left.bias + right.bias};
				return true;
			}
			default: break;
		}
		out = {inst, 1u, 0u};
		return true;
	}

	const MemoryInfo* PerLaneAddressReadMemory(const Inst& read, uint32_t& index) const {
		if (read.GetOpcode() != ValueOpcode::LoadAddressU32 || read.NumArgs() != 4u) {
			return nullptr;
		}
		index = read.Flags<MemoryFlags>().index;
		if (index >= m_program.memory_info.size()) {
			return nullptr;
		}
		const auto& memory = m_program.memory_info[index];
		return IsAddressResourceKind(memory.kind) && memory.kind != ResourceKind::Scratch &&
		               memory.data_bits == 32u && memory.data_dwords == 1u
		           ? &memory
		           : nullptr;
	}

	bool MatchReadLaneProbe(const Inst& key, uint32_t pc, uint32_t& material_source,
	                        uint32_t& selector_offset, uint32_t& selector_stride,
	                        uint32_t& item_bound, std::string& reason) {
		if (key.GetOpcode() != ValueOpcode::ReadLane || key.NumArgs() != 2u) {
			reason = "key is not a readlane of a per-lane value";
			return false;
		}
		const Inst* load = key.Arg(0).Resolve().TryInstruction();
		Value       predicate;
		if (load != nullptr && load->GetOpcode() == ValueOpcode::SelectU32) {
			predicate = load->Arg(0).Resolve();
			load      = load->Arg(1).Resolve().TryInstruction();
		}
		uint32_t memory_index = 0;
		if (load == nullptr || PerLaneAddressReadMemory(*load, memory_index) == nullptr ||
		    !MemoryIndexBelongsTo(memory_index, *load)) {
			reason = "per-lane key is not a 32-bit address load";
			return false;
		}
		if (predicate.IsEmpty()) {
			predicate = load->Arg(3).Resolve();
		} else if (load->Arg(3).Resolve() != predicate) {
			reason = "the select around the key load is not the load's enable";
			return false;
		}
		auto* material_handle = load->Arg(0).Resolve().TryInstruction();
		if (material_handle == nullptr) {
			reason = "key load has no address resource";
			return false;
		}
		Affine offset;
		if (!AffineOf(load->Arg(1), predicate, offset) || offset.leaf == nullptr ||
		    offset.scale == 0u) {
			reason = "key load offset is not record index times stride plus a constant";
			return false;
		}
		std::vector<const Inst*> conjuncts;
		CollectConjuncts(predicate, conjuncts);
		uint32_t bound = 0;
		if (!ConjunctBound(conjuncts, *offset.leaf, bound)) {
			reason = "record index has no immediate upper bound in the key load's enable";
			return false;
		}
		if (bound > MaxDenseIndirectImageEntries) {
			reason = "record index bound exceeds the enumeration cap";
			return false;
		}
		DescriptorSource material_descriptor;
		if (!MakeRuntimeAddressSource(*material_handle, pc, material_source,
		                              material_descriptor)) {
			reason = "record table address cannot be evaluated before the draw";
			return false;
		}
		selector_offset = offset.bias;
		selector_stride = offset.scale;
		item_bound      = bound;
		return true;
	}

	bool MatchKeyBound(const Inst& key, uint32_t& bound) const {
		for (const auto& use: key.Uses()) {
			uint32_t limit = 0;
			if (use.user == nullptr || use.user->GetOpcode() != ValueOpcode::ULessThan32 ||
			    use.user->NumArgs() != 2u || use.operand != 0u ||
			    !ImmediateU32(use.user->Arg(1), limit) || limit == 0u ||
			    limit > MaxDenseIndirectImageEntries) {
				continue;
			}
			bound = limit;
			return true;
		}
		return false;
	}

	bool TryMakeDenseIndirectImage(Inst& handle, uint32_t pc, IndirectImagePlan& plan) {
		if (handle.GetOpcode() != ValueOpcode::GetImageResource || handle.NumArgs() != 8u) {
			return false;
		}
		Inst*                   heap_handle = nullptr;
		const Inst*             based       = nullptr;
		std::array<Inst*, 8>    reads {};
		std::array<uint32_t, 8> memory_indices {};
		if (!MatchDenseTable(handle, heap_handle, based, reads, memory_indices)) {
			return false;
		}
		uint32_t table_offset = 0;
		Value    scaled_value;
		if (based->GetOpcode() != ValueOpcode::IAdd32 || based->NumArgs() != 2u) {
			return false;
		}
		if (ImmediateU32(based->Arg(1), table_offset)) {
			scaled_value = based->Arg(0);
		} else if (ImmediateU32(based->Arg(0), table_offset)) {
			scaled_value = based->Arg(1);
		} else {
			return false;
		}
		const auto* scaled = scaled_value.Resolve().TryInstruction();
		uint32_t    shift  = 0;
		if (scaled == nullptr || scaled->GetOpcode() != ValueOpcode::ShiftLeftLogical32 ||
		    scaled->NumArgs() != 2u || !ImmediateU32(scaled->Arg(1), shift) ||
		    shift != DenseIndirectImageShift) {
			return false;
		}
		auto*    key          = scaled->Arg(0).Resolve().TryInstruction();
		uint32_t bound        = 0;
		Value    loop_bound;
		bool     bound_signed = false;
		if (key == nullptr) {
			return false;
		}
		uint32_t material_source = 0;
		uint32_t selector_offset = 0;
		uint32_t selector_stride = 0;
		uint32_t item_bound      = 0;
		if (!MatchKeyBound(*key, bound)) {
			if (MatchLoopKeyBound(*key, handle, loop_bound, bound_signed)) {
				bound = MaxDenseIndirectImageEntries;
			} else if (!MatchReadLaneProbe(*key, pc, material_source, selector_offset,
			                               selector_stride, item_bound, m_indirect_reason)) {
				return false;
			}
		}
		const std::array<const Inst*, 1> image_users {&handle};
		for (const auto* read: reads) {
			if (!UsesOnly(*read, image_users)) {
				return false;
			}
		}

		DescriptorSource heap_source;
		uint32_t         heap_source_index = 0;
		if (!MakeRuntimeAddressSource(*heap_handle, pc, heap_source_index, heap_source)) {
			return false;
		}

		uint32_t bound_source = DescriptorSource::IndirectImage::NoBoundSource;
		if (!loop_bound.IsEmpty()) {
			DescriptorSource bound_descriptor;
			bound_descriptor.dword_count = 1u;
			bound_descriptor.dwords[0]   = loop_bound;
			uint32_t    bad_dword        = 0;
			std::string reason;
			if (!ValidateSource(bound_descriptor, bad_dword, reason)) {
				return false;
			}
			bound_source = InternSource(bound_descriptor);
		}

		DescriptorSource image_source;
		image_source.dword_count = 8u;
		image_source.dwords.fill(Value(key));
		image_source.dwords[0]      = heap_source.dwords[0];
		image_source.dwords[1]      = heap_source.dwords[1];
		image_source.indirect_image = DescriptorSource::IndirectImage {
		    .material_source = material_source,
		    .heap_source     = heap_source_index,
		    .selector_stride = selector_stride,
		    .selector_offset = selector_offset,
		    .table_offset    = table_offset,
		    .key_bound       = item_bound != 0u ? 0u : bound,
		    .bound_source    = bound_source,
		    .bound_signed    = bound_signed,
		    .item_bound      = item_bound};

		plan.handle = &handle;
		plan.source = InternSource(image_source);
		plan.key    = Value(key);
		plan.roots  = image_source.dwords;
		plan.dense  = true;
		std::copy(memory_indices.begin(), memory_indices.end(), plan.memory.begin());
		std::copy(reads.begin(), reads.end(), plan.reads.begin());
		return true;
	}

	const IndirectImagePlan* FindIndirectImage(const Inst& handle) const {
		const auto found =
		    std::find_if(m_indirect_images.begin(), m_indirect_images.end(),
		                 [&](const IndirectImagePlan& plan) {
			    return plan.handle == &handle;
		    });
		return found == m_indirect_images.end() ? nullptr : &*found;
	}

	bool IsIndirectPlanningMemory(uint32_t index) const {
		return std::any_of(m_indirect_images.begin(), m_indirect_images.end(),
		                   [&](const IndirectImagePlan& plan) {
			return std::ranges::find(plan.memory, index) != plan.memory.end();
		});
	}

	void PlanIndirectImages() {
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if (ImageOpcodeInfoOf(inst.GetOpcode()).access == ImageAccess::None ||
				    inst.NumArgs() == 0u) {
					continue;
				}
				auto* handle = inst.Arg(0).Resolve().TryInstruction();
				if (handle == nullptr || FindIndirectImage(*handle) != nullptr) {
					continue;
				}
				IndirectImagePlan plan;
				const auto        pc = inst.Flags<MemoryFlags>().pc;
				m_indirect_reason.clear();
				if (TryMakeIndirectImage(*handle, pc, plan) ||
				    TryMakeDenseIndirectImage(*handle, pc, plan)) {
					m_indirect_images.push_back(std::move(plan));
				} else if (!m_indirect_reason.empty()) {
					m_indirect_reasons[handle] = m_indirect_reason;
				}
			}
		}
	}

	void GetHandle(Value value, ValueOpcode expected, uint32_t width, uint32_t pc, Inst*& handle,
	               uint32_t& source, bool sampler = false, bool sample_adjust = false) {
		handle = value.Resolve().TryInstruction();
		if (handle == nullptr || handle->GetOpcode() != expected) {
			Fail(pc, fmt::format("memory operation requires {}", ValueOpcodeName(expected)));
		}
		DescriptorSource descriptor;
		MakeSource(*handle, width, sampler, sample_adjust, descriptor, pc);
		uint32_t    bad_dword = 0;
		std::string reason;
		if (expected == ValueOpcode::GetImageResource) {
			for (; bad_dword < descriptor.dword_count; bad_dword++) {
				const auto* value = descriptor.dwords[bad_dword].Resolve().TryInstruction();
				if (value != nullptr && value->GetOpcode() == ValueOpcode::ReadConstBuffer) {
					const auto indirect = m_indirect_reasons.find(handle);
					Fail(pc, fmt::format("{} dword {} is not a valid runtime value{}",
					                     ValueOpcodeName(expected), bad_dword,
					                     indirect != m_indirect_reasons.end()
					                         ? " (indirect table: " + indirect->second + ")"
					                         : ""));
				}
			}
			bad_dword = 0;
		}
		if (!ValidateSource(descriptor, bad_dword, reason)) {
			const auto indirect = m_indirect_reasons.find(handle);
			Fail(pc, fmt::format("{} dword {} is not a valid runtime value: {}{}",
			                     ValueOpcodeName(expected), bad_dword, reason,
			                     indirect != m_indirect_reasons.end()
			                         ? " (indirect table: " + indirect->second + ")"
			                         : ""));
		}
		source = InternSource(descriptor);
	}

	void ValidateAddressHandle(Value value, uint32_t pc) const {
		const auto* handle = value.Resolve().TryInstruction();
		if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetAddressResource) {
			Fail(pc, "address operation requires GetAddressResource");
		}
		if (handle->NumArgs() != 2) {
			Fail(pc, "GetAddressResource must have two address dwords");
		}
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
		const auto access        = BufferAccessOf(op);
		const bool atomic        = access == BufferAccess::Atomic;
		const bool write         = access == BufferAccess::Write || atomic;
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
		const auto resource_class = ImageOpcodeInfoOf(op).resource_class;
		const auto mip   = resource_class == ImageResourceClass::Storage && memory.image_has_mip
		                       ? ImageMipMode::DynamicStorage
		                       : ImageMipMode::None;
		const bool depth = (memory.image_sample_flags & Decoder::ImageSampleFlagCompare) != 0;
		for (uint32_t i = 0; i < m_info.images.size(); i++) {
			auto& image = m_info.images[i];
			if (image.source == source && image.resource_class == resource_class &&
			    image.dimension == memory.image_dimension && image.mip_mode == mip &&
			    image.depth_compare == depth && image.r128 == memory.image_r128) {
				Merge(image, op, pc);
				return i;
			}
		}
		if (m_info.images.size() >= ShaderInfo::MaxImages) {
			return UINT32_MAX;
		}
		ImageResource image;
		image.source         = source;
		image.first_use_pc   = pc;
		image.resource_class = resource_class;
		image.dimension      = memory.image_dimension;
		image.mip_mode       = mip;
		image.depth_compare  = depth;
		image.r128           = memory.image_r128;
		Merge(image, op, pc);
		m_info.images.push_back(image);
		return static_cast<uint32_t>(m_info.images.size() - 1);
	}

	static void Merge(ImageResource& image, ValueOpcode op, uint32_t pc) {
		const auto access  = ImageOpcodeInfoOf(op).access;
		const bool atomic  = access == ImageAccess::Atomic;
		const bool write   = access == ImageAccess::Write || atomic;
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

	void AddSampledPair(uint32_t image, uint32_t sampler, uint32_t pc) {
		for (auto& pair: m_info.sampled_pairs) {
			if (pair.image == image && pair.sampler == sampler) {
				pair.first_use_pc = std::min(pair.first_use_pc, pc);
				return;
			}
		}
		if (m_info.sampled_pairs.size() >= ShaderInfo::MaxSampledPairs) {
			Fail(pc, "sampled image/sampler pair limit exceeded");
		}
		m_info.sampled_pairs.push_back({image, sampler, pc});
	}

	void AddHandlePatch(Inst* handle, uint32_t resource, uint32_t pc) {
		for (const auto& patch: m_handle_patches) {
			if (patch.handle == handle) {
				if (patch.resource != resource) {
					Fail(pc, fmt::format("{} is reused with incompatible resource classes",
					                     ValueOpcodeName(handle->GetOpcode())));
				}
				return;
			}
		}
		m_handle_patches.push_back({handle, resource});
	}

	void AddMemoryPatch(uint32_t index, uint32_t resource, uint32_t sampler, bool has_sampler,
	                    uint32_t pc) {
		for (auto& patch: m_memory_patches) {
			if (patch.index != index) {
				continue;
			}
			if (patch.resource != resource ||
			    (has_sampler && patch.has_sampler && patch.sampler != sampler)) {
				Fail(pc, "memory metadata is reused with incompatible resources");
			}
			if (has_sampler) {
				patch.sampler     = sampler;
				patch.has_sampler = true;
			}
			return;
		}
		m_memory_patches.push_back({index, resource, sampler, has_sampler});
	}

	void Collect(Inst& inst) {
		const auto op           = inst.GetOpcode();
		const auto buffer       = BufferAccessOf(op);
		const auto address_info = AddressOpcodeInfoOf(op);
		const auto image_info   = ImageOpcodeInfoOf(op);
		if (buffer == BufferAccess::None && address_info.access == AddressAccess::None &&
		    image_info.access == ImageAccess::None) {
			return;
		}
		const auto flags = inst.Flags<MemoryFlags>();
		if (flags.index >= m_program.memory_info.size()) {
			Fail(flags.pc, fmt::format("memory metadata index {} is out of range", flags.index));
		}
		if (inst.NumArgs() == 0) {
			Fail(flags.pc, "memory operation has no resource handle");
		}
		const auto& memory = m_program.memory_info[flags.index];
		if (memory.planning_only || IsIndirectPlanningMemory(flags.index)) {
			return;
		}
		Inst*    handle   = nullptr;
		uint32_t source   = 0;
		uint32_t resource = 0;

		if (buffer != BufferAccess::None) {
			GetHandle(inst.Arg(0), ValueOpcode::GetBufferResource, 4, flags.pc, handle, source);
			resource = AddBuffer(source, memory, op, flags.pc);
			if (resource == UINT32_MAX) {
				Fail(flags.pc, "buffer resource limit exceeded");
			}
			AddHandlePatch(handle, resource, flags.pc);
			AddMemoryPatch(flags.index, resource, 0, false, flags.pc);
			return;
		}
		if (address_info.access != AddressAccess::None) {
			if (!IsAddressResourceKind(memory.kind)) {
				Fail(flags.pc, "address operation has invalid resource kind");
			}
			if (memory.kind == ResourceKind::Scratch) {
				handle = inst.Arg(0).Resolve().TryInstruction();
				if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetScratchResource ||
				    handle->NumArgs() != 0) {
					Fail(flags.pc, "scratch operation requires GetScratchResource");
				}
				if (m_program.scratch_dwords == 0) {
					Fail(flags.pc, "scratch operation requires a nonzero AGC per-thread size");
				}
				return;
			}
			ValidateAddressHandle(inst.Arg(0), flags.pc);
			m_info.uses_dma = true;
			return;
		}

		if (memory.kind != ResourceKind::Image ||
		    image_info.resource_class == ImageResourceClass::None) {
			Fail(flags.pc, "image operation has invalid resource kind");
		}
		handle               = inst.Arg(0).Resolve().TryInstruction();
		const auto* indirect = handle != nullptr ? FindIndirectImage(*handle) : nullptr;
		if (indirect != nullptr) {
			source = indirect->source;
		} else {
			GetHandle(inst.Arg(0), ValueOpcode::GetImageResource, 8, flags.pc, handle, source);
		}
		resource = AddImage(source, memory, op, flags.pc);
		if (resource == UINT32_MAX) {
			Fail(flags.pc, "image resource limit exceeded");
		}
		AddHandlePatch(handle, resource, flags.pc);
		uint32_t sampler = 0;
		if (image_info.needs_sampler) {
			if (inst.NumArgs() < 2) {
				Fail(flags.pc, "sampled image operation has no sampler handle");
			}
			Inst*      sampler_handle = nullptr;
			uint32_t   sampler_source = 0;
			const bool sample_adjust =
			    (memory.image_sample_flags & Decoder::ImageSampleFlagAdjust) != 0;
			GetHandle(inst.Arg(1), ValueOpcode::GetSamplerResource, 4, flags.pc, sampler_handle,
			          sampler_source, true, sample_adjust);
			sampler = AddSampler(sampler_source, flags.pc);
			if (sampler == UINT32_MAX) {
				Fail(flags.pc, "sampler resource limit exceeded");
			}
			AddHandlePatch(sampler_handle, sampler, flags.pc);
			AddSampledPair(resource, sampler, flags.pc);
		}
		AddMemoryPatch(flags.index, resource, sampler, image_info.needs_sampler, flags.pc);
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
					alias = alias && EquivalentValue(m_program, buffer_source->dwords[dword],
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
	ShaderInfo                     m_info;
	std::vector<DescriptorSource>  m_sources;
	std::vector<HandlePatch>       m_handle_patches;
	std::vector<MemoryPatch>       m_memory_patches;
	std::vector<IndirectImagePlan> m_indirect_images;
	std::unordered_map<const Inst*, std::string> m_indirect_reasons;
	std::string                                  m_indirect_reason;
};

} // namespace

void TrackResources(Program& program) {
	Tracker(program).Run();
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
