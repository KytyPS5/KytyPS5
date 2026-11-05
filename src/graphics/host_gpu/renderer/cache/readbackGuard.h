#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKGUARD_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKGUARD_H_

#include "common/common.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <utility>

namespace Libs::Graphics {

class PageManager;

/// The parts of an image that decide whether its readback can be held.
struct ReadbackShape {
	GuestRange data;
	uint32_t   levels = 0;
	uint32_t   layers = 0;
	uint32_t   depth  = 0;
	bool       tiled  = false;
};

/// Blocks guest access to a pending image readback until it lands, so a guest that reads the
/// image before the host GPU has finished waits for the data instead of seeing old bytes. Labels
/// are never held back, because a held label shows a game the host GPU's real speed.
class ReadbackGuard final {
public:
	/// How long a guest waits. A wait gives up only when the GPU stops making progress, because a
	/// long GPU stall is not a stuck readback.
	struct Limits {
		std::chrono::milliseconds wait_slice {250};
		std::chrono::milliseconds no_progress_timeout {2000};
		std::chrono::milliseconds absolute_timeout {30000};
		uint32_t                  consecutive_timeouts_to_stop = 3;
	};

	/// Why a wait ended. Only Landed means the guest will see the GPU's data.
	enum class WaitResult {
		NotPending, // Nothing pending covers the address, so the fault belongs to someone else.
		Landed,     // The readback landed while waiting.
		Lifted,     // Someone else lifted the hold. The guest sees old data.
		TimedOut,   // This wait lifted the hold. The guest sees old data.
	};

	/// Totals for measurement. A nonzero timeout count means guests read stale data.
	struct Counters {
		uint64_t holds       = 0;
		uint64_t waits       = 0;
		uint64_t wait_us     = 0;
		uint64_t wait_max_us = 0;
		uint64_t timeouts    = 0;
		uint64_t lifts       = 0;
		uint64_t refused     = 0;
		bool     stopped     = false;
	};

	/// Uses the default limits.
	/// @param progress returns a value that changes whenever the GPU makes progress. It runs with
	/// the guard's mutex held and on faulting threads, when a wait starts and after each wake, so
	/// it must not block or take locks that a landing needs.
	ReadbackGuard(PageManager& page_manager, std::function<uint64_t()> progress);
	/// Uses the given limits, which tests shorten.
	ReadbackGuard(PageManager& page_manager, std::function<uint64_t()> progress, Limits limits);

	KYTY_CLASS_NO_COPY(ReadbackGuard);

	/// Returns the whole pages of a readback that can be held, or an empty range. Only whole pages
	/// are held, because blocking a partial page would make a guest wait for unrelated data.
	[[nodiscard]] static GuestRange HoldRange(const ReadbackShape& shape) noexcept;

	/// Holds a page-aligned range from HoldRange. Returns false after repeated timeouts have
	/// stopped holds for this session, and then no Release follows.
	[[nodiscard]] bool Hold(GuestRange range);
	/// Ends one Hold of exactly this range after its readback landed, and wakes waiters.
	void Release(GuestRange range);

	/// Lifts protection covering an address without waiting, for threads that must never block.
	/// Returns whether anything was lifted. The later Release still arrives.
	bool LiftAt(uint64_t vaddr);
	/// Lifts protection overlapping a range, and wakes waiters. Used before the range is unmapped.
	void LiftRange(uint64_t address, uint64_t size);
	/// Lifts all protection, and wakes waiters. Used at shutdown.
	void LiftAll();

	/// Returns whether a protected range covers an address, without waiting.
	[[nodiscard]] bool Covers(uint64_t vaddr);

	/// Waits until nothing pending covers an address. The wait path neither allocates nor logs,
	/// because it runs inside a fault handler on a possibly small stack.
	WaitResult WaitAt(uint64_t vaddr) noexcept;
	/// Waits until nothing pending overlaps a range, for host calls that can't take a fault.
	WaitResult WaitRange(uint64_t address, uint64_t size) noexcept;

	/// Cheap check that needs no lock, so faults unrelated to readbacks stay fast.
	[[nodiscard]] bool HasPending() const noexcept {
		return m_protected_ranges.load(std::memory_order_acquire) != 0;
	}

	/// Returns how many threads are waiting right now.
	[[nodiscard]] uint32_t ActiveWaiters() const noexcept {
		return m_active_waiters.load(std::memory_order_acquire);
	}

	/// Returns the totals so far.
	[[nodiscard]] Counters GetCounters();
	/// Returns how many ranges still await a landing, including lifted ones.
	[[nodiscard]] size_t TrackedRanges();

private:
	struct Pending {
		uint32_t refs       = 0;
		bool     protected_ = false;
	};
	// Keyed by the exact range, because page counts must be released with the range they were
	// taken with.
	using RangeKey = std::pair<uint64_t, uint64_t>;

	/// Returns whether any protected range overlaps. Needs the mutex.
	[[nodiscard]] bool AnyProtectedOverlap(uint64_t address, uint64_t size) const noexcept;
	/// Returns whether a lifted range awaiting its landing overlaps. Needs the mutex.
	[[nodiscard]] bool AnyLiftedOverlap(uint64_t address, uint64_t size) const noexcept;
	/// Removes one range's protection and counts it as lifted. Needs the mutex.
	void LiftEntry(const RangeKey& key, Pending& pending);
	/// Lifts each protected range overlapping, and returns whether it lifted any. Needs the mutex.
	bool LiftOverlapping(uint64_t address, uint64_t size);
	/// Lifts every protected range. Needs the mutex.
	void LiftEverything();
	/// Waits for an overlap to clear, lifting it if the limits expire.
	WaitResult WaitOverlap(uint64_t address, uint64_t size) noexcept;
	/// Updates the totals and the timeout streak after a wait. Needs the mutex.
	void RecordWait(WaitResult result, uint64_t waited_us);

	PageManager&                m_page_manager;
	std::function<uint64_t()>   m_progress;
	Limits                      m_limits;
	std::mutex                  m_mutex;
	std::condition_variable     m_landed;
	std::map<RangeKey, Pending> m_pending;
	std::atomic<uint32_t>       m_protected_ranges {0};
	std::atomic<uint32_t>       m_active_waiters {0};
	uint32_t                    m_consecutive_timeouts = 0;
	bool                        m_stop_logged          = false;
	Counters                    m_counters;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKGUARD_H_ */
