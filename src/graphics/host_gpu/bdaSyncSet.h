#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_BDASYNCSET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_BDASYNCSET_H_

#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

namespace Libs::Graphics {

// The guest bytes whose host buffer copy may be stale.
//
// A shader that dereferences a raw guest pointer can reach any mapped byte, so before such a
// draw or dispatch every buffer the pointer could land in has to be current. PrepareBda used to
// establish that by walking every mapped range every time. This set is the same obligation kept
// as state: it is appended where memory actually changes hands and drained by the per-draw path.
//
// Two ordering rules carry the whole safety argument:
//
//   * Queue runs AFTER the state change it reports, never before. A queue that lands before its
//     dirty bit can be swallowed by a drain that has already walked past that page, and the page
//     would then never be uploaded - it reads as zero on the GPU and its stores are dropped.
//   * Take removes the whole set in one step, so anything queued from that moment on lands in
//     the fresh set and is drained next time. Being wrong in this direction costs one redundant
//     walk, which is why it is the direction to be wrong in.
class BdaSyncSet final {
public:
	void Queue(uint64_t vaddr, uint64_t size) {
		if (size == 0) {
			return;
		}
		if (RoundToPages()) {
			const auto begin = vaddr & ~(TRACKER_PAGE_SIZE - 1);
			const auto end   = (vaddr + size + TRACKER_PAGE_SIZE - 1) & ~(TRACKER_PAGE_SIZE - 1);
			vaddr            = begin;
			size             = end - begin;
		}
		std::lock_guard lock(m_mutex);
		m_pending.Add(vaddr, size);
	}

	[[nodiscard]] static bool RoundToPages() {
		static const bool enabled = [] {
			const char* text = std::getenv("KYTY_NO_BDA_SYNC_PAGES");
			return text == nullptr || text[0] == '\0' || std::strcmp(text, "0") == 0;
		}();
		return enabled;
	}

	// Only for memory that can no longer be reached at all - an unmapped range. Dropping a range
	// that is still mapped would strand its dirty pages.
	//
	// Whole tracker pages only, and only ones that lie entirely inside the range. Dirtiness is a
	// per-page fact: an unmap that covered half a page would dirty all of it and then drop the
	// half that is still mapped along with the half that is not. Guest unmaps are 16 KiB aligned
	// today and cannot split a 4 KiB tracker page, but nothing in this class should depend on it.
	void Drop(uint64_t vaddr, uint64_t size) {
		if (size == 0) {
			return;
		}
		const auto begin = (vaddr + TRACKER_PAGE_SIZE - 1) & ~(TRACKER_PAGE_SIZE - 1);
		const auto end   = (vaddr + size) & ~(TRACKER_PAGE_SIZE - 1);
		if (begin >= end) {
			return;
		}
		std::lock_guard lock(m_mutex);
		m_pending.Subtract(begin, end - begin);
	}

	[[nodiscard]] RangeSet Take() {
		std::lock_guard lock(m_mutex);
		return std::exchange(m_pending, RangeSet {});
	}

	[[nodiscard]] bool Empty() const {
		std::lock_guard lock(m_mutex);
		return m_pending.Empty();
	}

	// Equivalence checking only: a copy, so a drain can be compared against a full walk without
	// consuming the set.
	[[nodiscard]] RangeSet Peek() const {
		std::lock_guard lock(m_mutex);
		return m_pending;
	}

private:
	mutable std::mutex m_mutex;
	RangeSet           m_pending;
};

// The synchronise half of PrepareBda, in its two forms. Both hand `sync` half-open ranges in
// ascending address order; the incremental form omits exactly the ranges on which the full form
// provably does nothing, because nothing outside the queued set is CPU-dirty and no region
// outside it is missing.
template <typename Sync>
void ForEachBdaSyncRangeFull(const RangeSet& mapped, Sync&& sync) {
	mapped.ForEach([&](uint64_t begin, uint64_t end) { sync(begin, end); });
}

template <typename Sync>
void ForEachBdaSyncRange(const RangeSet& pending, const RangeSet& mapped, Sync&& sync) {
	pending.ForEach([&](uint64_t begin, uint64_t end) {
		mapped.ForEachInRange(begin, end - begin,
		                      [&](uint64_t start, uint64_t finish) { sync(start, finish); });
	});
}

// The mark half, with the nesting inverted: the tracked set bounds the work instead of the
// mapped set. The intervals produced are the same intersection in the same ascending order as
// walking the mapped set outside, at one lookup per tracked range instead of one per mapped
// range - and the mapped set is the larger of the two by three orders of magnitude.
template <typename Mark>
void ForEachBdaMarkRange(const RangeSet& tracked, const RangeSet& mapped, Mark&& mark) {
	tracked.ForEach([&](uint64_t begin, uint64_t end) {
		mapped.ForEachInRange(begin, end - begin,
		                      [&](uint64_t start, uint64_t finish) { mark(start, finish); });
	});
}

// Queues a range when it goes out of scope, so the queue cannot precede the state change it
// reports.
//
// The ordering rule above is the whole safety argument, and it is not covered by any test: the
// differential test in MemoryTrackerTests drives both worlds from one thread, where moving a queue
// above its state change changes nothing. It was verified by inverting one queue and watching the
// suite still pass. So the rule is enforced here instead - a scope-exit queue happens after every
// state change in its scope no matter where the declaration is written or how a future merge moves
// it.
//
// Declare it *after* any early return that means no state changed. Getting that wrong queues a
// range that did not need it, which costs one redundant walk and is the direction this class is
// designed to be wrong in; getting the explicit-call ordering wrong loses an upload permanently.
class BdaSyncOnExit final {
public:
	BdaSyncOnExit(BdaSyncSet& set, uint64_t vaddr, uint64_t size)
	    : m_set(set), m_vaddr(vaddr), m_size(size) {}
	~BdaSyncOnExit() { m_set.Queue(m_vaddr, m_size); }

	BdaSyncOnExit(const BdaSyncOnExit&)            = delete;
	BdaSyncOnExit& operator=(const BdaSyncOnExit&) = delete;
	BdaSyncOnExit(BdaSyncOnExit&&)                 = delete;
	BdaSyncOnExit& operator=(BdaSyncOnExit&&)      = delete;

private:
	BdaSyncSet& m_set;
	uint64_t    m_vaddr;
	uint64_t    m_size;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_BDASYNCSET_H_
