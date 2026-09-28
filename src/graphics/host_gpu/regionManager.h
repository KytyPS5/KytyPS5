#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_

#include "common/assert.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <atomic>
#include <mutex>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__)
#include <xmmintrin.h>
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#undef MemoryBarrier
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Libs::Graphics {

class TrackingSpinLock final {
public:
	void lock() noexcept {
		const auto thread = CurrentThread();
		while (m_lock.test_and_set(std::memory_order_acquire)) {
			EXIT_NOT_IMPLEMENTED(m_owner.load(std::memory_order_relaxed) == thread);
			// Wait on reads, so the holder's cache line is not written until the lock is free.
			while (m_lock.test(std::memory_order_relaxed)) {
#if defined(__x86_64__) || defined(_M_X64)
				_mm_pause();
#elif defined(_M_ARM64)
				__yield();
#elif defined(__aarch64__)
				asm volatile("yield");
#endif
			}
		}
		m_owner.store(thread, std::memory_order_relaxed);
	}
	void unlock() noexcept {
		EXIT_NOT_IMPLEMENTED(m_owner.load(std::memory_order_relaxed) != CurrentThread());
		m_owner.store(0, std::memory_order_relaxed);
		m_lock.clear(std::memory_order_release);
	}

private:
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

	std::atomic_flag     m_lock = ATOMIC_FLAG_INIT;
	std::atomic_uint32_t m_owner {0};
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
				UpdateProtection<false, true>(mask);
			}
		}
		for (const auto [first, last]: mask) {
			func(m_cpu_addr + first * TRACKER_PAGE_SIZE, (last - first) * TRACKER_PAGE_SIZE);
		}
	}

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

	inline static std::atomic<uint64_t> s_cpu_dirty_epoch {0};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
