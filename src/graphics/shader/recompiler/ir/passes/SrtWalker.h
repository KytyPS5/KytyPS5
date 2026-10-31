#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <span>

namespace Libs::Graphics::ShaderRecompiler::IR {

using SrtMemoryReader = bool (*)(void* userdata, uint64_t address, std::span<uint32_t> values);
// flat_slot: the flattened SRT dword that is the load's only use, else NoFlatSlot.
using SrtLoadRecorder = void (*)(void* userdata, uint64_t address, uint32_t value,
                                 uint32_t flat_slot);
inline constexpr uint32_t NoFlatSlot = UINT32_MAX;
// A load whose value is only one descriptor word with bits the execution thread may refresh
// (DescriptorRefreshMask()) reports DescriptorSlot(source, word) instead of a flat slot.
inline constexpr uint32_t DescriptorSlotFlag = 0x80000000u;
constexpr uint32_t DescriptorSlot(uint32_t source, uint32_t word) {
	return DescriptorSlotFlag | (source << 3u) | word;
}
constexpr bool IsDescriptorSlot(uint32_t slot) {
	return slot != NoFlatSlot && (slot & DescriptorSlotFlag) != 0;
}
constexpr uint32_t DescriptorSlotSource(uint32_t slot) {
	return (slot & ~DescriptorSlotFlag) >> 3u;
}
constexpr uint32_t DescriptorSlotWord(uint32_t slot) {
	return slot & 7u;
}

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

// The candidates of a sampled root that selects descriptors through records (tables that
// GPU-chosen records name, see ResourceMaterialization): the host keeps them resident in device
// slot words, a leading null slot and then one slot per candidate, as a table view keeps its
// table's descriptors, so the host can replace an element whose image changed.
struct ImageHeapList {
	const void*                      owner    = nullptr; // With resource, names the root's list.
	uint32_t                         resource = 0;
	const ImageResource*             root     = nullptr;
	std::span<const DescriptorValue> values;
};

class ImageHeapSource {
public:
	// False when the host does not keep the table resident.
	virtual bool Acquire(const ImageHeapRequest& request, ImageHeapView& view) = 0;
	// Fills view: value i selects slot i + 1, slot 0 and the slots past the values are null. False
	// when the host does not keep the descriptors resident.
	virtual bool AcquireList(const ImageHeapList& /*list*/, ImageHeapView& /*view*/) {
		return false;
	}

protected:
	~ImageHeapSource() = default;
};

struct SrtRuntime {
	std::span<const uint32_t> user_data;
	uint64_t                  shader_base                = 0;
	void*                     userdata                   = nullptr;
	SrtMemoryReader           read_specialization_memory = nullptr;
	std::span<const uint32_t> workgroup_counts;
	uint64_t                  indirect_args_address = 0;
	ImageHeapSource*          image_heap            = nullptr;
	// A walk ahead of execution: a guest load that faults fails the walk instead of resolving
	// the fault (GPU-written bytes cannot be downloaded there), and the walk reports every load
	// above the null page with the value it read. A load whose value only fills a flattened SRT
	// dword reports that dword: nothing else the walk resolved depends on its bytes.
	SrtLoadRecorder           record_load           = nullptr;
};

class SrtReadCapture {
public:
	SrtReadCapture(SrtRuntime source, std::vector<std::pair<uint64_t, uint64_t>>& ranges):
	    m_source(source), m_ranges(ranges) {}
	SrtRuntime ObservedRuntime();

private:
	static bool Read(void* userdata, uint64_t address, std::span<uint32_t> values);

	SrtRuntime                                  m_source;
	std::vector<std::pair<uint64_t, uint64_t>>& m_ranges;
};

enum class RuntimeValueType { Any, Integer, ImmutableInteger };

bool ValidateRuntimeValue(const ResourcePlan& program, Value value,
                          RuntimeValueType type = RuntimeValueType::Any);
// Uniform across a compute wave, including values derived from its workgroup ID.
// This does not imply that the host can evaluate the value.
bool ValidateWaveUniformValue(const ResourcePlan& program, Value value);
// Compiles the plan's x86 walker. Without values it walks the plan's resources; otherwise it
// only evaluates the values, with every block reachable.
void CompileSrtWalker(ResourcePlan& plan, std::span<const Value> values = {});
// Runs the walker. flat receives the scalar reads of reachable blocks and reads, when non-null,
// one DWORD range per guest load. False when a reachable read is unknown.
bool WalkSrt(const ResourcePlan& plan, const SrtRuntime& runtime, std::vector<uint32_t>& flat,
             std::vector<std::pair<uint64_t, uint64_t>>* reads);
// Results of the last walk; false when unknown.
bool WalkedValue(const ResourcePlan& plan, Value value, uint32_t& result);
bool WalkedDescriptor(const ResourcePlan& plan, uint32_t source, DescriptorValue& result);
bool WalkedActive(const ResourcePlan& plan, uint32_t source);
bool WalkedImageActive(const ResourcePlan& plan, uint32_t image);
// How walker loads read a 4 KiB guest page: directly, from bytes the emulator points to, or by
// asking the emulator for each dword.
enum class SrtWalkerPage : uint8_t { Direct, Backing, PerLoad };
// Set by the emulator; without it every load is direct. `ahead` tells a walk ahead of execution.
struct SrtWalkerGuestMemory {
	// The page holding `address`: Direct when a direct load reads it right (or faults as it
	// should), Backing when `backing` points to the bytes every load must read, PerLoad otherwise
	// (`backing` may then point to page bytes for `read`). The answer holds for the rest of the
	// walk.
	SrtWalkerPage (*page)(uint64_t address, bool ahead, const uint8_t*& backing) = nullptr;
	// Reads the dword at an address of a PerLoad page that a direct load may fault on (a page the
	// GPU owns), without faulting; false when the walker should load it directly.
	bool (*read)(uint64_t address, uint32_t& value, bool ahead, const uint8_t* backing) = nullptr;
};
void SetSrtWalkerGuestMemory(const SrtWalkerGuestMemory& memory);
// Length of the guest load at rip if it belongs to the walker running on this thread, else 0.
// The exception handler completes such a load as a zero read.
uint32_t SrtWalkerLoadLength(uint64_t rip);
// As SrtWalkerLoadLength, for a fault the walker on this thread must not resolve: a walk ahead
// of execution, which then fails. Loads below guest memory read zero in any walk.
uint32_t SrtWalkerAheadLoadLength(uint64_t rip, uint64_t address);

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

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_ */
