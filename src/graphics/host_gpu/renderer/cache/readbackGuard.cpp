#include "graphics/host_gpu/renderer/cache/readbackGuard.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/pageManager.h"

#include <algorithm>

namespace Libs::Graphics {

// Bigger readbacks, such as a photo mode frame, keep their old behaviour. Protecting them for every
// frame costs more than these rare reads gain.
static constexpr uint64_t HOLD_MAX_BYTES = 1024ull * 1024ull;
static constexpr uint64_t ONE_BYTE       = 1;

/// Returns whether two ranges share a byte, without overflowing near the top of the address space.
/// An empty range overlaps nothing.
static constexpr bool RangesOverlap(uint64_t first_address, uint64_t first_size,
                                    uint64_t second_address, uint64_t second_size) noexcept {
	if (first_size == 0 || second_size == 0) {
		return false;
	}
	return first_address <= second_address ? second_address - first_address < first_size
	                                       : first_address - second_address < second_size;
}

ReadbackGuard::ReadbackGuard(PageManager& page_manager, std::function<uint64_t()> progress)
    : ReadbackGuard(page_manager, std::move(progress), Limits {}) {}

ReadbackGuard::ReadbackGuard(PageManager& page_manager, std::function<uint64_t()> progress,
                             Limits limits)
    : m_page_manager(page_manager), m_progress(std::move(progress)), m_limits(limits) {
	// An empty function would terminate inside a fault handler, far from this mistake.
	EXIT_IF(!m_progress);
}

GuestRange ReadbackGuard::HoldRange(const ReadbackShape& shape) noexcept {
	if (shape.tiled || shape.levels != 1 || shape.layers != 1 || shape.depth != 1 ||
	    shape.data.size > HOLD_MAX_BYTES) {
		return {};
	}
	const auto begin = Common::AlignUp(shape.data.address, TRACKER_PAGE_SIZE);
	const auto end   = Common::AlignDown(shape.data.End(), TRACKER_PAGE_SIZE);
	return begin < end ? GuestRange {begin, end - begin} : GuestRange {};
}

bool ReadbackGuard::Hold(GuestRange range) {
	EXIT_IF(range.Empty() || range.address % TRACKER_PAGE_SIZE != 0 ||
	        range.size % TRACKER_PAGE_SIZE != 0);
	bool log_stop = false;
	{
		std::lock_guard lock(m_mutex);
		if (m_counters.stopped) {
			m_counters.refused++;
			log_stop      = !m_stop_logged;
			m_stop_logged = true;
		} else {
			m_counters.holds++;
			auto& pending = m_pending[RangeKey {range.address, range.size}];
			pending.refs++;
			if (!pending.protected_) {
				// The count rises first, so a fault as soon as the page is blocked takes the slow
				// path and finds this hold instead of skipping the guard.
				m_protected_ranges.fetch_add(1, std::memory_order_acq_rel);
				m_page_manager.AddReadbackHold(range.address, range.size);
				pending.protected_ = true;
			}
			return true;
		}
	}
	if (log_stop) {
		LOGF("ReadbackGuard: holds stopped for this session after repeated wait timeouts\n");
	}
	return false;
}

void ReadbackGuard::Release(GuestRange range) {
	{
		std::lock_guard lock(m_mutex);
		const auto      found = m_pending.find(RangeKey {range.address, range.size});
		EXIT_IF(found == m_pending.end());
		if (--found->second.refs != 0) {
			return;
		}
		if (found->second.protected_) {
			m_page_manager.RemoveReadbackHold(found->first.first, found->first.second);
			m_protected_ranges.fetch_sub(1, std::memory_order_acq_rel);
		}
		m_pending.erase(found);
	}
	m_landed.notify_all();
}

void ReadbackGuard::LiftEntry(const RangeKey& key, Pending& pending) {
	m_page_manager.RemoveReadbackHold(key.first, key.second);
	pending.protected_ = false;
	m_protected_ranges.fetch_sub(1, std::memory_order_acq_rel);
	m_counters.lifts++;
}

bool ReadbackGuard::LiftOverlapping(uint64_t address, uint64_t size) {
	bool lifted = false;
	for (auto& [key, pending]: m_pending) {
		if (pending.protected_ && RangesOverlap(key.first, key.second, address, size)) {
			LiftEntry(key, pending);
			lifted = true;
		}
	}
	return lifted;
}

void ReadbackGuard::LiftEverything() {
	for (auto& [key, pending]: m_pending) {
		if (pending.protected_) {
			LiftEntry(key, pending);
		}
	}
}

bool ReadbackGuard::LiftAt(uint64_t vaddr) {
	bool lifted = false;
	{
		std::lock_guard lock(m_mutex);
		lifted = LiftOverlapping(vaddr, ONE_BYTE);
	}
	m_landed.notify_all();
	return lifted;
}

void ReadbackGuard::LiftRange(uint64_t address, uint64_t size) {
	{
		std::lock_guard lock(m_mutex);
		LiftOverlapping(address, size);
	}
	m_landed.notify_all();
}

void ReadbackGuard::LiftAll() {
	{
		std::lock_guard lock(m_mutex);
		LiftEverything();
	}
	m_landed.notify_all();
}

bool ReadbackGuard::AnyProtectedOverlap(uint64_t address, uint64_t size) const noexcept {
	return std::ranges::any_of(m_pending, [&](const auto& entry) {
		return entry.second.protected_ &&
		       RangesOverlap(entry.first.first, entry.first.second, address, size);
	});
}

bool ReadbackGuard::AnyLiftedOverlap(uint64_t address, uint64_t size) const noexcept {
	return std::ranges::any_of(m_pending, [&](const auto& entry) {
		return !entry.second.protected_ &&
		       RangesOverlap(entry.first.first, entry.first.second, address, size);
	});
}

bool ReadbackGuard::Covers(uint64_t vaddr) {
	if (!HasPending()) {
		return false;
	}
	std::lock_guard lock(m_mutex);
	return AnyProtectedOverlap(vaddr, ONE_BYTE);
}

ReadbackGuard::WaitResult ReadbackGuard::WaitAt(uint64_t vaddr) noexcept {
	return WaitOverlap(vaddr, ONE_BYTE);
}

ReadbackGuard::WaitResult ReadbackGuard::WaitRange(uint64_t address, uint64_t size) noexcept {
	return WaitOverlap(address, size);
}

ReadbackGuard::WaitResult ReadbackGuard::WaitOverlap(uint64_t address, uint64_t size) noexcept {
	using Clock = std::chrono::steady_clock;
	if (!HasPending()) {
		return WaitResult::NotPending;
	}
	std::unique_lock lock(m_mutex);
	if (!AnyProtectedOverlap(address, size)) {
		return WaitResult::NotPending;
	}
	m_active_waiters.fetch_add(1, std::memory_order_acq_rel);
	const auto start          = Clock::now();
	auto       last_progress  = start;
	auto       progress_value = m_progress();
	bool       timed_out      = false;
	while (AnyProtectedOverlap(address, size)) {
		m_landed.wait_for(lock, m_limits.wait_slice);
		const auto now   = Clock::now();
		const auto value = m_progress();
		if (value != progress_value) {
			progress_value = value;
			last_progress  = now;
		}
		if (AnyProtectedOverlap(address, size) &&
		    (now - last_progress >= m_limits.no_progress_timeout ||
		     now - start >= m_limits.absolute_timeout)) {
			LiftOverlapping(address, size);
			timed_out = true;
			break;
		}
	}
	const auto result = timed_out                         ? WaitResult::TimedOut
	                    : AnyLiftedOverlap(address, size) ? WaitResult::Lifted
	                                                      : WaitResult::Landed;
	RecordWait(
	    result,
	    static_cast<uint64_t>(
	        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count()));
	m_active_waiters.fetch_sub(1, std::memory_order_acq_rel);
	lock.unlock();
	if (timed_out) {
		m_landed.notify_all();
	}
	return result;
}

void ReadbackGuard::RecordWait(WaitResult result, uint64_t waited_us) {
	m_counters.waits++;
	m_counters.wait_us += waited_us;
	m_counters.wait_max_us = std::max(m_counters.wait_max_us, waited_us);
	if (result == WaitResult::Landed) {
		m_consecutive_timeouts = 0;
	} else if (result == WaitResult::TimedOut) {
		m_counters.timeouts++;
		m_consecutive_timeouts++;
		// A readback that keeps timing out would stall the game on every frame. Going back to the
		// old stale reads is better than a near-freeze, and that includes what is already held.
		if (m_consecutive_timeouts >= m_limits.consecutive_timeouts_to_stop) {
			m_counters.stopped = true;
			LiftEverything();
		}
	}
}

ReadbackGuard::Counters ReadbackGuard::GetCounters() {
	std::lock_guard lock(m_mutex);
	return m_counters;
}

size_t ReadbackGuard::TrackedRanges() {
	std::lock_guard lock(m_mutex);
	return m_pending.size();
}

} // namespace Libs::Graphics
