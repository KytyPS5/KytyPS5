#include "graphics/shader/recompiler/ir/passes/ResourceMaterializationMemo.h"

#include <algorithm>
#include <bit>
#include <memory>

namespace Libs::Graphics::ShaderRecompiler::IR {

namespace {

// Past this a stage is an enumeration, not a descriptor walk.
constexpr size_t MaxRecordedReads = 4096;

bool IsRawReadOpcode(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::LoadAddressU32:
		case ValueOpcode::ReadConstBuffer:
		case ValueOpcode::LoadBufferU32:
		case ValueOpcode::LoadBufferU32x2:
		case ValueOpcode::LoadBufferU32x3:
		case ValueOpcode::LoadBufferU32x4: return true;
		default: return false;
	}
}

bool IsBufferRead(ValueOpcode op) {
	return op != ValueOpcode::LoadAddressU32;
}

// 32-bit integer operations the walker evaluates on their low words alone.
bool IsInteger32(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::ISub32:
		case ValueOpcode::IMul32:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftRightLogical32: return true;
		default: return false;
	}
}

// The carry of a 64-bit pointer add: ULessThan32 (LogicalOr on carry-in) through SelectU32.
bool IsCarryOp(ValueOpcode op) {
	return op == ValueOpcode::ULessThan32 || op == ValueOpcode::LogicalOr ||
	       op == ValueOpcode::SelectU32;
}

bool ImmediateValue(Value value, uint64_t& result) {
	switch (value.GetType()) {
		case Type::U1: result = value.U1(); return true;
		case Type::U8: result = value.U8(); return true;
		case Type::U16: result = value.U16(); return true;
		case Type::U32: result = value.U32(); return true;
		case Type::U64: result = value.U64(); return true;
		case Type::F32: result = std::bit_cast<uint32_t>(value.F32Value()); return true;
		default: return false;
	}
}

// The flat SRT slot a ReadConst names, or UINT32_MAX.
uint32_t SrtSlot(const ResourcePlan& plan, const Inst& inst) {
	if (inst.GetOpcode() != ValueOpcode::ReadConst || inst.NumArgs() != 2) {
		return UINT32_MAX;
	}
	const auto slot = inst.Arg(1).Resolve();
	return slot.IsImmediate() && slot.GetType() == Type::U32 && slot.U32() < plan.srt_reads.size()
	           ? slot.U32()
	           : UINT32_MAX;
}

uint64_t KeyBits(const Inst* inst, uint32_t component_bytes) {
	return (reinterpret_cast<uint64_t>(inst) << 2u) | (component_bytes / sizeof(uint32_t));
}

// The read a value is the word of: a raw read itself, or one dword extracted from a wide one.
bool ValueReadKey(Value value, uint64_t& key) {
	const auto* inst = value.Resolve().TryInstruction();
	if (inst == nullptr) {
		return false;
	}
	const auto op = inst->GetOpcode();
	if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer ||
	    op == ValueOpcode::LoadBufferU32) {
		key = KeyBits(inst, 0);
		return true;
	}
	if ((op == ValueOpcode::CompositeExtractU32x2 || op == ValueOpcode::CompositeExtractU32x4) &&
	    inst->NumArgs() == 2) {
		const auto* source = inst->Arg(0).ResolveInstruction();
		const auto  index  = inst->Arg(1).Resolve();
		if (source != nullptr && index.IsImmediate() && index.GetType() == Type::U32 &&
		    (source->GetOpcode() == ValueOpcode::LoadBufferU32x2 ||
		     source->GetOpcode() == ValueOpcode::LoadBufferU32x4)) {
			key = KeyBits(source, index.U32() * sizeof(uint32_t));
			return true;
		}
	}
	return false;
}

} // namespace

// What a plan's instructions are used for, derived once per cached plan.
struct MaterializeMemo::Analysis {
	enum : uint8_t { Unknown = 0, Visiting = 1, Yes = 2, No = 3 };
	struct Use {
		const Inst* user = nullptr; // nullptr: a descriptor, condition or fill value
		uint32_t    arg  = 0;
		bool        slot = false;
		// The descriptor source this external use is a dword of, or UINT32_MAX.
		uint32_t source = UINT32_MAX;
	};

	const ResourcePlan&                                    plan;
	std::unordered_map<const Inst*, std::vector<Use>>      uses;
	std::unordered_map<const Inst*, uint8_t>               address_only;
	std::unordered_map<uint64_t, std::vector<uint32_t>>    slots;
	std::unordered_map<uint64_t, std::vector<const Inst*>> nodes;
	std::vector<uint8_t>                                   changeable;
	// Sources whose words a replay may patch: plain resource descriptors only.
	std::vector<uint8_t> patchable;

	explicit Analysis(const ResourcePlan& p): plan(p) {
		const auto external = [&](Value value, bool slot) {
			if (const auto* inst = value.Resolve().TryInstruction(); inst != nullptr) {
				uses[inst].push_back({nullptr, 0, slot});
			}
		};
		for (const auto& inst: plan.value_storage) {
			if (inst.GetOpcode() == ValueOpcode::Identity) {
				continue;
			}
			for (size_t arg = 0; arg < inst.NumArgs(); arg++) {
				if (const auto* target = inst.Arg(arg).Resolve().TryInstruction();
				    target != nullptr) {
					uses[target].push_back({&inst, static_cast<uint32_t>(arg), false});
				}
			}
			// A flat SRT read evaluates its slot's value: a use of that value, not of an argument.
			if (const auto slot = SrtSlot(plan, inst); slot != UINT32_MAX) {
				if (const auto* target = plan.srt_reads[slot].value.Resolve().TryInstruction();
				    target != nullptr) {
					uses[target].push_back({&inst, UINT32_MAX, false});
				}
			}
			uint64_t key = 0;
			if (ValueReadKey(Value(const_cast<Inst*>(&inst)), key)) {
				nodes[key].push_back(&inst);
			}
		}
		for (uint32_t s = 0; s < plan.descriptor_sources.size(); s++) {
			const auto& source = plan.descriptor_sources[s];
			for (uint32_t i = 0; i < source.dword_count; i++) {
				if (const auto* inst = source.dwords[i].Resolve().TryInstruction();
				    inst != nullptr) {
					uses[inst].push_back({nullptr, i, false, s});
				}
			}
			if (source.indirect_descriptor.has_value()) {
				const auto& indirect = *source.indirect_descriptor;
				external(indirect.key_count, false);
				external(indirect.selector_first, false);
				external(indirect.selector_mask, false);
			}
		}
		for (const auto& block: plan.control_flow) {
			external(block.condition, false);
		}
		for (const auto& value: plan.uniform_fill.values) {
			external(value, false);
		}
		external(plan.uniform_fill.pattern_size, false);
		external(plan.uniform_fill.element_count, false);
		for (const auto& read: plan.srt_reads) {
			external(read.value, true);
			uint64_t key = 0;
			if (ValueReadKey(read.value, key)) {
				slots[key].push_back(read.flat_offset);
			}
		}
		ClassifyPatchable();
		changeable.assign(plan.user_data_count, 1u);
		for (const auto& inst: plan.value_storage) {
			if (inst.GetOpcode() != ValueOpcode::GetUserData) {
				continue;
			}
			const auto reg = RegIndex(inst.Arg(0).ScalarRegister());
			if (reg >= plan.user_data_base && reg - plan.user_data_base < changeable.size() &&
			    !AddressOnly(&inst, false)) {
				changeable[reg - plan.user_data_base] = 0u;
			}
		}
	}

	// Every use only steers an address (or, with `slots_ok`, also fills a flat SRT slot).
	bool AddressOnly(const Inst* node, bool slots_ok) {
		const auto found = uses.find(node);
		if (found == uses.end()) {
			return true;
		}
		if (!slots_ok) {
			auto& state = address_only[node];
			if (state == Yes || state == No) {
				return state == Yes;
			}
			if (state == Visiting) {
				return false;
			}
			state              = Visiting;
			const bool value   = Check(found->second, false);
			address_only[node] = value ? Yes : No;
			return value;
		}
		return Check(found->second, true);
	}

	bool Check(const std::vector<Use>& list, bool slots_ok) {
		return std::ranges::all_of(list, [&](const Use& use) {
			if (use.user == nullptr) {
				return (slots_ok && use.slot) ||
				       (use.source < patchable.size() && patchable[use.source] != 0u);
			}
			const auto op = use.user->GetOpcode();
			if (op == ValueOpcode::ReadConst) {
				return use.arg == UINT32_MAX && AddressOnly(use.user, false);
			}
			if (IsRawReadOpcode(op)) {
				return use.arg == RawReadOffsetArgument(op);
			}
			if (op == ValueOpcode::GetAddressResource || op == ValueOpcode::GetBufferResource) {
				return use.arg < 4u && HandleOnlyAddresses(use.user);
			}
			if (op == ValueOpcode::IAdd32 || op == ValueOpcode::CompositeConstructU32x2 ||
			    (op == ValueOpcode::CompositeExtractU32x2 && use.arg == 0u) || IsInteger32(op) ||
			    IsCarryOp(op)) {
				return AddressOnly(use.user, false);
			}
			// x == x and x != x hold whatever x reads, so the word steers nothing.
			if ((op == ValueOpcode::IEqual32 || op == ValueOpcode::INotEqual32) &&
			    use.user->NumArgs() == 2 &&
			    use.user->Arg(0).Resolve() == use.user->Arg(1).Resolve()) {
				return true;
			}
			return false;
		});
	}

	bool HandleOnlyAddresses(const Inst* handle) {
		const auto found = uses.find(handle);
		return found == uses.end() || std::ranges::all_of(found->second, [](const Use& use) {
			       return use.user != nullptr && use.arg == 0u &&
			              IsRawReadOpcode(use.user->GetOpcode());
		       });
	}

	// A value the replay's postfix programs can rebuild from user data and re-read words alone.
	bool Derivable(Value value, uint32_t depth = 0) const {
		value = value.Resolve();
		if (value.IsImmediate()) {
			uint64_t ignored = 0;
			return ImmediateValue(value, ignored);
		}
		uint64_t key = 0;
		if (ValueReadKey(value, key)) {
			return true;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || depth > 32) {
			return false;
		}
		switch (inst->GetOpcode()) {
			case ValueOpcode::GetUserData: {
				const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
				return reg >= plan.user_data_base &&
				       reg - plan.user_data_base < plan.user_data_count;
			}
			case ValueOpcode::ReadConst: {
				const auto slot = SrtSlot(plan, *inst);
				return slot != UINT32_MAX && Derivable(plan.srt_reads[slot].value, depth + 1);
			}
			case ValueOpcode::CompositeExtractU32x2: {
				const auto* source = inst->Arg(0).ResolveInstruction();
				const auto  index  = inst->Arg(1).Resolve();
				return source != nullptr && index.IsImmediate() && index.GetType() == Type::U32 &&
				       index.U32() < 2u &&
				       source->GetOpcode() == ValueOpcode::CompositeConstructU32x2 &&
				       Derivable(source->Arg(index.U32()), depth + 1);
			}
			case ValueOpcode::SelectU32:
				return Derivable(inst->Arg(0), depth + 1) && Derivable(inst->Arg(1), depth + 1) &&
				       Derivable(inst->Arg(2), depth + 1);
			default: {
				const auto op = inst->GetOpcode();
				return (op == ValueOpcode::IAdd32 || IsInteger32(op) ||
				        op == ValueOpcode::ULessThan32 || op == ValueOpcode::LogicalOr) &&
				       inst->NumArgs() == 2 && Derivable(inst->Arg(0), depth + 1) &&
				       Derivable(inst->Arg(1), depth + 1);
			}
		}
	}

	// A source qualifies when only plain buffers, images and samplers evaluate it, never a table.
	void ClassifyPatchable() {
		const auto count = plan.descriptor_sources.size();
		patchable.assign(count, 0u);
		std::vector<uint8_t> referenced(count, 0u);
		std::vector<uint8_t> refused(count, 0u);
		const auto           refuse = [&](uint32_t source) {
			if (source < count) {
				refused[source] = 1u;
			}
		};
		const auto reference = [&](uint32_t source) {
			if (source < count) {
				referenced[source] = 1u;
			}
		};
		for (uint32_t s = 0; s < count; s++) {
			const auto& source = plan.descriptor_sources[s];
			if (!source.indirect_descriptor.has_value()) {
				continue;
			}
			const auto& indirect = *source.indirect_descriptor;
			refuse(s);
			refuse(indirect.table_source);
			if (indirect.selector.has_value()) {
				refuse(indirect.selector->source);
				if (indirect.selector->rows.has_value()) {
					refuse(indirect.selector->rows->source);
				}
			}
			for (const auto other: indirect.sources) {
				refuse(other);
			}
		}
		for (const auto& buffer: plan.info.buffers) {
			reference(buffer.source);
		}
		for (const auto& image: plan.info.images) {
			reference(image.source);
		}
		for (const auto& sampler: plan.info.samplers) {
			reference(sampler.source);
		}
		for (uint32_t s = 0; s < count; s++) {
			const auto& source = plan.descriptor_sources[s];
			if (referenced[s] == 0u || refused[s] != 0u || source.dword_count == 0u ||
			    source.dword_count > 8u) {
				continue;
			}
			bool derivable = true;
			for (uint32_t d = 0; d < source.dword_count && derivable; d++) {
				derivable = Derivable(source.dwords[d]);
			}
			patchable[s] = derivable ? 1u : 0u;
		}
	}

	// The words of this read reach nothing but addresses and flat SRT slots.
	bool Replaceable(uint64_t key) {
		const auto found = nodes.find(key);
		return found != nodes.end() && std::ranges::all_of(found->second, [&](const Inst* node) {
			       return AddressOnly(node, true);
		       });
	}
};

MaterializeMemo::MaterializeMemo()                                      = default;
MaterializeMemo::~MaterializeMemo()                                     = default;
MaterializeMemo::MaterializeMemo(MaterializeMemo&&) noexcept            = default;
MaterializeMemo& MaterializeMemo::operator=(MaterializeMemo&&) noexcept = default;

bool MaterializeMemo::Supports(const ResourcePlan& plan) {
	// A SWAPPC program's captured reads start with its linked code, which the walk does not read.
	if (!plan.source_reads.empty()) {
		return false;
	}
	// Tables read outside a raw read; only image heap roots, which Commit confirms, replay. The
	// dispatch size a workgroup-indexed table reads is not replayed.
	for (uint32_t source = 0; source < plan.descriptor_sources.size(); source++) {
		const auto& indirect = plan.descriptor_sources[source].indirect_descriptor;
		if (indirect.has_value() &&
		    (indirect->workgroup_axis != UINT32_MAX ||
		     std::ranges::none_of(plan.info.images, [&](const ImageResource& image) {
			     return image.source == source;
		     }))) {
			return false;
		}
	}
	const auto indirect = [&](uint32_t source) {
		return source < plan.descriptor_sources.size() &&
		       plan.descriptor_sources[source].indirect_descriptor.has_value();
	};
	return std::ranges::none_of(
	           plan.info.buffers,
	           [&](const BufferResource& buffer) { return indirect(buffer.source); }) &&
	       std::ranges::none_of(plan.info.samplers, [&](const SamplerResource& sampler) {
		       return indirect(sampler.source) ||
		              sampler.indirect_root != SamplerResource::NoIndirectSampler;
	       });
}

SrtRuntime MaterializeMemo::Record(const SrtRuntime& runtime) {
	m_source = runtime;
	m_reads.clear();
	m_restarted            = false;
	m_heap_recorder.source = runtime.image_heap;
	m_heap_recorder.acquired.clear();
	// Without both readers the walk reads or refuses through paths the memo cannot see.
	m_replayable = runtime.read_memory != nullptr && runtime.read_specialization_memory != nullptr;
	auto wrapped = runtime;
	wrapped.userdata    = this;
	wrapped.read_memory = runtime.read_memory != nullptr ? ReadMemory : nullptr;
	wrapped.read_specialization_memory =
	    runtime.read_specialization_memory != nullptr ? ReadSpecialization : nullptr;
	wrapped.read_condition_memory =
	    runtime.read_condition_memory != nullptr ? ReadCondition : nullptr;
	wrapped.readable_extent = runtime.readable_extent != nullptr ? ReadableExtent : nullptr;
	wrapped.read_specialization_block =
	    runtime.read_specialization_block != nullptr ? ReadBlock : nullptr;
	wrapped.hash_specialization_block =
	    runtime.hash_specialization_block != nullptr ? HashBlock : nullptr;
	wrapped.gpu_owned      = runtime.gpu_owned != nullptr ? GpuOwned : nullptr;
	wrapped.walk_restarted = WalkRestarted;
	wrapped.zero_read      = ZeroRead;
	wrapped.image_heap     = runtime.image_heap != nullptr ? &m_heap_recorder : nullptr;
	return wrapped;
}

bool MaterializeMemo::HeapRecorder::Acquire(const ImageHeapRequest& request, ImageHeapView& view) {
	if (!source->Acquire(request, view)) {
		return false;
	}
	acquired.push_back({.request    = request,
	                    .kinds      = {view.kinds.begin(), view.kinds.end()},
	                    .slots      = view.slots,
	                    .slot_count = view.slot_count});
	return true;
}

bool MaterializeMemo::Note(Reader reader, bool read, uint64_t address,
                           std::span<const uint32_t> values) {
	// A read outside a raw-read evaluation has no recipe.
	const auto* event = CurrentRawReadEvent();
	if (event == nullptr || event->zero || values.size() != 1u ||
	    m_reads.size() >= MaxRecordedReads) {
		m_replayable = false;
		return read;
	}
	m_reads.push_back({reader, event->component_bytes, event->inst, address, read ? values[0] : 0u,
	                   event->operands, CurrentSrtReadSlot(), !read});
	return read;
}

bool MaterializeMemo::ReadMemory(void* userdata, uint64_t address, std::span<uint32_t> values) {
	auto& memo = *static_cast<MaterializeMemo*>(userdata);
	return memo.Note(Reader::Memory,
	                 memo.m_source.read_memory(memo.m_source.userdata, address, values), address,
	                 values);
}

bool MaterializeMemo::ReadSpecialization(void* userdata, uint64_t address,
                                         std::span<uint32_t> values) {
	auto& memo = *static_cast<MaterializeMemo*>(userdata);
	return memo.Note(
	    Reader::Specialization,
	    memo.m_source.read_specialization_memory(memo.m_source.userdata, address, values), address,
	    values);
}

bool MaterializeMemo::ReadCondition(void* userdata, uint64_t address, std::span<uint32_t> values) {
	auto& memo = *static_cast<MaterializeMemo*>(userdata);
	return memo.Note(Reader::Condition,
	                 memo.m_source.read_condition_memory(memo.m_source.userdata, address, values),
	                 address, values);
}

uint64_t MaterializeMemo::ReadableExtent(void* userdata, uint64_t address, uint64_t size) {
	auto& memo        = *static_cast<MaterializeMemo*>(userdata);
	memo.m_replayable = false;
	return memo.m_source.readable_extent(memo.m_source.userdata, address, size);
}

bool MaterializeMemo::ReadBlock(void* userdata, uint64_t address, void* data, uint64_t size) {
	auto& memo        = *static_cast<MaterializeMemo*>(userdata);
	memo.m_replayable = false;
	return memo.m_source.read_specialization_block(memo.m_source.userdata, address, data, size);
}

bool MaterializeMemo::HashBlock(void* userdata, uint64_t address, uint64_t size, uint64_t* hash) {
	auto& memo        = *static_cast<MaterializeMemo*>(userdata);
	memo.m_replayable = false;
	return memo.m_source.hash_specialization_block(memo.m_source.userdata, address, size, hash);
}

bool MaterializeMemo::GpuOwned(void* userdata, uint64_t address, uint64_t size) {
	auto& memo = *static_cast<MaterializeMemo*>(userdata);
	return memo.m_source.gpu_owned(memo.m_source.userdata, address, size);
}

void MaterializeMemo::WalkRestarted(void* userdata) {
	auto& memo       = *static_cast<MaterializeMemo*>(userdata);
	memo.m_restarted = true;
	if (memo.m_source.walk_restarted != nullptr) {
		memo.m_source.walk_restarted(memo.m_source.userdata);
	}
}

// A read outside its descriptor reads 0 without a reader: the replay re-checks its bounds.
void MaterializeMemo::ZeroRead(void* userdata) {
	auto&       memo  = *static_cast<MaterializeMemo*>(userdata);
	const auto* event = CurrentRawReadEvent();
	if (event == nullptr || !event->zero || memo.m_reads.size() >= MaxRecordedReads) {
		memo.m_replayable = false;
	} else {
		memo.m_reads.push_back({Reader::Memory, event->component_bytes, event->inst, 0u, 0u,
		                        event->operands, CurrentSrtReadSlot(), false, true});
	}
	if (memo.m_source.zero_read != nullptr) {
		memo.m_source.zero_read(memo.m_source.userdata);
	}
}

void MaterializeMemo::Invalidate() {
	m_valid = false;
}

void MaterializeMemo::AdoptAnalysis(const MaterializeMemo& other) {
	if (m_analysis == nullptr && other.m_analysis != nullptr) {
		m_analysis = other.m_analysis;
		m_plan     = other.m_plan;
	}
}

uint32_t MaterializeMemo::KeyOf(Value value) const {
	uint64_t bits = 0;
	if (!ValueReadKey(value, bits)) {
		return UINT32_MAX;
	}
	const auto found = m_keys.find(bits);
	return found == m_keys.end() ? UINT32_MAX : found->second;
}

bool MaterializeMemo::CanDerive(const ResourcePlan& plan, Value value, uint32_t before) const {
	value            = value.Resolve();
	uint64_t ignored = 0;
	if (value.IsImmediate()) {
		return ImmediateValue(value, ignored);
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) {
		return false;
	}
	if (const auto key = KeyOf(value); key != UINT32_MAX) {
		return m_key_first[key] < before;
	}
	const auto op = inst->GetOpcode();
	switch (op) {
		case ValueOpcode::GetUserData: {
			const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
			return reg >= plan.user_data_base && reg - plan.user_data_base < m_user_data.size();
		}
		case ValueOpcode::ReadConst: {
			const auto slot = SrtSlot(plan, *inst);
			return slot != UINT32_MAX && CanDerive(plan, plan.srt_reads[slot].value, before);
		}
		case ValueOpcode::CompositeExtractU32x2: {
			const auto* source = inst->Arg(0).ResolveInstruction();
			const auto  index  = inst->Arg(1).Resolve();
			return source != nullptr && index.IsImmediate() && index.GetType() == Type::U32 &&
			       index.U32() < 2u &&
			       source->GetOpcode() == ValueOpcode::CompositeConstructU32x2 &&
			       CanDerive(plan, source->Arg(index.U32()), before);
		}
		case ValueOpcode::SelectU32:
			return CanDerive(plan, inst->Arg(0), before) && CanDerive(plan, inst->Arg(1), before) &&
			       CanDerive(plan, inst->Arg(2), before);
		default:
			return (op == ValueOpcode::IAdd32 || IsInteger32(op) ||
			        op == ValueOpcode::ULessThan32 || op == ValueOpcode::LogicalOr) &&
			       inst->NumArgs() == 2 && CanDerive(plan, inst->Arg(0), before) &&
			       CanDerive(plan, inst->Arg(1), before);
	}
}

// Emits CanDerive's admitted operations as a postfix program mirroring SrtWalker.
void MaterializeMemo::Compile(const ResourcePlan& plan, Value value) {
	value = value.Resolve();
	if (value.IsImmediate()) {
		uint64_t imm = 0;
		ImmediateValue(value, imm);
		m_ops.push_back({Op::Imm, 0, imm});
		return;
	}
	const auto* inst = value.TryInstruction();
	if (const auto key = KeyOf(value); key != UINT32_MAX) {
		m_ops.push_back({Op::Key, key, 0});
		return;
	}
	const auto op = inst->GetOpcode();
	switch (op) {
		case ValueOpcode::GetUserData:
			m_ops.push_back(
			    {Op::UserData, RegIndex(inst->Arg(0).ScalarRegister()) - plan.user_data_base, 0});
			return;
		case ValueOpcode::ReadConst:
			Compile(plan, plan.srt_reads[SrtSlot(plan, *inst)].value);
			return;
		case ValueOpcode::CompositeExtractU32x2:
			Compile(plan, inst->Arg(0).ResolveInstruction()->Arg(inst->Arg(1).Resolve().U32()));
			return;
		case ValueOpcode::SelectU32:
			Compile(plan, inst->Arg(0));
			Compile(plan, inst->Arg(1));
			Compile(plan, inst->Arg(2));
			m_ops.push_back({Op::Select, 0, 0});
			return;
		case ValueOpcode::IAdd32:
		case ValueOpcode::ISub32:
		case ValueOpcode::IMul32:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::LogicalOr: {
			Compile(plan, inst->Arg(0));
			Compile(plan, inst->Arg(1));
			m_ops.push_back({op == ValueOpcode::IAdd32                ? Op::Add32
			                 : op == ValueOpcode::ISub32              ? Op::Sub32
			                 : op == ValueOpcode::IMul32              ? Op::Mul32
			                 : op == ValueOpcode::BitwiseAnd32        ? Op::And32
			                 : op == ValueOpcode::BitwiseOr32         ? Op::Or32
			                 : op == ValueOpcode::ShiftLeftLogical32  ? Op::Shl32
			                 : op == ValueOpcode::ShiftRightLogical32 ? Op::Shr32
			                 : op == ValueOpcode::ULessThan32         ? Op::ULess32
			                                                          : Op::LogicalOr,
			                 0, 0});
			return;
		}
		default: EXIT("materialize memo: compiled an operation CanDerive refused\n");
	}
}

// Mirrors SrtWalker's 32-bit evaluation.
template <typename Stack>
void MaterializeMemo::ApplyOp(Op::Kind kind, Stack& stack, uint32_t& top) {
	if (kind == Op::Select) {
		const auto false_value = stack[--top];
		const auto true_value  = stack[--top];
		auto&      out         = stack[top - 1];
		out                    = out != 0u ? true_value : false_value;
		return;
	}
	const auto rhs = stack[--top];
	auto&      out = stack[top - 1];
	const auto lhs = static_cast<uint32_t>(out);
	const auto low = static_cast<uint32_t>(rhs);
	switch (kind) {
		case Op::Sub32: out = static_cast<uint32_t>(lhs - low); break;
		case Op::Mul32: out = static_cast<uint32_t>(lhs * low); break;
		case Op::And32: out = lhs & low; break;
		case Op::Or32: out = lhs | low; break;
		case Op::Shl32: out = static_cast<uint32_t>(lhs << (low & 31u)); break;
		case Op::Shr32: out = lhs >> (low & 31u); break;
		case Op::ULess32: out = lhs < low ? 1u : 0u; break;
		case Op::LogicalOr: out = out != 0u || rhs != 0u ? 1u : 0u; break;
		default: out = static_cast<uint32_t>(lhs + low); break;
	}
}

// A program holds at most one value per operation, so the array cannot overflow.
uint64_t MaterializeMemo::Evaluate(uint32_t begin, uint32_t end,
                                   std::span<const uint32_t> user_data) const {
	constexpr uint32_t MaxFastOps = 32;
	if (end - begin > MaxFastOps) {
		return EvaluateScratch(begin, end, user_data);
	}
	std::array<uint64_t, MaxFastOps> stack;
	uint32_t                         top = 0;
	for (uint32_t index = begin; index < end; index++) {
		const auto& op = m_ops[index];
		switch (op.kind) {
			case Op::Imm: stack[top++] = op.imm; break;
			case Op::UserData: stack[top++] = user_data[op.index]; break;
			case Op::Key: stack[top++] = m_key_words[op.index]; break;
			default: ApplyOp(op.kind, stack, top); break;
		}
	}
	return stack[top - 1];
}

uint64_t MaterializeMemo::EvaluateScratch(uint32_t begin, uint32_t end,
                                          std::span<const uint32_t> user_data) const {
	// One scratch stack for every memo: it stays in cache, a per-memo one does not.
	static thread_local std::vector<uint64_t> stack;
	stack.resize(end - begin);
	uint32_t top = 0;
	for (uint32_t index = begin; index < end; index++) {
		const auto& op = m_ops[index];
		switch (op.kind) {
			case Op::Imm: stack[top++] = op.imm; break;
			case Op::UserData: stack[top++] = user_data[op.index]; break;
			case Op::Key: stack[top++] = m_key_words[op.index]; break;
			default: ApplyOp(op.kind, stack, top); break;
		}
	}
	return stack[top - 1];
}

// Equal programs read the same registers and key words, so one evaluation serves every read.
uint64_t MaterializeMemo::SharedOperand(const HotOperand&         derived,
                                        std::span<const uint32_t> user_data) {
	if (m_shared_stamps[derived.shared] != m_shared_epoch) {
		m_shared_stamps[derived.shared] = m_shared_epoch;
		m_shared_values[derived.shared] = Evaluate(derived.begin, derived.end, user_data);
	}
	return m_shared_values[derived.shared];
}

bool MaterializeMemo::SameShape(const ResourcePlan& plan) const {
	if (!m_shape_ok || m_plan != &plan || m_reads.size() != m_recorded.size() ||
	    plan.visited_blocks != m_visited_blocks) {
		return false;
	}
	for (size_t index = 0; index < m_reads.size(); index++) {
		const auto& a = m_reads[index];
		const auto& b = m_recorded[index];
		if (a.inst != b.inst || a.component_bytes != b.component_bytes || a.reader != b.reader ||
		    a.slot != b.slot || a.refused != b.refused || a.zero != b.zero) {
			return false;
		}
	}
	return true;
}

void MaterializeMemo::Pin(const ResourcePlan& plan, Value value, std::vector<uint8_t>& structural,
                          std::vector<const Inst*>& seen) {
	std::vector<const Inst*> pending;
	if (const auto* inst = value.Resolve().TryInstruction(); inst != nullptr) {
		pending.push_back(inst);
	}
	while (!pending.empty()) {
		const auto* inst = pending.back();
		pending.pop_back();
		if (std::ranges::find(seen, inst) != seen.end()) {
			continue;
		}
		seen.push_back(inst);
		if (const auto key = KeyOf(Value(const_cast<Inst*>(inst))); key != UINT32_MAX) {
			structural[key] = 1u;
			continue;
		}
		if (inst->GetOpcode() == ValueOpcode::GetUserData) {
			const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
			if (reg >= plan.user_data_base &&
			    reg - plan.user_data_base < m_fixed_registers.size()) {
				m_fixed_registers[reg - plan.user_data_base] = 1u;
			}
			continue;
		}
		if (const auto slot = SrtSlot(plan, *inst); slot != UINT32_MAX) {
			if (const auto* next = plan.srt_reads[slot].value.Resolve().TryInstruction();
			    next != nullptr) {
				pending.push_back(next);
			}
		}
		for (size_t arg = 0; arg < inst->NumArgs(); arg++) {
			if (const auto* next = inst->Arg(arg).Resolve().TryInstruction(); next != nullptr) {
				pending.push_back(next);
			}
		}
	}
}

bool MaterializeMemo::Build(const ResourcePlan& plan, const ResourceSnapshot& snapshot) {
	if (m_plan != &plan || m_analysis == nullptr) {
		m_analysis = std::make_shared<Analysis>(plan);
		m_plan     = &plan;
	}
	auto& analysis = *m_analysis;
	m_keys.clear();
	m_key_first.clear();
	std::vector<uint64_t> key_address;
	for (uint32_t index = 0; index < m_recorded.size(); index++) {
		const auto& read = m_recorded[index];
		if (read.refused) {
			continue;
		}
		const auto bits = KeyBits(read.inst, read.component_bytes);
		const auto [found, inserted] =
		    m_keys.try_emplace(bits, static_cast<uint32_t>(m_key_first.size()));
		if (inserted) {
			m_key_first.push_back(index);
			key_address.push_back(read.address);
		} else if (key_address[found->second] != read.address ||
		           m_recorded[m_key_first[found->second]].zero != read.zero) {
			// A lane sweep reads one instruction at several addresses; no single word stands.
			return false;
		}
	}
	m_fixed_registers.assign(m_user_data.size(), 1u);
	for (size_t reg = 0; reg < m_user_data.size() && reg < analysis.changeable.size(); reg++) {
		m_fixed_registers[reg] = analysis.changeable[reg] != 0u ? 0u : 1u;
	}
	std::vector<uint8_t> structural(m_key_first.size(), 0u);
	for (const auto& [bits, key]: m_keys) {
		structural[key] = analysis.Replaceable(bits) ? 0u : 1u;
	}
	m_steps.assign(m_recorded.size(), {});
	m_ops.clear();
	std::vector<const Inst*> seen;
	for (uint32_t index = 0; index < m_recorded.size(); index++) {
		const auto& read = m_recorded[index];
		auto&       step = m_steps[index];
		step.key = read.refused ? UINT32_MAX : m_keys.at(KeyBits(read.inst, read.component_bytes));
		step.refused       = read.refused;
		const auto* handle = read.inst->Arg(0).ResolveInstruction();
		if (handle == nullptr) {
			return false;
		}
		const bool buffer = IsBufferRead(read.inst->GetOpcode()) && handle->NumArgs() == 4u;
		const std::array<Value, 5> operands {
		    handle->Arg(0), handle->Arg(1),
		    read.inst->Arg(RawReadOffsetArgument(read.inst->GetOpcode())),
		    buffer ? handle->Arg(2) : Value {}, buffer ? handle->Arg(3) : Value {}};
		for (uint32_t operand = 0; operand < operands.size(); operand++) {
			if (operands[operand].IsEmpty()) {
				continue;
			}
			if (CanDerive(plan, operands[operand], index)) {
				step.op_begin[operand] = static_cast<uint32_t>(m_ops.size());
				Compile(plan, operands[operand]);
				step.op_end[operand] = static_cast<uint32_t>(m_ops.size());
				step.derived_mask |= static_cast<uint8_t>(1u << operand);
				continue;
			}
			// A kept operand must not move: pin every input it was computed from.
			Pin(plan, operands[operand], structural, seen);
		}
	}
	// Patch a plain descriptor only if every dword re-derives, else its inputs must not move.
	m_patches.clear();
	std::vector<uint8_t> active(plan.descriptor_sources.size(), 1u);
	if (!plan.control_flow.empty()) {
		if (plan.visited_blocks.size() != plan.control_flow.size()) {
			std::ranges::fill(active, 0u);
		} else {
			for (const auto& block: plan.control_flow) {
				for (const auto source: block.sources) {
					if (source < active.size()) {
						active[source] = 0u;
					}
				}
			}
			for (size_t block = 0; block < plan.control_flow.size(); block++) {
				if (plan.visited_blocks[block] == 0u) {
					continue;
				}
				for (const auto source: plan.control_flow[block].sources) {
					if (source < active.size()) {
						active[source] = 1u;
					}
				}
			}
		}
	}
	for (uint32_t source = 0; source < analysis.patchable.size(); source++) {
		if (analysis.patchable[source] == 0u) {
			continue;
		}
		const auto& descriptor = plan.descriptor_sources[source];
		bool        derivable  = active[source] != 0u && !m_restarted;
		for (uint32_t dword = 0; dword < descriptor.dword_count && derivable; dword++) {
			derivable =
			    CanDerive(plan, descriptor.dwords[dword], static_cast<uint32_t>(m_recorded.size()));
		}
		if (!derivable) {
			if (active[source] != 0u) {
				for (uint32_t dword = 0; dword < descriptor.dword_count; dword++) {
					Pin(plan, descriptor.dwords[dword], structural, seen);
				}
			}
			continue;
		}
		auto& patch       = m_patches.emplace_back();
		patch.source      = source;
		patch.dword_count = descriptor.dword_count;
		for (uint32_t dword = 0; dword < descriptor.dword_count; dword++) {
			patch.op_begin[dword] = static_cast<uint32_t>(m_ops.size());
			Compile(plan, descriptor.dwords[dword]);
			patch.op_end[dword] = static_cast<uint32_t>(m_ops.size());
		}
	}
	m_patch_targets.clear();
	for (auto& patch: m_patches) {
		patch.target_begin = static_cast<uint32_t>(m_patch_targets.size());
		for (uint32_t i = 0; i < plan.info.buffers.size(); i++) {
			if (plan.info.buffers[i].source == patch.source) {
				m_patch_targets.push_back({PatchTarget::Buffer, plan.info.buffers[i].written, i});
			}
		}
		for (uint32_t i = 0; i < plan.info.images.size(); i++) {
			if (plan.info.images[i].source == patch.source) {
				m_patch_targets.push_back({PatchTarget::Image, false, i});
			}
		}
		for (uint32_t i = 0; i < plan.info.samplers.size(); i++) {
			if (plan.info.samplers[i].source == patch.source) {
				m_patch_targets.push_back({PatchTarget::Sampler, false, i});
			}
		}
		patch.target_end = static_cast<uint32_t>(m_patch_targets.size());
	}
	// Slots the walk pruned stay zero, so only the slots it filled follow a re-read word.
	std::vector<uint8_t> filled(snapshot.flattened_srt.size(), 0u);
	if (plan.control_flow.empty()) {
		std::ranges::fill(filled, 1u);
	} else if (plan.visited_blocks.size() == plan.control_flow.size()) {
		for (size_t block = 0; block < plan.control_flow.size(); block++) {
			if (plan.visited_blocks[block] == 0u) {
				continue;
			}
			for (const auto slot: plan.control_flow[block].srt_reads) {
				if (slot < plan.srt_reads.size() &&
				    plan.srt_reads[slot].flat_offset < filled.size()) {
					filled[plan.srt_reads[slot].flat_offset] = 1u;
				}
			}
		}
	}
	m_slots.clear();
	for (uint32_t index = 0; index < m_recorded.size(); index++) {
		const auto& read = m_recorded[index];
		auto&       step = m_steps[index];
		step.structural  = step.refused || structural[step.key] != 0u;
		// Mirrors SrtWalker::EvaluateRawRead's ownership check.
		step.ownership_checked =
		    !read.zero && read.slot < plan.data_flat_slots.size() &&
		    plan.data_flat_slots[read.slot] != 0u &&
		    plan.srt_reads[read.slot].value.Resolve().TryInstruction() == read.inst;
		step.slot_begin = static_cast<uint32_t>(m_slots.size());
		if (!step.structural && m_key_first[step.key] == index) {
			const auto found = analysis.slots.find(KeyBits(read.inst, read.component_bytes));
			if (found != analysis.slots.end()) {
				for (const auto slot: found->second) {
					if (slot < filled.size() && filled[slot] != 0u) {
						if (snapshot.flattened_srt[slot] != read.word) {
							return false;
						}
						filled[slot] = 0u;
						m_slots.push_back(slot);
					}
				}
			}
		}
		step.slot_end = static_cast<uint32_t>(m_slots.size());
	}
	m_key_words.assign(m_key_first.size(), 0u);
	m_new_words.assign(m_recorded.size(), 0u);
	m_new_addresses.assign(m_recorded.size(), 0u);
	m_visited_blocks = plan.visited_blocks;
	return true;
}

void MaterializeMemo::Commit(const ResourcePlan& plan, const ResourceSnapshot& snapshot,
                             const ResourceSpecialization& specialization) {
	m_valid = false;
	if (!m_replayable) {
		return;
	}
	if (!Supports(plan) || !CommitHeaps(plan, snapshot, specialization)) {
		return;
	}
	m_user_data.assign(m_source.user_data.begin(), m_source.user_data.end());
	m_shader_base = m_source.shader_base;
	// A heap table's address follows its structural V#, so only scalar reads move.
	m_capture_addresses = !snapshot.ScalarReads().empty();
	// The same reads in the same order keep every derived property; only the values move.
	const bool same_shape = SameShape(plan) && m_restarted == m_built_restarted;
	m_recorded.swap(m_reads);
	if (!same_shape) {
		m_built_restarted = m_restarted;
		m_shape_ok        = Build(plan, snapshot);
		if (!m_shape_ok) {
			return;
		}
	}
	// The capture wrappers record each successful read once, in walk order.
	m_capture_mapped = false;
	if (m_capture_addresses) {
		const auto& captured = snapshot.specialization_reads;
		uint32_t    capture  = 0;
		bool        mapped   = true;
		for (uint32_t index = 0; index < m_recorded.size(); index++) {
			const auto& read       = m_recorded[index];
			m_steps[index].capture = UINT32_MAX;
			if (read.refused || read.zero) {
				continue;
			}
			while (capture < captured.size() && snapshot.HeapRead(capture)) {
				capture++;
			}
			if (capture >= captured.size() || captured[capture].first != read.address ||
			    captured[capture].second != sizeof(uint32_t)) {
				mapped = false;
				break;
			}
			m_steps[index].capture = capture++;
		}
		while (capture < captured.size() && snapshot.HeapRead(capture)) {
			capture++;
		}
		m_capture_mapped = mapped && capture == captured.size();
	}
	BuildHot(plan);
	m_valid = true;
}

// Every table the walk materialized must be a heap root a replay can acquire again.
bool MaterializeMemo::CommitHeaps(const ResourcePlan& plan, const ResourceSnapshot& snapshot,
                                  const ResourceSpecialization& specialization) {
	m_heaps.clear();
	m_heap_selector      = false;
	const auto& acquired = m_heap_recorder.acquired;
	for (uint32_t image = 0; image < plan.info.images.size(); image++) {
		const auto  source   = plan.info.images[image].source;
		const auto* indirect = source < plan.descriptor_sources.size() &&
		                               plan.descriptor_sources[source].indirect_descriptor
		                           ? &*plan.descriptor_sources[source].indirect_descriptor
		                           : nullptr;
		if (indirect == nullptr ||
		    (!plan.active_sources.empty() && plan.active_sources[source] == 0u)) {
			continue;
		}
		if (image >= specialization.images.size() || !specialization.images[image].heap) {
			return false;
		}
		// A restarted walk acquires again; its last acquisition is the one it kept.
		const auto found =
		    std::find_if(acquired.rbegin(), acquired.rend(), [&](const HeapRoot& heap) {
			    return heap.request.root == &plan.info.images[image];
		    });
		const auto mapping = specialization.images[image].indirect_mapping_offset;
		if (found == acquired.rend() || mapping + 3u > snapshot.flattened_srt.size()) {
			return false;
		}
		m_heaps.push_back(*found);
		m_heaps.back().mapping = mapping;
		m_heap_selector        = m_heap_selector || indirect->selector.has_value();
	}
	return true;
}

// Acquires each heap root again, as a walk would; a kind its view gained since needs a walk.
bool MaterializeMemo::AcquireHeaps(const SrtRuntime& runtime) {
	if (runtime.image_heap == nullptr) {
		return false;
	}
	for (auto& heap: m_heaps) {
		ImageHeapView view;
		if (!runtime.image_heap->Acquire(heap.request, view) ||
		    !std::ranges::equal(view.kinds, heap.kinds)) {
			return false;
		}
		heap.slots      = view.slots;
		heap.slot_count = view.slot_count;
	}
	return true;
}

void MaterializeMemo::BuildHot(const ResourcePlan& plan) {
	m_hot.resize(m_recorded.size());
	m_hot_operands.clear();
	// Distinct operand programs, keyed by their operations.
	struct Program {
		uint32_t begin = 0;
		uint32_t size  = 0;
		uint32_t id    = 0;
	};
	std::unordered_map<uint64_t, std::vector<Program>> programs;
	uint32_t                                           shared_count = 0;
	const auto SharedProgram = [&](uint32_t begin, uint32_t end) {
		uint64_t hash = 0xcbf29ce484222325ull;
		for (uint32_t index = begin; index < end; index++) {
			const auto& op = m_ops[index];
			for (const uint64_t part: {uint64_t {op.kind}, uint64_t {op.index}, op.imm}) {
				hash = (hash ^ part) * 0x100000001b3ull;
			}
		}
		auto& candidates = programs[hash];
		for (const auto& other: candidates) {
			if (other.size == end - begin &&
			    std::equal(m_ops.begin() + begin, m_ops.begin() + end, m_ops.begin() + other.begin,
			               [](const Op& a, const Op& b) {
				               return a.kind == b.kind && a.index == b.index && a.imm == b.imm;
			               })) {
				return other.id;
			}
		}
		candidates.push_back({begin, end - begin, shared_count});
		return shared_count++;
	};
	for (uint32_t index = 0; index < m_recorded.size(); index++) {
		const auto& read = m_recorded[index];
		const auto& step = m_steps[index];
		auto&       hot  = m_hot[index];
		hot              = {};
		hot.address      = read.address;
		for (uint32_t operand = 0; operand < read.operands.size(); operand++) {
			hot.operands[operand] = static_cast<uint32_t>(read.operands[operand]);
		}
		hot.word          = read.word;
		hot.key           = step.key;
		hot.capture       = step.capture;
		hot.slot_begin    = step.slot_begin;
		hot.slot_end      = step.slot_end;
		hot.reader        = static_cast<uint8_t>(read.reader);
		hot.operand_begin = static_cast<uint32_t>(m_hot_operands.size());
		for (uint32_t operand = 0; operand < read.operands.size(); operand++) {
			if ((step.derived_mask & (1u << operand)) != 0u) {
				m_hot_operands.push_back(
				    {step.op_begin[operand], step.op_end[operand], operand,
				     SharedProgram(step.op_begin[operand], step.op_end[operand])});
			}
		}
		hot.operand_count = static_cast<uint8_t>(m_hot_operands.size() - hot.operand_begin);
		RawReadShape shape;
		const bool   shape_ok     = RawReadShapeOf(plan, *read.inst, read.component_bytes, shape) &&
		                            shape.immediate >= INT32_MIN && shape.immediate <= INT32_MAX;
		const bool   wide_operand = std::ranges::any_of(
		    read.operands, [](uint64_t operand) { return (operand >> 32u) != 0u; });
		hot.kind      = static_cast<uint8_t>(shape.kind);
		hot.immediate = static_cast<int32_t>(shape_ok ? shape.immediate : 0);
		hot.flags     = static_cast<uint8_t>(
		    (step.structural ? HotStep::Structural : 0u) |
		    (step.ownership_checked ? HotStep::OwnershipChecked : 0u) |
		    (step.refused ? HotStep::Refused : 0u) |
		    (!step.refused && m_key_first[step.key] == index ? HotStep::FirstKey : 0u) |
		    (shape_ok && !wide_operand ? HotStep::ShapeOk : 0u) | (read.zero ? HotStep::Zero : 0u));
	}
	m_shared_values.assign(shared_count, 0u);
	m_shared_stamps.assign(shared_count, 0u);
	m_shared_epoch = 0;
}

// Replays the recorded reads in walk order with the walk's own checks.
bool MaterializeMemo::ReplayReads(const SrtRuntime& runtime, bool moved, bool& captured_moved) {
	if (++m_shared_epoch == 0u) {
		std::ranges::fill(m_shared_stamps, 0u);
		m_shared_epoch = 1;
	}
	for (uint32_t index = 0; index < m_hot.size(); index++) {
		const auto& hot     = m_hot[index];
		const bool  zero    = (hot.flags & HotStep::Zero) != 0u;
		uint64_t    address = hot.address;
		if (moved && hot.operand_count != 0u) {
			std::array<uint64_t, 5> operands {hot.operands[0], hot.operands[1], hot.operands[2],
			                                  hot.operands[3], hot.operands[4]};
			bool                    changed = false;
			for (uint32_t i = 0; i < hot.operand_count; i++) {
				const auto& derived = m_hot_operands[hot.operand_begin + i];
				const auto  value   = SharedOperand(derived, runtime.user_data);
				changed = changed ||
				          static_cast<uint32_t>(value) != hot.operands[derived.operand] ||
				          (value >> 32u) != 0u;
				operands[derived.operand] = value;
			}
			if (changed) {
				if ((hot.flags & HotStep::ShapeOk) == 0u) {
					return false;
				}
				// The bounds step: a read that leaves or enters its descriptor reads elsewhere.
				const RawReadShape shape {static_cast<RawReadShape::Kind>(hot.kind), hot.immediate};
				bool               outside = false;
				if (RawReadAddressFrom(shape, operands, address, outside) != SrtRefusal::None ||
				    outside != zero) {
					return false;
				}
			}
			if (!zero && address != hot.address && m_capture_addresses) {
				if (!m_capture_mapped ||
				    (hot.capture == UINT32_MAX && (hot.flags & HotStep::Refused) == 0u)) {
					return false;
				}
				captured_moved = true;
			}
		}
		m_new_addresses[index] = address;
		uint32_t word          = 0;
		if (!zero) {
			// The walk would refuse this read, and the rebuild keeps it native instead of draining.
			if ((hot.flags & HotStep::OwnershipChecked) != 0u && runtime.gpu_owned != nullptr &&
			    runtime.gpu_owned(runtime.userdata, address, sizeof(uint32_t))) {
				return false;
			}
			const auto kind   = static_cast<Reader>(hot.reader);
			const auto reader = kind == Reader::Memory ? runtime.read_memory
			                    : kind == Reader::Specialization
			                        ? runtime.read_specialization_memory
			                        : runtime.read_condition_memory;
			if ((hot.flags & HotStep::Refused) != 0u) {
				// A read that answers now would steer the walk elsewhere.
				if (reader == nullptr || reader(runtime.userdata, address, {&word, 1})) {
					return false;
				}
				continue;
			}
			if (reader == nullptr || !reader(runtime.userdata, address, {&word, 1})) {
				return false;
			}
		}
		if ((hot.flags & HotStep::Structural) != 0u && word != hot.word) {
			return false;
		}
		if ((hot.flags & HotStep::FirstKey) == 0u && m_key_words[hot.key] != word) {
			return false;
		}
		moved                = moved || word != hot.word;
		m_key_words[hot.key] = word;
		m_new_words[index]   = word;
	}
	return true;
}

bool MaterializeMemo::CapturedReadsOverlapWrites(const ResourcePlan&     plan,
                                                 const ResourceSnapshot& descriptors) const {
	if (std::ranges::none_of(plan.info.buffers,
	                         [](const auto& buffer) { return buffer.written; })) {
		return false;
	}
	for (uint32_t step = 0; step < m_steps.size(); step++) {
		if (m_steps[step].capture != UINT32_MAX &&
		    ReadOverlapsWrittenBuffer(plan, descriptors, m_new_addresses[step], sizeof(uint32_t))) {
			return true;
		}
	}
	return false;
}

bool MaterializeMemo::PatchDescriptors(const ResourcePlan& plan, const SrtRuntime& runtime,
                                       ResourceSnapshot&       snapshot,
                                       ResourceSpecialization* specialization,
                                       bool                    captured_moved) {
	static thread_local std::vector<DescriptorValue> values;
	values.resize(m_patches.size());
	for (size_t index = 0; index < m_patches.size(); index++) {
		const auto& patch = m_patches[index];
		auto&       value = values[index];
		value             = {};
		value.dword_count = patch.dword_count;
		for (uint32_t dword = 0; dword < patch.dword_count; dword++) {
			value.dwords[dword] = static_cast<uint32_t>(
			    Evaluate(patch.op_begin[dword], patch.op_end[dword], runtime.user_data));
		}
	}
	const auto& info            = plan.info;
	bool        changed         = false;
	bool        written_changed = false;
	if (snapshot.buffers.size() != info.buffers.size() ||
	    snapshot.images.size() < info.images.size() ||
	    snapshot.samplers.size() != info.samplers.size()) {
		return false;
	}
	for (size_t index = 0; index < m_patches.size(); index++) {
		const auto& patch = m_patches[index];
		const auto& value = values[index];
		for (uint32_t target = patch.target_begin; target < patch.target_end; target++) {
			const auto& bound   = m_patch_targets[target];
			const auto& current = bound.kind == PatchTarget::Buffer ? snapshot.buffers[bound.index]
			                      : bound.kind == PatchTarget::Image
			                          ? snapshot.images[bound.index]
			                          : snapshot.samplers[bound.index];
			if (!(current == value)) {
				changed         = true;
				written_changed = written_changed || bound.written;
			}
		}
	}
	// A written buffer that moves may now overlap the selector the walk bypassed for the heap.
	if (m_heap_selector && written_changed) {
		return false;
	}
	if (!changed) {
		if (captured_moved && CapturedReadsOverlapWrites(plan, snapshot)) {
			return false;
		}
		return true;
	}
	if (specialization == nullptr) {
		return false;
	}
	static thread_local ResourceSnapshot       next;
	static thread_local ResourceSpecialization next_specialization;
	next.buffers  = snapshot.buffers;
	next.images   = snapshot.images;
	next.samplers = snapshot.samplers;
	// A heap root specializes from its mapped slot count.
	if (m_heaps.empty()) {
		next.flattened_srt.clear();
	} else {
		next.flattened_srt = snapshot.flattened_srt;
	}
	next_specialization = *specialization;
	for (size_t index = 0; index < m_patches.size(); index++) {
		const auto& patch = m_patches[index];
		for (uint32_t target = patch.target_begin; target < patch.target_end; target++) {
			const auto& bound = m_patch_targets[target];
			auto&       slot  = bound.kind == PatchTarget::Buffer  ? next.buffers[bound.index]
			                    : bound.kind == PatchTarget::Image ? next.images[bound.index]
			                                                       : next.samplers[bound.index];
			slot              = values[index];
		}
	}
	// The walk's own specialization of the new descriptors: scalar_empty, samplers, fmask.
	if (!RefreshDescriptorState(plan, next, next_specialization) ||
	    next.images.size() != snapshot.images.size()) {
		return false;
	}
	if (m_capture_addresses && written_changed && !m_capture_mapped) {
		return false;
	}
	if (m_capture_addresses && (written_changed || captured_moved) &&
	    CapturedReadsOverlapWrites(plan, next)) {
		return false;
	}
	snapshot.buffers.swap(next.buffers);
	snapshot.images.swap(next.images);
	snapshot.samplers.swap(next.samplers);
	*specialization = next_specialization;
	return true;
}

bool MaterializeMemo::Replay(const ResourcePlan& plan, const SrtRuntime& runtime,
                             ResourceSnapshot& snapshot, ResourceSpecialization* specialization) {
	if (!m_valid || &plan != m_plan) {
		return false;
	}
	// No memoized plan reads the dispatch size or its arguments: only a workgroup-indexed table
	// does.
	if (runtime.shader_base != m_shader_base || runtime.user_data.size() != m_user_data.size()) {
		return false;
	}
	for (size_t reg = 0; reg < m_user_data.size(); reg++) {
		if (m_fixed_registers[reg] != 0u && runtime.user_data[reg] != m_user_data[reg]) {
			return false;
		}
	}
	// Until a register or a re-read word moves, every derived operand is the recorded one.
	bool moved          = false;
	bool captured_moved = false;
	for (size_t reg = 0; reg < m_user_data.size() && !moved; reg++) {
		moved = runtime.user_data[reg] != m_user_data[reg];
	}
	if (!ReplayReads(runtime, moved, captured_moved) ||
	    (!m_heaps.empty() && !AcquireHeaps(runtime))) {
		return false;
	}
	if (!m_patches.empty()
	        ? !PatchDescriptors(plan, runtime, snapshot, specialization, captured_moved)
	        : captured_moved && CapturedReadsOverlapWrites(plan, snapshot)) {
		return false;
	}
	for (uint32_t index = 0; index < m_recorded.size(); index++) {
		const auto& hot = m_hot[index];
		for (uint32_t slot = hot.slot_begin; slot < hot.slot_end; slot++) {
			snapshot.flattened_srt[m_slots[slot]] = m_new_words[index];
		}
		if (hot.capture != UINT32_MAX && m_capture_mapped &&
		    hot.capture < snapshot.specialization_reads.size()) {
			snapshot.specialization_reads[hot.capture].first = m_new_addresses[index];
		}
	}
	for (const auto& heap: m_heaps) {
		snapshot.flattened_srt[heap.mapping]      = heap.slot_count;
		snapshot.flattened_srt[heap.mapping + 1u] = static_cast<uint32_t>(heap.slots);
		snapshot.flattened_srt[heap.mapping + 2u] = static_cast<uint32_t>(heap.slots >> 32u);
	}
	snapshot.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
	return true;
}

void MaterializeMemoWays::ForgetCurrent() {
	m_current = UINT32_MAX;
}

bool MaterializeMemoWays::Valid() const {
	return std::ranges::any_of(m_ways, [](const Way& way) { return way.memo.Valid(); });
}

bool MaterializeMemoWays::Replay(const ResourcePlan& plan, const SrtRuntime& runtime,
                                 ResourceSnapshot&       snapshot,
                                 ResourceSpecialization& specialization) {
	// The current way is the one `snapshot` holds, so it replays in place.
	if (m_current < m_ways.size() && m_ways[m_current].memo.Valid()) {
		auto& way = m_ways[m_current];
		if (way.memo.Replay(plan, runtime, snapshot, &specialization)) {
			way.used = ++m_clock;
			return true;
		}
	}
	std::array<uint32_t, 32> order {};
	uint32_t                 count = 0;
	for (uint32_t index = 0; index < m_ways.size() && count < order.size(); index++) {
		if (index != m_current && m_ways[index].memo.Valid()) {
			order[count++] = index;
		}
	}
	std::sort(order.begin(), order.begin() + count,
	          [&](uint32_t a, uint32_t b) { return m_ways[a].used > m_ways[b].used; });
	for (uint32_t position = 0; position < count; position++) {
		auto& way = m_ways[order[position]];
		if (way.memo.Replay(plan, runtime, way.snapshot, &way.specialization)) {
			snapshot       = way.snapshot;
			specialization = way.specialization;
			m_current      = order[position];
			way.used       = ++m_clock;
			return true;
		}
	}
	return false;
}

SrtRuntime MaterializeMemoWays::Record(const SrtRuntime& runtime) {
	if (m_ways.size() < MaxWays) {
		m_recording = static_cast<uint32_t>(m_ways.size());
		m_ways.emplace_back();
	} else {
		// Reuse an empty way first, else the least recently replayed one.
		m_recording = 0;
		for (uint32_t index = 0; index < m_ways.size(); index++) {
			const auto& way  = m_ways[index];
			const auto& best = m_ways[m_recording];
			if (best.memo.Valid() && (!way.memo.Valid() || way.used < best.used)) {
				m_recording = index;
			}
		}
	}
	m_current       = UINT32_MAX;
	auto& recording = m_ways[m_recording];
	recording.memo.Invalidate();
	for (const auto& way: m_ways) {
		recording.memo.AdoptAnalysis(way.memo);
	}
	return recording.memo.Record(runtime);
}

void MaterializeMemoWays::Commit(const ResourcePlan& plan, const ResourceSnapshot& snapshot,
                                 const ResourceSpecialization& specialization) {
	if (m_recording >= m_ways.size()) {
		return;
	}
	auto& way = m_ways[m_recording];
	way.memo.Commit(plan, snapshot, specialization);
	if (!way.memo.Valid()) {
		// `snapshot` now holds a walk no way recorded.
		m_current = UINT32_MAX;
		return;
	}
	way.snapshot       = snapshot;
	way.specialization = specialization;
	way.used           = ++m_clock;
	m_current          = m_recording;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
