#include "graphics/host_gpu/pageManager.h"

#include "graphics/host_gpu/regionDefinitions.h"

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
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <sys/mman.h>
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

#if defined(__APPLE__)
// Map the tracker's Win32-style protection tags to POSIX mprotect flags.
static int PageProtToPosix(uint32_t protection) {
	switch (protection) {
		case PAGE_NOACCESS: return PROT_NONE;
		case PAGE_READONLY: return PROT_READ;
		case PAGE_READWRITE: return PROT_READ | PROT_WRITE;
		default: return PROT_NONE;
	}
}

// Query the current protection of the page containing vaddr via the Mach VM map and
// collapse it to the tracker's read/write tags (execute is irrelevant to write tracking).
static uint32_t MachQueryPageProt(uint64_t vaddr) {
	auto              region_addr = static_cast<mach_vm_address_t>(vaddr);
	mach_vm_size_t    region_size = 0;
	vm_region_basic_info_data_64_t info {};
	mach_msg_type_number_t         count       = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t                    object_name = MACH_PORT_NULL;

	kern_return_t kr =
	    mach_vm_region(mach_task_self(), &region_addr, &region_size, VM_REGION_BASIC_INFO_64,
	                   reinterpret_cast<vm_region_info_t>(&info), &count, &object_name);
	if (kr != KERN_SUCCESS || region_addr > vaddr) {
		return PAGE_NOACCESS; // no region covering vaddr
	}
	if ((info.protection & VM_PROT_WRITE) != 0) {
		return PAGE_READWRITE;
	}
	if ((info.protection & VM_PROT_READ) != 0) {
		return PAGE_READONLY;
	}
	return PAGE_NOACCESS;
}
#endif

thread_local bool g_in_fault_resolution = false;

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

uint32_t CurrentThread() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return GetCurrentThreadId();
#elif defined(__APPLE__)
	return static_cast<uint32_t>(pthread_mach_thread_np(pthread_self()));
#else
	FailFast();
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
		std::atomic_flag lock                 = ATOMIC_FLAG_INIT;
		uint32_t         mappings             = 0;
		uint32_t         gpu_read_mappings    = 0;
		uint32_t         gpu_write_mappings   = 0;
		uint32_t         write_watchers       = 0;
		uint32_t         access_watchers      = 0;
		uint32_t         original_protection  = 0;
		uint32_t         backing_writer       = 0;
		bool             resolving            = false;
		bool             resolving_read_write = false;
		bool             late_read_pending    = false;
		bool             late_write_pending   = false;
	};

	struct Region {
		std::array<PageState, REGION_PAGES> pages;
	};

	Impl(PageFaultHandler handler, void* context): fault_handler(handler), fault_context(context) {
		if (fault_handler == nullptr) {
			Fatal("null fault handler");
		}
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
			Fatal("unsupported host page size 0x%08" PRIx32,
			      static_cast<uint32_t>(getpagesize()));
		}
#else
		Fatal("page-fault invalidation is not implemented on this platform");
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
				if (page.mappings != 0 || page.gpu_read_mappings != 0 ||
				    page.gpu_write_mappings != 0 || page.write_watchers != 0 ||
				    page.access_watchers != 0 || page.backing_writer != 0 || page.resolving) {
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

	static void PublishDelayedFaults(PageState& page, uint32_t old_protection,
	                                 uint32_t new_protection) {
		if (old_protection == NO_ACCESS_PROTECTION && new_protection != NO_ACCESS_PROTECTION) {
			page.late_read_pending = true;
		}
		if ((old_protection == NO_ACCESS_PROTECTION ||
		     old_protection == READ_ONLY_PROTECTION) &&
		    new_protection == READ_WRITE_PROTECTION) {
			page.late_write_pending = true;
		}
	}

	static uint32_t QueryProtection(uint64_t vaddr) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(vaddr)), &info,
		                 sizeof(info)) == 0 ||
		    info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE) {
			Fatal("basic path requires PAGE_READWRITE at 0x%016" PRIx64 " (state=0x%08" PRIx32
			      ", protection=0x%08" PRIx32 ")",
			      vaddr, static_cast<uint32_t>(info.State), static_cast<uint32_t>(info.Protect));
		}
		return info.Protect;
#elif defined(__APPLE__)
		const uint32_t protection = MachQueryPageProt(vaddr);
		if (protection != PAGE_READWRITE) {
			Fatal("basic path requires PAGE_READWRITE at 0x%016" PRIx64 " (protection=0x%08" PRIx32
			      ")",
			      vaddr, protection);
		}
		return protection;
#else
		(void)vaddr;
		Fatal("page query is unsupported on this platform");
#endif
	}

	static bool AllowsAccess(uint64_t vaddr, PageFaultAccess access) noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(vaddr)), &info,
		                 sizeof(info)) == 0 ||
		    info.State != MEM_COMMIT) {
			return false;
		}
		switch (access) {
			case PageFaultAccess::Read:
				return info.Protect == PAGE_READONLY || info.Protect == PAGE_READWRITE;
			case PageFaultAccess::Write: return info.Protect == PAGE_READWRITE;
			default: return false;
		}
#elif defined(__APPLE__)
		const uint32_t protection = MachQueryPageProt(vaddr);
		switch (access) {
			case PageFaultAccess::Read:
				return protection == PAGE_READONLY || protection == PAGE_READWRITE;
			case PageFaultAccess::Write: return protection == PAGE_READWRITE;
			default: return false;
		}
#else
		(void)vaddr;
		return false;
#endif
	}

	static void Protect(uint64_t vaddr, uint32_t protection, uint32_t expected_old,
	                    bool fault_path) noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		DWORD old_protection = 0;
		if (VirtualProtect(reinterpret_cast<void*>(static_cast<uintptr_t>(vaddr)), PAGE_SIZE,
		                   protection, &old_protection) == 0 ||
		    old_protection != expected_old) {
			if (fault_path) {
				FailFast("VirtualProtect fault transition did not match expected protection");
			}
			Fatal("invalid protection transition at 0x%016" PRIx64 ", old=0x%08" PRIx32
			      ", expected=0x%08" PRIx32 ", new=0x%08" PRIx32,
			      vaddr, static_cast<uint32_t>(old_protection), expected_old, protection);
		}
#elif defined(__APPLE__)
		// mprotect cannot report the previous protection, so the expected_old comparison
		// is dropped; the tracker is the sole mutator of these pages and drives the
		// transition from its own shadow state.
		(void)expected_old;
		if (mprotect(reinterpret_cast<void*>(static_cast<uintptr_t>(vaddr)), PAGE_SIZE,
		             PageProtToPosix(protection)) != 0) {
			if (fault_path) {
				FailFast("mprotect fault transition failed");
			}
			Fatal("mprotect failed at 0x%016" PRIx64 ", new=0x%08" PRIx32, vaddr, protection);
		}
#else
		(void)vaddr;
		(void)protection;
		(void)fault_path;
		FailFast("page protection is unsupported on this platform");
#endif
	}

	std::unique_ptr<std::atomic<Region*>[]> regions;
	std::vector<std::unique_ptr<Region>>    region_storage;
	std::mutex                              region_mutex;
	PageFaultHandler                        fault_handler = nullptr;
	void*                                   fault_context = nullptr;
};

static_assert(std::atomic<void*>::is_always_lock_free);

PageManager::PageManager(PageFaultHandler fault_handler, void* fault_context)
    : m_impl(std::make_unique<Impl>(fault_handler, fault_context)) {}

PageManager::~PageManager() = default;

uint64_t PageManager::GetPageSize() const {
	if (g_in_fault_resolution) {
		FailFast("nested page fault while resolving a watched page");
	}
	return PAGE_SIZE;
}

bool PageManager::IsTracked(uint64_t vaddr) const noexcept {
	if (g_in_fault_resolution) {
		FailFast("IsTracked called during fault resolution");
	}
	auto* region = m_impl->FindRegion(vaddr);
	if (region == nullptr) {
		return false;
	}
	auto&     page = m_impl->GetPage(*region, vaddr);
	SpinGuard lock(page.lock);
	return page.write_watchers != 0 || page.access_watchers != 0;
}

bool PageManager::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (vaddr == 0 || size == 0 || vaddr >= ADDRESS_SIZE || size > ADDRESS_SIZE - vaddr) {
		return false;
	}
	const auto end = PageStart(vaddr + size - 1) + PAGE_SIZE;
	for (auto page_vaddr = PageStart(vaddr); page_vaddr < end; page_vaddr += PAGE_SIZE) {
		auto* region = m_impl->FindRegion(page_vaddr);
		if (region == nullptr) {
			return false;
		}
		auto&     page = m_impl->GetPage(*region, page_vaddr);
		SpinGuard lock(page.lock);
		if (page.mappings == 0) {
			return false;
		}
	}
	return true;
}

bool PageManager::HasAnyMapping(uint64_t vaddr, uint64_t size) const noexcept {
	if (g_in_fault_resolution || vaddr == 0 || size == 0 || vaddr >= ADDRESS_SIZE ||
	    size > ADDRESS_SIZE - vaddr) {
		return false;
	}
	const auto end = PageEnd(vaddr, size);
	for (auto page_vaddr = PageStart(vaddr); page_vaddr < end; page_vaddr += PAGE_SIZE) {
		auto* region = m_impl->FindRegion(page_vaddr);
		if (region == nullptr) {
			continue;
		}
		auto&     page = m_impl->GetPage(*region, page_vaddr);
		SpinGuard lock(page.lock);
		if (page.mappings != 0) {
			return true;
		}
	}
	return false;
}

bool PageManager::HasGpuAccess(uint64_t vaddr, uint64_t size, GpuAccess access) const noexcept {
	if (access != GpuAccess::Read && access != GpuAccess::Write && access != GpuAccess::ReadWrite) {
		FailFast("HasGpuAccess received an invalid GPU access mode");
	}
	const bool need_read  = access == GpuAccess::Read || access == GpuAccess::ReadWrite;
	const bool need_write = access == GpuAccess::Write || access == GpuAccess::ReadWrite;
	if (vaddr == 0 || size == 0 || vaddr >= ADDRESS_SIZE || size > ADDRESS_SIZE - vaddr) {
		return false;
	}
	const auto end = PageEnd(vaddr, size);
	for (auto addr = PageStart(vaddr); addr < end; addr += PAGE_SIZE) {
		auto* region = m_impl->FindRegion(addr);
		if (region == nullptr) {
			return false;
		}
		auto&     page = m_impl->GetPage(*region, addr);
		SpinGuard lock(page.lock);
		if ((need_read && page.gpu_read_mappings == 0) ||
		    (need_write && page.gpu_write_mappings == 0)) {
			return false;
		}
	}
	return true;
}

void PageManager::UpdatePageWatchers(bool track, uint64_t vaddr, uint64_t size,
                                     PageWatchMode mode) {
	if (mode != PageWatchMode::Write && mode != PageWatchMode::ReadWrite) {
		Fatal("invalid watcher mode");
	}
	const auto end = PageEnd(vaddr, size);
	for (auto page_vaddr = PageStart(vaddr); page_vaddr < end; page_vaddr += PAGE_SIZE) {
		auto* region =
		    track ? m_impl->GetOrCreateRegion(page_vaddr) : m_impl->FindRegion(page_vaddr);
		if (region == nullptr) {
			Fatal("untracking unknown page 0x%016" PRIx64, page_vaddr);
		}
		auto&     page = m_impl->GetPage(*region, page_vaddr);
		SpinGuard lock(page.lock);
		if (page.resolving && track) {
			FailFast("new page watcher raced active fault resolution");
		}
		if (page.mappings == 0) {
			Fatal("watching unmapped page 0x%016" PRIx64, page_vaddr);
		}
		auto& watchers =
		    (mode == PageWatchMode::ReadWrite ? page.access_watchers : page.write_watchers);
		if (track) {
			if (watchers == std::numeric_limits<uint32_t>::max()) {
				Fatal("watcher overflow at 0x%016" PRIx64, page_vaddr);
			}
			const bool first_watcher = page.write_watchers == 0 && page.access_watchers == 0;
			if (first_watcher) {
				page.original_protection = Impl::QueryProtection(page_vaddr);
			}
			const auto old_protection = Impl::WatcherProtection(page);
			watchers++;
			const auto new_protection = Impl::WatcherProtection(page);
			if (new_protection != old_protection) {
				Impl::Protect(page_vaddr, new_protection, old_protection, false);
			}
			switch (new_protection) {
				case NO_ACCESS_PROTECTION:
					page.late_read_pending  = false;
					page.late_write_pending = false;
					break;
				case READ_ONLY_PROTECTION: page.late_write_pending = false; break;
				default: break;
			}
		} else {
			if (watchers == 0) {
				Fatal("watcher underflow at 0x%016" PRIx64, page_vaddr);
			}
			if (page.backing_writer != 0 && page.backing_writer != CurrentThread()) {
				Fatal("backing write ownership changed at 0x%016" PRIx64, page_vaddr);
			}
			const auto old_protection = Impl::WatcherProtection(page);
			watchers--;
			const auto new_protection = Impl::WatcherProtection(page);
			if (page.backing_writer == 0 && new_protection != old_protection) {
				Impl::Protect(page_vaddr, new_protection, old_protection, false);
			}
			if (page.backing_writer == 0) {
				Impl::PublishDelayedFaults(page, old_protection, new_protection);
			}
			if (page.backing_writer == 0 && page.write_watchers == 0 && page.access_watchers == 0) {
				page.original_protection = 0;
			}
		}
	}
}

void PageManager::OnGpuMap(uint64_t vaddr, uint64_t size, GpuAccess access) {
	if (g_in_fault_resolution) {
		FailFast("GPU mapping changed during fault resolution");
	}
	if (access != GpuAccess::Read && access != GpuAccess::Write && access != GpuAccess::ReadWrite) {
		FailFast("GPU map received an invalid access mode");
	}
	const bool gpu_read  = access == GpuAccess::Read || access == GpuAccess::ReadWrite;
	const bool gpu_write = access == GpuAccess::Write || access == GpuAccess::ReadWrite;
	const auto end       = PageEnd(vaddr, size);
	for (auto addr = PageStart(vaddr); addr < end; addr += PAGE_SIZE) {
		auto&     page = m_impl->GetPage(*m_impl->GetOrCreateRegion(addr), addr);
		SpinGuard lock(page.lock);
		if (page.resolving || page.mappings == std::numeric_limits<uint32_t>::max() ||
		    (gpu_read && page.gpu_read_mappings == std::numeric_limits<uint32_t>::max()) ||
		    (gpu_write && page.gpu_write_mappings == std::numeric_limits<uint32_t>::max())) {
			Fatal("invalid map state at 0x%016" PRIx64, addr);
		}
		page.mappings++;
		page.gpu_read_mappings += gpu_read ? 1u : 0u;
		page.gpu_write_mappings += gpu_write ? 1u : 0u;
	}
}

void PageManager::OnGpuUnmap(uint64_t vaddr, uint64_t size, GpuAccess access) {
	if (g_in_fault_resolution) {
		FailFast("GPU unmapping changed during fault resolution");
	}
	if (access != GpuAccess::Read && access != GpuAccess::Write && access != GpuAccess::ReadWrite) {
		FailFast("GPU unmap received an invalid access mode");
	}
	const bool gpu_read  = access == GpuAccess::Read || access == GpuAccess::ReadWrite;
	const bool gpu_write = access == GpuAccess::Write || access == GpuAccess::ReadWrite;
	const auto end       = PageEnd(vaddr, size);
	for (auto page_vaddr = PageStart(vaddr); page_vaddr < end; page_vaddr += PAGE_SIZE) {
		auto* region = m_impl->FindRegion(page_vaddr);
		if (region == nullptr) {
			Fatal("unmapping unknown page 0x%016" PRIx64, page_vaddr);
		}
		auto&     page = m_impl->GetPage(*region, page_vaddr);
		SpinGuard lock(page.lock);
		if (page.resolving || page.mappings == 0 || (gpu_read && page.gpu_read_mappings == 0) ||
		    (gpu_write && page.gpu_write_mappings == 0) ||
		    (page.mappings == 1 && (page.write_watchers != 0 || page.access_watchers != 0))) {
			Fatal("invalid unmap state at 0x%016" PRIx64, page_vaddr);
		}
		page.mappings--;
		page.gpu_read_mappings -= gpu_read ? 1u : 0u;
		page.gpu_write_mappings -= gpu_write ? 1u : 0u;
		if (page.mappings == 0) {
			if (page.gpu_read_mappings != 0 || page.gpu_write_mappings != 0) {
				FailFast("GPU unmap left nonzero GPU mapping counts");
			}
			page.late_read_pending  = false;
			page.late_write_pending = false;
		}
	}
}

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
	if (g_in_fault_resolution) {
		FailFast("backing write began during fault resolution");
	}
	const auto end    = PageEnd(vaddr, size);
	const auto writer = CurrentThread();
	for (auto address = PageStart(vaddr); address < end; address += PAGE_SIZE) {
		auto* region = m_impl->FindRegion(address);
		if (region == nullptr) {
			Fatal("backing write reserves an unknown page at 0x%016" PRIx64, address);
		}
		auto&     page = m_impl->GetPage(*region, address);
		SpinGuard lock(page.lock);
		if (page.mappings == 0 || page.resolving || page.backing_writer != 0 ||
		    page.access_watchers == 0) {
			Fatal("backing write races page resolution at 0x%016" PRIx64, address);
		}
		page.resolving            = true;
		page.resolving_read_write = true;
		page.backing_writer       = writer;
	}
}

void PageManager::EndBackingWrite(uint64_t vaddr, uint64_t size) noexcept {
	if (g_in_fault_resolution) {
		FailFast("backing write ended during fault resolution");
	}
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
			Impl::Protect(address, new_protection, old_protection, false);
		}
		Impl::PublishDelayedFaults(page, old_protection, new_protection);
		if (page.write_watchers == 0 && page.access_watchers == 0) {
			page.original_protection = 0;
		}
		page.backing_writer       = 0;
		page.resolving            = false;
		page.resolving_read_write = false;
	}
}

bool PageManager::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	if (g_in_fault_resolution) {
		FailFast("nested HandleFault call");
	}
	auto* region = m_impl->FindRegion(fault_vaddr);
	if (region == nullptr) {
		return false;
	}
	auto& page   = m_impl->GetPage(*region, fault_vaddr);
	bool  waited = false;
	while (true) {
		SpinGuard lock(page.lock);
		if (access == PageFaultAccess::Read && page.late_read_pending &&
		    Impl::AllowsAccess(fault_vaddr, access)) {
			page.late_read_pending = false;
			return true;
		}
		if (access == PageFaultAccess::Write && page.late_write_pending &&
		    Impl::AllowsAccess(fault_vaddr, access)) {
			page.late_write_pending = false;
			return true;
		}
		if (page.resolving) {
			if (page.backing_writer == CurrentThread()) {
				FailFast("backing writer faulted on its own reserved page");
			}
			if ((!page.resolving_read_write && access != PageFaultAccess::Write) ||
			    (page.resolving_read_write && access != PageFaultAccess::Read &&
			     access != PageFaultAccess::Write)) {
				FailFast("fault access is incompatible with the active resolver");
			}
			waited = true;
			continue;
		}
		if (page.write_watchers == 0 && page.access_watchers == 0) {
			if (access != PageFaultAccess::Read && access != PageFaultAccess::Write) {
				return false;
			}
			bool&      pending = (access == PageFaultAccess::Read ? page.late_read_pending
			                                                      : page.late_write_pending);
			const bool allowed = Impl::AllowsAccess(fault_vaddr, access);
			pending            = false;
			if (waited && !allowed) {
				FailFast("page remained inaccessible after waiting for its resolver");
			}
			// More than one CPU can fault before a protection transition becomes visible. The first
			// delayed fault consumes the hint bit; later faults must also resume once the mapped
			// page already permits the requested access. A genuinely read-only/no-access page still
			// falls through to the guest exception path.
			return allowed;
		}
		if ((access != PageFaultAccess::Read && access != PageFaultAccess::Write) ||
		    (access == PageFaultAccess::Read && page.access_watchers == 0)) {
			FailFast("fault access is incompatible with active page watchers");
		}
		page.resolving            = true;
		page.resolving_read_write = page.access_watchers != 0;
		break;
	}
	g_in_fault_resolution = true;
	const bool handled    = m_impl->fault_handler(m_impl->fault_context, access, fault_vaddr, 1,
	                                              PageFaultPhase::Invalidate);
	g_in_fault_resolution = false;
	{
		SpinGuard lock(page.lock);
		if (!handled || !page.resolving) {
			FailFast("fault invalidation did not preserve the resolving state");
		}
	}
	g_in_fault_resolution = true;
	const bool completed  = m_impl->fault_handler(m_impl->fault_context, access, fault_vaddr, 1,
	                                              PageFaultPhase::Complete);
	g_in_fault_resolution = false;
	{
		SpinGuard lock(page.lock);
		if (!completed || !page.resolving) {
			FailFast("fault completion did not preserve the resolving state");
		}
		if (page.write_watchers != 0 || page.access_watchers != 0) {
			const auto old_protection  = Impl::WatcherProtection(page);
			const bool read_only_fault = access == PageFaultAccess::Read;
			if (read_only_fault && page.access_watchers == 0) {
				FailFast("read fault completed without a read/write watcher");
			}
			page.access_watchers = 0;
			if (!read_only_fault) {
				page.write_watchers = 0;
			}
			const auto restored_protection = Impl::WatcherProtection(page);
			Impl::Protect(PageStart(fault_vaddr), restored_protection, old_protection, true);
			if (page.write_watchers == 0) {
				page.original_protection = 0;
			}
			Impl::PublishDelayedFaults(page, old_protection, restored_protection);
		} else if (!Impl::AllowsAccess(fault_vaddr, access)) {
			FailFast("fault completion left the page inaccessible");
		}
		page.resolving            = false;
		page.resolving_read_write = false;
	}
	g_in_fault_resolution = true;
	const bool released   = m_impl->fault_handler(m_impl->fault_context, access, fault_vaddr, 1,
	                                              PageFaultPhase::Release);
	g_in_fault_resolution = false;
	if (!released) {
		FailFast("fault release callback failed");
	}
	return true;
}

bool PageManager::HandleWriteRange(uint64_t vaddr, uint64_t size) noexcept {
	if (g_in_fault_resolution || vaddr == 0 || size == 0 || vaddr >= ADDRESS_SIZE ||
	    size > ADDRESS_SIZE - vaddr) {
		return false;
	}
	const auto end = PageEnd(vaddr, size);
	for (auto page_vaddr = PageStart(vaddr); page_vaddr < end; page_vaddr += PAGE_SIZE) {
		if (!IsMapped(page_vaddr, 1)) {
			continue;
		}
		const auto fault_vaddr = std::max(page_vaddr, vaddr);
		if (!HandleFault(PageFaultAccess::Write, fault_vaddr)) {
			return false;
		}
	}
	return true;
}

} // namespace Libs::Graphics
