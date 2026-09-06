#include "graphics/shader/recompiler/ir/passes/ResourceTracking.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <algorithm>
#include <fmt/format.h>
#include <span>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint32_t SamplerBorderClampMask    = (1u << 2u) | (1u << 5u) | (1u << 8u);
constexpr uint32_t SamplerDword3ReservedMask = 0x3ffff000u;

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
		PlanBoundedReads();
		PlanIndirectImages();
		PlanInlineDescriptors();
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
			memory.buffer_table = patch.buffer_table;
			memory.resource = patch.resource;
			if (patch.has_sampler) {
				memory.sampler = patch.sampler;
			}
		}
		ApplyBoundedRootReads();
		ApplyBoundedReads();
		for (const auto& plan: m_indirect_images) {
			plan.handle->SetArg(0, plan.key);
			for (uint32_t dword = 0; dword < 4u; dword++) {
				plan.handle->SetArg(dword + 1u, plan.roots[dword + 4u]);
			}
			for (uint32_t dword = 5u; dword < plan.roots.size(); dword++) {
				plan.handle->SetArg(dword, plan.key);
			}
			for (const auto index: plan.memory) {
				m_program.memory_info[index].planning_only = true;
			}
		}
		for (const auto& plan: m_inline_descriptors) {
			plan.handle->SetArg(0, plan.key);
			for (uint32_t dword = 1; dword < plan.handle->NumArgs(); dword++) {
				plan.handle->SetArg(dword, dword <= plan.root_count ? plan.roots[dword - 1u] : plan.key);
			}
		}
		for (const auto index: m_inline_planning_memory) {
			m_program.memory_info[index].planning_only = true;
		}
		std::erase_if(m_program.dynamic_reads, [&](Value value) {
			const auto* inst = value.Resolve().TryInstruction();
			return std::ranges::find(m_inline_planning_reads, inst) != m_inline_planning_reads.end() ||
			       std::any_of(m_indirect_images.begin(), m_indirect_images.end(),
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
		uint32_t buffer_table = UINT32_MAX;
	};

	struct IndirectImagePlan {
		Inst*                      handle = nullptr;
		uint32_t                   source = 0;
		Value                      key;
		std::array<Value, 8>       roots {};
		std::array<uint32_t, 8>    memory {};
		std::array<const Inst*, 8> reads {};
	};

	struct InlineDescriptorPlan {
		Inst*                      handle = nullptr;
		uint32_t                   source = 0;
		Value                      key;
		std::array<Value, 6>        roots {};
		std::array<uint32_t, 8>     memory {};
		std::array<const Inst*, 8>  reads {};
		uint32_t                   root_count = 4;
		uint32_t                   read_count = 4;
	};

	[[noreturn]] void Fail(uint32_t pc, const std::string& reason) const {
		const auto message =
		    fmt::format("shader resource tracking: hash=0x{:016x} stage={} pc=0x{:08x} {}",
		                m_program.shader_hash, StageName(m_program.stage), pc, reason);
		EXIT("%s", message.c_str());
		std::abort();
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

	bool ValidateSource(const DescriptorSource& descriptor, uint32_t& bad_dword) const {
		for (uint32_t i = 0; i < descriptor.dword_count; i++) {
			bad_dword = i;
			if (descriptor.dwords[i].Resolve().GetType() != Type::U32) {
				return false;
			}
			if (!ValidateRuntimeValue(m_program, descriptor.dwords[i])) {
				return false;
			}
		}
		return true;
	}

	struct BoundedReadPlan {
		Inst* read = nullptr;
		BoundedSrtReadProof proof;
		uint32_t read_id = 0;
	};
	struct BoundedBufferPlan {
		Inst* handle = nullptr;
		Value index;
		std::array<Value, 3> roots;
	};

	void PlanBoundedReads() {
		if (m_program.stage != ShaderType::Compute) return;
		for (auto* block : m_program.blocks) {
			for (auto& inst : *block) {
				if (!inst.HasUses()) continue;
				const auto proof = ProveBoundedSrtRead(m_program, inst);
				if (!proof) continue;
				if (proof->workgroup_axis == UINT32_MAX) PlanBoundedRootReads(proof->count);
				PlanBoundedRootReads(proof->address_low);
				PlanBoundedRootReads(proof->address_high);
				DescriptorSource address;
				address.dword_count = 2u;
				address.dwords[0] = proof->address_low;
				address.dwords[1] = proof->address_high;
				const auto address_source = InternSource(address);
				uint32_t count_source = UINT32_MAX;
				if (proof->workgroup_axis == UINT32_MAX) {
					DescriptorSource count;
					count.dword_count = 1u;
					count.dwords[0] = proof->count;
					count_source = InternSource(count);
				}
				const BoundedSrtRead read {address_source, count_source,
				                          proof->offset_scale, proof->offset_bias, proof->memory_offset,
				                          proof->workgroup_axis};
				auto found = std::ranges::find(m_bounded_srt_reads, read);
				uint32_t read_id = static_cast<uint32_t>(found - m_bounded_srt_reads.begin());
				if (found == m_bounded_srt_reads.end()) m_bounded_srt_reads.push_back(read);
				m_bounded_reads.push_back({&inst, *proof, read_id});
			}
		}
	}

	const BoundedReadPlan* BoundedRead(const Inst* read) const {
		const auto found = std::ranges::find_if(m_bounded_reads,
		    [&](const BoundedReadPlan& plan) { return plan.read == read; });
		return found == m_bounded_reads.end() ? nullptr : &*found;
	}

	bool MakeBoundedBufferSource(Inst& handle, uint32_t& source) {
		if (handle.NumArgs() != 4u) return false;
		std::array<const BoundedReadPlan*, 4> words;
		for (uint32_t word = 0; word < words.size(); ++word) {
			words[word] = BoundedRead(handle.Arg(word).Resolve().TryInstruction());
			if (words[word] == nullptr) return false;
		}
		const auto& first = m_bounded_srt_reads[words[0]->read_id];
		// Workgroup snapshots currently cover scalar payloads, not descriptor candidates.
		if (first.workgroup_axis != UINT32_MAX) return false;
		for (uint32_t word = 1; word < words.size(); ++word) {
			const auto& next = m_bounded_srt_reads[words[word]->read_id];
			if (words[word]->proof.index != words[0]->proof.index ||
			    first.address_source != next.address_source || first.count_source != next.count_source ||
			    next.workgroup_axis != UINT32_MAX ||
			    first.offset_scale != next.offset_scale || first.offset_bias != next.offset_bias ||
			    next.memory_offset != first.memory_offset + word * sizeof(uint32_t)) return false;
		}
		DescriptorSource descriptor;
		descriptor.dword_count = 4u;
		descriptor.dwords[0] = words[0]->proof.address_low;
		descriptor.dwords[1] = words[0]->proof.address_high;
		descriptor.dwords[2] = words[0]->proof.count;
		descriptor.dwords[3] = Value(0u);
		descriptor.bounded_buffer = DescriptorSource::BoundedBuffer {};
		for (uint32_t word = 0; word < words.size(); ++word)
			descriptor.bounded_buffer->reads[word] = words[word]->read_id;
		source = InternSource(descriptor);
		if (std::ranges::none_of(m_bounded_buffers,
		    [&](const BoundedBufferPlan& plan) { return plan.handle == &handle; }))
			m_bounded_buffers.push_back({&handle, words[0]->proof.index,
			                            {descriptor.dwords[0], descriptor.dwords[1], descriptor.dwords[2]}});
		return true;
	}

	// Only roots of an already-proved bounded read receive this extension.
	// Their raw ancestors execute before the unavoidable unsigned guard and
	// use host-evaluable addresses; arbitrary dynamic shader reads stay live.
	void PlanBoundedRootReads(Value value) {
		value = value.Resolve();
		auto* inst = value.TryInstruction();
		if (inst == nullptr || std::ranges::find(m_bounded_root_visited, inst) !=
		                           m_bounded_root_visited.end()) return;
		m_bounded_root_visited.push_back(inst);
		if (inst->GetOpcode() == ValueOpcode::ReadConst) {
			const auto slot = inst->Arg(1).Resolve();
			if (slot.IsImmediate() && slot.GetType() == Type::U32 &&
			    slot.U32() < m_program.srt_reads.size())
				PlanBoundedRootReads(m_program.srt_reads[slot.U32()].value);
		}
		for (size_t arg = 0; arg < inst->NumArgs(); ++arg)
			PlanBoundedRootReads(inst->Arg(arg));
		const auto op = inst->GetOpcode();
		if (op != ValueOpcode::LoadAddressU32 && op != ValueOpcode::ReadConstBuffer) return;
		const auto flags = inst->Flags<MemoryFlags>();
		if (flags.index >= m_program.memory_info.size())
			Fail(flags.pc, "bounded snapshot root has invalid memory metadata");
		const auto& memory = m_program.memory_info[flags.index];
		if (memory.planning_only) return;
		if ((op == ValueOpcode::LoadAddressU32 && memory.kind != ResourceKind::ScalarAddress) ||
		    (op == ValueOpcode::ReadConstBuffer && memory.kind != ResourceKind::ScalarBuffer) ||
		    inst->Parent() == nullptr || !ValidateRuntimeValue(m_program, value))
			Fail(flags.pc, "bounded snapshot root is not a valid runtime scalar read");
		m_bounded_root_reads.push_back(inst);
	}

	void ApplyBoundedRootReads() {
		for (auto* read : m_bounded_root_reads) {
			auto* block = read->Parent();
			const auto where = std::ranges::find_if(block->Instructions(),
			    [&](const Inst& inst) { return &inst == read; });
			const auto slot = static_cast<uint32_t>(m_program.srt_reads.size());
			const auto srt = Value(&*block->PrependNewInst(where, ValueOpcode::GetSrtResource));
			const auto flat = Value(&*block->PrependNewInst(where, ValueOpcode::ReadConst,
			                                                  {srt, Value(slot)}));
			const auto original = Value(read);
			const auto uses = read->Uses();
			for (const auto& use : uses) use.user->SetArg(use.operand, flat);
			const auto rewrite = [&](Value& value) {
				if (value.Resolve() == original) value = flat;
			};
			for (auto& info : m_program.block_info) {
				rewrite(info.condition);
				rewrite(info.indirect_target);
			}
			// These retained values are not registered IR Uses. In particular,
			// the count/address sources must point to ReadConst so extraction
			// marks the exact flat slot for the coherent specialization reader.
			for (auto& source : m_sources)
				for (uint32_t word = 0; word < source.dword_count; ++word)
					rewrite(source.dwords[word]);
			for (auto& plan : m_bounded_buffers)
				for (auto& root : plan.roots) rewrite(root);
			for (auto& plan : m_indirect_images) {
				rewrite(plan.key);
				for (auto& root : plan.roots) rewrite(root);
			}
			for (auto& plan : m_inline_descriptors) {
				rewrite(plan.key);
				for (auto& root : plan.roots) rewrite(root);
			}
			// Keep a real raw template for host evaluation, without invalidating
			// it as ReplaceUsesWith would. Isolate its planning metadata from any
			// live access which happens to share the original MemoryInfo index.
			auto flags = read->Flags<MemoryFlags>();
			auto memory = m_program.memory_info[flags.index];
			memory.planning_only = true;
			flags.index = static_cast<uint32_t>(m_program.memory_info.size());
			m_program.memory_info.push_back(memory);
			read->SetFlags(flags);
			m_program.srt_reads.push_back({original, slot});
			block->AppendNewInst(ValueOpcode::ReferenceU32, {original});
		}
		std::erase_if(m_program.dynamic_reads, [&](Value value) {
			return std::ranges::find(m_bounded_root_reads, value.Resolve().TryInstruction()) !=
			       m_bounded_root_reads.end();
		});
	}

	void ApplyBoundedReads() {
		m_program.bounded_srt_reads = m_bounded_srt_reads;
		for (const auto& plan : m_bounded_reads) {
			auto* block = plan.read->Parent();
			auto where = std::ranges::find_if(block->Instructions(),
			    [&](const Inst& inst) { return &inst == plan.read; });
			const auto replacement = Value(&*block->PrependNewInst(
			    where, ValueOpcode::ReadBoundedSrtU32, {plan.proof.index}, plan.read_id));
			// Real indexed immutable reads replace every consumer, including ordinary
			// scalar threshold data. The raw memory instruction is no longer emitted;
			// no planning_only shortcut discards its runtime index or shared readers.
			plan.read->ReplaceUsesWith(replacement);
		}
		for (const auto& plan : m_bounded_buffers) {
			plan.handle->SetArg(0, plan.index);
			for (uint32_t word = 1; word < 4u; ++word) plan.handle->SetArg(word, plan.roots[word - 1u]);
		}
		std::erase_if(m_program.dynamic_reads, [](Value value) {
			const auto* inst = value.Resolve().TryInstruction();
			return inst != nullptr && inst->GetOpcode() == ValueOpcode::ReadBoundedSrtU32;
		});
		// Descriptor sources are non-owning Values. Pure coefficient reads no
		// longer have a resource handle retaining their address/count operands.
		// Keep the final roots alive through DCE and plan extraction, including
		// ReadConst roots introduced by ApplyBoundedRootReads above.
		std::vector<uint8_t> retained_sources(m_sources.size());
		std::vector<Inst*> retained_roots;
		const auto retain = [&](uint32_t source) {
			if (source >= m_sources.size()) Fail(0, "bounded snapshot source is missing");
			if (retained_sources[source] != 0u) return;
			retained_sources[source] = 1u;
			auto& descriptor = m_sources[source];
			for (uint32_t word = 0; word < descriptor.dword_count; ++word) {
				auto& value = descriptor.dwords[word];
				value = value.Resolve();
				auto* inst = value.TryInstruction();
				if (inst == nullptr) continue;
				if (value.GetType() != Type::U32 || inst->Parent() == nullptr)
					Fail(0, "bounded snapshot root is not a defined U32 value");
				if (std::ranges::find(retained_roots, inst) != retained_roots.end()) continue;
				retained_roots.push_back(inst);
				inst->Parent()->AppendNewInst(ValueOpcode::ReferenceU32, {value});
			}
		};
		for (const auto& read : m_bounded_srt_reads) {
			retain(read.address_source);
			if (read.workgroup_axis == UINT32_MAX) retain(read.count_source);
		}
	}

	uint32_t InternSource(const DescriptorSource& descriptor) {
		for (uint32_t candidate = 0; candidate < m_sources.size(); candidate++) {
			const auto& current = m_sources[candidate];
			if (current.dword_count != descriptor.dword_count ||
			    current.indirect_image != descriptor.indirect_image ||
			    current.inline_descriptor != descriptor.inline_descriptor ||
			    current.bounded_buffer != descriptor.bounded_buffer) {
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
		uint32_t bad_dword = 0;
		if (!ValidateSource(descriptor, bad_dword)) {
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

	const IndirectImagePlan* FindIndirectImage(const Inst& handle) const {
		const auto found =
		    std::find_if(m_indirect_images.begin(), m_indirect_images.end(),
		                 [&](const IndirectImagePlan& plan) {
			    return plan.handle == &handle;
		    });
		return found == m_indirect_images.end() ? nullptr : &*found;
	}

	bool IsIndirectPlanningMemory(uint32_t index) const {
		return std::ranges::find(m_inline_planning_memory, index) != m_inline_planning_memory.end() ||
		       std::any_of(m_indirect_images.begin(), m_indirect_images.end(),
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
				if (TryMakeIndirectImage(*handle, inst.Flags<MemoryFlags>().pc, plan)) {
					m_indirect_images.push_back(std::move(plan));
				}
			}
		}
	}

	const InlineDescriptorPlan* FindInlineDescriptor(const Inst& handle) const {
		const auto found = std::ranges::find_if(m_inline_descriptors, [&](const auto& plan) {
			return plan.handle == &handle;
		});
		return found == m_inline_descriptors.end() ? nullptr : &*found;
	}

	static bool MatchInlineStride(Value key, uint32_t& stride) {
		const auto* multiply = key.Resolve().TryInstruction();
		return multiply != nullptr && multiply->GetOpcode() == ValueOpcode::IMul32 &&
		       multiply->NumArgs() == 2u &&
		       (ImmediateU32(multiply->Arg(0), stride) || ImmediateU32(multiply->Arg(1), stride)) &&
		       stride != 0u;
	}

	bool TryMakeInlineDescriptor(Inst& handle, uint32_t pc, InlineDescriptorPlan& plan,
	                             bool compact_image = true) {
		const bool image = handle.GetOpcode() == ValueOpcode::GetImageResource;
		if ((!image && handle.GetOpcode() != ValueOpcode::GetSamplerResource) ||
		    handle.NumArgs() != (image ? 8u : 4u)) {
			return false;
		}
		const uint32_t descriptor_dwords = image && !compact_image ? 8u : 4u;
		if (image && compact_image) {
			for (uint32_t i = 4u; i < 8u; i++) {
				uint32_t upper = 0;
				if (!ImmediateU32(handle.Arg(i), upper) || upper != 0u) {
					return false;
				}
			}
		}
		Inst* buffer = nullptr;
		uint32_t base_offset = 0;
		for (uint32_t i = 0; i < descriptor_dwords; i++) {
			const auto* read = handle.Arg(i).Resolve().TryInstruction();
			uint32_t memory_index = 0;
			const auto* memory = read != nullptr ? ScalarReadMemory(*read, memory_index) : nullptr;
			if (memory == nullptr || !MemoryIndexBelongsTo(memory_index, *read)) {
				return false;
			}
			auto* current_buffer = read->Arg(0).Resolve().TryInstruction();
			if (current_buffer == nullptr || (buffer != nullptr && buffer != current_buffer)) {
				return false;
			}
			buffer = current_buffer;
			if (i == 0u) {
				base_offset = memory->offset;
				plan.key = read->Arg(1).Resolve();
			} else if (static_cast<uint64_t>(base_offset) + i * 4u != memory->offset ||
			           !EquivalentValue(m_program, plan.key, read->Arg(1))) {
				return false;
			}
			plan.memory[i] = memory_index;
			plan.reads[i] = read;
		}
		// Keep the wrapped byte offset on the GPU, including loop-carried selectors.
		// Only the buffer descriptor itself must be runtime-uniform.
		uint32_t stride = 0;
		if (!MatchInlineStride(plan.key, stride)) {
			return false;
		}
		DescriptorSource buffer_source;
		uint32_t buffer_source_index = 0;
		if (!MakeRuntimeBufferSource(*buffer, pc, buffer_source_index, buffer_source)) {
			return false;
		}
		DescriptorSource source;
		source.dword_count = image ? 8u : 4u;
		std::copy_n(buffer_source.dwords.begin(), 4u, source.dwords.begin());
		for (uint32_t i = 4u; i < source.dword_count; i++) {
			source.dwords[i] = Value(0u);
		}
		source.inline_descriptor = DescriptorSource::InlineDescriptor {
		    buffer_source_index, stride, base_offset, 0u};
		source.inline_descriptor->descriptor_dwords = descriptor_dwords;
		plan.handle = &handle;
		plan.source = InternSource(source);
		plan.read_count = descriptor_dwords;
		std::copy_n(buffer_source.dwords.begin(), 4u, plan.roots.begin());
		return true;
	}

	bool TryMakeMaterialImageTable(Inst& handle, uint32_t pc, InlineDescriptorPlan& plan) {
		if (handle.GetOpcode() != ValueOpcode::GetImageResource || handle.NumArgs() != 8u) {
			return false;
		}
		Inst* address = nullptr;
		Value table_index;
		uint32_t table_offset = 0;
		for (uint32_t i = 0; i < 8u; i++) {
			const auto* read = handle.Arg(i).Resolve().TryInstruction();
			if (read == nullptr || read->GetOpcode() != ValueOpcode::LoadAddressU32 || read->NumArgs() != 4u) {
				return false;
			}
			const auto memory_index = read->Flags<MemoryFlags>().index;
			if (memory_index >= m_program.memory_info.size() || !MemoryIndexBelongsTo(memory_index, *read)) {
				return false;
			}
			const auto& memory = m_program.memory_info[memory_index];
			uint32_t high_offset = 0;
			const auto enabled = read->Arg(3).Resolve();
			if (memory.kind != ResourceKind::ScalarAddress || memory.data_bits != 32u ||
			    memory.data_dwords != 1u || !ImmediateU32(read->Arg(2), high_offset) || high_offset != 0u ||
			    !enabled.IsImmediate() || enabled.GetType() != Type::U1 || !enabled.U1()) {
				return false;
			}
			auto* current_address = read->Arg(0).Resolve().TryInstruction();
			if (current_address == nullptr || current_address->GetOpcode() != ValueOpcode::GetAddressResource ||
			    (address != nullptr && address != current_address)) {
				return false;
			}
			address = current_address;
			if (i == 0u) {
				table_offset = memory.offset;
				table_index = read->Arg(1).Resolve();
			} else if (static_cast<uint64_t>(table_offset) + i * 4u != memory.offset ||
			           !EquivalentValue(m_program, table_index, read->Arg(1))) {
				return false;
			}
			plan.memory[i] = memory_index;
			plan.reads[i] = read;
		}
		const auto* scale = table_index.TryInstruction();
		uint32_t table_shift = 0;
		if (scale == nullptr || scale->GetOpcode() != ValueOpcode::ShiftLeftLogical32 ||
		    !ImmediateU32(scale->Arg(1), table_shift) || table_shift != 5u) {
			return false;
		}
		Value packed;
		uint32_t index_shift = 0;
		uint32_t index_mask = 0;
		const auto* extract = scale->Arg(0).Resolve().TryInstruction();
		if (extract != nullptr && extract->GetOpcode() == ValueOpcode::BitwiseAnd32) {
			if (ImmediateU32(extract->Arg(0), index_mask)) {
				packed = extract->Arg(1).Resolve();
			} else if (ImmediateU32(extract->Arg(1), index_mask)) {
				packed = extract->Arg(0).Resolve();
			} else {
				return false;
			}
			const auto* shift = packed.TryInstruction();
			if (shift != nullptr && shift->GetOpcode() == ValueOpcode::ShiftRightLogical32) {
				if (!ImmediateU32(shift->Arg(1), index_shift)) {
					return false;
				}
				index_shift &= 31u;
				packed = shift->Arg(0).Resolve();
			}
		} else if (extract != nullptr && extract->GetOpcode() == ValueOpcode::BitFieldUExtract) {
			uint32_t width = 0;
			if (!ImmediateU32(extract->Arg(1), index_shift) || !ImmediateU32(extract->Arg(2), width) ||
			    width == 0u || width > 8u || index_shift > 32u - width) {
				return false;
			}
			index_mask = (1u << width) - 1u;
			packed = extract->Arg(0).Resolve();
		} else {
			return false;
		}
		if (index_mask == 0u || index_mask > 0xffu) {
			return false;
		}
		const auto* material_read = packed.TryInstruction();
		uint32_t material_memory = 0;
		const auto* memory = material_read != nullptr ? ScalarReadMemory(*material_read, material_memory) : nullptr;
		if (memory == nullptr) {
			return false;
		}
		plan.key = material_read->Arg(1).Resolve();
		uint32_t stride = 0;
		auto* buffer = material_read->Arg(0).Resolve().TryInstruction();
		if (buffer == nullptr || !MatchInlineStride(plan.key, stride)) {
			return false;
		}
		DescriptorSource address_source;
		MakeSource(*address, 2u, false, false, address_source, pc);
		uint32_t bad_dword = 0;
		if (!ValidateSource(address_source, bad_dword)) {
			return false;
		}
		DescriptorSource buffer_source;
		uint32_t buffer_index = 0;
		if (!MakeRuntimeBufferSource(*buffer, pc, buffer_index, buffer_source)) {
			return false;
		}
		DescriptorSource source;
		source.dword_count = 8u;
		std::copy_n(buffer_source.dwords.begin(), 4u, source.dwords.begin());
		std::copy_n(address_source.dwords.begin(), 2u, source.dwords.begin() + 4u);
		source.dwords[6] = Value(0u);
		source.dwords[7] = Value(0u);
		source.inline_descriptor = DescriptorSource::InlineDescriptor {
		    buffer_index, stride, memory->offset, 0u,
		    DescriptorSource::InlineDescriptor::ImageTable {InternSource(address_source), table_offset, index_shift, index_mask}};
		plan.handle = &handle;
		plan.source = InternSource(source);
		plan.root_count = 6u;
		plan.read_count = 8u;
		std::copy_n(source.dwords.begin(), 6u, plan.roots.begin());
		return true;
	}

	void PlanInlineDescriptors() {
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if (inst.GetOpcode() != ValueOpcode::ImageSampleRaw || inst.NumArgs() < 2u) {
					continue;
				}
				const auto flags = inst.Flags<MemoryFlags>();
				if (flags.index >= m_program.memory_info.size()) {
					continue;
				}
				auto* image = inst.Arg(0).Resolve().TryInstruction();
				auto* sampler = inst.Arg(1).Resolve().TryInstruction();
				InlineDescriptorPlan image_plan;
				InlineDescriptorPlan sampler_plan;
				if (image == nullptr || sampler == nullptr || FindIndirectImage(*image) != nullptr ||
				    !(TryMakeInlineDescriptor(*image, flags.pc, image_plan,
				                              m_program.memory_info[flags.index].image_r128) ||
				      (!m_program.memory_info[flags.index].image_r128 &&
				       TryMakeMaterialImageTable(*image, flags.pc, image_plan)))) {
					continue;
				}
				const bool inline_sampler = TryMakeInlineDescriptor(*sampler, flags.pc, sampler_plan);
				if (inline_sampler) {
					const auto& image_source = *m_sources[image_plan.source].inline_descriptor;
					const auto& sampler_source = *m_sources[sampler_plan.source].inline_descriptor;
					if (image_source.buffer_source != sampler_source.buffer_source ||
					    image_source.selector_stride != sampler_source.selector_stride ||
					    !EquivalentValue(m_program, image_plan.key, sampler_plan.key)) {
						Fail(flags.pc, "inline image and sampler require the same material buffer and selector");
					}
				} else {
					if (sampler->GetOpcode() != ValueOpcode::GetSamplerResource) {
						continue;
					}
					DescriptorSource sampler_source;
					const bool sample_adjust = (m_program.memory_info[flags.index].image_sample_flags &
					                            Decoder::ImageSampleFlagAdjust) != 0;
					MakeSource(*sampler, 4u, true, sample_adjust, sampler_source, flags.pc);
					uint32_t bad_dword = 0;
					if (!ValidateSource(sampler_source, bad_dword)) {
						continue;
					}
				}
				if (FindInlineDescriptor(*image) == nullptr) {
					m_inline_descriptors.push_back(image_plan);
				}
				if (inline_sampler && FindInlineDescriptor(*sampler) == nullptr) {
					m_inline_descriptors.push_back(sampler_plan);
				}
			}
		}
		std::vector<const Inst*> handles;
		for (const auto& plan: m_inline_descriptors) {
			handles.push_back(plan.handle);
		}
		for (const auto& plan: m_inline_descriptors) {
			for (uint32_t i = 0; i < plan.read_count; i++) {
				// A descriptor word may also be used by ordinary shader arithmetic.
				// Only remove loads whose complete use set is rewritten by this plan.
				if (UsesOnly(*plan.reads[i], handles)) {
					m_inline_planning_reads.push_back(plan.reads[i]);
					m_inline_planning_memory.push_back(plan.memory[i]);
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
		if (expected == ValueOpcode::GetBufferResource && MakeBoundedBufferSource(*handle, source)) return;
		DescriptorSource descriptor;
		MakeSource(*handle, width, sampler, sample_adjust, descriptor, pc);
		uint32_t bad_dword = 0;
		if (expected == ValueOpcode::GetImageResource) {
			for (; bad_dword < descriptor.dword_count; bad_dword++) {
				const auto* value = descriptor.dwords[bad_dword].Resolve().TryInstruction();
				if (value != nullptr && value->GetOpcode() == ValueOpcode::ReadConstBuffer) {
					Fail(pc, fmt::format("{} dword {} is not a valid runtime value",
					                     ValueOpcodeName(expected), bad_dword));
				}
			}
			bad_dword = 0;
		}
		if (!ValidateSource(descriptor, bad_dword)) {
			Fail(pc, fmt::format("{} dword {} is not a valid runtime value",
			                     ValueOpcodeName(expected), bad_dword));
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
		resource.descriptor_formatted_only = true;
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
		resource.descriptor_formatted_only =
		    resource.descriptor_formatted_only && memory.kind == ResourceKind::Buffer &&
		    memory.formatted && !memory.typed && !atomic;
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
			Fail(pc, fmt::format("sampled image/sampler pair limit exceeded (required={} limit={})",
			                     m_info.sampled_pairs.size() + 1u, ShaderInfo::MaxSampledPairs));
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
	                    uint32_t pc, uint32_t buffer_table = UINT32_MAX) {
		for (auto& patch: m_memory_patches) {
			if (patch.index != index) {
				continue;
			}
			if (patch.resource != resource || patch.buffer_table != buffer_table ||
			    (has_sampler && patch.has_sampler && patch.sampler != sampler)) {
				Fail(pc, "memory metadata is reused with incompatible resources");
			}
			if (has_sampler) {
				patch.sampler     = sampler;
				patch.has_sampler = true;
			}
			return;
		}
		m_memory_patches.push_back({index, resource, sampler, has_sampler, buffer_table});
	}

	void Collect(Inst& inst) {
		if (BoundedRead(&inst) != nullptr ||
		    std::ranges::find(m_bounded_root_reads, &inst) != m_bounded_root_reads.end()) return;
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
				Fail(flags.pc, fmt::format("buffer resource limit exceeded (required={} limit={})",
				                           m_info.buffers.size() + 1u, ShaderInfo::MaxBuffers));
			}
			AddHandlePatch(handle, resource, flags.pc);
			AddMemoryPatch(flags.index, resource, 0, false, flags.pc,
			               m_sources[source].bounded_buffer.has_value() ? resource : UINT32_MAX);
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
		const auto* inline_descriptor = handle != nullptr ? FindInlineDescriptor(*handle) : nullptr;
		if (indirect != nullptr) {
			source = indirect->source;
		} else if (inline_descriptor != nullptr) {
			source = inline_descriptor->source;
		} else {
			GetHandle(inst.Arg(0), ValueOpcode::GetImageResource, 8, flags.pc, handle, source);
		}
		resource = AddImage(source, memory, op, flags.pc);
		if (resource == UINT32_MAX) {
			Fail(flags.pc, fmt::format("image resource limit exceeded (required={} limit={})",
			                           m_info.images.size() + 1u, ShaderInfo::MaxImages));
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
			sampler_handle = inst.Arg(1).Resolve().TryInstruction();
			const auto* inline_sampler = sampler_handle != nullptr ? FindInlineDescriptor(*sampler_handle) : nullptr;
			if (inline_sampler != nullptr) {
				sampler_source = inline_sampler->source;
			} else {
				GetHandle(inst.Arg(1), ValueOpcode::GetSamplerResource, 4, flags.pc, sampler_handle,
				          sampler_source, true, sample_adjust);
			}
			sampler = AddSampler(sampler_source, flags.pc);
			if (sampler == UINT32_MAX) {
				Fail(flags.pc, fmt::format("sampler resource limit exceeded (required={} limit={})",
				                           m_info.samplers.size() + 1u, ShaderInfo::MaxSamplers));
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
			if (buffer_source == nullptr || buffer_source->dword_count != 4 ||
			    buffer_source->bounded_buffer.has_value()) {
				continue;
			}
			for (uint32_t image = 0; image < m_info.images.size(); image++) {
				const auto* image_source = Source(m_info.images[image].source);
				if (image_source == nullptr || image_source->dword_count != 8 ||
				    image_source->indirect_image.has_value() || image_source->inline_descriptor.has_value()) {
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
	std::vector<BoundedSrtRead> m_bounded_srt_reads;
	std::vector<BoundedReadPlan> m_bounded_reads;
	std::vector<const Inst*> m_bounded_root_visited;
	std::vector<Inst*> m_bounded_root_reads;
	std::vector<BoundedBufferPlan> m_bounded_buffers;
	std::vector<HandlePatch>       m_handle_patches;
	std::vector<MemoryPatch>       m_memory_patches;
	std::vector<IndirectImagePlan> m_indirect_images;
	std::vector<InlineDescriptorPlan> m_inline_descriptors;
	std::vector<const Inst*>       m_inline_planning_reads;
	std::vector<uint32_t>          m_inline_planning_memory;
};

} // namespace

void TrackResources(Program& program) {
	Tracker(program).Run();
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
