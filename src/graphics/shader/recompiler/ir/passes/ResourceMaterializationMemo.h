#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATIONMEMO_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATIONMEMO_H_

#include "graphics/shader/recompiler/ir/ResourceSnapshot.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Replays a stage's recorded guest reads: a replay yields exactly what a walk would.
class MaterializeMemo {
public:
	// Only plain descriptors and resident image heap roots are memoized.
	[[nodiscard]] static bool Supports(const ResourcePlan& plan);

	// The runtime to walk with: it forwards every callback and records the guest reads.
	[[nodiscard]] SrtRuntime Record(const SrtRuntime& runtime);
	// Keeps the recording after a successful walk, or forgets the memo if it cannot be replayed.
	void Commit(const ResourcePlan& plan, const ResourceSnapshot& snapshot,
	            const ResourceSpecialization& specialization);
	void Invalidate();
	bool Replay(const ResourcePlan& plan, const SrtRuntime& runtime, ResourceSnapshot& snapshot,
	            ResourceSpecialization* specialization = nullptr);
	// Shares another recording's per-plan analysis instead of deriving it again.
	void               AdoptAnalysis(const MaterializeMemo& other);
	[[nodiscard]] bool Valid() const { return m_valid; }

private:
	enum class Reader : uint8_t { Memory, Specialization, Condition };

	struct Read {
		Reader                  reader          = Reader::Memory;
		uint32_t                component_bytes = 0;
		const Inst*             inst            = nullptr;
		uint64_t                address         = 0;
		uint32_t                word            = 0;
		std::array<uint64_t, 5> operands {};
		uint32_t                slot = UINT32_MAX;
		// The reader refused; the walk took its fallback, so a replay must see it refuse again.
		bool refused = false;
		// Outside its descriptor: the walk read 0 without calling a reader.
		bool zero = false;
	};

	// A postfix program that re-derives one operand from user data and earlier words.
	struct Op {
		enum Kind : uint8_t {
			Imm,
			UserData,
			Key,
			Add32,
			Sub32,
			Mul32,
			And32,
			Or32,
			Shl32,
			Shr32,
			ULess32,
			LogicalOr,
			Select
		};
		Kind     kind  = Imm;
		uint32_t index = 0;
		uint64_t imm   = 0;
	};

	struct Step {
		uint32_t key = 0;
		// Operands with a program are re-derived; the rest keep their recorded value.
		std::array<uint32_t, 5> op_begin {};
		std::array<uint32_t, 5> op_end {};
		uint8_t                 derived_mask = 0;
		bool                    structural   = true;
		// The walk asks gpu_owned before this read and refuses it when the GPU owns the bytes.
		bool     ownership_checked = false;
		bool     refused           = false;
		uint32_t slot_begin        = 0;
		uint32_t slot_end          = 0;
		// The snapshot.specialization_reads entry this read made, or UINT32_MAX.
		uint32_t capture = UINT32_MAX;
	};

	// One plain resource descriptor whose dwords all re-derive from user data and re-read words.
	struct DescriptorPatch {
		uint32_t                source      = 0;
		uint32_t                dword_count = 0;
		std::array<uint32_t, 8> op_begin {};
		std::array<uint32_t, 8> op_end {};
		// The resources bound from `source`, as a range of m_patch_targets.
		uint32_t target_begin = 0;
		uint32_t target_end   = 0;
	};

	// One buffer, image or sampler binding a patched descriptor feeds.
	struct PatchTarget {
		enum Kind : uint8_t { Buffer, Image, Sampler };
		Kind     kind    = Buffer;
		bool     written = false;
		uint32_t index   = 0;
	};

	struct HotStep {
		uint64_t                address = 0;
		std::array<uint32_t, 5> operands {};
		uint32_t                word          = 0;
		uint32_t                key           = 0;
		uint32_t                operand_begin = 0;
		uint32_t                capture       = UINT32_MAX;
		uint32_t                slot_begin    = 0;
		uint32_t                slot_end      = 0;
		int32_t                 immediate     = 0;
		uint8_t                 kind          = 0;
		uint8_t                 operand_count = 0;
		uint8_t                 flags         = 0;
		uint8_t                 reader        = 0;

		static constexpr uint8_t Structural       = 1u;
		static constexpr uint8_t OwnershipChecked = 2u;
		static constexpr uint8_t Refused          = 4u;
		static constexpr uint8_t FirstKey         = 8u;
		static constexpr uint8_t ShapeOk          = 16u;
		static constexpr uint8_t Zero             = 32u;
	};
	struct HotOperand {
		uint32_t begin   = 0;
		uint32_t end     = 0;
		uint32_t operand = 0;
		// Index into m_shared_values: reads whose programs are identical share one evaluation.
		uint32_t shared = 0;
	};

	// One heap root's acquisition: a replay acquires it again and needs the same kinds.
	struct HeapRoot {
		ImageHeapRequest             request;
		std::vector<DescriptorValue> kinds;
		uint32_t                     mapping    = 0;
		uint64_t                     slots      = 0;
		uint32_t                     slot_count = 0;
	};

	// Forwards the walk's heap acquisitions and keeps each one for Commit.
	class HeapRecorder final: public ImageHeapSource {
	public:
		bool Acquire(const ImageHeapRequest& request, ImageHeapView& view) override;

		ImageHeapSource*      source = nullptr;
		std::vector<HeapRoot> acquired;
	};

	struct Analysis;

	static bool ReadMemory(void* userdata, uint64_t address, std::span<uint32_t> values);
	static bool ReadSpecialization(void* userdata, uint64_t address, std::span<uint32_t> values);
	static bool ReadCondition(void* userdata, uint64_t address, std::span<uint32_t> values);
	static uint64_t ReadableExtent(void* userdata, uint64_t address, uint64_t size);
	static bool     ReadBlock(void* userdata, uint64_t address, void* data, uint64_t size);
	static bool     HashBlock(void* userdata, uint64_t address, uint64_t size, uint64_t* hash);
	static bool     GpuOwned(void* userdata, uint64_t address, uint64_t size);
	static void     WalkRestarted(void* userdata);
	static void     ZeroRead(void* userdata);
	bool Note(Reader reader, bool read, uint64_t address, std::span<const uint32_t> values);
	bool Build(const ResourcePlan& plan, const ResourceSnapshot& snapshot);
	bool CanDerive(const ResourcePlan& plan, Value value, uint32_t before) const;
	void Compile(const ResourcePlan& plan, Value value);
	template <typename Stack>
	static void            ApplyOp(Op::Kind kind, Stack& stack, uint32_t& top);
	[[nodiscard]] uint64_t Evaluate(uint32_t begin, uint32_t end,
	                                std::span<const uint32_t> user_data) const;
	[[nodiscard]] uint64_t EvaluateScratch(uint32_t begin, uint32_t end,
	                                       std::span<const uint32_t> user_data) const;
	[[nodiscard]] uint64_t SharedOperand(const HotOperand&         derived,
	                                     std::span<const uint32_t> user_data);
	[[nodiscard]] bool     SameShape(const ResourcePlan& plan) const;
	[[nodiscard]] uint32_t KeyOf(Value value) const;
	void                   BuildHot(const ResourcePlan& plan);
	bool                   ReplayReads(const SrtRuntime& runtime, bool moved, bool& captured_moved);
	bool                   CommitHeaps(const ResourcePlan& plan, const ResourceSnapshot& snapshot,
	                                   const ResourceSpecialization& specialization);
	bool                   AcquireHeaps(const SrtRuntime& runtime);
	void Pin(const ResourcePlan& plan, Value value, std::vector<uint8_t>& structural,
	         std::vector<const Inst*>& seen);
	bool PatchDescriptors(const ResourcePlan& plan, const SrtRuntime& runtime,
	                      ResourceSnapshot& snapshot, ResourceSpecialization* specialization,
	                      bool captured_moved);
	[[nodiscard]] bool CapturedReadsOverlapWrites(const ResourcePlan&     plan,
	                                              const ResourceSnapshot& descriptors) const;

	SrtRuntime        m_source;
	std::vector<Read> m_reads;
	HeapRecorder          m_heap_recorder;
	std::vector<HeapRoot> m_heaps;
	// A heap root's table bypasses its selector only while no written buffer overlaps the selector.
	bool              m_heap_selector = false;
	bool              m_replayable = false;
	// The materialization walked twice; see WalkRestarted.
	bool m_restarted       = false;
	bool m_built_restarted = false;

	const ResourcePlan*                    m_plan = nullptr;
	std::shared_ptr<Analysis>              m_analysis;
	std::vector<Read>                      m_recorded;
	std::vector<Step>                      m_steps;
	std::vector<uint32_t>                  m_slots;
	std::unordered_map<uint64_t, uint32_t> m_keys;
	std::vector<uint32_t>                  m_key_first;
	std::vector<uint8_t>                   m_fixed_registers;
	std::vector<uint32_t>                  m_user_data;
	uint64_t                               m_shader_base       = 0;
	bool                                   m_capture_addresses = false;
	// Every captured range is one recorded read, so a relocated read can move its range too.
	bool                          m_capture_mapped = false;
	bool                          m_valid          = false;
	bool                          m_shape_ok       = false;
	mutable std::vector<uint32_t> m_key_words;
	std::vector<uint32_t>         m_new_words;
	std::vector<uint64_t>         m_new_addresses;
	std::vector<Op>               m_ops;
	std::vector<uint8_t>          m_visited_blocks;
	std::vector<DescriptorPatch>  m_patches;
	std::vector<PatchTarget>      m_patch_targets;
	std::vector<HotStep>          m_hot;
	std::vector<HotOperand>       m_hot_operands;
	// One value per distinct operand program, valid while its stamp equals m_shared_epoch.
	std::vector<uint64_t> m_shared_values;
	std::vector<uint32_t> m_shared_stamps;
	uint32_t              m_shared_epoch = 0;

public:
	MaterializeMemo();
	~MaterializeMemo();
	MaterializeMemo(MaterializeMemo&&) noexcept;
	MaterializeMemo& operator=(MaterializeMemo&&) noexcept;
};

// A few recordings per cached stage, so a stage alternating between bindings replays each.
class MaterializeMemoWays {
public:
	// Replays the matching recording into `snapshot` and `specialization`.
	bool Replay(const ResourcePlan& plan, const SrtRuntime& runtime, ResourceSnapshot& snapshot,
	            ResourceSpecialization& specialization);
	[[nodiscard]] SrtRuntime Record(const SrtRuntime& runtime);
	void                     Commit(const ResourcePlan& plan, const ResourceSnapshot& snapshot,
	                                const ResourceSpecialization& specialization);
	[[nodiscard]] bool       Valid() const;
	// The caller walked without recording, so `snapshot` holds no way's state any more.
	void ForgetCurrent();

private:
	static constexpr uint32_t MaxWays = 8;

	struct Way {
		MaterializeMemo        memo;
		ResourceSnapshot       snapshot;
		ResourceSpecialization specialization;
		uint64_t               used = 0;
	};

	std::vector<Way> m_ways;
	uint32_t         m_current   = UINT32_MAX;
	uint32_t         m_recording = UINT32_MAX;
	uint64_t         m_clock     = 0;
};

// Skips the memo for a while once a stage keeps missing.
class MaterializeMemoBackoff {
public:
	static constexpr uint32_t Threshold     = 8;
	static constexpr uint32_t InitialWindow = 16;
	static constexpr uint32_t MaxWindow     = 512;

	// Whether this call should skip the memo entirely.
	bool Skip() {
		if (m_skip == 0) {
			return false;
		}
		--m_skip;
		return true;
	}
	void OnHit() {
		m_misses = 0;
		m_tries  = 0;
		m_window = InitialWindow;
	}
	void OnMiss() {
		if (++m_misses < Threshold || ++m_tries < 2) {
			return;
		}
		m_tries  = 0;
		m_skip   = m_window;
		m_window = std::min(m_window * 2u, MaxWindow);
	}

private:
	uint32_t m_misses = 0;
	uint32_t m_tries  = 0;
	uint32_t m_skip   = 0;
	uint32_t m_window = InitialWindow;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATIONMEMO_H_
