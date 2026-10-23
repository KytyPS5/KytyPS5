#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <array>
#include <functional>
#include <span>
#include <string_view>

namespace Libs::Graphics::ShaderRecompiler::IR {

class Value;

using SrtMemoryReader = bool (*)(void* userdata, uint64_t address, std::span<uint32_t> values);
// Bytes readable from address, capped at size.
using SrtExtentQuery = uint64_t (*)(void* userdata, uint64_t address, uint64_t size);
using SrtBlockReader = bool (*)(void* userdata, uint64_t address, void* data, uint64_t size);
// XXH3-64 of a contiguous block, false wherever the strict reader's first read would refuse.
using SrtBlockHasher    = bool (*)(void* userdata, uint64_t address, uint64_t size, uint64_t* hash);
using SrtOwnershipQuery = bool (*)(void* userdata, uint64_t address, uint64_t size);

// Image descriptors at base + first + k * step, k < count, within size bytes that a sampled
// root selects by GPU-computed byte offset. The host keeps them resident instead of
// materializing them.
struct ImageHeapRequest {
	uint64_t             base  = 0;
	uint64_t             size  = 0;
	uint64_t             step  = 0;
	uint64_t             first = 0;
	uint64_t             count = 0;
	const ImageResource* root  = nullptr;
};

struct ImageHeapView {
	uint64_t                         slots      = 0; // Device address of one slot per heap dword.
	uint32_t                         slot_count = 0; // Keys past it select a trailing null slot.
	std::span<const DescriptorValue> kinds; // One representative descriptor per kind.
};

class ImageHeapSource {
public:
	// False when the host does not keep the table resident.
	virtual bool Acquire(const ImageHeapRequest& request, ImageHeapView& view) = 0;

protected:
	~ImageHeapSource() = default;
};

struct SrtRuntime {
	std::span<const uint32_t> user_data;
	uint64_t                  shader_base                = 0;
	SrtMemoryReader           read_memory                = nullptr;
	void*                     userdata                   = nullptr;
	SrtMemoryReader           read_specialization_memory = nullptr;
	std::span<const uint32_t> workgroup_counts;
	uint64_t                  indirect_args_address = 0;
	SrtOwnershipQuery         gpu_owned             = nullptr;
	// Reads a branch condition's memory without draining the GPU; a refusal visits both arms.
	SrtMemoryReader read_condition_memory     = nullptr;
	SrtExtentQuery  readable_extent           = nullptr;
	SrtBlockReader  read_specialization_block = nullptr;
	SrtBlockHasher  hash_specialization_block = nullptr;
	// Told when a materialization throws its walk away and walks again without pruning.
	void (*walk_restarted)(void* userdata) = nullptr;
	// Told when a raw read lies outside its descriptor and reads as zero (CurrentRawReadEvent).
	void (*zero_read)(void* userdata) = nullptr;
	ImageHeapSource* image_heap       = nullptr;
};

class SrtReadCapture {
public:
	SrtReadCapture(SrtRuntime source, std::vector<std::pair<uint64_t, uint64_t>>& ranges):
	    m_source(source), m_ranges(ranges) {}
	SrtRuntime ObservedRuntime();

private:
	static bool ReadStrict(void* userdata, uint64_t address, std::span<uint32_t> values);
	static bool ReadOrdinary(void* userdata, uint64_t address, std::span<uint32_t> values);

	SrtRuntime                                  m_source;
	std::vector<std::pair<uint64_t, uint64_t>>& m_ranges;
};

// Retained reads no longer need the guest instruction PC. A clean read evaluates
// its address and bounds through the strict reader as well as the final DWORD.
struct SrtReadFlags {
	uint32_t index = 0;
	uint32_t clean = 0;
};

enum class RuntimeValueType { Any, Integer, ImmutableInteger };

bool ValidateRuntimeValue(const ResourcePlan& program, Value value,
                          RuntimeValueType type = RuntimeValueType::Any);
// Uniform across a compute wave, including values derived from its workgroup ID.
// This does not imply that the host can evaluate the value.
bool ValidateWaveUniformValue(const ResourcePlan& program, Value value);
void CompileSrtWalker(ResourcePlan& program);
// Uses the strict reader for values that affect shader specialization.
SrtRuntime CleanRuntime(SrtRuntime runtime);

// Why a walk refused, for the one-line refusal report.
enum class SrtRefusal : uint8_t {
	None,
	UnsupportedOpcode,
	UnsupportedOperand,
	UndefinedValue,
	VaryingPhi,
	PhiNoEntry,
	PhiMerge,
	ReadOutOfBounds,
	ReadRefused,
	GpuOwned,
	VaryingLane,
};

[[nodiscard]] std::string_view SrtRefusalName(SrtRefusal reason);

// The flat SRT slot whose walk is reading now; UINT32_MAX outside one.
uint32_t CurrentSrtReadSlot();

struct RawReadEvent {
	const Inst*             inst            = nullptr;
	uint32_t                component_bytes = 0;
	std::array<uint64_t, 5> operands {}; // base low, base high, offset, num_records, word3
	// Out of the descriptor's bounds, so the read produced 0 rather than memory.
	bool zero = false;
};
[[nodiscard]] const RawReadEvent* CurrentRawReadEvent();

// The raw-read address rule, shared with replays of a recorded walk.
struct RawReadShape {
	enum class Kind : uint8_t { Address, ConstBuffer, VectorBuffer, VectorDescriptor };
	Kind    kind      = Kind::Address;
	int64_t immediate = 0;
};
[[nodiscard]] size_t RawReadOffsetArgument(ValueOpcode op);
[[nodiscard]] bool   RawReadShapeOf(const ResourcePlan& program, const Inst& inst,
                                    uint32_t component_bytes, RawReadShape& shape);
// None with `zero` set: the read lies outside its descriptor and reads as zero.
[[nodiscard]] SrtRefusal RawReadAddressFrom(const RawReadShape&            shape,
                                            const std::array<uint64_t, 5>& operands,
                                            uint64_t& address, bool& zero);

// The lane a per-lane sweep is on; `dependent` is set once a walk asks for it.
struct LaneScope {
	uint32_t lane      = 0;
	bool     dependent = false;
};

class InstSet {
public:
	[[nodiscard]] bool contains(const Inst* inst) const {
		return std::binary_search(m_items.begin(), m_items.end(), inst, std::less<const Inst*> {});
	}
	void insert(const Inst* inst) {
		const auto at =
		    std::lower_bound(m_items.begin(), m_items.end(), inst, std::less<const Inst*> {});
		if (at == m_items.end() || *at != inst) m_items.insert(at, inst);
	}

private:
	std::vector<const Inst*> m_items;
};

class InstValueMap {
public:
	using Entry = std::pair<const Inst*, uint64_t>;

	[[nodiscard]] const uint64_t* find(const Inst* inst) const {
		const auto at = LowerBound(inst);
		return at != m_items.end() && at->first == inst ? &at->second : nullptr;
	}
	[[nodiscard]] bool contains(const Inst* inst) const { return find(inst) != nullptr; }
	// Returns the value already held for `inst`, or inserts and returns `value`.
	uint64_t emplace(const Inst* inst, uint64_t value) {
		const auto at = LowerBound(inst);
		if (at != m_items.end() && at->first == inst) return at->second;
		m_items.insert(at, Entry {inst, value});
		return value;
	}
	[[nodiscard]] auto begin() const { return m_items.begin(); }
	[[nodiscard]] auto end() const { return m_items.end(); }

private:
	[[nodiscard]] std::vector<Entry>::const_iterator LowerBound(const Inst* inst) const {
		return std::lower_bound(m_items.begin(), m_items.end(), inst,
		                        [](const Entry& entry, const Inst* key) {
			                        return std::less<const Inst*> {}(entry.first, key);
		                        });
	}

	std::vector<Entry> m_items;
};

// Nonnegative affine coefficients: element zero is constant, remaining elements are
// caller-selected leaves. Runtime range proofs must additionally exclude U32 wrap.
template <size_t Size>
std::optional<std::array<uint64_t, Size>> AffineIndex(
    Value value, const auto& resolve, const auto& leaf, uint32_t depth = 0) {
	value = resolve(value);
	if (depth > 32u || value.GetType() != Type::U32) return {};
	std::array<uint64_t, Size> result {};
	if (value.IsImmediate()) {
		result[0] = value.U32();
		return result;
	}
	if (const auto index = leaf(value)) {
		result[*index] = 1u;
		return result;
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) return {};
	const auto op = inst->GetOpcode();
	if (op != ValueOpcode::IAdd32 && op != ValueOpcode::IMul32 &&
	    op != ValueOpcode::ShiftLeftLogical32) return {};
	auto left = AffineIndex<Size>(inst->Arg(0), resolve, leaf, depth + 1u);
	auto right = AffineIndex<Size>(inst->Arg(1), resolve, leaf, depth + 1u);
	if (!left || !right) return {};
	const auto constant = [](const auto& coefficients) {
		return std::ranges::all_of(std::span(coefficients).subspan(1u),
		                          [](uint64_t coefficient) { return coefficient == 0u; });
	};
	if (op == ValueOpcode::IMul32 && !constant(*right)) std::swap(left, right);
	if (op != ValueOpcode::IAdd32 && !constant(*right)) return {};
	if (op == ValueOpcode::ShiftLeftLogical32) {
		if ((*right)[0] >= 32u) return {};
		(*right)[0] = uint64_t {1} << (*right)[0];
	}
	for (size_t index = 0; index < Size; ++index) {
		(*left)[index] = op == ValueOpcode::IAdd32
		    ? (*left)[index] + (*right)[index] : (*left)[index] * (*right)[0];
		if ((*left)[index] > UINT32_MAX) return {};
	}
	return left;
}

// One memoized evaluation session shared by the entire shader resource refresh.
class SrtWalker {
public:
	SrtWalker(const ResourcePlan& program, const SrtRuntime& runtime,
	          SrtWalker* clean_evaluator = nullptr);
	~SrtWalker();
	SrtWalker(const SrtWalker&)            = delete;
	SrtWalker& operator=(const SrtWalker&) = delete;

	bool Evaluate(Value value, uint32_t& result);
	bool EvaluateDescriptor(uint32_t source, DescriptorValue& result);
	// Refreshes reachable scalar reads and active descriptor sources in one walk.
	// A GPU-owned data slot reads 0, is listed in `gpu_owned_slots`, and the refresh refuses.
	bool RefreshFlatBuffer(std::vector<uint32_t>& flat, bool prune = true,
	                       SrtWalker*             conditions      = nullptr,
	                       std::vector<uint32_t>* gpu_owned_slots = nullptr);
	// Why the last refused Evaluate, EvaluateDescriptor or RefreshFlatBuffer refused.
	[[nodiscard]] SrtRefusal Refusal() const { return m_refusal; }

private:
	// Shared by a walker and every walker it starts.
	struct WalkState {
		SrtRefusal last     = SrtRefusal::None;
		uint64_t   refusals = 0;
		// GPU-owned reads: never remembered, and nothing that saw one is.
		uint64_t volatile_refusals = 0;
		// Visit order of in-progress values, and the earliest one a cycle reached.
		uint64_t visits     = 0;
		uint64_t cycle_root = UINT64_MAX;
	};
	struct Visit {
		uint64_t   order;
		uint64_t   volatile_refusals;
		uint64_t   outer_cycle_root;
		LaneScope* lane;
		bool       outer_dependent;
	};

	SrtWalker(SrtWalker& parent, Value active_mask, bool clean = false,
	          SrtWalker* clean_evaluator = nullptr, bool isolated = false);
	ResourcePlan::EvaluationContext& AcquireContext();
	void ResetUnknowns();
	static float Float32(uint64_t bits);
	bool                             Refuse(SrtRefusal reason);
	bool                             RefuseVolatile(SrtRefusal reason);
	[[nodiscard]] uint64_t           VolatileRefusals() const;
	[[nodiscard]] LaneScope*         DependenceScope() const {
		return m_lane != nullptr ? m_lane : m_suspended_lane;
	}
	Visit       BeginVisit(ResourcePlan::EvaluationContext::Entry& entry);
	void        EndVisit(const Visit& visit, uint32_t index, bool evaluated, uint64_t value);
	bool        Delegate(SrtWalker& walker, Value value, uint64_t& result);
	bool        EvaluateWide(Value value, uint64_t& result);
	bool        EvaluateChain(const Inst& root, uint64_t& result);
	const Inst* ColdPlainOperand(Value value, uint32_t& index);
	bool        Arg(const Inst& inst, size_t index, uint64_t& result);
	bool        EvaluatePhi(const Inst& inst, uint64_t& result);
	const ResourcePlan::PhiPlan* FrozenPhiPlan(const Inst& inst);
	bool                         EvaluateExtract(const Inst& inst, uint64_t& result);
	bool     EvaluateExtractU32x4(const Inst& inst, uint32_t component, uint64_t& result);
	bool     EvaluateRawRead(const Inst& inst, uint64_t& result, uint32_t component_bytes = 0u);
	bool     EvaluateBallot(const Inst& inst, uint64_t& result);
	void     BarAssumptions(SrtWalker& walker) const;
	uint32_t ClosedBallotIndex(const Inst& ballot);
	void     ForgetLaneValues();
	bool     EvaluateInst(const Inst& inst, uint64_t& result);
	bool     EvaluateRule(const Inst& inst, uint64_t& result);

	const ResourcePlan&              m_program;
	SrtRuntime                      m_runtime;
	SrtWalker*                      m_clean_evaluator = nullptr;
	Value                           m_active_mask;
	const std::vector<uint8_t>*      m_reachable_blocks = nullptr;
	const SrtWalker*                m_session;
	uint8_t                         m_read_policy;
	bool                             m_isolated = false;
	WalkState                        m_own_state;
	WalkState*                       m_state;
	ResourcePlan::EvaluationContext& m_context;
	SrtRefusal                       m_refusal = SrtRefusal::None;
	// Loop-carried phi values a fixpoint trial takes on trust, and phis no walk here may assume.
	InstValueMap m_assumed;
	InstSet      m_barred;
	// Set only inside a ballot's per-lane sweep; a flat-slot read or a readfirstlane suspends it.
	LaneScope* m_lane           = nullptr;
	LaneScope* m_suspended_lane = nullptr;
	// A walker reused across the lanes of one sweep forgets its lane-dependent values per lane.
	bool                  m_shares_lanes = false;
	std::vector<uint32_t> m_lane_entries;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_ */
