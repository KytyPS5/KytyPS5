#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CX_COMPAT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CX_COMPAT_H_

// What chenxiao07's table path (tableXpr.inc, nativeXpr.inc) reads from his tree's live control, counters and
// speculation layers, in the form this tree has them: the switches as plain atomics read from KYTY_* once at boot
// (RenderExecutor::TableLoadSwitches), and his counters, census and speculation (KYTY_SPECULATE=4, which he does not
// have on by default), which are not part of this port: their calls compile to nothing.

#include <atomic>
#include <cstdint>
#include <string_view>

extern "C++" {
// His performance-switches.h names: each an atomic.
inline volatile std::atomic_uint32_t kyty_local_table_xpr_mode {2};
inline volatile std::atomic_uint32_t kyty_local_table_dispatch_mode {0};
inline volatile std::atomic_uint32_t kyty_local_table_indirect_mode {1};
inline volatile std::atomic_uint32_t kyty_local_table_store_budget {32};
inline volatile std::atomic_uint32_t kyty_local_table_set_proofs {1};
inline volatile std::atomic_uint32_t kyty_local_table_mru {7};
inline volatile std::atomic_uint32_t kyty_local_table_target_read {0};
inline volatile std::atomic_uint32_t kyty_local_table_gather_memo {1};
inline volatile std::atomic_uint32_t kyty_local_table_memo {1};
inline volatile std::atomic_uint32_t kyty_local_table_memo_verify {0};
inline volatile std::atomic_uint32_t kyty_local_table_serials {0}; // (ImageSerials are not ported: the epochs decide)
// (1: a draw that reads what the draw before wrote as storage gets its barrier, the order of overlapping decals holds.)
inline volatile std::atomic_uint32_t kyty_local_draw_read_after_write {1};
inline volatile std::atomic_uint32_t kyty_local_speculate_mode {0};
inline volatile std::atomic_uint32_t kyty_local_speculations_live {0};
}

extern "C" {
extern volatile std::atomic_uint32_t kyty_local_scout_mode; // (scout.cpp)
}

namespace Libs::Graphics {

// --- speculation (not ported) -----------------------------------------------------------------------------------------

namespace Spec {
struct State {
	const char* refused = nullptr;
	void        NoteSet(uint64_t) {}
	void        NoteBufferRange(uint64_t, uint64_t) {}
	void        NoteCleanRead(uint64_t, uint64_t) {}
	bool        JournalIntersects(uint64_t, uint64_t) const { return false; }
};
[[nodiscard]] inline State* Current() noexcept { return nullptr; }
[[nodiscard]] inline bool   On() noexcept { return false; }
// (His census of the refusals by reason.)
inline void Refuse(const char*) noexcept {}
} // namespace Spec

// --- his counters and census (not ported) -----------------------------------------------------------------------------

namespace LiveCounters {
enum Id : uint32_t {
	TableDrawNative, TableStoreDeferred, TableStoreTargets, TableRingSharedBytes, TableRingShared, TableRegistered,
	SerialsSkipped, SerialsExtra, SerialsUnsafe, TableStoreVariant, TableRingCopies, TableRingBytes, TableRefusedSets,
	TableProofMismatches, TableNoSet, TableMemoVerified, TableMemoMismatch, TableMemoHits, TableGatherMemoHits,
	TableEvaluations, TableDraws, TableDispatches, TableContinued,
	ScoutMissUnwalked, ScoutMissTicket, ScoutMissNotNative, ScoutMissContinuation, ScoutMissNotTable, ScoutMissConstant,
	ScoutMissBehind, ScoutMissNoViewEmpty, ScoutMissNoViewOther, ScoutMissRefused, ScoutMissShort, ScoutMissTooBig,
	ScoutMissFenced,
	ScoutUsedBacking, ScoutUsed, ScoutMismatchWords, ScoutMismatchSlots, ScoutMismatchRefused, ScoutMismatchLod,
	ScoutMismatches, ScoutMismatchEntries, ScoutMismatchApply, ScoutLostWritten, ScoutLostWriteBack, ScoutLostWatched,
	ScoutLostRegisters, ScoutLostPair, ScoutLostNoView, ScoutLostMissing, ScoutLostIdentity, ScoutLostEpoch,
	ScoutLostDirty, ScoutLostChanged, ScoutChecks, ScoutApplySegments, ScoutApplyRuns,
	Count
};
inline void Add(Id, uint64_t = 1) noexcept {}
} // namespace LiveCounters

namespace LiveCensus {
enum Kind : uint32_t { DrawPhase };
struct Scope {
	Scope(Kind, uint32_t, uint32_t) noexcept {}
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;
};
} // namespace LiveCensus

// The image serial sums of KYTY_TABLE_SERIALS (his image.h ImageSerials): not ported, so the switch stays 0.
namespace ImageSerials {
inline std::atomic<bool> g_on {false};
[[nodiscard]] inline uint64_t Unattributed() noexcept { return 0; }
[[nodiscard]] inline uint64_t Read(uint32_t) noexcept { return 0; }
} // namespace ImageSerials

namespace LiveSwitch {
inline std::atomic<int64_t>& Find(std::string_view, int64_t fallback) {
	static std::atomic<int64_t> value {0};
	value.store(fallback, std::memory_order_relaxed);
	return value;
}
} // namespace LiveSwitch

namespace SlowLog {
inline double HitchThreshold() noexcept { return 0.0; }
} // namespace SlowLog

namespace FrameGen {
inline bool Enabled() noexcept { return false; }
inline bool WantsCamera() noexcept { return false; }
inline bool IsTaaShader(uint64_t) noexcept { return false; }
inline bool IsGbufferVertexShader(uint64_t) noexcept { return false; }
inline void NoteScene() noexcept {}
} // namespace FrameGen

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CX_COMPAT_H_ */
