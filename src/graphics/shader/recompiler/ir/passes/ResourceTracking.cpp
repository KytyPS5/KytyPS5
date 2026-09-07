#include "graphics/shader/recompiler/ir/passes/ResourceTracking.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <span>
#include <unordered_set>
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
		m_shader_writes = HasShaderMemoryWrites(program);
	}

	void Run() {
		if (m_program.resource_tracking_complete) {
			Fail(0, "resources already tracked");
		}
		if (!m_program.srt_plan_complete) {
			Fail(0, "SRT plan is not ready");
		}
		const char* trace_env = std::getenv("KYTY_RESOURCE_TRACKING_TRACE");
		const bool  trace     = trace_env != nullptr && *trace_env != '\0';
		const auto phase = [&](const char* name, auto&& action) {
			const auto start = std::chrono::steady_clock::now();
			if (trace) {
				std::printf("ResourceTracking begin: hash=0x%016" PRIx64 " phase=%s\n",
				            m_program.shader_hash, name);
				std::fflush(stdout);
			}
			action();
			if (trace) {
				const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				                         std::chrono::steady_clock::now() - start)
				                         .count();
				std::printf("ResourceTracking end: hash=0x%016" PRIx64
				            " phase=%s elapsed_ms=%" PRId64 "\n",
				            m_program.shader_hash, name, elapsed);
				std::fflush(stdout);
			}
		};
		phase("PlanBoundedReads", [&] { PlanBoundedReads(); });
		phase("PlanIndirectImages", [&] { PlanIndirectImages(); });
		phase("PlanInlineDescriptors", [&] { PlanInlineDescriptors(); });
		phase("Collect", [&] {
			for (auto* block: m_program.blocks) {
				for (auto& inst: *block) {
					Collect(inst);
				}
			}
		});
		phase("LinkImageAliases", [&] { LinkImageAliases(); });
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
		phase("ApplyBoundedRootReads", [&] { ApplyBoundedRootReads(); });
		phase("ApplyBoundedReads", [&] { ApplyBoundedReads(); });
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

	Value LowerDescriptorPhi(Value value, const Block* use) {
		value           = value.Resolve();
		const auto* phi = value.TryInstruction();
		if (m_shader_writes || phi == nullptr || phi->GetOpcode() != ValueOpcode::Phi ||
		    phi->NumArgs() != 2u || phi->NumPhiBlocks() != 2u || phi->GetType() != Type::U32 ||
		    m_program.blocks.size() != m_program.block_info.size()) {
			return value;
		}
		const auto* merge = phi->Parent();
		const auto* branch = phi->PhiBlock(0);
		if (merge == nullptr || branch == nullptr || phi->PhiBlock(1) == nullptr ||
		    branch == phi->PhiBlock(1)) {
			return value;
		}
		// Structurization can merge the descriptor and its use predicate in parallel Phis.
		// Match their incoming blocks to exclude only edges that cannot reach this use.
		if (use != nullptr && use->ImmPredecessors().size() == 1u &&
		    use->ImmPredecessors()[0] == merge) {
			const auto merge_it = std::ranges::find(m_program.blocks, merge);
			const auto use_it   = std::ranges::find(m_program.blocks, use);
			if (merge_it != m_program.blocks.end() && use_it != m_program.blocks.end()) {
				const auto& info = m_program.block_info[merge_it - m_program.blocks.begin()];
				const auto& term = info.terminator;
				const auto id    = m_program.block_info[use_it - m_program.blocks.begin()].id;
				const auto* condition = info.condition.Resolve().TryInstruction();
				if (term.kind == CFG::TerminatorKind::ConditionalBranch &&
				    term.true_block != term.false_block &&
				    (id == term.true_block || id == term.false_block) && condition != nullptr &&
				    condition->GetOpcode() == ValueOpcode::Phi && condition->GetType() == Type::U1 &&
				    condition->Parent() == merge && condition->NumArgs() == 2u &&
				    condition->NumPhiBlocks() == 2u) {
					const bool taken = id == term.true_block;
					for (uint32_t skipped = 0; skipped < 2u; skipped++) {
						const auto excluded = condition->Arg(skipped).Resolve();
						const auto included = condition->Arg(skipped ^ 1u).Resolve();
						if (!excluded.IsImmediate() || excluded.GetType() != Type::U1 ||
						    excluded.U1() == taken ||
						    (included.IsImmediate() && included.U1() != taken)) {
							continue;
						}
						for (uint32_t selected = 0; selected < 2u; selected++) {
							if (phi->PhiBlock(selected) == condition->PhiBlock(skipped ^ 1u) &&
							    phi->PhiBlock(selected ^ 1u) == condition->PhiBlock(skipped)) {
								return phi->Arg(selected);
							}
						}
					}
				}
			}
		}
		for (const auto& [original, selected]: m_descriptor_selections) {
			if (original == phi) {
				return selected;
			}
		}
		if (branch->ImmSuccessors().size() != 2u) {
			if (branch->ImmPredecessors().size() != 1u) {
				return value;
			}
			branch = branch->ImmPredecessors()[0];
		}
		if (branch == merge || branch->ImmSuccessors().size() != 2u) {
			return value;
		}
		std::array<uint32_t, 2> target_ids;
		for (uint32_t arm = 0; arm < 2; arm++) {
			const auto* incoming = phi->PhiBlock(arm);
			if (incoming == merge ||
			    (incoming != branch &&
			     (incoming->ImmPredecessors().size() != 1u ||
			      incoming->ImmPredecessors()[0] != branch ||
			      incoming->ImmSuccessors().size() != 1u ||
			      incoming->ImmSuccessors()[0] != merge))) {
				return value;
			}
			const auto* target = incoming == branch ? merge : incoming;
			const auto  it     = std::ranges::find(m_program.blocks, target);
			if (it == m_program.blocks.end()) {
				return value;
			}
			target_ids[arm] = m_program.block_info[it - m_program.blocks.begin()].id;
		}
		const auto branch_it = std::ranges::find(m_program.blocks, branch);
		if (branch_it == m_program.blocks.end()) {
			return value;
		}
		const auto& info = m_program.block_info[branch_it - m_program.blocks.begin()];
		const auto& term = info.terminator;
		if (term.kind != CFG::TerminatorKind::ConditionalBranch ||
		    !((term.true_block == target_ids[0] && term.false_block == target_ids[1]) ||
		      (term.false_block == target_ids[0] && term.true_block == target_ids[1])) ||
		    !ValidateRuntimeValue(m_program, info.condition, RuntimeValueType::Integer) ||
		    !ValidateRuntimeValue(m_program, phi->Arg(0)) ||
		    !ValidateRuntimeValue(m_program, phi->Arg(1))) {
			return value;
		}
		// Retain a host expression; replacing the GPU Phi would break SSA dominance.
		const auto true_arg = term.true_block == target_ids[0] ? 0u : 1u;
		auto&      selected = m_program.value_storage.emplace_back(ValueOpcode::SelectU32);
		selected.SetArg(0, info.condition);
		selected.SetArg(1, phi->Arg(true_arg));
		selected.SetArg(2, phi->Arg(true_arg ^ 1u));
		m_descriptor_selections.emplace_back(phi, Value(&selected));
		return Value(&selected);
	}

	void MakeSource(const Inst& handle, uint32_t width, bool sampler, bool sample_adjust,
	                DescriptorSource& descriptor, uint32_t pc) {
		if (handle.NumArgs() != width) {
			Fail(pc, fmt::format("{} has {} descriptor dwords, expected {}",
			                     ValueOpcodeName(handle.GetOpcode()), handle.NumArgs(), width));
		}
		descriptor.dword_count = width;
		for (uint32_t i = 0; i < width; i++) {
			descriptor.dwords[i] = LowerDescriptorPhi(handle.Arg(i), handle.Parent());
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
	struct BoundedImagePlan {
		Inst* handle = nullptr;
		Value index;
	};
	struct BoundedSamplerPlan {
		Inst* handle = nullptr;
		Value index;
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
				if (proof->source_dwords == 4u) {
					PlanBoundedRootReads(proof->descriptor_word2);
					PlanBoundedRootReads(proof->descriptor_word3);
				}
				DescriptorSource address;
				address.dword_count = proof->source_dwords;
				address.dwords[0] = proof->address_low;
				address.dwords[1] = proof->address_high;
				if (proof->source_dwords == 4u) {
					address.dwords[2] = proof->descriptor_word2;
					address.dwords[3] = proof->descriptor_word3;
				}
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
				                          proof->workgroup_axis, proof->count_signed};
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

	const BoundedReadPlan* BoundedReadValue(Value value) const {
		value = value.Resolve();
		const Inst* read = value.TryInstruction();
		std::unordered_set<uint32_t> visited_slots;
		while (read != nullptr && read->GetOpcode() == ValueOpcode::ReadConst &&
		       read->NumArgs() == 2u) {
			const auto slot = read->Arg(1).Resolve();
			if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
			    slot.U32() >= m_program.srt_reads.size() ||
			    !visited_slots.insert(slot.U32()).second) {
				return nullptr;
			}
			read = m_program.srt_reads[slot.U32()].value.Resolve().TryInstruction();
		}
		return BoundedRead(read);
	}

	bool CollectBoundedDependencies(Value value, std::vector<const BoundedReadPlan*>& dependencies,
	                                std::unordered_set<const Inst*>& visiting,
	                                std::unordered_set<const Inst*>& completed) const {
		value = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) return true;
		if (completed.contains(inst)) return true;
		if (const auto* bounded = BoundedRead(inst); bounded != nullptr) {
			if (std::ranges::find(dependencies, bounded) == dependencies.end())
				dependencies.push_back(bounded);
			completed.insert(inst);
			return true;
		}
		if (!visiting.insert(inst).second) return false;
		const auto finish = [&](bool result) {
			visiting.erase(inst);
			if (result) completed.insert(inst);
			return result;
		};
		if (inst->GetOpcode() == ValueOpcode::ReadConst && inst->NumArgs() == 2u) {
			const auto slot = inst->Arg(1).Resolve();
			if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
			    slot.U32() >= m_program.srt_reads.size()) return finish(false);
			return finish(CollectBoundedDependencies(m_program.srt_reads[slot.U32()].value,
			                                           dependencies, visiting, completed));
		}
		for (uint32_t arg = 0; arg < inst->NumArgs(); ++arg) {
			if (!CollectBoundedDependencies(inst->Arg(arg), dependencies, visiting, completed))
				return finish(false);
		}
		return finish(true);
	}

	uint32_t InternBoundedSelector(Value selector) {
		selector = selector.Resolve();
		for (uint32_t group = 0; group < m_bounded_selectors.size(); ++group) {
			if (EquivalentValue(m_program, selector, m_bounded_selectors[group])) return group;
		}
		m_bounded_selectors.push_back(selector);
		return static_cast<uint32_t>(m_bounded_selectors.size() - 1u);
	}

	bool MakeBoundedBufferExpression(Inst& handle, DescriptorSource descriptor,
	                                 uint32_t& source, std::string& rejection) {
		std::vector<const BoundedReadPlan*> dependencies;
		std::unordered_set<const Inst*> visiting;
		std::unordered_set<const Inst*> completed;
		for (uint32_t word = 0; word < descriptor.dword_count; ++word) {
			if (!CollectBoundedDependencies(descriptor.dwords[word], dependencies, visiting,
			                                completed)) {
				rejection = "descriptor dependency graph is cyclic or has an invalid flat slot";
				return false;
			}
		}
		if (dependencies.empty()) return false;
		const auto& first = m_bounded_srt_reads[dependencies.front()->read_id];
		if (first.workgroup_axis != UINT32_MAX) {
			rejection = "descriptor dependency is indexed by a workgroup axis";
			return false;
		}
		for (const auto* dependency: dependencies) {
			const auto& read = m_bounded_srt_reads[dependency->read_id];
			if (dependency->proof.index != dependencies.front()->proof.index ||
			    read.count_source != first.count_source || read.count_signed != first.count_signed ||
			    read.workgroup_axis != first.workgroup_axis) {
				rejection = "descriptor dependencies do not share one selector and count";
				return false;
			}
		}
		descriptor.bounded_buffer.emplace();
		descriptor.bounded_buffer->expression = true;
		descriptor.bounded_buffer->selector_group =
		    InternBoundedSelector(dependencies.front()->proof.index);
		for (const auto* dependency: dependencies)
			descriptor.bounded_buffer->dependencies.push_back(dependency->read_id);
		source = InternSource(descriptor);
		if (std::ranges::none_of(m_bounded_buffers,
		    [&](const BoundedBufferPlan& plan) { return plan.handle == &handle; })) {
			m_bounded_buffers.push_back(
			    {&handle, dependencies.front()->proof.index, {Value(0u), Value(0u), Value(0u)}});
		}
		return true;
	}

	bool MakeBoundedImageExpression(Inst& handle, DescriptorSource descriptor,
	                                uint32_t& source, std::string& rejection) {
		std::vector<const BoundedReadPlan*> dependencies;
		std::unordered_set<const Inst*> visiting;
		std::unordered_set<const Inst*> completed;
		for (uint32_t word = 0; word < descriptor.dword_count; ++word) {
			if (!CollectBoundedDependencies(descriptor.dwords[word], dependencies, visiting,
			                                completed)) {
				rejection = "descriptor dependency graph is cyclic or has an invalid flat slot";
				return false;
			}
		}
		if (dependencies.empty()) return false;
		const auto& first = m_bounded_srt_reads[dependencies.front()->read_id];
		if (first.workgroup_axis != UINT32_MAX) {
			rejection = "descriptor dependency is indexed by a workgroup axis";
			return false;
		}
		for (const auto* dependency: dependencies) {
			const auto& read = m_bounded_srt_reads[dependency->read_id];
			if (dependency->proof.index != dependencies.front()->proof.index ||
			    read.count_source != first.count_source || read.count_signed != first.count_signed ||
			    read.workgroup_axis != first.workgroup_axis) {
				rejection = "descriptor dependencies do not share one selector and count";
				return false;
			}
		}
		descriptor.bounded_image.emplace();
		descriptor.bounded_image->expression = true;
		descriptor.bounded_image->selector_group =
		    InternBoundedSelector(dependencies.front()->proof.index);
		for (const auto* dependency: dependencies)
			descriptor.bounded_image->dependencies.push_back(dependency->read_id);
		source = InternSource(descriptor);
		if (std::ranges::none_of(m_bounded_images,
		    [&](const BoundedImagePlan& plan) { return plan.handle == &handle; }))
			m_bounded_images.push_back({&handle, dependencies.front()->proof.index});
		return true;
	}

	bool MakeBoundedSamplerExpression(Inst& handle, DescriptorSource descriptor,
	                                  uint32_t& source, std::string& rejection) {
		std::vector<const BoundedReadPlan*> dependencies;
		std::unordered_set<const Inst*> visiting;
		std::unordered_set<const Inst*> completed;
		for (uint32_t word = 0; word < descriptor.dword_count; ++word) {
			if (!CollectBoundedDependencies(descriptor.dwords[word], dependencies, visiting,
			                                completed)) {
				rejection = "descriptor dependency graph is cyclic or has an invalid flat slot";
				return false;
			}
		}
		if (dependencies.empty()) return false;
		const auto& first = m_bounded_srt_reads[dependencies.front()->read_id];
		if (first.workgroup_axis != UINT32_MAX) {
			rejection = "descriptor dependency is indexed by a workgroup axis";
			return false;
		}
		for (const auto* dependency: dependencies) {
			const auto& read = m_bounded_srt_reads[dependency->read_id];
			if (dependency->proof.index != dependencies.front()->proof.index ||
			    read.count_source != first.count_source || read.count_signed != first.count_signed ||
			    read.workgroup_axis != first.workgroup_axis) {
				rejection = "descriptor dependencies do not share one selector and count";
				return false;
			}
		}
		descriptor.bounded_sampler.emplace();
		descriptor.bounded_sampler->selector_group =
		    InternBoundedSelector(dependencies.front()->proof.index);
		for (const auto* dependency: dependencies)
			descriptor.bounded_sampler->dependencies.push_back(dependency->read_id);
		source = InternSource(descriptor);
		if (std::ranges::none_of(m_bounded_samplers,
		    [&](const BoundedSamplerPlan& plan) { return plan.handle == &handle; }))
			m_bounded_samplers.push_back({&handle, dependencies.front()->proof.index});
		return true;
	}

	bool MakeBoundedBufferSource(Inst& handle, uint32_t& source, std::string& rejection) {
		const auto reject = [&](std::string reason) {
			rejection = std::move(reason);
			return false;
		};
		if (handle.NumArgs() != 4u) return reject("descriptor width is not four DWORDs");
		std::array<const BoundedReadPlan*, 4> words;
		for (uint32_t word = 0; word < words.size(); ++word) {
			words[word] = BoundedReadValue(handle.Arg(word));
			if (words[word] == nullptr)
				return reject(fmt::format("column {} is not a proved bounded read", word));
		}
		const auto& first = m_bounded_srt_reads[words[0]->read_id];
		// Workgroup snapshots currently cover scalar payloads, not descriptor candidates.
		if (first.workgroup_axis != UINT32_MAX)
			return reject("column 0 is indexed by a workgroup axis");
		for (uint32_t word = 1; word < words.size(); ++word) {
			const auto& next = m_bounded_srt_reads[words[word]->read_id];
			if (words[word]->proof.index != words[0]->proof.index ||
			    first.address_source != next.address_source || first.count_source != next.count_source ||
			    first.count_signed != next.count_signed ||
			    next.workgroup_axis != UINT32_MAX ||
			    first.offset_scale != next.offset_scale || first.offset_bias != next.offset_bias ||
			    next.memory_offset != first.memory_offset + word * sizeof(uint32_t))
				return reject(fmt::format(
				    "column {} is not correlated (read={} first_read={} offset={} expected={})",
				    word, words[word]->read_id, words[0]->read_id, next.memory_offset,
				    first.memory_offset + word * sizeof(uint32_t)));
		}
		DescriptorSource descriptor;
		descriptor.dword_count = 4u;
		descriptor.dwords[0] = words[0]->proof.address_low;
		descriptor.dwords[1] = words[0]->proof.address_high;
		descriptor.dwords[2] = words[0]->proof.source_dwords == 4u
		                           ? words[0]->proof.descriptor_word2
		                           : words[0]->proof.count;
		descriptor.dwords[3] = words[0]->proof.source_dwords == 4u
		                           ? words[0]->proof.descriptor_word3
		                           : Value(0u);
		descriptor.bounded_buffer = DescriptorSource::BoundedBuffer {};
		descriptor.bounded_buffer->selector_group = InternBoundedSelector(words[0]->proof.index);
		for (uint32_t word = 0; word < words.size(); ++word)
			descriptor.bounded_buffer->reads[word] = words[word]->read_id;
		source = InternSource(descriptor);
		if (std::ranges::none_of(m_bounded_buffers,
		    [&](const BoundedBufferPlan& plan) { return plan.handle == &handle; }))
			m_bounded_buffers.push_back({&handle, words[0]->proof.index,
			                            {descriptor.dwords[0], descriptor.dwords[1], descriptor.dwords[2]}});
		return true;
	}

	bool MakeBoundedImageSource(Inst& handle, uint32_t& source) {
		if (handle.NumArgs() != 8u) return false;
		std::array<const BoundedReadPlan*, 8> words;
		for (uint32_t word = 0; word < words.size(); ++word) {
			words[word] = BoundedReadValue(handle.Arg(word));
			if (words[word] == nullptr || words[word]->proof.source_dwords != 2u) return false;
		}
		const auto& first = m_bounded_srt_reads[words[0]->read_id];
		if (first.workgroup_axis != UINT32_MAX) return false;
		for (uint32_t word = 1; word < words.size(); ++word) {
			const auto& next = m_bounded_srt_reads[words[word]->read_id];
			if (words[word]->proof.index != words[0]->proof.index ||
			    first.address_source != next.address_source || first.count_source != next.count_source ||
			    first.count_signed != next.count_signed ||
			    next.workgroup_axis != UINT32_MAX || first.offset_scale != next.offset_scale ||
			    first.offset_bias != next.offset_bias ||
			    next.memory_offset != first.memory_offset + word * sizeof(uint32_t)) return false;
		}
		DescriptorSource descriptor;
		descriptor.dword_count = 8u;
		descriptor.dwords[0] = words[0]->proof.address_low;
		descriptor.dwords[1] = words[0]->proof.address_high;
		descriptor.dwords[2] = words[0]->proof.count;
		for (uint32_t word = 3; word < descriptor.dword_count; ++word)
			descriptor.dwords[word] = Value(0u);
		descriptor.bounded_image.emplace();
		descriptor.bounded_image->selector_group = InternBoundedSelector(words[0]->proof.index);
		for (uint32_t word = 0; word < words.size(); ++word)
			descriptor.bounded_image->reads[word] = words[word]->read_id;
		source = InternSource(descriptor);
		if (std::ranges::none_of(m_bounded_images,
		    [&](const BoundedImagePlan& plan) { return plan.handle == &handle; }))
			m_bounded_images.push_back({&handle, words[0]->proof.index});
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
		for (const auto& plan : m_bounded_images) {
			plan.handle->SetArg(0, plan.index);
			for (uint32_t word = 1; word < 8u; ++word) plan.handle->SetArg(word, Value(0u));
		}
		for (const auto& plan : m_bounded_samplers) {
			plan.handle->SetArg(0, plan.index);
			for (uint32_t word = 1; word < 4u; ++word) plan.handle->SetArg(word, Value(0u));
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
		for (const auto& buffer: m_info.buffers) {
			if (buffer.source < m_sources.size() && m_sources[buffer.source].bounded_buffer.has_value() &&
			    m_sources[buffer.source].bounded_buffer->expression) {
				retain(buffer.source);
			}
		}
		for (const auto& image: m_info.images) {
			if (image.source < m_sources.size() && m_sources[image.source].bounded_image.has_value() &&
			    m_sources[image.source].bounded_image->expression) {
				retain(image.source);
			}
		}
		for (const auto& sampler: m_info.samplers) {
			if (sampler.source < m_sources.size() &&
			    m_sources[sampler.source].bounded_sampler.has_value()) {
				retain(sampler.source);
			}
		}
	}

	uint32_t InternSource(const DescriptorSource& descriptor) {
		for (uint32_t candidate = 0; candidate < m_sources.size(); candidate++) {
			const auto& current = m_sources[candidate];
			if (current.dword_count != descriptor.dword_count ||
			    current.indirect_image.has_value() != descriptor.indirect_image.has_value()) {
				continue;
			}
			if (current.indirect_image.has_value()) {
				const auto& a = *current.indirect_image;
				const auto& b = *descriptor.indirect_image;
				if (a.material_source != b.material_source || a.table_source != b.table_source ||
				    a.selector_stride != b.selector_stride || a.selector_offset != b.selector_offset ||
				    a.table_offset != b.table_offset ||
				    !EquivalentValue(m_program, a.key_count, b.key_count) ||
				    a.selector_mask.IsEmpty() != b.selector_mask.IsEmpty() ||
				    (!a.selector_mask.IsEmpty() &&
				     !EquivalentValue(m_program, a.selector_mask, b.selector_mask))) continue;
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
		const bool address = read.GetOpcode() == ValueOpcode::LoadAddressU32;
		if (!(address ? read.NumArgs() == 4u
		              : read.GetOpcode() == ValueOpcode::ReadConstBuffer && read.NumArgs() == 2u)) {
			return nullptr;
		}
		if (address) {
			const auto high = read.Arg(2).Resolve();
			const auto enabled = read.Arg(3).Resolve();
			if (!high.IsImmediate() || high.GetType() != Type::U32 || high.U32() != 0u ||
			    !enabled.IsImmediate() || enabled.GetType() != Type::U1 || !enabled.U1()) {
				return nullptr;
			}
		}
		index = read.Flags<MemoryFlags>().index;
		if (index >= m_program.memory_info.size()) {
			return nullptr;
		}
		const auto& memory = m_program.memory_info[index];
		return memory.kind == (address ? ResourceKind::ScalarAddress : ResourceKind::ScalarBuffer) &&
		               memory.data_bits == 32u && memory.data_dwords == 1u
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

	bool MakeRuntimeTableSource(const Inst& handle, uint32_t pc,
	                            DescriptorSource& descriptor) {
		const auto width = handle.GetOpcode() == ValueOpcode::GetBufferResource ? 4u
		                 : handle.GetOpcode() == ValueOpcode::GetAddressResource ? 2u : 0u;
		if (width == 0u) {
			return false;
		}
		MakeSource(handle, width, false, false, descriptor, pc);
		uint32_t bad_dword = 0;
		return ValidateSource(descriptor, bad_dword);
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

	bool NonzeroOnEntry(Value value, const Block* block) const {
		if (m_program.blocks.size() != m_program.block_info.size()) {
			return false;
		}
		// Each unique predecessor must execute before this use. Stop at joins: an
		// unrelated comparison is not a bound on FindILsb's zero-input sentinel.
		for (size_t depth = 0; block != nullptr && depth < m_program.blocks.size(); ++depth) {
			if (block->ImmPredecessors().size() != 1u) {
				return false;
			}
			const auto* previous = block->ImmPredecessors()[0];
			const auto current_it = std::ranges::find(m_program.blocks, block);
			const auto previous_it = std::ranges::find(m_program.blocks, previous);
			if (current_it == m_program.blocks.end() || previous_it == m_program.blocks.end()) {
				return false;
			}
			const auto& info = m_program.block_info[previous_it - m_program.blocks.begin()];
			const auto id = m_program.block_info[current_it - m_program.blocks.begin()].id;
			const auto& term = info.terminator;
			if (term.kind == CFG::TerminatorKind::ConditionalBranch &&
			    (term.true_block == id) != (term.false_block == id)) {
				bool positive = term.true_block == id;
				const auto* test = info.condition.Resolve().TryInstruction();
				while (test != nullptr && test->GetOpcode() == ValueOpcode::LogicalNot) {
					positive = !positive;
					test = test->Arg(0).Resolve().TryInstruction();
				}
				if (test != nullptr && test->NumArgs() == 2u &&
				    ((test->GetOpcode() == ValueOpcode::INotEqual32 && positive) ||
				     (test->GetOpcode() == ValueOpcode::IEqual32 && !positive))) {
					for (uint32_t arg = 0; arg < 2u; ++arg) {
						uint32_t immediate;
						if (ImmediateU32(test->Arg(arg), immediate) && immediate == 0u &&
						    EquivalentValue(m_program, test->Arg(arg ^ 1u), value)) {
							return true;
						}
					}
				}
			}
			block = previous;
		}
		return false;
	}

	bool MatchTableOffset(Value value, Value& key, uint32_t& offset) const {
		offset = 0;
		for (;;) {
			const auto* inst = value.Resolve().TryInstruction();
			if (inst == nullptr || inst->NumArgs() != 2u) {
				return false;
			}
			uint32_t immediate;
			if (inst->GetOpcode() == ValueOpcode::ShiftLeftLogical32 &&
			    ImmediateU32(inst->Arg(1), immediate) && immediate == 5u) {
				key = inst->Arg(0).Resolve();
				return key.GetType() == Type::U32;
			}
			if (inst->GetOpcode() != ValueOpcode::IAdd32) {
				return false;
			}
			if (ImmediateU32(inst->Arg(0), immediate)) {
				value = inst->Arg(1);
			} else if (ImmediateU32(inst->Arg(1), immediate)) {
				value = inst->Arg(0);
			} else {
				return false;
			}
			// These additions are shader U32 arithmetic, before the scalar memory offset.
			offset += immediate;
		}
	}

	const Block* FindBlock(uint32_t id) const {
		const auto info = std::ranges::find(m_program.block_info, id, &BlockInfo::id);
		return info == m_program.block_info.end()
		           ? nullptr
		           : m_program.blocks[info - m_program.block_info.begin()];
	}

	bool CanReach(const Block* start, const Block* target, const Block* avoid) const {
		std::vector<const Block*> pending {start};
		std::vector<const Block*> visited;
		while (!pending.empty()) {
			const auto* block = pending.back();
			pending.pop_back();
			if (block == nullptr || block == avoid ||
			    std::ranges::find(visited, block) != visited.end()) continue;
			if (block == target) return true;
			visited.push_back(block);
			for (const auto* next: block->ImmSuccessors()) pending.push_back(next);
		}
		return false;
	}

	bool ConditionalEdge(const Block* from, const Block* to, Value& condition,
	                     bool& positive) const {
		const auto position = std::ranges::find(m_program.blocks, from);
		const auto target = std::ranges::find(m_program.blocks, to);
		if (position == m_program.blocks.end() || target == m_program.blocks.end()) return false;
		const auto& info = m_program.block_info[position - m_program.blocks.begin()];
		const auto& term = info.terminator;
		const auto id = m_program.block_info[target - m_program.blocks.begin()].id;
		if (term.kind != CFG::TerminatorKind::ConditionalBranch ||
		    (term.true_block == id) == (term.false_block == id)) return false;
		condition = info.condition;
		positive = term.true_block == id;
		while (const auto* inst = condition.Resolve().TryInstruction()) {
			if (inst->GetOpcode() != ValueOpcode::LogicalNot) break;
			condition = inst->Arg(0);
			positive = !positive;
		}
		condition = condition.Resolve();
		return true;
	}

	Value PositiveUseGuard(const Block* use) const {
		if (use == nullptr || use->ImmPredecessors().size() != 1u) return {};
		Value condition;
		bool positive = false;
		return ConditionalEdge(use->ImmPredecessors()[0], use, condition, positive) && positive
		           ? condition : Value {};
	}

	Value SimplifyGuard(Value guard) const {
		const auto invariant = ResolveInvariantPhi(m_program, guard);
		return (invariant.IsEmpty() ? guard : invariant).Resolve();
	}

	bool Implies(Value guard, Value required) const {
		guard = SimplifyGuard(guard);
		required = required.Resolve();
		if (EquivalentValue(m_program, guard, required)) return true;
		const auto* inst = guard.TryInstruction();
		return inst != nullptr && inst->GetOpcode() == ValueOpcode::LogicalAnd &&
		       (Implies(inst->Arg(0), required) || Implies(inst->Arg(1), required));
	}

	Value EqualLocalKey(Value guard, Value key) const {
		guard = SimplifyGuard(guard);
		const auto* inst = guard.TryInstruction();
		if (inst == nullptr) return {};
		if (inst->GetOpcode() == ValueOpcode::LogicalAnd) {
			const auto left = EqualLocalKey(inst->Arg(0), key);
			return left.IsEmpty() ? EqualLocalKey(inst->Arg(1), key) : left;
		}
		if (inst->GetOpcode() != ValueOpcode::IEqual32 || inst->NumArgs() != 2u) return {};
		if (EquivalentValue(m_program, inst->Arg(0), key)) return inst->Arg(1).Resolve();
		if (EquivalentValue(m_program, inst->Arg(1), key)) return inst->Arg(0).Resolve();
		return {};
	}

	struct AffineOffset {
		Value    index;
		uint64_t stride = 0;
		uint64_t offset = 0;
	};

	bool MatchAffineOffset(Value value, Value guard, AffineOffset& out,
	                       uint32_t depth = 0) const {
		if (depth > 16u) return false;
		value = value.Resolve();
		uint32_t immediate = 0;
		if (ImmediateU32(value, immediate)) {
			out.offset = immediate;
			return true;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) return false;
		if (inst->GetOpcode() == ValueOpcode::SelectU32 && inst->NumArgs() == 3u &&
		    Implies(guard, inst->Arg(0))) {
			return MatchAffineOffset(inst->Arg(1), guard, out, depth + 1u);
		}
		if (inst->GetOpcode() == ValueOpcode::IAdd32 && inst->NumArgs() == 2u) {
			AffineOffset left, right;
			if (!MatchAffineOffset(inst->Arg(0), guard, left, depth + 1u) ||
			    !MatchAffineOffset(inst->Arg(1), guard, right, depth + 1u) ||
			    (!left.index.IsEmpty() && !right.index.IsEmpty() &&
			     !EquivalentValue(m_program, left.index, right.index))) return false;
			out.index = left.index.IsEmpty() ? right.index : left.index;
			out.stride = left.stride + right.stride;
			out.offset = left.offset + right.offset;
			return out.stride <= UINT32_MAX && out.offset <= UINT32_MAX;
		}
		if (inst->GetOpcode() == ValueOpcode::ShiftLeftLogical32 && inst->NumArgs() == 2u &&
		    ImmediateU32(inst->Arg(1), immediate) && immediate < 32u) {
			if (!MatchAffineOffset(inst->Arg(0), guard, out, depth + 1u)) return false;
			out.stride <<= immediate;
			out.offset <<= immediate;
			return out.stride <= UINT32_MAX && out.offset <= UINT32_MAX;
		}
		out.index = value;
		out.stride = 1u;
		return value.GetType() == Type::U32;
	}

	bool ImpliesNonzero(Value guard, Value value) const {
		guard = SimplifyGuard(guard);
		const auto* inst = guard.TryInstruction();
		if (inst == nullptr) return false;
		if (inst->GetOpcode() == ValueOpcode::LogicalAnd) {
			return ImpliesNonzero(inst->Arg(0), value) ||
			       ImpliesNonzero(inst->Arg(1), value);
		}
		if (inst->GetOpcode() != ValueOpcode::INotEqual32 || inst->NumArgs() != 2u)
			return false;
		uint32_t zero = 1u;
		return (ImmediateU32(inst->Arg(0), zero) && zero == 0u &&
		        EquivalentValue(m_program, inst->Arg(1), value)) ||
		       (ImmediateU32(inst->Arg(1), zero) && zero == 0u &&
		        EquivalentValue(m_program, inst->Arg(0), value));
	}

	bool ImpliesIndexBelow32(Value guard, Value index) const {
		guard = SimplifyGuard(guard);
		const auto* inst = guard.TryInstruction();
		if (inst == nullptr) return false;
		if (inst->GetOpcode() == ValueOpcode::LogicalAnd) {
			return ImpliesIndexBelow32(inst->Arg(0), index) ||
			       ImpliesIndexBelow32(inst->Arg(1), index);
		}
		if (inst->NumArgs() != 2u) return false;
		uint32_t limit = 0;
		return (inst->GetOpcode() == ValueOpcode::SLessThan32 &&
		        EquivalentValue(m_program, inst->Arg(0), index) &&
		        ImmediateU32(inst->Arg(1), limit) && limit == 32u) ||
		       (inst->GetOpcode() == ValueOpcode::SGreaterThan32 &&
		        ImmediateU32(inst->Arg(0), limit) && limit == 32u &&
		        EquivalentValue(m_program, inst->Arg(1), index));
	}

	bool ClearsFirstSetBit(Value value, Value mask) const {
		const auto* update = value.Resolve().TryInstruction();
		if (update == nullptr || update->GetOpcode() != ValueOpcode::BitwiseXor32 ||
		    update->NumArgs() != 2u) return false;
		Value bit;
		if (EquivalentValue(m_program, update->Arg(0), mask)) bit = update->Arg(1);
		else if (EquivalentValue(m_program, update->Arg(1), mask)) bit = update->Arg(0);
		else return false;
		const auto* shift = bit.Resolve().TryInstruction();
		uint32_t one = 0;
		if (shift == nullptr || shift->GetOpcode() != ValueOpcode::ShiftLeftLogical32 ||
		    shift->NumArgs() != 2u || !ImmediateU32(shift->Arg(0), one) || one != 1u)
			return false;
		Value position = shift->Arg(1).Resolve();
		const auto* masked = position.TryInstruction();
		uint32_t lane_mask = 0;
		if (masked != nullptr && masked->GetOpcode() == ValueOpcode::BitwiseAnd32 &&
		    masked->NumArgs() == 2u) {
			if (ImmediateU32(masked->Arg(0), lane_mask) && lane_mask == 31u)
				position = masked->Arg(1);
			else if (ImmediateU32(masked->Arg(1), lane_mask) && lane_mask == 31u)
				position = masked->Arg(0);
		}
		const auto* first = position.Resolve().TryInstruction();
		return first != nullptr && first->GetOpcode() == ValueOpcode::FindILsb32 &&
		       first->NumArgs() == 1u && EquivalentValue(m_program, first->Arg(0), mask);
	}

	Value InitialClearingMask(Value value, const Block* update_block) const {
		const auto* phi = value.Resolve().TryInstruction();
		if (phi == nullptr || phi->GetOpcode() != ValueOpcode::Phi ||
		    phi->NumArgs() != 2u || phi->GetType() != Type::U32) return {};
		for (uint32_t back = 0; back < 2u; ++back) {
			if (phi->PhiBlock(back) != update_block ||
			    !ClearsFirstSetBit(phi->Arg(back), value)) continue;
			const auto initial = phi->Arg(back ^ 1u).Resolve();
			return ValidateRuntimeValue(m_program, initial, RuntimeValueType::Integer)
			           ? initial : Value {};
		}
		return {};
	}

	bool EdgeOn(const Block* from, const Block* to, Value predicate, bool positive) const {
		Value condition;
		bool taken = false;
		return ConditionalEdge(from, to, condition, taken) && taken == positive &&
		       EquivalentValue(m_program, condition, predicate);
	}

	Value BoundedSetBitMask(Value index, Value guard) const {
		const auto* phi = index.Resolve().TryInstruction();
		if (phi == nullptr || phi->GetOpcode() != ValueOpcode::Phi ||
		    phi->NumArgs() != 3u || phi->GetType() != Type::U32 ||
		    !ImpliesIndexBelow32(guard, index)) return {};
		for (uint32_t bit_arm = 0; bit_arm < 3u; ++bit_arm) {
			const auto bit_guard = PositiveUseGuard(phi->PhiBlock(bit_arm));
			const auto* selected = phi->Arg(bit_arm).Resolve().TryInstruction();
			if (selected == nullptr || selected->GetOpcode() != ValueOpcode::SelectU32 ||
			    selected->NumArgs() != 3u ||
			    !Implies(bit_guard, selected->Arg(0))) continue;
			const auto* first = selected->Arg(1).Resolve().TryInstruction();
			if (first == nullptr || first->GetOpcode() != ValueOpcode::FindILsb32 ||
			    first->NumArgs() != 1u || !ImpliesNonzero(bit_guard, first->Arg(0)))
				continue;
			const auto initial_mask = InitialClearingMask(first->Arg(0), phi->PhiBlock(bit_arm));
			if (initial_mask.IsEmpty()) continue;
			for (uint32_t sentinel_arm = 0; sentinel_arm < 3u; ++sentinel_arm) {
				if (sentinel_arm == bit_arm) continue;
				const auto sentinel_guard = PositiveUseGuard(phi->PhiBlock(sentinel_arm));
				const auto* sentinel = phi->Arg(sentinel_arm).Resolve().TryInstruction();
				uint32_t bound = 0;
				if (sentinel == nullptr || sentinel->GetOpcode() != ValueOpcode::SelectU32 ||
				    sentinel->NumArgs() != 3u ||
				    !Implies(sentinel_guard, sentinel->Arg(0)) ||
				    !ImmediateU32(sentinel->Arg(1), bound) || bound != 32u) continue;
				const auto loop_active = sentinel->Arg(0).Resolve();
				const auto* active_phi = loop_active.TryInstruction();
				if (active_phi == nullptr || active_phi->GetOpcode() != ValueOpcode::Phi ||
				    active_phi->NumArgs() != 2u) continue;
				bool invariant = false;
				for (uint32_t initial = 0; initial < 2u; ++initial) {
					invariant = Implies(guard, active_phi->Arg(initial)) &&
					            EdgeOn(active_phi->PhiBlock(initial ^ 1u), active_phi->Parent(),
					                   active_phi->Arg(initial ^ 1u), true);
					if (invariant) break;
				}
				if (!invariant) continue;
				const auto other_arm = 3u - bit_arm - sentinel_arm;
				if (EdgeOn(phi->PhiBlock(other_arm), phi->Parent(), loop_active, false))
					return initial_mask;
			}
		}
		return {};
	}

	bool MatchUniformizedMaterialKey(Value key, const Inst& image,
	                                DescriptorSource::IndirectImage& indirect,
	                                DescriptorSource& material_source, uint32_t pc) {
		const auto guard = PositiveUseGuard(image.Parent());
		if (guard.IsEmpty()) return false;
		const auto local = EqualLocalKey(guard, key);
		const auto* selected = local.Resolve().TryInstruction();
		if (selected == nullptr || selected->GetOpcode() != ValueOpcode::SelectU32 ||
		    selected->NumArgs() != 3u || !Implies(guard, selected->Arg(0))) return false;
		const auto active = selected->Arg(0).Resolve();
		const auto* read = selected->Arg(1).Resolve().TryInstruction();
		if (read == nullptr || read->GetOpcode() != ValueOpcode::LoadAddressU32 ||
		    read->NumArgs() != 4u || !EquivalentValue(m_program, read->Arg(3), active))
			return false;
		uint32_t high = 1u;
		if (!ImmediateU32(read->Arg(2), high) || high != 0u) return false;
		const auto memory_index = read->Flags<MemoryFlags>().index;
		if (memory_index >= m_program.memory_info.size()) return false;
		const auto& memory = m_program.memory_info[memory_index];
		if (memory.kind != ResourceKind::Global || memory.data_bits != 32u ||
		    memory.data_dwords != 1u || !MemoryIndexBelongsTo(memory_index, *read)) return false;
		const auto* material_handle = read->Arg(0).Resolve().TryInstruction();
		if (material_handle == nullptr ||
		    material_handle->GetOpcode() != ValueOpcode::GetAddressResource ||
		    !MakeRuntimeTableSource(*material_handle, pc, material_source)) return false;
		AffineOffset offset;
		if (!MatchAffineOffset(read->Arg(1), active, offset) || offset.index.IsEmpty() ||
		    offset.stride == 0u || offset.offset + memory.offset > UINT32_MAX ||
		    offset.offset + memory.offset + 31u * offset.stride + 4u >
		        static_cast<uint64_t>(UINT32_MAX) + 1u) return false;
		const auto mask = BoundedSetBitMask(offset.index, active);
		if (mask.IsEmpty()) return false;
		indirect.material_source = InternSource(material_source);
		indirect.selector_stride = static_cast<uint32_t>(offset.stride);
		indirect.selector_offset = static_cast<uint32_t>(offset.offset + memory.offset);
		indirect.key_count = Value(32u);
		indirect.selector_mask = mask;
		return true;
	}

	Value BoundedLoopCount(Value key, const Block* use) const {
		const auto* phi = key.Resolve().TryInstruction();
		if (m_shader_writes || phi == nullptr || phi->GetOpcode() != ValueOpcode::Phi ||
		    phi->GetType() != Type::U32 || phi->NumArgs() != 2u ||
		    m_program.blocks.size() != m_program.block_info.size()) {
			return {};
		}
		bool induction = false;
		for (uint32_t initial = 0; initial < 2u; ++initial) {
			const auto zero = phi->Arg(initial).Resolve();
			const auto* step = phi->Arg(initial ^ 1u).Resolve().TryInstruction();
			if (!zero.IsImmediate() || zero.GetType() != Type::U32 || zero.U32() != 0u ||
			    step == nullptr || step->GetOpcode() != ValueOpcode::IAdd32 ||
			    step->Parent() != phi->PhiBlock(initial ^ 1u)) {
				continue;
			}
			uint32_t increment = 0;
			induction = (step->Arg(0).Resolve() == key &&
			             ImmediateU32(step->Arg(1), increment) && increment == 1u) ||
			            (step->Arg(1).Resolve() == key &&
			             ImmediateU32(step->Arg(0), increment) && increment == 1u);
			if (induction) break;
		}
		if (!induction) return {};

		const auto contains = [&](auto&& self, Value value, const Inst* comparison) -> bool {
			value = value.Resolve();
			if (value.TryInstruction() == comparison) return true;
			const auto* inst = value.TryInstruction();
			return inst != nullptr && inst->GetOpcode() == ValueOpcode::LogicalAnd &&
			       (self(self, inst->Arg(0), comparison) || self(self, inst->Arg(1), comparison));
		};
		for (const auto& use_of_key: phi->Uses()) {
			const auto* compare = use_of_key.user;
			if (compare->GetOpcode() != ValueOpcode::SLessThan32 || use_of_key.operand != 0u ||
			    !ValidateRuntimeValue(m_program, compare->Arg(1))) continue;
			for (uint32_t i = 0; i < m_program.block_info.size(); ++i) {
				const auto& info = m_program.block_info[i];
				const auto* negated = info.condition.Resolve().TryInstruction();
				if (info.terminator.kind != CFG::TerminatorKind::ConditionalBranch ||
				    compare->Parent() != m_program.blocks[i] || negated == nullptr ||
				    negated->GetOpcode() != ValueOpcode::LogicalNot ||
				    !contains(contains, negated->Arg(0), compare) ||
				    use == m_program.blocks[i] ||
				    CanReach(m_program.blocks.front(), use, m_program.blocks[i]) ||
				    CanReach(phi->Parent(), use, m_program.blocks[i]) ||
				    CanReach(FindBlock(info.terminator.true_block), use, phi->Parent()) ||
				    !CanReach(FindBlock(info.terminator.false_block), use, phi->Parent())) {
					continue;
				}
				return compare->Arg(1);
			}
		}
		return {};
	}

	bool TryMakeIndirectImage(Inst& handle, uint32_t pc, IndirectImagePlan& plan) {
		if (handle.GetOpcode() != ValueOpcode::GetImageResource || handle.NumArgs() != 8u) {
			return false;
		}
		Inst* table_handle = nullptr;
		Value key;
		uint32_t table_offset = 0;
		for (uint32_t dword = 0; dword < plan.reads.size(); ++dword) {
			auto* read = handle.Arg(dword).Resolve().TryInstruction();
			if (read == nullptr) {
				return false;
			}
			uint32_t memory_index = 0;
			const auto* memory = ScalarReadMemory(*read, memory_index);
			if (memory == nullptr || memory->offset > INT32_MAX || (memory->offset & 3u) != 0u ||
			    !MemoryIndexBelongsTo(memory_index, *read)) {
				return false;
			}
			auto* current_handle = read->Arg(0).Resolve().TryInstruction();
			Value current_key;
			uint32_t offset = 0;
			if (current_handle == nullptr ||
			    current_handle->GetOpcode() != (memory->kind == ResourceKind::ScalarAddress
			                                      ? ValueOpcode::GetAddressResource
			                                      : ValueOpcode::GetBufferResource) ||
			    (memory->kind == ResourceKind::ScalarAddress && read->Parent() != handle.Parent()) ||
			    (table_handle != nullptr &&
			     !EquivalentValue(m_program, Value(table_handle), Value(current_handle))) ||
			    !MatchTableOffset(read->Arg(1), current_key, offset) ||
			    memory->offset > UINT32_MAX - offset) {
				return false;
			}
			offset += memory->offset;
			if (dword == 0u) {
				key = current_key;
				table_offset = offset;
			} else if (!EquivalentValue(m_program, key, current_key) ||
			           static_cast<uint64_t>(table_offset) + dword * sizeof(uint32_t) != offset) {
				return false;
			}
			table_handle = current_handle;
			const std::array<const Inst*, 1> image_users {&handle};
			if (!UsesOnly(*read, image_users)) {
				return false;
			}
			plan.memory[dword] = memory_index;
			plan.reads[dword] = read;
		}

		DescriptorSource table_source;
		if (!MakeRuntimeTableSource(*table_handle, pc, table_source)) {
			return false;
		}
		DescriptorSource material_source;
		DescriptorSource::IndirectImage indirect;
		indirect.table_offset = table_offset;
		if (table_source.dword_count == 2u) {
			const auto* selector = key.Resolve().TryInstruction();
			const bool bitscan = selector != nullptr && selector->GetOpcode() == ValueOpcode::FindILsb32 &&
			    selector->NumArgs() == 1u && !m_shader_writes &&
			    NonzeroOnEntry(selector->Arg(0), handle.Parent());
			if (bitscan) {
				indirect.key_count = Value(32u);
			} else {
				indirect.key_count = BoundedLoopCount(key, handle.Parent());
			}
			if (indirect.key_count.IsEmpty() &&
			    !MatchUniformizedMaterialKey(key, handle, indirect, material_source, pc)) {
				return false;
			}
			if ((table_offset & 3u) != 0u ||
			    (bitscan && table_offset > UINT32_MAX - (32u * 32u - 1u))) return false;
		} else {
			auto* material_read = key.Resolve().TryInstruction();
			uint32_t material_memory_index = 0;
			const auto* memory = material_read != nullptr
			                         ? ScalarReadMemory(*material_read, material_memory_index) : nullptr;
			if (table_offset != 0u || memory == nullptr || memory->kind != ResourceKind::ScalarBuffer ||
			    memory->offset != 0u || !MemoryIndexBelongsTo(material_memory_index, *material_read)) {
				return false;
			}
			Value selector;
			if (!MatchMaterialOffset(material_read->Arg(1), selector, indirect.selector_stride,
			                         indirect.selector_offset)) {
				return false;
			}
			const auto* shift = plan.reads[0]->Arg(1).Resolve().TryInstruction();
			const std::array<const Inst*, 1> material_users {shift};
			if (!UsesOnly(*material_read, material_users) || !UsesOnly(*shift, plan.reads)) {
				return false;
			}
			const auto* material_handle = material_read->Arg(0).Resolve().TryInstruction();
			if (material_handle == nullptr ||
			    material_handle->GetOpcode() != ValueOpcode::GetBufferResource ||
			    !MakeRuntimeTableSource(*material_handle, pc, material_source)) {
				return false;
			}
			indirect.material_source = InternSource(material_source);
		}
		indirect.table_source = InternSource(table_source);
		DescriptorSource image_source;
		image_source.dword_count = 8u;
		image_source.dwords.fill(Value(0u));
		std::copy_n(material_source.dwords.begin(), material_source.dword_count,
		            image_source.dwords.begin());
		std::copy_n(table_source.dwords.begin(), table_source.dword_count,
		            image_source.dwords.begin() + 4u);
		image_source.indirect_image = indirect;
		plan.handle = &handle;
		plan.source = InternSource(image_source);
		plan.key = key;
		plan.roots = image_source.dwords;
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

	bool GetHandle(Value value, ValueOpcode expected, uint32_t width, uint32_t pc, Inst*& handle,
	               uint32_t& source, bool sampler = false, bool sample_adjust = false) {
		handle = value.Resolve().TryInstruction();
		if (handle == nullptr || handle->GetOpcode() != expected) {
			Fail(pc, fmt::format("memory operation requires {}", ValueOpcodeName(expected)));
		}
		if (expected == ValueOpcode::GetBufferResource) {
			for (uint32_t dword = 0; dword < handle->NumArgs(); ++dword) {
				handle->SetArg(dword, LowerRuntimeDescriptorPhi(handle->Arg(dword), *handle));
			}
		}
		std::string bounded_rejection;
		if (expected == ValueOpcode::GetBufferResource &&
		    MakeBoundedBufferSource(*handle, source, bounded_rejection)) return;
		bool has_bounded_column = false;
		if (expected == ValueOpcode::GetBufferResource) {
			for (uint32_t word = 0; word < handle->NumArgs(); ++word) {
				has_bounded_column |= BoundedReadValue(handle->Arg(word)) != nullptr;
			}
		}
		if (has_bounded_column) {
			static const bool trace = [] {
				const char* value = std::getenv("KYTY_SHADER_PHASE_TRACE");
				return value != nullptr && *value != '\0' && std::string_view(value) != "0";
			}();
			if (trace) {
				std::fprintf(stderr,
				             "shader resource tracking: hash=0x%016" PRIx64
				             " pc=0x%08" PRIx32 " bounded buffer recognition rejected: %s\n",
				             m_program.shader_hash, pc, bounded_rejection.c_str());
			}
		}
		if (expected == ValueOpcode::GetImageResource && MakeBoundedImageSource(*handle, source)) return;
		DescriptorSource descriptor;
		MakeSource(*handle, width, sampler, sample_adjust, descriptor, pc);
		if (expected == ValueOpcode::GetBufferResource &&
		    MakeBoundedBufferExpression(*handle, descriptor, source, bounded_rejection)) return;
		if (expected == ValueOpcode::GetImageResource &&
		    MakeBoundedImageExpression(*handle, descriptor, source, bounded_rejection)) return;
		if (expected == ValueOpcode::GetSamplerResource &&
		    MakeBoundedSamplerExpression(*handle, descriptor, source, bounded_rejection)) return;
		uint32_t bad_dword = 0;
		if (!ValidateSource(descriptor, bad_dword)) {
			if (expected == ValueOpcode::GetBufferResource &&
			    std::all_of(descriptor.dwords.begin(), descriptor.dwords.begin() + width,
			                [](Value word) { return word.Resolve().GetType() == Type::U32; })) {
				return false;
			}
			Fail(pc, fmt::format("{} dword {} is not a valid runtime value",
			                     ValueOpcodeName(expected), bad_dword) +
			             (definition != nullptr
			                  ? fmt::format(" (root={})", ValueOpcodeName(definition->GetOpcode()))
			                  : fmt::format(" (type={})", TypeName(value.GetType()))));
		}
		source = InternSource(descriptor);
		return true;
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
			if (!GetHandle(inst.Arg(0), ValueOpcode::GetBufferResource, 4, flags.pc, handle,
			               source)) {
				if (memory.kind != ResourceKind::Buffer || !memory.SupportsIndirectBufferLoad(op)) {
					Fail(flags.pc,
					     "buffer descriptor is not a valid runtime value; GPU-selected access "
					     "requires a raw DWORD x2/x3/x4 load");
				}
				m_program.memory_info[flags.index].kind = ResourceKind::IndirectBuffer;
				m_info.uses_dma                         = true;
				return;
			}
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
			if (address_info.access == AddressAccess::Write) {
				m_program.has_address_writes = true;
			}
			m_info.uses_dma = true;
			m_info.writes_dma |= address_info.access == AddressAccess::Write;
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
				    image_source->indirect_image.has_value() || image_source->inline_descriptor.has_value() ||
				    image_source->bounded_image.has_value()) {
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

	Program&                                   m_program;
	ShaderInfo                                 m_info;
	std::vector<DescriptorSource>              m_sources;
	std::vector<HandlePatch>                   m_handle_patches;
	std::vector<MemoryPatch>                   m_memory_patches;
	std::vector<IndirectImagePlan>             m_indirect_images;
	std::vector<std::pair<const Inst*, Value>> m_descriptor_selections;
	bool                                       m_shader_writes = false;
};

} // namespace

void TrackResources(Program& program) {
	Tracker(program).Run();
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
