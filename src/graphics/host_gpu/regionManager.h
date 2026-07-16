#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_

#include "common/assert.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <atomic>
#include <mutex>
#include <utility>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#endif

namespace Libs::Graphics {

enum class CpuFaultAction { Untracked, Continue, Download };

class TrackingSpinLock final {
public:
	void lock() noexcept {
		const auto thread = CurrentThread();
		if (m_owner.load(std::memory_order_relaxed) == thread) {
			EXIT("recursive region tracking lock\n");
		}
		while (m_lock.test_and_set(std::memory_order_acquire)) {
			if (m_owner.load(std::memory_order_relaxed) == thread) {
				EXIT("recursive region tracking lock while contended\n");
			}
			std::atomic_signal_fence(std::memory_order_seq_cst);
		}
		m_owner.store(thread, std::memory_order_relaxed);
	}
	void unlock() noexcept {
		if (m_owner.load(std::memory_order_relaxed) != CurrentThread()) {
			EXIT("region tracking lock released by non-owner\n");
		}
		m_owner.store(0, std::memory_order_relaxed);
		m_lock.clear(std::memory_order_release);
	}

private:
	static uint32_t CurrentThread() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		return GetCurrentThreadId();
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
		if (m_cpu_addr % TRACKER_REGION_SIZE != 0) {
			EXIT("invalid region tracking manager construction\n");
		}
		m_cpu_dirty.set();
		m_writable.set();
	}

	KYTY_CLASS_NO_COPY(RegionManager);

	[[nodiscard]] uint64_t GetCpuAddr() const { return m_cpu_addr; }
	void                   Track(uint64_t vaddr, uint64_t size) {
		const auto [start, end] = GetPageRange(vaddr, size);
		for (auto page = start; page < end; page++) {
			m_tracked.set(page);
		}
	}
	void Untrack(uint64_t vaddr, uint64_t size) {
		const auto [start, end] = GetPageRange(vaddr, size);
		for (auto page = start; page < end; page++) {
			m_tracked.reset(page);
		}
	}
	template <DirtySource source>
	[[nodiscard]] bool IsModified(uint64_t offset, uint64_t size) const {
		const auto [start, end] = GetPageRange(m_cpu_addr + offset, size);
		const auto& bits        = GetBits<source>();
		for (auto page = start; page < end; page++) {
			if (bits.test(page)) {
				return true;
			}
		}
		return false;
	}

	template <DirtySource source>
	[[nodiscard]] bool IsFullyModified(uint64_t offset, uint64_t size) const {
		const auto [start, end] = GetPageRange(m_cpu_addr + offset, size);
		const auto& bits        = GetBits<source>();
		for (auto page = start; page < end; page++) {
			if (!bits.test(page)) {
				return false;
			}
		}
		return true;
	}

	template <DirtySource source, bool enable>
	RegionBits ChangeState(uint64_t vaddr, uint64_t size) {
		const auto [start, end] = GetPageRange(vaddr, size);
		if constexpr (source == DirtySource::Cpu && enable) {
			for (auto page = start; page < end; page++) {
				if (m_gpu_dirty.test(page) || m_fault_pending.test(page)) {
					EXIT("CPU dirty state conflicts with GPU dirty or pending fault state\n");
				}
			}
		}
		if constexpr (source == DirtySource::Gpu && enable) {
			for (auto page = start; page < end; page++) {
				if (m_cpu_dirty.test(page) || m_fault_pending.test(page)) {
					EXIT("GPU dirty state conflicts with CPU dirty or pending fault state\n");
				}
			}
		}
		auto& bits    = GetBits<source>();
		auto  changed = bits;
		for (auto page = start; page < end; page++) {
			bits.set(page, enable);
		}
		changed ^= bits;
		if constexpr (source == DirtySource::Cpu) {
			changed    = m_cpu_dirty ^ m_writable;
			m_writable = m_cpu_dirty;
		}
		return changed;
	}

	[[nodiscard]] CpuFaultAction BeginCpuFault(uint64_t vaddr, uint64_t size,
	                                           PageFaultAccess access = PageFaultAccess::Write) {
		if (access != PageFaultAccess::Read && access != PageFaultAccess::Write) {
			EXIT("unsupported CPU fault access while beginning ownership transfer\n");
		}
		const auto [start, end] = GetPageRange(vaddr, size);
		const bool tracked      = m_tracked.test(start);
		for (auto page = start; page < end; page++) {
			if (m_tracked.test(page) != tracked) {
				EXIT("CPU fault spans mixed tracked and untracked pages\n");
			}
			if (m_fault_pending.test(page)) {
				return CpuFaultAction::Untracked;
			}
			if (m_cpu_dirty.test(page) != m_writable.test(page) ||
			    (m_gpu_dirty.test(page) && (m_cpu_dirty.test(page) || m_writable.test(page)))) {
				EXIT("inconsistent CPU fault page state\n");
			}
		}
		if (!tracked) {
			return CpuFaultAction::Untracked;
		}
		bool gpu_dirty = m_gpu_dirty.test(start);
		bool writable  = m_writable.test(start);
		for (auto page = start + 1; page < end; page++) {
			if (m_gpu_dirty.test(page) != gpu_dirty || m_writable.test(page) != writable) {
				EXIT("CPU fault spans pages with incompatible dirty or writable state\n");
			}
		}
		for (auto page = start; page < end; page++) {
			if (!gpu_dirty && access == PageFaultAccess::Write) {
				m_cpu_dirty.set(page);
				m_writable.set(page);
			}
			m_fault_pending.set(page);
		}
		return gpu_dirty ? CpuFaultAction::Download : CpuFaultAction::Continue;
	}

	[[nodiscard]] bool CompleteCpuFault(uint64_t vaddr, uint64_t size, PageFaultAccess access,
	                                    bool downloaded) {
		const auto [start, end] = GetPageRange(vaddr, size);
		for (auto page = start; page < end; page++) {
			if (!m_fault_pending.test(page)) {
				return false;
			}
		}
		for (auto page = start; page < end; page++) {
			const bool gpu_dirty = m_gpu_dirty.test(page);
			if (gpu_dirty != downloaded) {
				EXIT("CPU fault download result disagrees with GPU dirty state\n");
			}
			if (gpu_dirty) {
				m_gpu_dirty.reset(page);
				switch (access) {
					case PageFaultAccess::Read: break;
					case PageFaultAccess::Write:
						m_cpu_dirty.set(page);
						m_writable.set(page);
						break;
					default: EXIT("unsupported CPU fault access after GPU download\n");
				}
			}
			m_fault_pending.reset(page);
		}
		return true;
	}

	[[nodiscard]] bool HasPendingFault(uint64_t vaddr, uint64_t size) const {
		const auto [start, end] = GetPageRange(vaddr, size);
		for (auto page = start; page < end; page++) {
			if (m_fault_pending.test(page)) {
				return true;
			}
		}
		return false;
	}

	[[nodiscard]] bool CompleteVirtualGpuWrite(uint64_t vaddr, uint64_t size) {
		const auto [start, end] = GetPageRange(vaddr, size);
		for (auto page = start; page < end; page++) {
			if (!m_fault_pending.test(page)) {
				return false;
			}
			if (!m_gpu_dirty.test(page)) {
				EXIT("virtual GPU write completion found a non-GPU-dirty page\n");
			}
		}
		for (auto page = start; page < end; page++) {
			m_gpu_dirty.reset(page);
			m_cpu_dirty.set(page);
			m_writable.set(page);
			m_fault_pending.reset(page);
		}
		return true;
	}

	template <DirtySource source, bool clear, typename Func>
	RegionBits ForEachModifiedRange(uint64_t vaddr, uint64_t size, Func&& func) {
		const auto [start, end] = GetPageRange(vaddr, size);
		auto mask               = GetBits<source>();
		if constexpr (source == DirtySource::Cpu) {
			mask &= ~m_fault_pending;
		}
		for (auto page = 0u; page < start; page++) {
			mask.reset(page);
		}
		for (auto page = end; page < TRACKER_REGION_PAGES; page++) {
			mask.reset(page);
		}
		if constexpr (clear) {
			auto& bits = GetBits<source>();
			for (auto page = start; page < end; page++) {
				if (mask.test(page)) {
					bits.reset(page);
				}
			}
		}
		if constexpr (source == DirtySource::Cpu && clear) {
			auto changed = m_cpu_dirty ^ m_writable;
			m_writable   = m_cpu_dirty;
			ApplyProtection(changed, true);
			ForEachRange(mask, std::forward<Func>(func));
			return changed;
		}
		ForEachRange(mask, std::forward<Func>(func));
		if constexpr (clear) {
			return mask;
		}
		return {};
	}

	void ApplyProtection(const RegionBits& changed, bool track) {
		ForEachRange(changed, [this, track](uint64_t vaddr, uint64_t size) {
			m_page_manager.UpdatePageWatchers(track, vaddr, size);
		});
	}

	void ApplyGpuProtection(const RegionBits& changed, bool track, PageWatchMode mode) {
		if (mode != PageWatchMode::Write && mode != PageWatchMode::ReadWrite) {
			EXIT("unsupported GPU page-watch mode\n");
		}
		ForEachRange(changed, [this, track, mode](uint64_t vaddr, uint64_t size) {
			m_page_manager.UpdatePageWatchers(track, vaddr, size, mode);
		});
	}

	TrackingSpinLock lock;

private:
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

	template <typename Func>
	void ForEachRange(const RegionBits& bits, Func&& func) const {
		size_t page = 0;
		while (page < TRACKER_REGION_PAGES) {
			while (page < TRACKER_REGION_PAGES && !bits.test(page)) {
				page++;
			}
			const auto start = page;
			while (page < TRACKER_REGION_PAGES && bits.test(page)) {
				page++;
			}
			if (start != page) {
				func(m_cpu_addr + start * TRACKER_PAGE_SIZE, (page - start) * TRACKER_PAGE_SIZE);
			}
		}
	}

	PageManager& m_page_manager;
	uint64_t     m_cpu_addr = 0;
	RegionBits   m_cpu_dirty;
	RegionBits   m_gpu_dirty;
	RegionBits   m_writable;
	RegionBits   m_fault_pending;
	RegionBits   m_tracked;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
