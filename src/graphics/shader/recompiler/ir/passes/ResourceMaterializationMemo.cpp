#include "graphics/shader/recompiler/ir/passes/ResourceMaterializationMemo.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace Libs::Graphics::ShaderRecompiler::IR {

namespace {

// Past this a stage is an enumeration, not a descriptor walk.
constexpr size_t MaxRecordedReads = 4096;

bool EnvFlag(const char* name) {
	const char* text = std::getenv(name);
	return text != nullptr && text[0] != '\0' && std::strcmp(text, "0") != 0;
}

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
	const bool           integer_ops  = MaterializeMemoConfig::Get().integer_ops;
	const bool           self_compare = MaterializeMemoConfig::Get().self_compare;

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
		}
		for (const auto& block: plan.control_flow) {
			external(block.condition, false);
		}
		for (const auto& value: plan.uniform_fill.values) {
			external(value, false);
		}
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
			if (op == ValueOpcode::IAdd32 || op == ValueOpcode::IAddCarry32 ||
			    op == ValueOpcode::CompositeConstructU32x2 ||
			    (op == ValueOpcode::CompositeExtractU32x2 && use.arg == 0u) ||
			    (integer_ops && IsInteger32(op))) {
				return AddressOnly(use.user, false);
			}
			// x == x and x != x hold whatever x reads, so the word steers nothing.
			if (self_compare && (op == ValueOpcode::IEqual32 || op == ValueOpcode::INotEqual32) &&
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
			case ValueOpcode::IAdd32:
				return Derivable(inst->Arg(0), depth + 1) && Derivable(inst->Arg(1), depth + 1);
			case ValueOpcode::ReadConst: {
				const auto slot = SrtSlot(plan, *inst);
				return slot != UINT32_MAX && Derivable(plan.srt_reads[slot].value, depth + 1);
			}
			case ValueOpcode::CompositeExtractU32x2: {
				const auto* source = inst->Arg(0).ResolveInstruction();
				const auto  index  = inst->Arg(1).Resolve();
				if (source == nullptr || !index.IsImmediate() || index.GetType() != Type::U32 ||
				    index.U32() >= 2u) {
					return false;
				}
				if (source->GetOpcode() == ValueOpcode::IAddCarry32) {
					return Derivable(source->Arg(0), depth + 1) &&
					       Derivable(source->Arg(1), depth + 1);
				}
				return source->GetOpcode() == ValueOpcode::CompositeConstructU32x2 &&
				       Derivable(source->Arg(index.U32()), depth + 1);
			}
			default:
				return integer_ops && IsInteger32(inst->GetOpcode()) && inst->NumArgs() == 2 &&
				       Derivable(inst->Arg(0), depth + 1) && Derivable(inst->Arg(1), depth + 1);
		}
	}

	bool ServesAsHandle(Value value) const {
		std::vector<const Inst*> pending;
		std::vector<const Inst*> seen;
		if (const auto* inst = value.Resolve().TryInstruction(); inst != nullptr) {
			pending.push_back(inst);
		}
		while (!pending.empty()) {
			const auto* node = pending.back();
			pending.pop_back();
			if (std::ranges::find(seen, node) != seen.end()) {
				continue;
			}
			seen.push_back(node);
			// A flat slot read carries its slot's word; so does every other read of that slot.
			if (const auto slot = SrtSlot(plan, *node); slot != UINT32_MAX) {
				if (const auto* read = plan.srt_reads[slot].value.Resolve().TryInstruction();
				    read != nullptr) {
					pending.push_back(read);
				}
			}
			const auto found = uses.find(node);
			if (found == uses.end()) {
				continue;
			}
			for (const auto& use: found->second) {
				if (use.user == nullptr) {
					continue;
				}
				const auto op = use.user->GetOpcode();
				if (op == ValueOpcode::GetAddressResource || op == ValueOpcode::GetBufferResource) {
					return true;
				}
				if (op == ValueOpcode::ReadConst && use.arg == UINT32_MAX) {
					pending.push_back(use.user);
				}
			}
		}
		return false;
	}

	// A source qualifies when only plain buffers, images and samplers evaluate it, never a table.
	void ClassifyPatchable() {
		const auto count = plan.descriptor_sources.size();
		patchable.assign(count, 0u);
		if (!MaterializeMemoConfig::Get().descriptor_patch) {
			return;
		}
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
			if (source.indirect_buffer.has_value()) {
				refuse(s);
				refuse(source.indirect_buffer->heap_source);
			}
			if (source.indirect_descriptor.has_value()) {
				const auto& indirect = *source.indirect_descriptor;
				refuse(s);
				refuse(indirect.material_source);
				refuse(indirect.table_source);
				for (const auto other: indirect.sources) {
					refuse(other);
				}
			}
		}
		for (const auto& buffer: plan.info.buffers) {
			reference(buffer.source);
		}
		for (uint32_t i = 0; i < plan.info.images.size(); i++) {
			if (ImageServedBindless(plan, i)) {
				refuse(plan.info.images[i].source);
			} else {
				reference(plan.info.images[i].source);
			}
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
				derivable = Derivable(source.dwords[d]) && !ServesAsHandle(source.dwords[d]);
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

MaterializeMemoConfig& MaterializeMemoConfig::Get() {
	static MaterializeMemoConfig config = [] {
		MaterializeMemoConfig result;
		if (const char* text = std::getenv("KYTY_MATERIALIZE_MEMO_WAYS"); text != nullptr) {
			result.ways = std::clamp<uint32_t>(
			    static_cast<uint32_t>(std::strtoul(text, nullptr, 10)), 1u, 32u);
		}
		result.exact_workgroups   = EnvFlag("KYTY_MEMO_EXACT_WORKGROUPS");
		result.refusal_steps      = !EnvFlag("KYTY_MEMO_NO_REFUSAL_STEPS");
		result.bindless           = !EnvFlag("KYTY_MEMO_NO_BINDLESS");
		result.capture_relocation = !EnvFlag("KYTY_MEMO_NO_CAPTURE_RELOCATION");
		result.integer_ops        = !EnvFlag("KYTY_MEMO_NO_INTEGER_OPS");
		result.self_compare       = !EnvFlag("KYTY_MEMO_NO_SELF_COMPARE");
		result.descriptor_patch   = !EnvFlag("KYTY_MEMO_NO_DESCRIPTOR_PATCH");
		result.hot_replay         = !EnvFlag("KYTY_MEMO_NO_HOT_REPLAY");
		return result;
	}();
	return config;
}

bool MaterializeMemoBackoff::Enabled() {
	static const bool enabled = !EnvFlag("KYTY_NO_MEMO_MISS_BACKOFF");
	return enabled;
}

MaterializeMemo::MaterializeMemo()                                      = default;
MaterializeMemo::~MaterializeMemo()                                     = default;
MaterializeMemo::MaterializeMemo(MaterializeMemo&&) noexcept            = default;
MaterializeMemo& MaterializeMemo::operator=(MaterializeMemo&&) noexcept = default;

bool MaterializeMemo::Supports(const ResourcePlan& plan) {
	const bool bindless = MaterializeMemoConfig::Get().bindless;
	const auto indirect = [&](uint32_t source) {
		return source < plan.descriptor_sources.size() &&
		       plan.descriptor_sources[source].indirect_descriptor.has_value();
	};
	for (const auto& source: plan.descriptor_sources) {
		if (source.indirect_buffer.has_value() ||
		    (source.indirect_descriptor.has_value() &&
		     (!bindless || !source.indirect_descriptor->bindless))) {
			return false;
		}
	}
	// A bindless heap is one host-evaluated descriptor; every other table use enumerates keys.
	for (uint32_t image = 0; image < plan.info.images.size(); image++) {
		if (indirect(plan.info.images[image].source) && !ImageServedBindless(plan, image)) {
			return false;
		}
	}
	return std::ranges::none_of(
	           plan.info.buffers,
	           [&](const BufferResource& buffer) { return indirect(buffer.source); }) &&
	       std::ranges::none_of(plan.info.samplers, [&](const SamplerResource& sampler) {
		       return indirect(sampler.source);
	       });
}

SrtRuntime MaterializeMemo::Record(const SrtRuntime& runtime) {
	m_source = runtime;
	m_reads.clear();
	m_restarted = false;
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
	wrapped.describe_read_refusal =
	    runtime.describe_read_refusal != nullptr ? DescribeRefusal : nullptr;
	wrapped.read_specialization_block =
	    runtime.read_specialization_block != nullptr ? ReadBlock : nullptr;
	wrapped.hash_specialization_block =
	    runtime.hash_specialization_block != nullptr ? HashBlock : nullptr;
	wrapped.gpu_owned      = runtime.gpu_owned != nullptr ? GpuOwned : nullptr;
	wrapped.walk_restarted = WalkRestarted;
	return wrapped;
}

bool MaterializeMemo::Note(Reader reader, bool read, uint64_t address,
                           std::span<const uint32_t> values) {
	const auto* event = CurrentRawReadEvent();
	if (!read && event != nullptr && values.size() == 1u && m_reads.size() < MaxRecordedReads &&
	    MaterializeMemoConfig::Get().refusal_steps) {
		m_reads.push_back({reader, event->component_bytes, event->inst, address, 0u,
		                   event->operands, CurrentSrtReadSlot(), true});
		return read;
	}
	// A refused read steers the walk, and a read outside a raw-read evaluation has no recipe.
	if (!read || event == nullptr || values.size() != 1u || m_reads.size() >= MaxRecordedReads) {
		m_replayable = false;
		return read;
	}
	m_reads.push_back({reader, event->component_bytes, event->inst, address, values[0],
	                   event->operands, CurrentSrtReadSlot()});
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

const char* MaterializeMemo::DescribeRefusal(void* userdata, uint64_t address) {
	auto& memo        = *static_cast<MaterializeMemo*>(userdata);
	memo.m_replayable = false;
	return memo.m_source.describe_read_refusal(memo.m_source.userdata, address);
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

bool MaterializeMemo::RefusedMoveAllowed() {
	static const bool allowed = !EnvFlag("KYTY_MEMO_NO_REFUSED_MOVE");
	return allowed;
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
	switch (inst->GetOpcode()) {
		case ValueOpcode::GetUserData: {
			const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
			return reg >= plan.user_data_base && reg - plan.user_data_base < m_user_data.size();
		}
		case ValueOpcode::IAdd32:
			return CanDerive(plan, inst->Arg(0), before) && CanDerive(plan, inst->Arg(1), before);
		case ValueOpcode::ISub32:
		case ValueOpcode::IMul32:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftRightLogical32:
			return MaterializeMemoConfig::Get().integer_ops && inst->NumArgs() == 2 &&
			       CanDerive(plan, inst->Arg(0), before) && CanDerive(plan, inst->Arg(1), before);
		case ValueOpcode::ReadConst: {
			const auto slot = SrtSlot(plan, *inst);
			return slot != UINT32_MAX && CanDerive(plan, plan.srt_reads[slot].value, before);
		}
		case ValueOpcode::CompositeExtractU32x2: {
			const auto* source = inst->Arg(0).ResolveInstruction();
			const auto  index  = inst->Arg(1).Resolve();
			if (source == nullptr || !index.IsImmediate() || index.GetType() != Type::U32 ||
			    index.U32() >= 2u) {
				return false;
			}
			if (source->GetOpcode() == ValueOpcode::IAddCarry32) {
				return CanDerive(plan, source->Arg(0), before) &&
				       CanDerive(plan, source->Arg(1), before);
			}
			if (source->GetOpcode() == ValueOpcode::CompositeConstructU32x2) {
				return CanDerive(plan, source->Arg(index.U32()), before);
			}
			return false;
		}
		default: return false;
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
	switch (inst->GetOpcode()) {
		case ValueOpcode::GetUserData:
			m_ops.push_back(
			    {Op::UserData, RegIndex(inst->Arg(0).ScalarRegister()) - plan.user_data_base, 0});
			return;
		case ValueOpcode::ReadConst:
			Compile(plan, plan.srt_reads[SrtSlot(plan, *inst)].value);
			return;
		case ValueOpcode::IAdd32:
			Compile(plan, inst->Arg(0));
			Compile(plan, inst->Arg(1));
			m_ops.push_back({Op::Add32, 0, 0});
			return;
		case ValueOpcode::ISub32:
		case ValueOpcode::IMul32:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftRightLogical32: {
			Compile(plan, inst->Arg(0));
			Compile(plan, inst->Arg(1));
			const auto op = inst->GetOpcode();
			m_ops.push_back({op == ValueOpcode::ISub32               ? Op::Sub32
			                 : op == ValueOpcode::IMul32             ? Op::Mul32
			                 : op == ValueOpcode::BitwiseAnd32       ? Op::And32
			                 : op == ValueOpcode::BitwiseOr32        ? Op::Or32
			                 : op == ValueOpcode::ShiftLeftLogical32 ? Op::Shl32
			                                                         : Op::Shr32,
			                 0, 0});
			return;
		}
		case ValueOpcode::CompositeExtractU32x2: {
			const auto* source = inst->Arg(0).ResolveInstruction();
			const auto  index  = inst->Arg(1).Resolve().U32();
			if (source->GetOpcode() == ValueOpcode::CompositeConstructU32x2) {
				Compile(plan, source->Arg(index));
				return;
			}
			Compile(plan, source->Arg(0));
			Compile(plan, source->Arg(1));
			m_ops.push_back({index == 0u ? Op::CarryLow : Op::CarryHigh, 0, 0});
			return;
		}
		default: EXIT("materialize memo: compiled an operation CanDerive refused\n");
	}
}

uint64_t MaterializeMemo::Evaluate(uint32_t begin, uint32_t end,
                                   std::span<const uint32_t> user_data) const {
	// One scratch stack for every memo: it stays in cache, a per-memo one does not.
	static thread_local std::vector<uint64_t> stack;
	auto&                                     m_stack = stack;
	m_stack.clear();
	for (uint32_t index = begin; index < end; index++) {
		const auto& op = m_ops[index];
		switch (op.kind) {
			case Op::Imm: m_stack.push_back(op.imm); break;
			case Op::UserData: m_stack.push_back(user_data[op.index]); break;
			case Op::Key: m_stack.push_back(m_key_words[op.index]); break;
			default: {
				const auto rhs = m_stack.back();
				m_stack.pop_back();
				const auto lhs = static_cast<uint32_t>(m_stack.back());
				const auto low = static_cast<uint32_t>(rhs);
				// Mirrors SrtWalker: each 32-bit operation keeps the low word of its result.
				switch (op.kind) {
					case Op::Sub32: m_stack.back() = static_cast<uint32_t>(lhs - low); break;
					case Op::Mul32: m_stack.back() = static_cast<uint32_t>(lhs * low); break;
					case Op::And32: m_stack.back() = lhs & low; break;
					case Op::Or32: m_stack.back() = lhs | low; break;
					case Op::Shl32:
						m_stack.back() = static_cast<uint32_t>(lhs << (low & 31u));
						break;
					case Op::Shr32: m_stack.back() = lhs >> (low & 31u); break;
					default: {
						const auto sum = static_cast<uint64_t>(lhs) + low;
						m_stack.back() =
						    op.kind == Op::CarryHigh ? sum >> 32u : static_cast<uint32_t>(sum);
						break;
					}
				}
				break;
			}
		}
	}
	return m_stack.back();
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
		    a.slot != b.slot || a.refused != b.refused) {
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
		} else if (key_address[found->second] != read.address) {
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
		// Mirrors SrtWalker::EvaluateRawRead: only a data slot's own read asks for ownership.
		step.ownership_checked =
		    read.slot < plan.data_flat_slots.size() && plan.data_flat_slots[read.slot] != 0u &&
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

void MaterializeMemo::Commit(const ResourcePlan& plan, const ResourceSnapshot& snapshot) {
	m_valid = false;
	if (!m_replayable) {
		m_miss = Miss::Unreplayable;
		return;
	}
	// A bindless table is the heap descriptor a walk evaluated, so it replays with that walk.
	if (!Supports(plan) || !snapshot.key_feedback.empty() ||
	    (!MaterializeMemoConfig::Get().bindless && !snapshot.bindless_tables.empty())) {
		m_miss = Miss::Unsupported;
		return;
	}
	m_user_data.assign(m_source.user_data.begin(), m_source.user_data.end());
	m_workgroup_counts.assign(m_source.workgroup_counts.begin(), m_source.workgroup_counts.end());
	m_shader_base       = m_source.shader_base;
	m_capture_addresses = !snapshot.specialization_reads.empty();
	// The same reads in the same order keep every derived property; only the values move.
	const bool same_shape = SameShape(plan) && m_restarted == m_built_restarted;
	m_recorded.swap(m_reads);
	if (!same_shape) {
		m_built_restarted = m_restarted;
		m_shape_ok        = Build(plan, snapshot);
		if (!m_shape_ok) {
			m_miss = Miss::Unreplayable;
			return;
		}
	}
	// The capture wrappers record each successful read once, in walk order.
	m_capture_mapped = false;
	if (m_capture_addresses && MaterializeMemoConfig::Get().capture_relocation) {
		const auto& captured = snapshot.specialization_reads;
		uint32_t    capture  = 0;
		bool        mapped   = true;
		for (uint32_t index = 0; index < m_recorded.size(); index++) {
			const auto& read       = m_recorded[index];
			m_steps[index].capture = UINT32_MAX;
			if (read.refused) {
				continue;
			}
			if (capture >= captured.size() || captured[capture].first != read.address ||
			    captured[capture].second != sizeof(uint32_t)) {
				mapped = false;
				break;
			}
			m_steps[index].capture = capture++;
		}
		m_capture_mapped = mapped && capture == captured.size();
	}
	if (MaterializeMemoConfig::Get().hot_replay) {
		BuildHot(plan);
	}
	m_valid = true;
	m_miss  = Miss::None;
}

void MaterializeMemo::BuildHot(const ResourcePlan& plan) {
	m_hot.resize(m_recorded.size());
	m_hot_operands.clear();
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
				m_hot_operands.push_back({step.op_begin[operand], step.op_end[operand], operand});
			}
		}
		hot.operand_count = static_cast<uint8_t>(m_hot_operands.size() - hot.operand_begin);
		RawReadShape shape;
		const bool   shape_ok =
		    RawReadShapeOf(plan, *read.inst, read.component_bytes, shape) == RawReadReject::None &&
		    shape.immediate >= INT32_MIN && shape.immediate <= INT32_MAX;
		hot.kind      = static_cast<uint8_t>(shape.kind);
		hot.immediate = static_cast<int32_t>(shape_ok ? shape.immediate : 0);
		hot.flags     = static_cast<uint8_t>(
		    (step.structural ? HotStep::Structural : 0u) |
		    (step.ownership_checked ? HotStep::OwnershipChecked : 0u) |
		    (step.refused ? HotStep::Refused : 0u) |
		    (!step.refused && m_key_first[step.key] == index ? HotStep::FirstKey : 0u) |
		    (shape_ok ? HotStep::ShapeOk : 0u));
	}
}

// The recorded-read loop of Replay over the packed records; same checks, same order.
bool MaterializeMemo::ReplayReadsHot(const SrtRuntime& runtime, bool moved, bool& captured_moved) {
	for (uint32_t index = 0; index < m_hot.size(); index++) {
		const auto& hot     = m_hot[index];
		uint64_t    address = hot.address;
		if (moved && hot.operand_count != 0u) {
			std::array<uint64_t, 5> operands {hot.operands[0], hot.operands[1], hot.operands[2],
			                                  hot.operands[3], hot.operands[4]};
			bool                    changed = false;
			for (uint32_t i = 0; i < hot.operand_count; i++) {
				const auto& derived = m_hot_operands[hot.operand_begin + i];
				const auto  value   = Evaluate(derived.begin, derived.end, runtime.user_data);
				changed = changed ||
				          static_cast<uint32_t>(value) != hot.operands[derived.operand] ||
				          (value >> 32u) != 0u;
				operands[derived.operand] = value;
			}
			if (changed) {
				if ((hot.flags & HotStep::ShapeOk) == 0u) {
					m_miss = Miss::AddressChanged;
					return false;
				}
				const RawReadShape shape {static_cast<RawReadShape::Kind>(hot.kind), hot.immediate};
				if (RawReadAddressFrom(shape, operands, address) != RawReadReject::None) {
					m_miss = Miss::AddressChanged;
					return false;
				}
			}
			if (address != hot.address && m_capture_addresses) {
				if (!m_capture_mapped ||
				    (hot.capture == UINT32_MAX &&
				     !((hot.flags & HotStep::Refused) != 0u && RefusedMoveAllowed()))) {
					m_miss = Miss::AddressChanged;
					return false;
				}
				captured_moved = true;
			}
		}
		m_new_addresses[index] = address;
		// The walk would refuse this read, and the rebuild keeps it native instead of draining.
		if ((hot.flags & HotStep::OwnershipChecked) != 0u && runtime.gpu_owned != nullptr &&
		    runtime.gpu_owned(runtime.userdata, address, sizeof(uint32_t))) {
			m_miss = Miss::ReadRefused;
			return false;
		}
		uint32_t   word   = 0;
		const auto kind   = static_cast<Reader>(hot.reader);
		const auto reader = kind == Reader::Memory           ? runtime.read_memory
		                    : kind == Reader::Specialization ? runtime.read_specialization_memory
		                                                     : runtime.read_condition_memory;
		if ((hot.flags & HotStep::Refused) != 0u) {
			// The walk only saw this read refuse; a read that now answers would steer it elsewhere.
			if (reader == nullptr || reader(runtime.userdata, address, {&word, 1})) {
				m_miss = Miss::StructureChanged;
				return false;
			}
			continue;
		}
		if (reader == nullptr || !reader(runtime.userdata, address, {&word, 1})) {
			m_miss = Miss::ReadRefused;
			return false;
		}
		if ((hot.flags & HotStep::Structural) != 0u && word != hot.word) {
			m_miss = Miss::StructureChanged;
			return false;
		}
		if ((hot.flags & HotStep::FirstKey) == 0u && m_key_words[hot.key] != word) {
			m_miss = Miss::StructureChanged;
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
	if (snapshot.buffers.size() < info.buffers.size() ||
	    snapshot.images.size() != info.images.size() ||
	    snapshot.samplers.size() != info.samplers.size()) {
		m_miss = Miss::StructureChanged;
		return false;
	}
	for (size_t index = 0; index < m_patches.size(); index++) {
		const auto  source = m_patches[index].source;
		const auto& value  = values[index];
		for (uint32_t i = 0; i < info.buffers.size(); i++) {
			if (info.buffers[i].source == source && !(snapshot.buffers[i] == value)) {
				changed         = true;
				written_changed = written_changed || info.buffers[i].written;
			}
		}
		for (uint32_t i = 0; i < info.images.size(); i++) {
			changed =
			    changed || (info.images[i].source == source && !(snapshot.images[i] == value));
		}
		for (uint32_t i = 0; i < info.samplers.size(); i++) {
			changed =
			    changed || (info.samplers[i].source == source && !(snapshot.samplers[i] == value));
		}
	}
	if (!changed) {
		if (captured_moved && CapturedReadsOverlapWrites(plan, snapshot)) {
			m_miss = Miss::AddressChanged;
			return false;
		}
		return true;
	}
	if (specialization == nullptr) {
		m_miss = Miss::StructureChanged;
		return false;
	}
	static thread_local ResourceSnapshot       next;
	static thread_local ResourceSpecialization next_specialization;
	next.buffers        = snapshot.buffers;
	next.images         = snapshot.images;
	next.samplers       = snapshot.samplers;
	next_specialization = *specialization;
	for (size_t index = 0; index < m_patches.size(); index++) {
		const auto  source = m_patches[index].source;
		const auto& value  = values[index];
		for (uint32_t i = 0; i < info.buffers.size(); i++) {
			if (info.buffers[i].source == source) {
				next.buffers[i] = value;
			}
		}
		for (uint32_t i = 0; i < info.images.size(); i++) {
			if (info.images[i].source == source) {
				next.images[i] = value;
			}
		}
		for (uint32_t i = 0; i < info.samplers.size(); i++) {
			if (info.samplers[i].source == source) {
				next.samplers[i] = value;
			}
		}
	}
	if (!RefreshDescriptorState(plan, next, next_specialization) ||
	    next.images.size() != info.images.size()) {
		m_miss = Miss::StructureChanged;
		return false;
	}
	if (m_capture_addresses && written_changed && !m_capture_mapped) {
		m_miss = Miss::StructureChanged;
		return false;
	}
	if (m_capture_addresses && (written_changed || captured_moved) &&
	    CapturedReadsOverlapWrites(plan, next)) {
		m_miss = Miss::StructureChanged;
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
		// Keep the reason the last commit was refused.
		m_miss = m_miss == Miss::None ? Miss::Empty : m_miss;
		return false;
	}
	// Only an indirect table reads the dispatch size, and those plans are never memoized.
	if (runtime.shader_base != m_shader_base || runtime.user_data.size() != m_user_data.size() ||
	    (MaterializeMemoConfig::Get().exact_workgroups &&
	     !std::ranges::equal(runtime.workgroup_counts, m_workgroup_counts))) {
		m_miss = Miss::InputChanged;
		return false;
	}
	for (size_t reg = 0; reg < m_user_data.size(); reg++) {
		if (m_fixed_registers[reg] != 0u && runtime.user_data[reg] != m_user_data[reg]) {
			m_miss = Miss::InputChanged;
			return false;
		}
	}
	// Until a register or a re-read word moves, every derived operand is the recorded one.
	bool moved          = false;
	bool captured_moved = false;
	for (size_t reg = 0; reg < m_user_data.size() && !moved; reg++) {
		moved = runtime.user_data[reg] != m_user_data[reg];
	}
	const bool hot = m_hot.size() == m_recorded.size() && MaterializeMemoConfig::Get().hot_replay;
	if (hot && !ReplayReadsHot(runtime, moved, captured_moved)) {
		return false;
	}
	for (uint32_t index = 0; !hot && index < m_recorded.size(); index++) {
		const auto& read    = m_recorded[index];
		const auto& step    = m_steps[index];
		uint64_t    address = read.address;
		if (moved && step.derived_mask != 0u) {
			auto operands = read.operands;
			for (uint32_t operand = 0; operand < operands.size(); operand++) {
				if ((step.derived_mask & (1u << operand)) != 0u) {
					operands[operand] =
					    Evaluate(step.op_begin[operand], step.op_end[operand], runtime.user_data);
				}
			}
			if (operands != read.operands &&
			    RawReadAddress(plan, *read.inst, read.component_bytes, operands, address) !=
			        RawReadReject::None) {
				m_miss = Miss::AddressChanged;
				return false;
			}
			if (address != read.address && m_capture_addresses) {
				if (!m_capture_mapped ||
				    (step.capture == UINT32_MAX && !(step.refused && RefusedMoveAllowed()))) {
					m_miss = Miss::AddressChanged;
					return false;
				}
				captured_moved = true;
			}
		}
		m_new_addresses[index] = address;
		// The walk would refuse this read, and the rebuild keeps it native instead of draining.
		if (step.ownership_checked && runtime.gpu_owned != nullptr &&
		    runtime.gpu_owned(runtime.userdata, address, sizeof(uint32_t))) {
			m_miss = Miss::ReadRefused;
			return false;
		}
		uint32_t   word   = 0;
		const auto reader = read.reader == Reader::Memory ? runtime.read_memory
		                    : read.reader == Reader::Specialization
		                        ? runtime.read_specialization_memory
		                        : runtime.read_condition_memory;
		if (step.refused) {
			// The walk only saw this read refuse; a read that now answers would steer it elsewhere.
			if (reader == nullptr || reader(runtime.userdata, address, {&word, 1})) {
				m_miss = Miss::StructureChanged;
				return false;
			}
			continue;
		}
		if (reader == nullptr || !reader(runtime.userdata, address, {&word, 1})) {
			m_miss = Miss::ReadRefused;
			return false;
		}
		if (step.structural && word != read.word) {
			m_miss = Miss::StructureChanged;
			return false;
		}
		if (m_key_first[step.key] != index && m_key_words[step.key] != word) {
			m_miss = Miss::StructureChanged;
			return false;
		}
		moved                 = moved || word != read.word;
		m_key_words[step.key] = word;
		m_new_words[index]    = word;
	}
	if (!m_patches.empty()
	        ? !PatchDescriptors(plan, runtime, snapshot, specialization, captured_moved)
	        : captured_moved && CapturedReadsOverlapWrites(plan, snapshot)) {
		if (m_patches.empty()) {
			m_miss = Miss::AddressChanged;
		}
		return false;
	}
	for (uint32_t index = 0; index < m_recorded.size(); index++) {
		const auto slot_begin = hot ? m_hot[index].slot_begin : m_steps[index].slot_begin;
		const auto slot_end   = hot ? m_hot[index].slot_end : m_steps[index].slot_end;
		const auto capture    = hot ? m_hot[index].capture : m_steps[index].capture;
		for (uint32_t slot = slot_begin; slot < slot_end; slot++) {
			snapshot.flattened_srt[m_slots[slot]] = m_new_words[index];
		}
		if (capture != UINT32_MAX && m_capture_mapped &&
		    capture < snapshot.specialization_reads.size()) {
			snapshot.specialization_reads[capture].first = m_new_addresses[index];
		}
	}
	snapshot.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
	m_miss = Miss::None;
	return true;
}

void MaterializeMemoWays::ForgetCurrent() {
	// With one way the caller's snapshot is that way's only copy, and the walk just replaced it.
	if (m_current < m_ways.size() && MaterializeMemoConfig::Get().ways <= 1u) {
		m_ways[m_current].memo.Invalidate();
	}
	m_current = UINT32_MAX;
}

bool MaterializeMemoWays::Valid() const {
	return std::ranges::any_of(m_ways, [](const Way& way) { return way.memo.Valid(); });
}

bool MaterializeMemoWays::Replay(const ResourcePlan& plan, const SrtRuntime& runtime,
                                 ResourceSnapshot&       snapshot,
                                 ResourceSpecialization& specialization) {
	bool tried = false;
	// The current way is the one `snapshot` holds, so it replays in place.
	if (m_current < m_ways.size() && m_ways[m_current].memo.Valid()) {
		auto& way = m_ways[m_current];
		tried     = true;
		if (way.memo.Replay(plan, runtime, snapshot, &specialization)) {
			way.used = ++m_clock;
			m_miss   = Miss::None;
			return true;
		}
		m_miss = way.memo.LastMiss();
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
			m_miss         = Miss::None;
			return true;
		}
		if (!tried) {
			m_miss = way.memo.LastMiss();
			tried  = true;
		}
	}
	if (!tried) {
		// No recording stands; report why the last one was refused.
		const auto* last = m_recording < m_ways.size() ? &m_ways[m_recording].memo : nullptr;
		m_miss = last != nullptr && last->LastMiss() != Miss::None ? last->LastMiss() : Miss::Empty;
	}
	return false;
}

SrtRuntime MaterializeMemoWays::Record(const SrtRuntime& runtime) {
	const auto ways = MaterializeMemoConfig::Get().ways;
	if (m_ways.size() < ways) {
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
	way.memo.Commit(plan, snapshot);
	m_miss = way.memo.LastMiss();
	if (!way.memo.Valid()) {
		// `snapshot` now holds a walk no way recorded.
		m_current = UINT32_MAX;
		return;
	}
	// With one way the caller's snapshot is the only copy ever replayed.
	if (MaterializeMemoConfig::Get().ways > 1u) {
		way.snapshot       = snapshot;
		way.specialization = specialization;
	}
	way.used  = ++m_clock;
	m_current = m_recording;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
