#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_

#include "common/assert.h"
#include "common/threads.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__)
#include <xmmintrin.h>
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#pragma comment(lib, "Synchronization.lib")
#undef min
#undef max
#undef MemoryBarrier
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Libs::Graphics {

// The lock of the short critical sections of the page tracking, which the GPU threads and the
// guest's threads (in their page-fault handlers) share. Uncontended it costs what a spin lock
// costs. A waiter spins for some tens of microseconds and then sleeps until the holder releases
// the lock: holders change page protections inside it, and a holder the scheduler preempted there
// otherwise waited for a processor behind its spinning waiters. ASTRO BOT froze for 2-6 s while a
// dozen game threads and the execution thread spun on one region's lock and its holder, ready,
// did not get to run. The GPU stage threads that have a reserved core keep spinning.
class TrackingSpinLock final {
public:
	void lock() noexcept {
		const auto thread = CurrentThread();
		if (m_state.exchange(1, std::memory_order_acquire) != 0) [[unlikely]] {
			LockSlow(thread);
		}
		m_owner.store(thread, std::memory_order_relaxed);
	}
	void unlock() noexcept {
		EXIT_NOT_IMPLEMENTED(m_owner.load(std::memory_order_relaxed) != CurrentThread());
		m_owner.store(0, std::memory_order_relaxed);
		// Sequentially consistent, so the release is visible before the check for sleepers: a
		// waiter registers as a sleeper before it looks at the lock word for the last time.
		m_state.store(0, std::memory_order_seq_cst);
		if (m_sleepers.load(std::memory_order_seq_cst) != 0) [[unlikely]] {
			WakeSleeper();
		}
	}

private:
	// About 50 us of pauses before a waiter sleeps.
	static constexpr uint32_t SpinRounds = 4096;

#if defined(_MSC_VER) && !defined(__clang__)
	__declspec(noinline)
#else
	[[gnu::noinline]]
#endif
	void LockSlow(uint32_t thread) noexcept {
		// A GPU stage thread alone on its reserved core takes no processor from anyone by spinning,
		// and as the critical path it does not pay for a wake-up: it keeps spinning.
		const bool own_core = Common::Thread::HasReservedCore();
		for (uint32_t round = 0;; round++) {
			EXIT_NOT_IMPLEMENTED(m_owner.load(std::memory_order_relaxed) == thread);
			if (round < SpinRounds || own_core) {
#if defined(__x86_64__) || defined(_M_X64)
				_mm_pause();
#elif defined(_M_ARM64)
				__yield();
#elif defined(__aarch64__)
				asm volatile("yield");
#endif
			} else {
				m_sleepers.fetch_add(1, std::memory_order_seq_cst);
				SleepWhileHeld();
				m_sleepers.fetch_sub(1, std::memory_order_relaxed);
			}
			if (m_state.load(std::memory_order_relaxed) == 0 &&
			    m_state.exchange(1, std::memory_order_acquire) == 0) {
				return;
			}
		}
	}

	void SleepWhileHeld() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		// At most a millisecond: a release that missed this sleeper does not keep it waiting.
		uint32_t held = 1;
		(void)::WaitOnAddress(&m_state, &held, sizeof(held), 1);
#else
		std::this_thread::yield();
#endif
	}

	void WakeSleeper() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		::WakeByAddressSingle(&m_state);
#endif
	}

	static uint32_t CurrentThread() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		// Cached: the lock is taken for most dirty-page queries of the GPU threads.
		static thread_local const uint32_t tid = GetCurrentThreadId();
		return tid;
#elif defined(__APPLE__)
		// mach thread port is a nonzero per-thread id (0 is the "no owner" sentinel).
		static thread_local const uint32_t tid =
		    static_cast<uint32_t>(pthread_mach_thread_np(pthread_self()));
		return tid;
#elif defined(__linux__)
		static thread_local const uint32_t tid = static_cast<uint32_t>(::syscall(SYS_gettid));
		return tid;
#else
		EXIT("region tracking thread identity is unsupported on this platform\n");
#endif
	}

	std::atomic_uint32_t m_state {0};
	std::atomic_uint32_t m_owner {0};
	std::atomic_uint32_t m_sleepers {0};
};

static_assert(std::atomic_uint32_t::is_always_lock_free);

class RegionManager final {
public:
	RegionManager(PageManager& page_manager, uint64_t cpu_addr)
	    : m_page_manager(page_manager), m_cpu_addr(cpu_addr) {
		m_cpu_dirty.Fill();
		m_cpu_written.Fill();
		s_cpu_dirty_epoch.fetch_add(1, std::memory_order_acq_rel);
	}

	// Advances whenever pages of any region become CPU dirty: while it is unchanged, no page
	// was dirtied since a synchronization that started after reading it.
	[[nodiscard]] static uint64_t CpuDirtyEpoch() {
		return s_cpu_dirty_epoch.load(std::memory_order_acquire);
	}

	// Advances, after the pages change under the lock, whenever pages of this region become CPU
	// dirty; a new region starts dirty at 1.
	[[nodiscard]] uint64_t RegionCpuDirtyEpoch() const {
		return m_cpu_dirty_epoch.load(std::memory_order_acquire);
	}

	// What a proof of unchanged bytes holds on: moves when pages of the region become CPU dirty, when GPU-written
	// pages are given back to the CPU (their bytes are the GPU's now) and when the emulator wrote CPU-dirty pages
	// (NoteHostWrite); with the guest's writes to dirty pages, every change of the region's bytes the CPU can see.
	// (chenxiao07's RegionManager::CpuModificationEpoch.)
	[[nodiscard]] uint64_t CpuModificationEpoch() const {
		return m_cpu_dirty_epoch.load(std::memory_order_acquire) + m_gpu_clean_epoch.load(std::memory_order_acquire) +
		       m_host_write_epoch.load(std::memory_order_acquire);
	}
	// The emulator wrote CPU-dirty pages of the region (their state stays).
	void NoteHostWrite() noexcept { m_host_write_epoch.fetch_add(1, std::memory_order_release); }

	// Advances whenever pages of any region stop being GPU dirty.
	[[nodiscard]] static uint64_t GpuCleanEpoch() {
		return s_gpu_clean_epoch.load(std::memory_order_acquire);
	}

	// Advances, after the pages change under the lock, whenever pages of this region stop being
	// GPU dirty; a new region starts at 1.
	[[nodiscard]] uint64_t RegionGpuCleanEpoch() const {
		return m_gpu_clean_epoch.load(std::memory_order_acquire);
	}

	KYTY_CLASS_NO_COPY(RegionManager);

	template <DirtySource source>
	[[nodiscard]] bool IsModified(uint64_t offset, uint64_t size) const {
		const auto [start, end] = GetPageRange(offset, size);
		return GetBits<source>().Any(start, end);
	}

	template <DirtySource source, bool enable>
	void ChangeState(uint64_t offset, uint64_t size) {
		const auto [start, end] = GetPageRange(offset, size);
		if constexpr (source == DirtySource::Cpu && enable) {
			if (m_gpu_dirty.Any(start, end)) {
				EXIT("CPU dirty state conflicts with GPU dirty state\n");
			}
		}
		auto&      bits = GetBits<source>();
		RegionBits changed = enable ? RegionBits(~bits, start, end) : RegionBits(bits, start, end);
		if constexpr (enable) {
			bits.SetRange(start, end);
		} else {
			bits.UnsetRange(start, end);
		}
		if constexpr (source == DirtySource::Cpu) {
			if constexpr (enable) {
				m_cpu_written.SetRange(start, end);
				m_cpu_dirty_epoch.fetch_add(1, std::memory_order_acq_rel);
				s_cpu_dirty_epoch.fetch_add(1, std::memory_order_acq_rel);
			}
			m_has_cpu_dirty.store(!m_cpu_dirty.None(), std::memory_order_release);
			UpdateProtection<!enable, false>(changed);
		} else {
			if constexpr (!enable) {
				m_gpu_clean_epoch.fetch_add(1, std::memory_order_acq_rel);
				s_gpu_clean_epoch.fetch_add(1, std::memory_order_acq_rel);
			} else {
				m_gpu_written.SetRange(start, end);
			}
			UpdateProtection<enable, true>(changed);
		}
	}

	template <DirtySource source, bool clear, typename Func>
	void ForEachModifiedRange(uint64_t offset, uint64_t size, Func&& func) {
		const auto [start, end] = GetPageRange(offset, size);
		auto&      bits         = GetBits<source>();
		if (!bits.Any(start, end)) {
			return;
		}
		RegionBits mask(bits, start, end);
		if constexpr (clear) {
			bits.UnsetRange(start, end);
			if constexpr (source == DirtySource::Cpu) {
				m_has_cpu_dirty.store(!m_cpu_dirty.None(), std::memory_order_release);
				UpdateProtection<true, false>(mask);
			} else {
				m_gpu_clean_epoch.fetch_add(1, std::memory_order_acq_rel);
				s_gpu_clean_epoch.fetch_add(1, std::memory_order_acq_rel);
				UpdateProtection<false, true>(mask);
			}
		}
		for (const auto [first, last]: mask) {
			func(m_cpu_addr + first * TRACKER_PAGE_SIZE, (last - first) * TRACKER_PAGE_SIZE);
		}
	}

	// ForEachModifiedRange<DirtySource::Cpu, true>() over several ranges of the region, ordered
	// and not overlapping: one protection update covers them all, so the page manager protects
	// runs of their pages with one call, through clean pages between them that are protected
	// already. Calls func(index, address, size) for the runs of pages of ranges[index] (offsets
	// in the region). Callers hold the lock.
	template <typename Func>
	void ConsumeCpuDirtyRanges(std::span<const std::pair<uint64_t, uint64_t>> ranges, Func&& func) {
		RegionBits mask;
		for (const auto& [offset, size]: ranges) {
			const auto [start, end] = GetPageRange(offset, size);
			if (m_cpu_dirty.FirstRangeFrom(start).first < end) {
				mask |= RegionBits(m_cpu_dirty, start, end);
			}
		}
		if (mask.None()) {
			return;
		}
		for (const auto [first, last]: mask) {
			m_cpu_dirty.UnsetRange(first, last);
		}
		m_has_cpu_dirty.store(!m_cpu_dirty.None(), std::memory_order_release);
		UpdateProtection<true, false>(mask);
		for (size_t index = 0; index < ranges.size(); index++) {
			const auto [start, end] = GetPageRange(ranges[index].first, ranges[index].second);
			for (const auto [first, last]: RegionBits(mask, start, end)) {
				func(index, m_cpu_addr + first * TRACKER_PAGE_SIZE,
				     (last - first) * TRACKER_PAGE_SIZE);
			}
		}
	}

	// ConsumeCpuDirtyRanges() for the synchronization of everything device addresses may read
	// (RenderContext::PrepareBda()), the `sync`-th one. That synchronization uploads every CPU
	// write under a cached buffer, and cleaning a page protects it, so the guest's next write to
	// it faults. The guest rewrites the data of each frame (the constants its draws stream)
	// every frame: synchronized, each of those pages costs a protection call here and a fault on
	// a guest thread per frame. A page that faulted again within MaxGap synchronizations of its
	// last visit, MinWrites times in a row, stays CPU dirty: it is uploaded with every
	// synchronization without being protected, as long as nothing else cleans it; that is the
	// state of a page the guest wrote right after a synchronization. Every RevalidatePeriod
	// synchronizations it is cleaned once, and stays dirty again only if the guest writes it soon
	// after. Pages the GPU ever wrote do not stay dirty: their uploads would overwrite what the
	// GPU stored, which the guest's memory only gets when a fault reads it back.
	// Returns whether pages stayed dirty. Callers hold the lock.
	template <typename Func>
	bool ConsumeCpuDirtyRangesKeepingBusy(std::span<const std::pair<uint64_t, uint64_t>> ranges,
	                                      uint32_t sync, Func&& func) {
		constexpr uint32_t MaxGap           = 3;
		constexpr uint8_t  MinWrites        = 3;
		constexpr uint32_t RevalidatePeriod = 12;
		RegionBits mask;
		for (const auto& [offset, size]: ranges) {
			const auto [start, end] = GetPageRange(offset, size);
			if (m_cpu_dirty.FirstRangeFrom(start).first < end) {
				mask |= RegionBits(m_cpu_dirty, start, end);
			}
		}
		if (mask.None()) {
			return false;
		}
		if (m_busy == nullptr) {
			m_busy = std::make_unique<BusyPages>();
		}
		auto&      busy = *m_busy;
		RegionBits clean;
		bool       kept = false;
		for (const auto [first, last]: mask) {
			for (auto page = first; page < last; page++) {
				// Only a page that faulted is known to have been written: the pages a fault releases
				// with it (BufferCache::InvalidateFaultedPage()) are dirty too, and one left dirty
				// does not fault.
				if (busy.faulted.Get(page)) {
					busy.faulted.Unset(page);
					const bool soon   = busy.seen[page] != 0 && sync - busy.seen[page] <= MaxGap;
					busy.writes[page] = soon ? static_cast<uint8_t>(std::min(busy.writes[page] + 1, 255)) : 1;
				}
				busy.seen[page] = sync;
				if (busy.writes[page] >= MinWrites && sync - busy.cleaned[page] < RevalidatePeriod &&
				    !m_gpu_written.Get(page)) {
					kept = true;
				} else {
					// Cleaned, it must fault again to stay dirty the next time.
					busy.writes[page]  = std::min(busy.writes[page], static_cast<uint8_t>(MinWrites - 1));
					busy.cleaned[page] = sync;
					clean.Set(page);
					m_cpu_dirty.Unset(page);
				}
			}
		}
		m_has_cpu_dirty.store(!m_cpu_dirty.None(), std::memory_order_release);
		UpdateProtection<true, false>(clean);
		if (kept) {
			m_kept_sync.store(sync, std::memory_order_relaxed);
		}
		for (size_t index = 0; index < ranges.size(); index++) {
			const auto [start, end] = GetPageRange(ranges[index].first, ranges[index].second);
			for (const auto [first, last]: RegionBits(mask, start, end)) {
				func(index, m_cpu_addr + first * TRACKER_PAGE_SIZE,
				     (last - first) * TRACKER_PAGE_SIZE);
			}
		}
		return kept;
	}

	// A guest write faulted on the page at `offset` (see ConsumeCpuDirtyRangesKeepingBusy()).
	// Callers hold the lock.
	void NoteGuestWriteFault(uint64_t offset) {
		if (m_busy != nullptr) {
			m_busy->faulted.Set(static_cast<size_t>(offset / TRACKER_PAGE_SIZE));
		}
	}

	// The last synchronization that left pages of the region CPU dirty (see
	// ConsumeCpuDirtyRangesKeepingBusy()), 0 when none did: the next one visits the region even
	// if no page faulted since.
	[[nodiscard]] uint32_t KeptSync() const { return m_kept_sync.load(std::memory_order_relaxed); }

	// Calls func(address, size) for the runs of pages in the range that became CPU dirty since
	// the previous call over them, or are still CPU dirty (further writes to a dirty page do not
	// fault), and forgets the former. Callers hold the lock.
	template <typename Func>
	void ConsumeCpuWritten(uint64_t offset, uint64_t size, Func&& func) {
		const auto [start, end] = GetPageRange(offset, size);
		RegionBits mask(m_cpu_written, start, end);
		mask |= RegionBits(m_cpu_dirty, start, end);
		m_cpu_written.UnsetRange(start, end);
		for (const auto [first, last]: mask) {
			func(m_cpu_addr + first * TRACKER_PAGE_SIZE, (last - first) * TRACKER_PAGE_SIZE);
		}
	}

	// Marks the pages of the range GPU dirty unless they are CPU dirty, and calls func(address,
	// size) for the runs it marked. Callers hold the lock.
	template <typename Func>
	void MarkGpuDirtyUnlessCpuDirty(uint64_t offset, uint64_t size, Func&& func) {
		const auto [start, end] = GetPageRange(offset, size);
		m_gpu_written.SetRange(start, end);
		RegionBits mask(~m_cpu_dirty, start, end);
		if (mask.None()) {
			return;
		}
		// Only the pages that were not GPU dirty yet start being read-watched.
		auto either = m_cpu_dirty;
		either |= m_gpu_dirty;
		RegionBits changed(~either, start, end);
		for (const auto [first, last]: mask) {
			m_gpu_dirty.SetRange(first, last);
		}
		UpdateProtection<true, true>(changed);
		for (const auto [first, last]: mask) {
			func(m_cpu_addr + first * TRACKER_PAGE_SIZE, (last - first) * TRACKER_PAGE_SIZE);
		}
	}

	// Whether any page holds CPU writes not yet uploaded. Read without the lock: a concurrent
	// CPU write that is not observed yet is equivalent to one that lands after the read.
	[[nodiscard]] bool HasCpuDirty() const {
		return m_has_cpu_dirty.load(std::memory_order_acquire);
	}

	// Whether any page is GPU dirty, i.e. read protected. Read without the lock: a stale answer
	// only chooses between a plain read and a checked one.
	[[nodiscard]] bool HasGpuDirty() const {
		return m_has_gpu_dirty.load(std::memory_order_acquire);
	}

	TrackingSpinLock lock;

private:
	// CPU-dirty pages are not write-watched; GPU-dirty pages are read-watched.
	template <bool track, bool is_read>
	void UpdateProtection(RegionBits& changed) {
		if (changed.None()) {
			return;
		}
		if constexpr (is_read) {
			m_has_gpu_dirty.store(!m_gpu_dirty.None(), std::memory_order_release);
		}
		m_page_manager.UpdatePageWatchersForRegion<track, is_read>(m_cpu_addr, changed);
	}

	template <DirtySource source>
	RegionBits& GetBits() {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_dirty;
		} else {
			return m_gpu_dirty;
		}
	}

	template <DirtySource source>
	const RegionBits& GetBits() const {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_dirty;
		} else {
			return m_gpu_dirty;
		}
	}

	[[nodiscard]] static std::pair<size_t, size_t> GetPageRange(uint64_t offset, uint64_t size) {
		return {static_cast<size_t>(offset / TRACKER_PAGE_SIZE),
		        static_cast<size_t>((offset + size + TRACKER_PAGE_SIZE - 1) / TRACKER_PAGE_SIZE)};
	}

	PageManager& m_page_manager;
	uint64_t     m_cpu_addr = 0;
	RegionBits   m_cpu_dirty;
	// Pages that became CPU dirty since ConsumeCpuWritten last visited them.
	RegionBits   m_cpu_written;
	RegionBits   m_gpu_dirty;
	// A new region starts with every page CPU dirty.
	std::atomic_bool      m_has_cpu_dirty {true};
	std::atomic_bool      m_has_gpu_dirty {false};
	std::atomic<uint64_t> m_cpu_dirty_epoch {1};
	std::atomic<uint64_t> m_gpu_clean_epoch {1};
	// See ConsumeCpuDirtyRangesKeepingBusy(). Pages the GPU wrote, or was noted to write, at any
	// time; and per page: the synchronization that last visited it dirty and the one that last
	// cleaned it, its write faults in a row soon after a visit, and whether a guest write faulted
	// on it since its last visit.
	struct BusyPages {
		std::array<uint32_t, TRACKER_REGION_PAGES> seen {};
		std::array<uint32_t, TRACKER_REGION_PAGES> cleaned {};
		std::array<uint8_t, TRACKER_REGION_PAGES>  writes {};
		RegionBits                                 faulted;
	};
	RegionBits                 m_gpu_written;
	std::unique_ptr<BusyPages> m_busy;
	std::atomic<uint32_t>      m_kept_sync {0};
	std::atomic<uint64_t> m_host_write_epoch {0};

	inline static std::atomic<uint64_t> s_cpu_dirty_epoch {0};
	inline static std::atomic<uint64_t> s_gpu_clean_epoch {0};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
