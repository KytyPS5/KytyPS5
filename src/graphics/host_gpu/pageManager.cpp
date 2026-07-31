#include "graphics/host_gpu/pageManager.h"

#include "graphics/host_gpu/regionDefinitions.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <limits>
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
#elif defined(__APPLE__)
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#else
#include <execinfo.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Libs::Graphics {
namespace {

constexpr uint64_t PAGE_SIZE    = TRACKER_PAGE_SIZE;
constexpr uint64_t REGION_SIZE  = TRACKER_REGION_SIZE;
constexpr uint64_t ADDRESS_SIZE = TRACKER_ADDRESS_SIZE;
constexpr uint64_t REGION_COUNT = ADDRESS_SIZE / REGION_SIZE;

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
// The tracker reuses Win32 memory-protection tags as internal page-state values (on
// Windows they come from <windows.h> and are what VirtualQuery returns). Mirror the
// canonical Win32 numeric values so the shared state-machine logic is identical.
constexpr uint32_t PAGE_NOACCESS  = 0x01;
constexpr uint32_t PAGE_READONLY  = 0x02;
constexpr uint32_t PAGE_READWRITE = 0x04;
#endif
constexpr uint64_t REGION_PAGES = REGION_SIZE / PAGE_SIZE;

constexpr uint32_t NO_ACCESS_PROTECTION  = PAGE_NOACCESS;
constexpr uint32_t READ_ONLY_PROTECTION  = PAGE_READONLY;
constexpr uint32_t READ_WRITE_PROTECTION = PAGE_READWRITE;
// Zero is the unknown protection sentinel.
constexpr uint32_t UNKNOWN_PROTECTION = 0;

[[noreturn]] void FailFast(const char* reason = nullptr) noexcept {
	std::fputs("PageManager fail-fast: ", stderr);
	std::fputs(reason != nullptr ? reason : "invalid page state", stderr);
	std::fputc('\n', stderr);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	void*      frames[16] {};
	const auto frame_count =
	    CaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)), frames, nullptr);
	const auto image_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	for (uint16_t i = 0; i < frame_count; i++) {
		const auto address = reinterpret_cast<uintptr_t>(frames[i]);
		std::fprintf(stderr, "  frame[%u]=0x%016" PRIxPTR " image_rva=0x%016" PRIxPTR "\n", i,
		             address, address >= image_base ? address - image_base : 0);
	}
#elif !defined(__APPLE__)
	void*     frames[16] {};
	const int frame_count = ::backtrace(frames, static_cast<int>(std::size(frames)));
	::backtrace_symbols_fd(frames, frame_count, STDERR_FILENO);
#endif
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

Common::VirtualMemory::Mode ToMemoryMode(uint32_t protection) {
	switch (protection) {
		case NO_ACCESS_PROTECTION: return Common::VirtualMemory::Mode::NoAccess;
		case READ_ONLY_PROTECTION: return Common::VirtualMemory::Mode::Read;
		case READ_WRITE_PROTECTION: return Common::VirtualMemory::Mode::ReadWrite;
		default: Fatal("unmappable protection 0x%08" PRIx32, protection);
	}
}

uint32_t CurrentThread() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return GetCurrentThreadId();
#elif defined(__APPLE__)
	return static_cast<uint32_t>(pthread_mach_thread_np(pthread_self()));
#elif defined(__linux__)
	static thread_local const uint32_t tid = [] {
		const auto raw = static_cast<uint32_t>(::syscall(SYS_gettid));
		if (raw == 0) {
			FailFast("gettid returned the reserved zero owner token");
		}
		return raw;
	}();
	return tid;
#else
	FailFast("page tracking thread identity is unsupported on this platform");
#endif
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
	if (vaddr == 0 || size == 0 || vaddr >= ADDRESS_SIZE || size > ADDRESS_SIZE - vaddr) {
		Fatal("invalid range vaddr=0x%016" PRIx64 ", size=0x%016" PRIx64, vaddr, size);
	}
}

uint64_t PageStart(uint64_t vaddr) {
	return vaddr & ~(PAGE_SIZE - 1);
}

uint64_t PageEnd(uint64_t vaddr, uint64_t size) {
	ValidateRange(vaddr, size);
	return PageStart(vaddr + size - 1) + PAGE_SIZE;
}

} // namespace

struct PageManager::Impl {
	struct PageState {
		std::atomic_flag lock                = ATOMIC_FLAG_INIT;
		uint32_t         write_watchers      = 0;
		uint32_t         access_watchers     = 0;
		uint32_t         original_protection = 0;
		uint32_t         backing_writer      = 0;
		// Shadow the protection applied through Protect().
		uint32_t current_protection = UNKNOWN_PROTECTION;
		bool     resolving          = false;
	};

	struct Region {
		std::array<PageState, REGION_PAGES> pages;
	};

	class PageRangeGuard final {
	public:
		explicit PageRangeGuard(std::span<PageState*> pages): m_pages(pages) {
			for (auto* page: m_pages) {
				while (page->lock.test_and_set(std::memory_order_acquire)) {
					std::atomic_signal_fence(std::memory_order_seq_cst);
				}
			}
		}
		~PageRangeGuard() {
			for (auto it = m_pages.rbegin(); it != m_pages.rend(); ++it) {
				(*it)->lock.clear(std::memory_order_release);
			}
		}
		KYTY_CLASS_NO_COPY(PageRangeGuard);

	private:
		std::span<PageState*> m_pages;
	};

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
		for (uint64_t i = 0; i < REGION_COUNT; i++) {
			regions[i].store(nullptr, std::memory_order_relaxed);
		}
	}

	~Impl() {
		for (const auto& region: region_storage) {
			for (auto& page: region->pages) {
				SpinGuard lock(page.lock);
				if (page.write_watchers != 0 || page.access_watchers != 0 ||
				    page.backing_writer != 0 || page.resolving) {
					FailFast("PageManager destroyed with live page state");
				}
			}
		}
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
		region_storage.push_back(std::move(region));
		regions[index].store(ptr, std::memory_order_release);
		return ptr;
	}

	PageState& GetPage(Region& region, uint64_t vaddr) const {
		return region.pages[(vaddr % REGION_SIZE) / PAGE_SIZE];
	}

	static uint32_t WatcherProtection(const PageState& page) {
		if (page.access_watchers != 0) {
			return NO_ACCESS_PROTECTION;
		}
		if (page.write_watchers != 0) {
			return READ_ONLY_PROTECTION;
		}
		return page.original_protection;
	}

	static void InitializeProtection(std::span<PageState*> pages) {
		for (auto* page: pages) {
			page->original_protection = READ_WRITE_PROTECTION;
			page->current_protection  = READ_WRITE_PROTECTION;
		}
	}

	void ProtectRange(std::span<PageState*> pages, uint64_t vaddr, uint32_t protection,
	                  std::span<const uint32_t> expected_old) noexcept {
		const auto size = pages.size() * PAGE_SIZE;
		if (pages.size() != expected_old.size()) {
			FailFast("protection range state size mismatch");
		}
		for (size_t i = 0; i < pages.size(); i++) {
			const auto actual = pages[i]->current_protection;
			if (actual != UNKNOWN_PROTECTION && actual != expected_old[i]) {
				Fatal("invalid protection transition at 0x%016" PRIx64 ", old=0x%08" PRIx32
				      ", expected=0x%08" PRIx32 ", new=0x%08" PRIx32,
				      vaddr + i * PAGE_SIZE, actual, expected_old[i], protection);
			}
		}
		if (!Libs::LibKernel::Memory::ProtectGuestHostMemory(vaddr, size,
		                                                     ToMemoryMode(protection))) {
			Fatal("address-space protection failed at 0x%016" PRIx64 ", new=0x%08" PRIx32, vaddr,
			      protection);
		}
		for (auto* page: pages) {
			page->current_protection = protection;
		}
	}

	void Protect(PageState& page, uint64_t vaddr, uint32_t protection,
	             uint32_t expected_old) noexcept {
		PageState* pages[]    = {&page};
		uint32_t   expected[] = {expected_old};
		ProtectRange(pages, vaddr, protection, expected);
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

void PageManager::UpdatePageWatchers(bool track, uint64_t vaddr, uint64_t size,
                                     PageWatchMode mode) {
	if (mode != PageWatchMode::Write && mode != PageWatchMode::ReadWrite) {
		Fatal("invalid watcher mode");
	}
	const auto begin = PageStart(vaddr);
	const auto end   = PageEnd(vaddr, size);
	for (auto chunk_begin = begin; chunk_begin < end;) {
		const auto chunk_end = std::min(end, (chunk_begin / REGION_SIZE + 1) * REGION_SIZE);
		auto*      region =
		    track ? m_impl->GetOrCreateRegion(chunk_begin) : m_impl->FindRegion(chunk_begin);
		if (region == nullptr) {
			Fatal("untracking unknown page 0x%016" PRIx64, chunk_begin);
		}

		const auto page_count = static_cast<size_t>((chunk_end - chunk_begin) / PAGE_SIZE);
		std::vector<Impl::PageState*> pages;
		pages.reserve(page_count);
		for (auto address = chunk_begin; address < chunk_end; address += PAGE_SIZE) {
			pages.push_back(&m_impl->GetPage(*region, address));
		}
		Impl::PageRangeGuard lock(pages);

		std::vector<uint8_t> first_watchers(page_count);
		for (size_t i = 0; i < page_count; i++) {
			auto&      page    = *pages[i];
			const auto address = chunk_begin + i * PAGE_SIZE;
			if (page.resolving && track) {
				FailFast("new page watcher raced active fault resolution");
			}
			auto& watchers =
			    (mode == PageWatchMode::ReadWrite ? page.access_watchers : page.write_watchers);
			if (track) {
				if (watchers == std::numeric_limits<uint32_t>::max()) {
					Fatal("watcher overflow at 0x%016" PRIx64, address);
				}
				first_watchers[i] = page.write_watchers == 0 && page.access_watchers == 0;
			} else {
				if (watchers == 0) {
					Fatal("watcher underflow at 0x%016" PRIx64, address);
				}
				if (page.backing_writer != 0 && page.backing_writer != CurrentThread()) {
					Fatal("backing write ownership changed at 0x%016" PRIx64, address);
				}
			}
		}

		if (track) {
			for (size_t first = 0; first < page_count;) {
				while (first < page_count && first_watchers[first] == 0) {
					first++;
				}
				auto last = first;
				while (last < page_count && first_watchers[last] != 0) {
					last++;
				}
				if (first != last) {
					Impl::InitializeProtection(std::span {pages}.subspan(first, last - first));
				}
				first = last;
			}
		}

		std::vector<uint32_t> old_protections(page_count);
		std::vector<uint32_t> new_protections(page_count);
		std::vector<uint8_t>  transitions(page_count);
		for (size_t i = 0; i < page_count; i++) {
			auto& page = *pages[i];
			auto& watchers =
			    (mode == PageWatchMode::ReadWrite ? page.access_watchers : page.write_watchers);
			const auto old_protection = Impl::WatcherProtection(page);
			if (track) {
				watchers++;
			} else {
				watchers--;
			}
			const auto new_protection = Impl::WatcherProtection(page);
			old_protections[i]        = old_protection;
			new_protections[i]        = new_protection;
			if (new_protection != old_protection && (track || page.backing_writer == 0)) {
				transitions[i] = 1;
			}
		}

		for (size_t first = 0; first < page_count;) {
			while (first < page_count && transitions[first] == 0) {
				first++;
			}
			if (first == page_count) {
				break;
			}
			const auto protection = new_protections[first];
			auto       current    = first + 1;
			auto       last       = current;
			for (; current < page_count && new_protections[current] == protection; current++) {
				if (old_protections[current] != new_protections[current] &&
				    transitions[current] == 0) {
					break;
				}
				if (transitions[current] != 0) {
					last = current + 1;
				}
			}
			m_impl->ProtectRange(std::span {pages}.subspan(first, last - first),
			                     chunk_begin + first * PAGE_SIZE, protection,
			                     std::span {old_protections}.subspan(first, last - first));
			first = current;
		}

		for (auto* page: pages) {
			if (!track && page->backing_writer == 0 && page->write_watchers == 0 &&
			    page->access_watchers == 0) {
				page->original_protection = 0;
			}
		}
		chunk_begin = chunk_end;
	}
}

void PageManager::OnGpuMap(uint64_t, uint64_t) {}

void PageManager::OnGpuUnmap(uint64_t, uint64_t) {}

PageManager::BackingWrite::BackingWrite(PageManager& manager, uint64_t vaddr,
                                        uint64_t size) noexcept
    : m_manager(manager), m_vaddr(vaddr), m_size(size) {
	m_manager.BeginBackingWrite(vaddr, size);
}

PageManager::BackingWrite::~BackingWrite() {
	m_manager.EndBackingWrite(m_vaddr, m_size);
}

std::vector<std::unique_ptr<PageManager::BackingWrite>>
PageManager::ReserveBackingWrites(std::span<const RangeSet::Range> ranges) {
	if (ranges.empty()) {
		Fatal("cannot reserve empty backing-write ranges");
	}
	std::vector<std::unique_ptr<BackingWrite>> writes;
	writes.reserve(ranges.size());
	uint64_t begin = 0;
	uint64_t end   = 0;
	for (const auto& range: ranges) {
		if (range.address == 0 || range.size == 0 || range.size > UINT64_MAX - range.address ||
		    range.address + range.size > UINT64_MAX - (PAGE_SIZE - 1)) {
			Fatal("invalid backing-write range");
		}
		const auto page_begin = PageStart(range.address);
		const auto page_end   = PageStart(range.address + range.size + PAGE_SIZE - 1);
		if (begin != 0 && page_begin > end) {
			writes.push_back(std::make_unique<BackingWrite>(*this, begin, end - begin));
			begin = 0;
		}
		if (begin == 0) {
			begin = page_begin;
			end   = page_end;
		} else {
			end = std::max(end, page_end);
		}
	}
	writes.push_back(std::make_unique<BackingWrite>(*this, begin, end - begin));
	return writes;
}

void PageManager::BeginBackingWrite(uint64_t vaddr, uint64_t size) noexcept {
	const auto end    = PageEnd(vaddr, size);
	const auto writer = CurrentThread();
	for (auto address = PageStart(vaddr); address < end; address += PAGE_SIZE) {
		auto* region = m_impl->FindRegion(address);
		if (region == nullptr) {
			Fatal("backing write reserves an unknown page at 0x%016" PRIx64, address);
		}
		auto&     page = m_impl->GetPage(*region, address);
		SpinGuard lock(page.lock);
		if (page.resolving || page.backing_writer != 0 || page.access_watchers == 0) {
			Fatal("backing write races page resolution at 0x%016" PRIx64, address);
		}
		page.resolving      = true;
		page.backing_writer = writer;
	}
}

void PageManager::EndBackingWrite(uint64_t vaddr, uint64_t size) noexcept {
	const auto end    = PageEnd(vaddr, size);
	const auto writer = CurrentThread();
	for (auto address = PageStart(vaddr); address < end; address += PAGE_SIZE) {
		auto* region = m_impl->FindRegion(address);
		if (region == nullptr) {
			FailFast("backing write ended for an unknown page");
		}
		auto&     page = m_impl->GetPage(*region, address);
		SpinGuard lock(page.lock);
		if (!page.resolving || page.backing_writer != writer) {
			FailFast("backing write ended without matching owner and resolving state");
		}
		const auto old_protection = NO_ACCESS_PROTECTION;
		const auto new_protection = Impl::WatcherProtection(page);
		if (new_protection != old_protection) {
			m_impl->Protect(page, address, new_protection, old_protection);
		}
		if (page.write_watchers == 0 && page.access_watchers == 0) {
			page.original_protection = 0;
		}
		page.backing_writer = 0;
		page.resolving      = false;
	}
}

} // namespace Libs::Graphics
