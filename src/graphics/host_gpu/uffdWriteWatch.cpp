#include "graphics/host_gpu/uffdWriteWatch.h"

#include "common/logging/log.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Libs::Graphics {

#if defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)

UffdWriteWatch* UffdWriteWatch::Get() {
	static UffdWriteWatch* const watch = []() -> UffdWriteWatch* {
		if (const char* value = std::getenv("KYTY_UFFD"); value != nullptr && std::strcmp(value, "0") == 0) {
			return nullptr;
		}
		// Unprivileged processes may watch their own user-mode writes only (vm.unprivileged_
		// userfaultfd=0); the asynchronous mode resolves kernel-mode writes too.
		const int uffd = static_cast<int>(::syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
		if (uffd < 0) {
			return nullptr;
		}
		uffdio_api api {};
		api.api      = UFFD_API;
		api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_HUGETLBFS_SHMEM | UFFD_FEATURE_WP_UNPOPULATED;
		const int pagemap = ::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
		if (::ioctl(uffd, UFFDIO_API, &api) != 0 || (api.features & UFFD_FEATURE_WP_ASYNC) == 0 || pagemap < 0) {
			::close(uffd);
			if (pagemap >= 0) ::close(pagemap);
			return nullptr;
		}
		Log::WriteToConsoleAndLog("GPU: CPU writes to hot pages are seen through userfaultfd.\n");
		return new UffdWriteWatch(uffd, pagemap);
	}();
	return watch;
}

void UffdWriteWatch::Register(uint64_t vaddr, uint64_t size) {
	uffdio_register reg {};
	reg.range.start = vaddr;
	reg.range.len   = size;
	reg.mode        = UFFDIO_REGISTER_MODE_WP;
	if (::ioctl(m_uffd, UFFDIO_REGISTER, &reg) != 0) {
		return;
	}
	std::unique_lock lock(m_mutex);
	m_registered.Add(vaddr, size);
}

void UffdWriteWatch::Unregister(uint64_t vaddr, uint64_t size) {
	std::unique_lock lock(m_mutex);
	m_registered.Subtract(vaddr, size);
}

bool UffdWriteWatch::Covers(uint64_t vaddr, uint64_t size) const {
	std::shared_lock lock(m_mutex);
	return m_registered.Contains(vaddr, size);
}

void UffdWriteWatch::Protect(uint64_t vaddr, uint64_t size) {
	uffdio_writeprotect protect {};
	protect.range.start = vaddr;
	protect.range.len   = size;
	protect.mode        = UFFDIO_WRITEPROTECT_MODE_WP;
	(void)::ioctl(m_uffd, UFFDIO_WRITEPROTECT, &protect);
}

void UffdWriteWatch::TakeWritten(uint64_t vaddr, uint64_t size, std::vector<GuestRange>& written) {
	std::array<page_region, 32> regions {};
	uint64_t                    start = vaddr;
	const uint64_t              end   = vaddr + size;
	while (start < end) {
		pm_scan_arg arg {};
		arg.size          = sizeof(arg);
		arg.flags         = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
		arg.start         = start;
		arg.end           = end;
		arg.vec           = reinterpret_cast<uint64_t>(regions.data());
		arg.vec_len       = regions.size();
		arg.category_mask = PAGE_IS_WRITTEN;
		arg.return_mask   = PAGE_IS_WRITTEN;
		const auto count  = ::ioctl(m_pagemap, PAGEMAP_SCAN, &arg);
		if (count < 0) {
			written.push_back({start, end - start});
			return;
		}
		for (long i = 0; i < count; i++) {
			written.push_back({regions[i].start, regions[i].end - regions[i].start});
		}
		if (arg.walk_end <= start) {
			return;
		}
		start = arg.walk_end;
	}
}

#else

UffdWriteWatch* UffdWriteWatch::Get() {
	return nullptr;
}
void UffdWriteWatch::Register(uint64_t, uint64_t) {}
void UffdWriteWatch::Unregister(uint64_t, uint64_t) {}
bool UffdWriteWatch::Covers(uint64_t, uint64_t) const {
	return false;
}
void UffdWriteWatch::Protect(uint64_t, uint64_t) {}
void UffdWriteWatch::TakeWritten(uint64_t vaddr, uint64_t size, std::vector<GuestRange>& written) {
	written.push_back({vaddr, size});
}

#endif

} // namespace Libs::Graphics
