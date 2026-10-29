#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_

#include "common/assert.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/uffdWriteWatch.h"
#include "graphics/host_gpu/writeHeat.h"

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__)
#include <xmmintrin.h>
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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

class TrackingSpinLock final {
public:
	void lock() noexcept {
		const auto thread = CurrentThread();
		while (m_lock.test_and_set(std::memory_order_acquire)) {
			EXIT_NOT_IMPLEMENTED(m_owner.load(std::memory_order_relaxed) == thread);
#if defined(__x86_64__) || defined(_M_X64)
			_mm_pause();
#elif defined(_M_ARM64)
			__yield();
#elif defined(__aarch64__)
			asm volatile("yield");
#endif
		}
		m_owner.store(thread, std::memory_order_relaxed);
		// Odd while held: a reader that started before sees the number change (TryRead).
		m_sequence.store(m_sequence.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
	}
	void unlock() noexcept {
		EXIT_NOT_IMPLEMENTED(m_owner.load(std::memory_order_relaxed) != CurrentThread());
		m_owner.store(0, std::memory_order_relaxed);
		m_sequence.store(m_sequence.load(std::memory_order_relaxed) + 1, std::memory_order_release);
		m_lock.clear(std::memory_order_release);
	}

	// Runs `read` without the lock, and tells whether it ran with no holder in between: if not,
	// what it read may be torn and must be dropped. `read` may only read plain data, and must
	// stay safe to run on torn data (no pointers taken from it). It takes no atomic read-modify-
	// write, where lock() takes one, which is most of what a short read of a few words costs.
	template <typename Read>
	[[nodiscard]] bool TryRead(Read&& read) const noexcept {
		const auto before = m_sequence.load(std::memory_order_acquire);
		if ((before & 1u) != 0) {
			return false;
		}
		read();
		std::atomic_thread_fence(std::memory_order_acquire);
		return m_sequence.load(std::memory_order_relaxed) == before;
	}

	// Whether the calling thread holds the lock now.
	[[nodiscard]] bool HeldByCurrentThread() const noexcept {
		return m_owner.load(std::memory_order_relaxed) == CurrentThread();
	}

private:
	static uint32_t CurrentThread() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		return GetCurrentThreadId();
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
	std::atomic_uint32_t m_sequence {0};
};

static_assert(std::atomic_uint32_t::is_always_lock_free);

// A TrackingSpinLock the holder can take again. A caller that makes many short calls into one
// object takes the lock once around them; each call inside then costs a read, not an atomic
// read-modify-write.
class RecursiveTrackingLock final {
public:
	void lock() noexcept {
		if (m_lock.HeldByCurrentThread()) {
			m_depth++;
			return;
		}
		m_lock.lock();
		m_depth = 1;
	}
	void unlock() noexcept {
		if (--m_depth == 0) {
			m_lock.unlock();
		}
	}

private:
	TrackingSpinLock m_lock;
	uint32_t         m_depth = 0; // written by the holder only
};

class RegionManager final {
public:
	RegionManager(PageManager& page_manager, uint64_t cpu_addr)
	    : m_page_manager(page_manager), m_cpu_addr(cpu_addr) {
		if (m_cpu_addr % TRACKER_REGION_SIZE != 0) {
			EXIT("invalid region tracking manager construction\n");
		}
		m_cpu_dirty.Fill();
		m_writable.Fill();
		m_readable.Fill();
	}

	KYTY_CLASS_NO_COPY(RegionManager);

	[[nodiscard]] uint64_t GetCpuAddr() const { return m_cpu_addr; }
	template <DirtySource source>
	[[nodiscard]] bool IsModified(uint64_t offset, uint64_t size) const {
		const auto [start, end] = GetPageRange(m_cpu_addr + offset, size);
		return GetBits<source>().FirstRangeFrom(start).first < end;
	}

	template <DirtySource source, bool enable>
	void ChangeState(uint64_t vaddr, uint64_t size) {
		const auto [start, end] = GetPageRange(vaddr, size);
		ForgetOldHeat();
		if constexpr (source == DirtySource::Cpu && enable) {
			if (RegionBits(m_gpu_dirty, start, end).Any()) {
				EXIT("CPU dirty state conflicts with GPU dirty state\n");
			}
			// A write to a clean page: the page came back after an upload.
			for (size_t page = start; page < end; page++) {
				if (!m_cpu_dirty.Get(page)) {
					m_write_heat.NoteWrite(page, page + 1, Frame());
				}
			}
		}
		if constexpr (source == DirtySource::Cpu && !enable) {
			m_write_heat.Cool(start, end);
			m_armed.UnsetRange(start, end);
		}
		if constexpr (source == DirtySource::Gpu && enable) {
			// A hot page is kept dirty only so that it is uploaded at every use. The caller has
			// just uploaded it; from here the GPU owns it.
			for (size_t page = start; page < end; page++) {
				if (m_write_heat.IsHot(page)) {
					m_cpu_dirty.UnsetRange(page, page + 1);
				}
			}
			m_armed.UnsetRange(start, end);
			if (m_write_heat.Cool(start, end)) {
				UpdateProtection<true, false>();
			}
			if (RegionBits(m_cpu_dirty, start, end).Any()) {
				EXIT("GPU dirty state conflicts with CPU dirty state\n");
			}
		}
		auto& bits = GetBits<source>();
		if constexpr (enable) {
			bits.SetRange(start, end);
		} else {
			bits.UnsetRange(start, end);
		}
		if constexpr (source == DirtySource::Cpu) {
			UpdateProtection<!enable, false>();
		} else {
			UpdateProtection<enable, true>();
		}
	}

	template <DirtySource source, bool clear, typename Func>
	void ForEachModifiedRange(uint64_t vaddr, uint64_t size, Func&& func) {
		const auto [start, end] = GetPageRange(vaddr, size);
		auto&      bits         = GetBits<source>();
		if (bits.FirstRangeFrom(start).first >= end) {
			return;
		}
		RegionBits mask(bits, start, end);
		if constexpr (clear) {
			bits.UnsetRange(start, end);
			if constexpr (source == DirtySource::Cpu) {
				ForgetOldHeat();
				auto* const watch = UffdWriteWatch::Get();
				if (watch != nullptr &&
				    watch->Covers(m_cpu_addr + start * TRACKER_PAGE_SIZE,
				                  (end - start) * TRACKER_PAGE_SIZE)) {
					// The hot pages stay open but clean, protected through the watch: a scan
					// finds the ones written since (HarvestIfDue).
					auto kept = bits;
					m_write_heat.KeepHotDirty(kept, start, end, Frame());
					// Pages armed already were protected again by their scan.
					const auto arm = (kept ^ bits) & ~m_armed;
					for (const auto [first, last]: arm) {
						watch->Protect(m_cpu_addr + first * TRACKER_PAGE_SIZE,
						               (last - first) * TRACKER_PAGE_SIZE);
					}
					m_armed |= arm;
					if (arm.Any()) {
						maybe_armed.store(true, std::memory_order_relaxed);
						s_arm_generation.fetch_add(1, std::memory_order_relaxed);
					}
				} else {
					m_write_heat.KeepHotDirty(bits, start, end, Frame());
				}
				UpdateProtection<true, false>();
			} else {
				UpdateProtection<false, true>();
			}
		}
		for (const auto [first, last]: mask) {
			func(m_cpu_addr + first * TRACKER_PAGE_SIZE, (last - first) * TRACKER_PAGE_SIZE);
		}
	}

	// The range is about to be uploaded for a GPU write: its hot pages lose their heat, so that
	// they are protected before their bytes are copied, as at the end of a lease (writeHeat.h). An
	// open page could take a CPU write after the copy; the GPU's version of the page would then
	// hide that write, and a later read-back would overwrite it.
	void EndHeat(uint64_t vaddr, uint64_t size) {
		const auto [start, end] = GetPageRange(vaddr, size);
		ForgetOldHeat();
		m_write_heat.Cool(start, end);
		// Armed pages written since their scan are dirty for this upload; then they are closed.
		const RegionBits armed(m_armed, start, end);
		if (armed.Any()) {
			std::vector<GuestRange> written;
			for (const auto [first, last]: armed) {
				UffdWriteWatch::Get()->TakeWritten(m_cpu_addr + first * TRACKER_PAGE_SIZE,
				                                   (last - first) * TRACKER_PAGE_SIZE, written);
			}
			for (const auto& range: written) {
				const auto [first, last] = GetPageRange(range.address, range.size);
				m_cpu_dirty.SetRange(first, last);
			}
			m_armed.UnsetRange(start, end);
			UpdateProtection<true, false>();
		}
	}

	// Memory that is no longer tracked: its pages are no longer armed.
	void Disarm(uint64_t vaddr, uint64_t size) {
		const auto [start, end] = GetPageRange(vaddr, size);
		m_armed.UnsetRange(start, end);
	}

	// Set when pages are armed; HarvestIfDue clears it once none are. Read without the lock.
	std::atomic<bool> maybe_armed {false};

	// A guest submission started: armed pages are due for a scan (HarvestIfDue).
	static void NoteSubmission() noexcept { s_submission.fetch_add(1, std::memory_order_relaxed); }
	static uint64_t Submission() noexcept { return s_submission.load(std::memory_order_relaxed); }
	// Counts the times a region got armed pages: a scan that found nothing due stays that way
	// until the submission or this changes.
	static uint64_t ArmGeneration() noexcept { return s_arm_generation.load(std::memory_order_relaxed); }

	// Under the lock, before the CPU dirty state is read: once per guest submission, the armed
	// pages the CPU wrote since their last scan become dirty, as if the writes had faulted, and
	// are appended to `written`. Armed pages whose heat ended are closed (protected as clean).
	void HarvestIfDue(std::vector<GuestRange>& written) {
		if (!maybe_armed.load(std::memory_order_relaxed)) {
			return;
		}
		const auto epoch = s_submission.load(std::memory_order_relaxed);
		if (m_harvest_epoch == epoch) {
			return;
		}
		m_harvest_epoch = epoch;
		ForgetOldHeat();
		const auto first = m_armed.FirstRange().first;
		const auto last  = m_armed.LastRange().second;
		if (first < last) {
			// One scan from the first armed page to the last; it reports open dirty pages too,
			// which are left out.
			m_scan.clear();
			UffdWriteWatch::Get()->TakeWritten(m_cpu_addr + first * TRACKER_PAGE_SIZE,
			                                   (last - first) * TRACKER_PAGE_SIZE, m_scan);
			for (const auto& range: m_scan) {
				const auto [start, end] = GetPageRange(range.address, range.size);
				for (const auto [armed_start, armed_end]: RegionBits(m_armed, start, end)) {
					for (size_t page = armed_start; page < armed_end; page++) {
						if (!m_cpu_dirty.Get(page)) {
							m_write_heat.NoteWrite(page, page + 1, Frame());
						}
					}
					m_cpu_dirty.SetRange(armed_start, armed_end);
					written.push_back({m_cpu_addr + armed_start * TRACKER_PAGE_SIZE,
					                   (armed_end - armed_start) * TRACKER_PAGE_SIZE});
				}
			}
		}
		const auto still_hot = m_armed & m_write_heat.Hot();
		if ((still_hot ^ m_armed).Any()) {
			m_armed = still_hot;
			UpdateProtection<true, false>();
		}
		if (m_armed.None()) {
			maybe_armed.store(false, std::memory_order_relaxed);
		}
	}

	TrackingSpinLock lock;

	// Ends the heat of every region, at its next use. Called from time to time, so that a page
	// the guest has stopped writing is protected again.
	static void CoolAllRegions() noexcept { s_heat_epoch.fetch_add(1, std::memory_order_relaxed); }

	// A guest frame ended (a flip). Hot pages count their lease in these (writeHeat.h).
	static void NoteFrame() noexcept { s_frame.fetch_add(1, std::memory_order_relaxed); }

private:
	static uint32_t Frame() noexcept { return s_frame.load(std::memory_order_relaxed); }

	void ForgetOldHeat() {
		const auto epoch = s_heat_epoch.load(std::memory_order_relaxed);
		if (m_heat_epoch != epoch) {
			m_heat_epoch = epoch;
			m_write_heat.CoolAll();
		}
	}

	template <bool track, bool is_read>
	void UpdateProtection() {
		// Armed pages stay writable: the watch sees their writes.
		const auto protection = is_read ? ~m_gpu_dirty : (m_cpu_dirty | m_armed);
		auto&      previous   = is_read ? m_readable : m_writable;
		auto       mask       = protection ^ previous;
		if (mask.None()) {
			return;
		}
		previous = protection;
		m_page_manager.UpdatePageWatchersForRegion<track, is_read>(m_cpu_addr, mask);
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

	[[nodiscard]] std::pair<size_t, size_t> GetPageRange(uint64_t vaddr, uint64_t size) const {
		if (size == 0 || vaddr < m_cpu_addr || vaddr >= m_cpu_addr + TRACKER_REGION_SIZE ||
		    size > m_cpu_addr + TRACKER_REGION_SIZE - vaddr) {
			EXIT("range lies outside its tracking region\n");
		}
		const auto offset = vaddr - m_cpu_addr;
		return {static_cast<size_t>(offset / TRACKER_PAGE_SIZE),
		        static_cast<size_t>((offset + size + TRACKER_PAGE_SIZE - 1) / TRACKER_PAGE_SIZE)};
	}

	PageManager& m_page_manager;
	uint64_t     m_cpu_addr = 0;
	RegionBits   m_cpu_dirty;
	RegionBits   m_gpu_dirty;
	RegionBits   m_writable;
	RegionBits   m_readable;
	// Hot pages that are open and clean, write-protected through the UffdWriteWatch.
	RegionBits   m_armed;
	std::vector<GuestRange> m_scan;
	uint64_t                m_harvest_epoch = 0;
	// Pages the guest writes again and again stay open and dirty (see writeHeat.h).
	WriteHeat<TRACKER_REGION_PAGES>     m_write_heat;
	uint32_t                            m_heat_epoch = 0;
	static inline std::atomic<uint32_t> s_heat_epoch {0};
	static inline std::atomic<uint32_t> s_frame {0};
	static inline std::atomic<uint64_t> s_submission {1};
	static inline std::atomic<uint64_t> s_arm_generation {0};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
