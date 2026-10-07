#include "graphics/host_gpu/pageManager.h"

#include "common/alignment.h"
#include "common/virtualMemory.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#else
#include <unistd.h>
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#if defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)
#define KYTY_ASYNC_WRITE_WATCH 1
#endif
#endif

namespace Libs::Graphics {
namespace {

constexpr uint64_t PAGE_SIZE    = TRACKER_PAGE_SIZE;
constexpr uint64_t REGION_SIZE  = TRACKER_REGION_SIZE;
constexpr uint64_t ADDRESS_SIZE = TRACKER_ADDRESS_SIZE;
constexpr uint64_t REGION_COUNT = ADDRESS_SIZE / REGION_SIZE;

constexpr uint64_t REGION_PAGES = REGION_SIZE / PAGE_SIZE;

[[noreturn]] void FailFast(const char* reason = nullptr) noexcept {
	std::fputs("PageManager fail-fast: ", stderr);
	std::fputs(reason != nullptr ? reason : "invalid page state", stderr);
	std::fputc('\n', stderr);
	std::fflush(stderr);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	TerminateProcess(GetCurrentProcess(), static_cast<UINT>(EXCEPTION_NONCONTINUABLE_EXCEPTION));
#endif
	std::_Exit(322);
}

[[noreturn]] void Fatal(const char* format, ...) {
	std::fputs("PageManager fatal: ", stderr);
	va_list args;
	va_start(args, format);
	std::vfprintf(stderr, format, args);
	va_end(args);
	std::fputc('\n', stderr);
	std::fflush(stderr);
	std::_Exit(322);
}

class SpinGuard final {
public:
	explicit SpinGuard(std::atomic_flag& lock): m_lock(lock) {
		while (m_lock.test_and_set(std::memory_order_acquire)) {
			std::atomic_signal_fence(std::memory_order_seq_cst);
		}
	}
	~SpinGuard() { m_lock.clear(std::memory_order_release); }
	KYTY_CLASS_NO_COPY(SpinGuard);

private:
	std::atomic_flag& m_lock;
};

void ValidateRange(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		Fatal("invalid range vaddr=0x%016" PRIx64 ", size=0x%016" PRIx64, vaddr, size);
	}
}

} // namespace

struct PageManager::Impl {
	struct PageState {
		uint8_t write_watchers  : 7 = 0;
		uint8_t access_watchers : 1 = 0;

		[[nodiscard]] Common::VirtualMemory::Mode Perms() const noexcept {
			if (access_watchers != 0) {
				return Common::VirtualMemory::Mode::NoAccess;
			}
			if (write_watchers != 0) {
				return Common::VirtualMemory::Mode::Read;
			}
			return Common::VirtualMemory::Mode::ReadWrite;
		}

		template <int delta, bool is_read>
		uint32_t AddDelta(uint64_t address) {
			static_assert(delta >= -1 && delta <= 1);
			if constexpr (is_read) {
				if constexpr (delta == 1) {
					if (access_watchers != 0) {
						Fatal("read-watcher overflow at 0x%016" PRIx64, address);
					}
					return ++access_watchers;
				} else if constexpr (delta == -1) {
					if (access_watchers == 0) {
						Fatal("read-watcher underflow at 0x%016" PRIx64, address);
					}
					return --access_watchers;
				} else {
					return access_watchers;
				}
			} else {
				if constexpr (delta == 1) {
					if (write_watchers == 0x7f) {
						Fatal("write-watcher overflow at 0x%016" PRIx64, address);
					}
					return ++write_watchers;
				} else if constexpr (delta == -1) {
					if (write_watchers == 0) {
						Fatal("write-watcher underflow at 0x%016" PRIx64, address);
					}
					return --write_watchers;
				} else {
					return write_watchers;
				}
			}
		}
	};
	static_assert(sizeof(PageState) == 1);

	enum class WatchMode : uint8_t {
		Sync,      // write watches use mprotect and are reported by write faults
		Promoting, // queued to switch to asynchronous write watches
		Async,     // write watches use userfaultfd write protection, see HarvestWrites
		Failed,    // the switch failed; the region stays synchronous
	};

	struct Region {
		std::atomic_flag                    lock = ATOMIC_FLAG_INIT;
		std::array<PageState, REGION_PAGES> pages;
		uint64_t                            base = 0;
		std::atomic<WatchMode>              mode {WatchMode::Sync};
		std::atomic_uint32_t                write_faults {0};
		std::atomic_uint64_t                fault_window {0};
		uint32_t                            idle_harvests = 0;
		// Async mode: write-watched pages whose userfaultfd protection is armed. A harvest
		// reports armed pages the CPU wrote since they were armed.
		RegionBits armed;
	};

	// A region switches to asynchronous write watches after this many write faults within
	// a window of harvests, and back after this many harvests in a row find no write.
	static constexpr uint32_t ASYNC_PROMOTION_FAULTS = 64;
	static constexpr uint64_t ASYNC_PROMOTION_WINDOW = 64;
	static constexpr uint32_t ASYNC_IDLE_HARVESTS    = 512;

	Impl() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		SYSTEM_INFO info {};
		GetSystemInfo(&info);
		if (info.dwPageSize != PAGE_SIZE) {
			Fatal("unsupported host page size 0x%08" PRIx32,
			      static_cast<uint32_t>(info.dwPageSize));
		}
#elif defined(__APPLE__)
		// Under Rosetta the host page size is 4 KB, matching TRACKER_PAGE_SIZE.
		if (static_cast<uint64_t>(getpagesize()) != PAGE_SIZE) {
			Fatal("unsupported host page size 0x%08" PRIx32, static_cast<uint32_t>(getpagesize()));
		}
#else
		const auto host_page_size = ::sysconf(_SC_PAGESIZE);
		if (host_page_size < 0 || static_cast<uint64_t>(host_page_size) != PAGE_SIZE) {
			Fatal("unsupported host page size %ld", static_cast<long>(host_page_size));
		}
#endif
		regions = std::make_unique<std::atomic<Region*>[]>(REGION_COUNT);
#if defined(KYTY_ASYNC_WRITE_WATCH)
		OpenAsyncWriteWatch();
#endif
	}

	~Impl() {
		for (const auto& region: region_storage) {
			SpinGuard lock(region->lock);
			for (auto& page: region->pages) {
				if (page.write_watchers != 0 || page.access_watchers != 0) {
					FailFast("PageManager destroyed with live page state");
				}
			}
		}
#if defined(KYTY_ASYNC_WRITE_WATCH)
		CloseAsyncWriteWatch();
#endif
	}

	Region* FindRegion(uint64_t vaddr) const noexcept {
		return vaddr < ADDRESS_SIZE ? regions[vaddr / REGION_SIZE].load(std::memory_order_acquire)
		                            : nullptr;
	}

	Region* GetOrCreateRegion(uint64_t vaddr) {
		const auto index = vaddr / REGION_SIZE;
		if (auto* region = regions[index].load(std::memory_order_acquire); region != nullptr) {
			return region;
		}
		std::lock_guard lock(region_mutex);
		if (auto* region = regions[index].load(std::memory_order_acquire); region != nullptr) {
			return region;
		}
		auto  region = std::make_unique<Region>();
		auto* ptr    = region.get();
		ptr->base    = index * REGION_SIZE;
		region_storage.push_back(std::move(region));
		regions[index].store(ptr, std::memory_order_release);
		return ptr;
	}

	void Protect(uint64_t vaddr, uint64_t size, Common::VirtualMemory::Mode mode) noexcept {
		if (!Libs::LibKernel::Memory::ProtectGuestHostMemory(vaddr, size, mode)) {
			Fatal("address-space protection failed at 0x%016" PRIx64 ", mode=0x%08" PRIx32, vaddr,
			      static_cast<uint32_t>(mode));
		}
	}

	template <bool track, bool is_read, bool masked>
	void UpdateRegionWatchers(Region& region, uint64_t base_addr, size_t first, size_t last,
	                          const RegionBits* mask = nullptr) {
		SpinGuard lock(region.lock);
#if defined(KYTY_ASYNC_WRITE_WATCH)
		if (region.mode.load(std::memory_order_relaxed) == WatchMode::Async) {
			UpdateAsyncRegionWatchers<track, is_read, masked>(region, base_addr, first, last, mask);
			return;
		}
#endif
		auto      perms                 = region.pages[first].Perms();
		uint64_t  range_begin           = 0;
		uint64_t  range_bytes           = 0;
		uint64_t  potential_range_bytes = 0;

		const auto release_pending = [&] {
			if (range_bytes != 0) {
				Protect(base_addr + range_begin * PAGE_SIZE, range_bytes, perms);
				range_bytes           = 0;
				potential_range_bytes = 0;
			}
		};

		for (size_t page_index = first; page_index < last; page_index++) {
			auto&      page    = region.pages[page_index];
			const auto address = base_addr + page_index * PAGE_SIZE;
			const bool update  = !masked || mask->Get(page_index);

			const auto old_perms = page.Perms();
			const auto new_count = update ? page.AddDelta<track ? 1 : -1, is_read>(address)
			                              : page.AddDelta<0, is_read>(address);
			const auto new_perms = page.Perms();

			if (new_perms != perms) [[unlikely]] {
				release_pending();
				perms = new_perms;
			} else if (range_bytes != 0) {
				potential_range_bytes += PAGE_SIZE;
			}

			if (!update) {
				continue;
			}

			const bool watcher_edge = (track && new_count == 1) || (!track && new_count == 0);
			if (watcher_edge && old_perms != new_perms) {
				if (range_bytes == 0) {
					range_begin           = page_index;
					potential_range_bytes = PAGE_SIZE;
				}
				range_bytes = potential_range_bytes;
			}
		}

		release_pending();
	}

#if defined(KYTY_ASYNC_WRITE_WATCH)
	using Mode   = Common::VirtualMemory::Mode;
	using Writes = std::vector<std::pair<uint64_t, uint64_t>>;

	void OpenAsyncWriteWatch() {
		const char* setting = std::getenv("KYTY_ASYNC_WRITE_WATCH");
		if (setting == nullptr || std::strcmp(setting, "0") == 0) {
			return;
		}
		int fd = static_cast<int>(::syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK));
		if (fd < 0 && errno == EPERM) {
			fd = static_cast<int>(
			    ::syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
		}
		if (fd < 0) {
			std::fprintf(stderr, "PageManager: userfaultfd unavailable (%s)\n", std::strerror(errno));
			return;
		}
		constexpr uint64_t features =
		    UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_HUGETLBFS_SHMEM | UFFD_FEATURE_WP_UNPOPULATED;
		uffdio_api api {};
		api.api      = UFFD_API;
		api.features = features;
		if (::ioctl(fd, UFFDIO_API, &api) != 0 || (api.features & features) != features) {
			std::fprintf(stderr, "PageManager: asynchronous write protection unsupported\n");
			::close(fd);
			return;
		}
		const int pagemap = ::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
		if (pagemap < 0) {
			::close(fd);
			return;
		}
		uffd         = fd;
		pagemap_fd   = pagemap;
		std::fprintf(stderr, "PageManager: asynchronous write watches enabled\n");
	}

	void CloseAsyncWriteWatch() {
		if (uffd >= 0) {
			::close(pagemap_fd);
			::close(uffd);
		}
	}

	bool Register(uint64_t vaddr, uint64_t size) const noexcept {
		uffdio_register registration {};
		registration.range.start = vaddr;
		registration.range.len   = size;
		registration.mode        = UFFDIO_REGISTER_MODE_WP;
		return ::ioctl(uffd, UFFDIO_REGISTER, &registration) == 0;
	}

	// Ranges outside a registered mapping (ENOENT) are registered on first use.
	bool WriteProtect(uint64_t vaddr, uint64_t size) const noexcept {
		uffdio_writeprotect protect {};
		protect.range.start = vaddr;
		protect.range.len   = size;
		protect.mode        = UFFDIO_WRITEPROTECT_MODE_WP;
		if (::ioctl(uffd, UFFDIO_WRITEPROTECT, &protect) == 0) {
			return true;
		}
		return errno == ENOENT && Register(vaddr, size) &&
		       ::ioctl(uffd, UFFDIO_WRITEPROTECT, &protect) == 0;
	}

	// Reports armed pages in [first, last) written since they were armed and re-arms them
	// in the same scan, so a later write is reported again. The reported pages are disarmed;
	// the caller re-marks those still write-watched after their owners have been notified.
	bool CollectWritesLocked(Region& region, size_t first, size_t last, Writes& out) {
		const auto armed = region.armed.FirstRangeFrom(first);
		if (armed.first >= last) {
			return true;
		}
		const auto scan_last = std::min(last, region.armed.LastRangeFrom(last).second);
		std::array<page_region, 64> found {};
		auto       cursor = region.base + armed.first * PAGE_SIZE;
		const auto end    = region.base + scan_last * PAGE_SIZE;
		while (cursor < end) {
			pm_scan_arg scan {};
			scan.size          = sizeof(scan);
			// Mappings without asynchronous write protection are skipped; armed pages are
			// always in registered mappings.
			scan.flags         = PM_SCAN_WP_MATCHING;
			scan.start         = cursor;
			scan.end           = end;
			scan.vec           = reinterpret_cast<uint64_t>(found.data());
			scan.vec_len       = found.size();
			scan.category_mask = PAGE_IS_WRITTEN;
			scan.return_mask   = PAGE_IS_WRITTEN;
			const auto count   = ::ioctl(pagemap_fd, PAGEMAP_SCAN, &scan);
			if (count < 0) {
				return false;
			}
			for (long index = 0; index < count; index++) {
				const auto begin_page = static_cast<size_t>((found[index].start - region.base) / PAGE_SIZE);
				const auto end_page   = static_cast<size_t>((found[index].end - region.base) / PAGE_SIZE);
				for (auto page = region.armed.FirstRangeFrom(begin_page); page.first < end_page;
				     page = region.armed.FirstRangeFrom(page.second)) {
					const auto stop = std::min(page.second, end_page);
					out.emplace_back(region.base + page.first * PAGE_SIZE, (stop - page.first) * PAGE_SIZE);
					region.armed.UnsetRange(page.first, stop);
					if (stop == end_page) {
						break;
					}
				}
			}
			if (scan.walk_end <= cursor || scan.walk_end >= end) {
				break;
			}
			cursor = scan.walk_end;
		}
		return true;
	}

	// Returns the region to mprotect-based watches. Armed pages are reported as written,
	// since their state can no longer be read.
	void DemoteLocked(Region& region, Writes& out) {
		for (const auto [first, last]: region.armed) {
			out.emplace_back(region.base + first * PAGE_SIZE, (last - first) * PAGE_SIZE);
		}
		region.armed.Clear();
		for (size_t page = 0; page < REGION_PAGES;) {
			const auto perms = region.pages[page].Perms();
			size_t     end   = page + 1;
			while (end < REGION_PAGES && region.pages[end].Perms() == perms) {
				end++;
			}
			if (perms == Mode::Read) {
				Protect(region.base + page * PAGE_SIZE, (end - page) * PAGE_SIZE, perms);
			}
			page = end;
		}
		region.mode.store(WatchMode::Failed, std::memory_order_relaxed);
	}

	template <bool track, bool is_read>
	bool ApplyAsync(Region& region, size_t first, size_t last, Mode perms) {
		const auto vaddr = region.base + first * PAGE_SIZE;
		const auto size  = (last - first) * PAGE_SIZE;
		// Only an access watch moves pages out of NoAccess, and only then does the host
		// protection have to be lifted.
		constexpr bool from_no_access = is_read && !track;
		if (perms == Mode::NoAccess) {
			// Block access first: a write after this faults, and one before it is collected.
			Protect(vaddr, size, Mode::NoAccess);
			Writes writes;
			if (!CollectWritesLocked(region, first, last, writes)) {
				return false;
			}
			QueueWrites(writes);
			region.armed.UnsetRange(first, last);
			return true;
		}
		if (perms == Mode::Read) {
			// Arm before lifting NoAccess so that no write slips through untracked.
			if (!WriteProtect(vaddr, size)) {
				return false;
			}
			if constexpr (from_no_access) {
				Protect(vaddr, size, Mode::ReadWrite);
			}
			region.armed.SetRange(first, last);
			return true;
		}
		// A released write watch needs no system call: the kernel lifts a protection that is
		// still armed on the next write, and unarmed pages are never reported.
		region.armed.UnsetRange(first, last);
		if constexpr (from_no_access) {
			Protect(vaddr, size, Mode::ReadWrite);
		}
		return true;
	}

	// Async regions apply each page's own transition: re-arming a page that is already
	// armed would discard a write that has not been harvested yet.
	template <bool track, bool is_read, bool masked>
	void UpdateAsyncRegionWatchers(Region& region, uint64_t base_addr, size_t first, size_t last,
	                               const RegionBits* mask) {
		RegionBits changed;
		for (size_t page_index = first; page_index < last; page_index++) {
			if (masked && !mask->Get(page_index)) {
				continue;
			}
			auto&      page      = region.pages[page_index];
			const auto old_perms = page.Perms();
			(void)page.AddDelta<track ? 1 : -1, is_read>(base_addr + page_index * PAGE_SIZE);
			if (page.Perms() != old_perms) {
				changed.Set(page_index);
			}
		}
		for (const auto [begin, end]: changed) {
			for (size_t run = begin; run < end;) {
				const auto perms   = region.pages[run].Perms();
				size_t     run_end = run + 1;
				while (run_end < end && region.pages[run_end].Perms() == perms) {
					run_end++;
				}
				if (!ApplyAsync<track, is_read>(region, run, run_end, perms)) {
					Writes writes;
					DemoteLocked(region, writes);
					QueueWrites(writes);
					return;
				}
				run = run_end;
			}
		}
	}

	void QueueWrites(const Writes& writes) {
		if (writes.empty()) {
			return;
		}
		std::lock_guard lock(pending_mutex);
		pending_writes.insert(pending_writes.end(), writes.begin(), writes.end());
	}

	// Returns an idle region to mprotect-based watches. Writes that land before the
	// protection is restored are collected by the scan that follows it.
	void RelaxLocked(Region& region, Writes& out) {
		for (const auto [first, last]: region.armed) {
			Protect(region.base + first * PAGE_SIZE, (last - first) * PAGE_SIZE, Mode::Read);
		}
		if (!CollectWritesLocked(region, 0, REGION_PAGES, out)) {
			DemoteLocked(region, out);
			return;
		}
		region.armed.Clear();
		region.idle_harvests = 0;
		region.write_faults.store(0, std::memory_order_relaxed);
		region.fault_window.store(harvest_epoch.load(std::memory_order_relaxed),
		                          std::memory_order_relaxed);
		region.mode.store(WatchMode::Sync, std::memory_order_relaxed);
	}

	void Promote(Region& region) {
		SpinGuard lock(region.lock);
		if (region.mode.load(std::memory_order_relaxed) != WatchMode::Promoting) {
			return;
		}
		// Arm every write-watched page while mprotect still blocks writes, then lift it.
		RegionBits watched;
		for (size_t page = 0; page < REGION_PAGES; page++) {
			if (region.pages[page].Perms() == Mode::Read) {
				watched.Set(page);
			}
		}
		// Registering the whole region fails when it has unmapped holes; the watched pages
		// are then registered run by run, and others on first use (see WriteProtect).
		if (!Register(region.base, REGION_SIZE)) {
			for (const auto [first, last]: watched) {
				if (!Register(region.base + first * PAGE_SIZE, (last - first) * PAGE_SIZE)) {
					region.mode.store(WatchMode::Failed, std::memory_order_relaxed);
					return;
				}
			}
		}
		for (const auto [first, last]: watched) {
			if (!WriteProtect(region.base + first * PAGE_SIZE, (last - first) * PAGE_SIZE)) {
				region.mode.store(WatchMode::Failed, std::memory_order_relaxed);
				return;
			}
		}
		for (const auto [first, last]: watched) {
			Protect(region.base + first * PAGE_SIZE, (last - first) * PAGE_SIZE, Mode::ReadWrite);
		}
		region.armed         = watched;
		region.idle_harvests = 0;
		region.mode.store(WatchMode::Async, std::memory_order_relaxed);
		std::lock_guard async_lock(async_mutex);
		async_regions.push_back(&region);
		async_regions_count.fetch_add(1, std::memory_order_release);
	}

	void PromoteQueued() {
		std::vector<Region*> candidates;
		{
			std::lock_guard lock(region_mutex);
			for (const auto& region: region_storage) {
				if (region->mode.load(std::memory_order_relaxed) == WatchMode::Promoting) {
					candidates.push_back(region.get());
				}
			}
		}
		for (auto* region: candidates) {
			Promote(*region);
		}
	}

	// Pages still write-watched after their owners were notified were re-armed by the scan.
	void RemarkArmed(const Writes& writes) {
		for (const auto& [vaddr, size]: writes) {
			auto* region = FindRegion(vaddr);
			if (region == nullptr) {
				continue;
			}
			SpinGuard  lock(region->lock);
			if (region->mode.load(std::memory_order_relaxed) != WatchMode::Async) {
				continue;
			}
			const auto first = static_cast<size_t>((vaddr - region->base) / PAGE_SIZE);
			const auto last  = first + static_cast<size_t>(size / PAGE_SIZE);
			for (auto page = first; page < last; page++) {
				if (region->pages[page].Perms() == Mode::Read) {
					region->armed.Set(page);
				}
			}
		}
	}

	void HarvestWrites(WriteSink sink, void* context) {
		if (uffd < 0) {
			return;
		}
		harvest_epoch.fetch_add(1, std::memory_order_relaxed);
		if (promotion_pending.exchange(false, std::memory_order_acq_rel)) {
			PromoteQueued();
		}
		Writes writes;
		{
			std::lock_guard lock(pending_mutex);
			writes.swap(pending_writes);
		}
		const auto           queued = writes.size();
		std::vector<Region*> regions_snapshot;
		{
			std::lock_guard lock(async_mutex);
			regions_snapshot = async_regions;
		}
		std::vector<Region*> leaving;
		for (auto* region: regions_snapshot) {
			SpinGuard lock(region->lock);
			if (region->mode.load(std::memory_order_relaxed) != WatchMode::Async) {
				leaving.push_back(region);
				continue;
			}
			const auto before = writes.size();
			if (!region->armed.None() && !CollectWritesLocked(*region, 0, REGION_PAGES, writes)) {
				DemoteLocked(*region, writes);
				leaving.push_back(region);
			} else if (writes.size() != before) {
				region->idle_harvests = 0;
			} else if (++region->idle_harvests >= ASYNC_IDLE_HARVESTS) {
				RelaxLocked(*region, writes);
				leaving.push_back(region);
			}
		}
		if (!leaving.empty()) {
			std::lock_guard lock(async_mutex);
			std::erase_if(async_regions, [&](Region* region) {
				return std::find(leaving.begin(), leaving.end(), region) != leaving.end();
			});
			async_regions_count.store(static_cast<uint32_t>(async_regions.size()),
			                          std::memory_order_release);
		}
		for (const auto& [vaddr, size]: writes) {
			sink(context, vaddr, size);
		}
		RemarkArmed(Writes(writes.begin() + static_cast<std::ptrdiff_t>(queued), writes.end()));
	}

	void HarvestRange(uint64_t vaddr, uint64_t size, WriteSink sink, void* context) {
		if (uffd < 0 || async_regions_count.load(std::memory_order_acquire) == 0) {
			return;
		}
		Writes     writes;
		const auto begin = Common::AlignDown(vaddr, PAGE_SIZE);
		const auto end   = Common::AlignUp(vaddr + size, PAGE_SIZE);
		for (auto chunk = begin; chunk < end;) {
			const auto chunk_end   = std::min(end, Common::AlignUp(chunk + 1, REGION_SIZE));
			auto*      region      = FindRegion(chunk);
			const auto region_base = Common::AlignDown(chunk, REGION_SIZE);
			if (region != nullptr && region->mode.load(std::memory_order_relaxed) == WatchMode::Async) {
				SpinGuard lock(region->lock);
				if (region->mode.load(std::memory_order_relaxed) == WatchMode::Async &&
				    !CollectWritesLocked(*region, (chunk - region_base) / PAGE_SIZE,
				                         (chunk_end - region_base) / PAGE_SIZE, writes)) {
					DemoteLocked(*region, writes);
				}
			}
			chunk = chunk_end;
		}
		for (const auto& [address, bytes]: writes) {
			sink(context, address, bytes);
		}
		RemarkArmed(writes);
		QueueWrites(writes);
	}

	void NoteWriteFault(uint64_t vaddr) noexcept {
		if (uffd < 0) {
			return;
		}
		auto* region = FindRegion(vaddr);
		if (region == nullptr) {
			return;
		}
		const auto epoch = harvest_epoch.load(std::memory_order_relaxed);
		if (epoch - region->fault_window.load(std::memory_order_relaxed) > ASYNC_PROMOTION_WINDOW) {
			region->fault_window.store(epoch, std::memory_order_relaxed);
			region->write_faults.store(0, std::memory_order_relaxed);
		}
		if (region->write_faults.fetch_add(1, std::memory_order_relaxed) + 1 !=
		    ASYNC_PROMOTION_FAULTS) {
			return;
		}
		auto expected = WatchMode::Sync;
		if (region->mode.compare_exchange_strong(expected, WatchMode::Promoting,
		                                         std::memory_order_relaxed)) {
			promotion_pending.store(true, std::memory_order_release);
		}
	}

	int                  uffd       = -1;
	int                  pagemap_fd = -1;
	std::atomic_bool     promotion_pending {false};
	std::atomic_uint64_t harvest_epoch {0};
	std::atomic_uint32_t async_regions_count {0};
	std::mutex           async_mutex;
	std::vector<Region*> async_regions;
	std::mutex           pending_mutex;
	Writes               pending_writes;
#endif

	template <bool track, bool is_read>
	void UpdatePageWatchers(uint64_t vaddr, uint64_t size) {
		ValidateRange(vaddr, size);
		const auto begin = Common::AlignDown(vaddr, PAGE_SIZE);
		const auto end   = Common::AlignUp(vaddr + size, PAGE_SIZE);
		for (auto chunk_begin = begin; chunk_begin < end;) {
			const auto chunk_end = std::min(end, Common::AlignUp(chunk_begin + 1, REGION_SIZE));
			const auto region_base = Common::AlignDown(chunk_begin, REGION_SIZE);
			auto*      region = track ? GetOrCreateRegion(chunk_begin) : FindRegion(chunk_begin);
			if (region == nullptr) {
				Fatal("untracking unknown page 0x%016" PRIx64, chunk_begin);
			}
			const auto first = static_cast<size_t>((chunk_begin - region_base) / PAGE_SIZE);
			const auto last  = static_cast<size_t>((chunk_end - region_base) / PAGE_SIZE);
			UpdateRegionWatchers<track, is_read, false>(*region, region_base, first, last);
			chunk_begin = chunk_end;
		}
	}

	std::unique_ptr<std::atomic<Region*>[]> regions;
	std::vector<std::unique_ptr<Region>>    region_storage;
	std::mutex                              region_mutex;
};

static_assert(std::atomic<void*>::is_always_lock_free);

PageManager::PageManager(): m_impl(std::make_unique<Impl>()) {}

PageManager::~PageManager() = default;

uint64_t PageManager::GetPageSize() const {
	return PAGE_SIZE;
}

template <bool track>
void PageManager::UpdatePageWatchers(uint64_t vaddr, uint64_t size) {
	m_impl->UpdatePageWatchers<track, false>(vaddr, size);
}

template void PageManager::UpdatePageWatchers<true>(uint64_t, uint64_t);
template void PageManager::UpdatePageWatchers<false>(uint64_t, uint64_t);

template <bool track, bool is_read>
void PageManager::UpdatePageWatchersForRegion(uint64_t base_addr, RegionBits& mask) {
	if (base_addr % REGION_SIZE != 0 || base_addr >= ADDRESS_SIZE ||
	    REGION_SIZE > ADDRESS_SIZE - base_addr) {
		Fatal("invalid tracking region base 0x%016" PRIx64, base_addr);
	}

	const auto start_range = mask.FirstRange();
	const auto end_range   = mask.LastRange();
	if (start_range.first == REGION_PAGES) {
		FailFast("empty region watcher mask");
	}
	const auto first = start_range.first;
	const auto last  = end_range.second;
	if (start_range.second == end_range.second) {
		m_impl->UpdatePageWatchers<track, is_read>(base_addr + first * PAGE_SIZE,
		                                           (last - first) * PAGE_SIZE);
		return;
	}

	auto* region = track ? m_impl->GetOrCreateRegion(base_addr) : m_impl->FindRegion(base_addr);
	if (region == nullptr) {
		Fatal("untracking unknown region 0x%016" PRIx64, base_addr);
	}
	m_impl->UpdateRegionWatchers<track, is_read, true>(*region, base_addr, first, last, &mask);
}

void PageManager::HarvestWrites(WriteSink sink, void* context) {
#if defined(KYTY_ASYNC_WRITE_WATCH)
	m_impl->HarvestWrites(sink, context);
#else
	(void)sink;
	(void)context;
#endif
}

void PageManager::HarvestRange(uint64_t vaddr, uint64_t size, WriteSink sink, void* context) {
#if defined(KYTY_ASYNC_WRITE_WATCH)
	m_impl->HarvestRange(vaddr, size, sink, context);
#else
	(void)vaddr;
	(void)size;
	(void)sink;
	(void)context;
#endif
}

void PageManager::NoteWriteFault(uint64_t vaddr) noexcept {
#if defined(KYTY_ASYNC_WRITE_WATCH)
	m_impl->NoteWriteFault(vaddr);
#else
	(void)vaddr;
#endif
}

bool PageManager::AsyncWriteWatchEnabled() const noexcept {
#if defined(KYTY_ASYNC_WRITE_WATCH)
	return m_impl->uffd >= 0;
#else
	return false;
#endif
}

template void PageManager::UpdatePageWatchersForRegion<true, true>(uint64_t, RegionBits&);
template void PageManager::UpdatePageWatchersForRegion<true, false>(uint64_t, RegionBits&);
template void PageManager::UpdatePageWatchersForRegion<false, true>(uint64_t, RegionBits&);
template void PageManager::UpdatePageWatchersForRegion<false, false>(uint64_t, RegionBits&);

} // namespace Libs::Graphics
