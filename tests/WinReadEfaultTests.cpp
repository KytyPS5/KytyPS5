// A file read into a page the GPU resource tracker has made unwritable fails in the host
// read and must be reported as EFAULT, so the guest bounce-buffer retry can run. On Windows
// ReadFile returns ERROR_NOACCESS; on POSIX, read(2) already returns EFAULT.
#include "common/file.h"
#include "common/platform/sysFileIO.h"
#include "kernel/guestFileRead.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

constexpr uint32_t PageSize = 4096;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "WinReadEfaultTests: failed: %s\n", message);
		std::exit(1);
	}
}

uint8_t Pattern(uint32_t i) {
	return static_cast<uint8_t>(i * 7u + 3u);
}

uint8_t* AllocPage() {
#if defined(_WIN32)
	auto* page = static_cast<uint8_t*>(
	    VirtualAlloc(nullptr, PageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	Check(page != nullptr, "alloc page");
#else
	auto* page = static_cast<uint8_t*>(
	    mmap(nullptr, PageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
	Check(page != MAP_FAILED, "alloc page");
#endif
	return page;
}

// Stands in for the tracker: read-only on Windows, as the tracker leaves it; no access on POSIX.
void ProtectPage(uint8_t* page, bool protect) {
#if defined(_WIN32)
	DWORD old = 0;
	Check(VirtualProtect(page, PageSize, protect ? PAGE_READONLY : PAGE_READWRITE, &old) != 0,
	      "protect page");
#else
	Check(mprotect(page, PageSize, protect ? PROT_NONE : (PROT_READ | PROT_WRITE)) == 0,
	      "protect page");
#endif
}

void FreePage(uint8_t* page) {
#if defined(_WIN32)
	VirtualFree(page, 0, MEM_RELEASE);
#else
	munmap(page, PageSize);
#endif
}

uint8_t* g_tracked_page = nullptr;
int      g_check_calls  = 0; // known_unwritable, before the first read
int      g_copy_calls   = 0; // recovers_fault, before each bounce copy

// Memory the guest map does not describe, such as a host buffer handed to a guest callback: not
// known to fail.
bool NotKnown(const void* /*dst*/, uint64_t /*size*/) {
	g_check_calls++;
	return false;
}

// Guest memory the map describes as reserved or without write access.
bool KnownUnwritable(const void* /*dst*/, uint64_t /*size*/) {
	g_check_calls++;
	return true;
}

// Only the first page of the destination is mapped writable guest memory; the next one is
// reserved.
bool PastFirstPage(const void* dst, uint64_t size) {
	g_check_calls++;
	return static_cast<const uint8_t*>(dst) + size > g_tracked_page + PageSize;
}

// The destination is not committed writable guest memory, so a fault in it is not recovered.
// That includes memory outside the guest map.
bool NotRecovered(const void* /*dst*/, uint64_t /*size*/) {
	g_copy_calls++;
	return false;
}

// The destination is tracked guest memory: the copy into it takes the fault, so unprotect it, as
// the fault handler would.
bool UnprotectTracked(const void* dst, uint64_t size) {
	g_copy_calls++;
	Check(dst == g_tracked_page && size == PageSize, "copy check covers the read");
	ProtectPage(g_tracked_page, false);
	return true;
}

void TestSysFileReadEx(const std::filesystem::path& path) {
	auto* f = SysFileOpenR(path);
	Check(f != nullptr, "open temp file");

	uint8_t  writable[PageSize] {};
	uint32_t got = 0;
	int      err = 1;
	SysFileReadEx(writable, sizeof(writable), *f, &got, &err);
	Check(err == 0 && got == sizeof(writable), "read into a writable buffer succeeds");
	Check(writable[0] == 3 && writable[1] == 10 && writable[PageSize - 1] == Pattern(PageSize - 1),
	      "read data matches");

	got = 123;
	err = 1;
	SysFileReadEx(writable, sizeof(writable), *f, &got, &err);
	Check(got == 0 && err == 0, "read at end of file is not an error");
	Check(SysFileSeek(*f, 0), "seek to start");

	auto* page = AllocPage();
	ProtectPage(page, true);
	got = 123;
	err = 0;
	SysFileReadEx(page, PageSize, *f, &got, &err);
	Check(got == 0, "read into a protected page delivers 0 bytes");
	Check(err == EFAULT, "read into a protected page is reported as EFAULT");
	ProtectPage(page, false);
	FreePage(page);
	SysFileClose(f);

#if defined(_WIN32)
	// A handle without read access fails with ERROR_ACCESS_DENIED.
	auto* w = SysFileOpenW(path);
	Check(w != nullptr, "open temp file for writing");
	got = 123;
	err = 0;
	SysFileReadEx(writable, sizeof(writable), *w, &got, &err);
	Check(got == 0 && err == EIO, "other read failures are reported as EIO");
	SysFileClose(w);
#else
	// A directory opens for reading, but read(2) fails with EISDIR.
	const std::filesystem::path dir = "win_read_efault.dir";
	std::filesystem::create_directory(dir);
	auto* d = SysFileOpenR(dir);
	Check(d != nullptr, "open directory");
	got = 123;
	err = 0;
	SysFileReadEx(writable, sizeof(writable), *d, &got, &err);
	Check(got == 0 && err != 0 && err != EFAULT, "other read failures report their errno");
	SysFileClose(d);
	std::filesystem::remove(dir);
#endif
}

void TestReadIntoGuest(const std::filesystem::path& path) {
	using Libs::LibKernel::FileSystem::ReadIntoGuest;

	Common::File f;
	Check(f.Open(path, Common::File::Mode::Read), "open temp file");

	// Short read at end of file: no error and no bounce copy.
	static uint8_t twice[PageSize * 2];
	uint32_t       got = 0;
	g_check_calls      = 0;
	g_copy_calls       = 0;
	Check(ReadIntoGuest(f, twice, sizeof(twice), 0, &got, NotKnown, NotRecovered) == 0 &&
	          got == PageSize,
	      "short read at end of file returns the bytes read");
	Check(g_check_calls == 1 && g_copy_calls == 0, "short read is checked once, with no copy");

	// A destination outside the guest map (here a host heap buffer, as the AvPlayer file reader
	// passes to the guest's read callback) is read into directly. It is not committed guest
	// memory, so only the bounce copy would reject it, and the read never gets there.
	{
		std::vector<uint8_t> host(PageSize);
		Check(f.Seek(0), "seek to start");
		g_check_calls = 0;
		g_copy_calls  = 0;
		got           = 0;
		Check(ReadIntoGuest(f, host.data(), PageSize, 0, &got, NotKnown, NotRecovered) == 0 &&
		          got == PageSize,
		      "read into a buffer outside the guest map succeeds");
		Check(g_check_calls == 1 && g_copy_calls == 0, "outside the map: no bounce copy");
		Check(host[0] == 3 && host[PageSize - 1] == Pattern(PageSize - 1),
		      "outside-the-map data matches");
		Check(f.Tell() == PageSize, "outside-the-map read advances the offset");
	}

	auto* page = AllocPage();

	// Known unwritable before the first read: the page is host-writable here, so a read would
	// succeed and fill it. EFAULT without reading, touching the destination or moving the offset.
	Check(f.Seek(0), "seek to start");
	g_check_calls = 0;
	g_copy_calls  = 0;
	got           = 123;
	Check(ReadIntoGuest(f, page, PageSize, 0, &got, KnownUnwritable, NotRecovered) == EFAULT,
	      "read into a known unwritable destination fails with EFAULT before the first read");
	Check(g_check_calls == 1 && g_copy_calls == 0 && got == 0,
	      "pre-checked destination receives no bytes");
	Check(f.Tell() == 0, "pre-checked read leaves the file offset unchanged");
	Check(page[0] == 0 && page[PageSize - 1] == 0, "pre-checked destination is untouched");

	// The host read fails and the fault would not be recovered: EFAULT without touching the
	// destination or moving past delivered bytes.
	Check(f.Seek(0), "seek to start");
	ProtectPage(page, true);
	g_check_calls = 0;
	g_copy_calls  = 0;
	got           = 123;
	Check(ReadIntoGuest(f, page, PageSize, 0, &got, NotKnown, NotRecovered) == EFAULT,
	      "read into an unrecovered destination fails with EFAULT");
	Check(g_check_calls == 1 && g_copy_calls == 1 && got == 0,
	      "unrecovered destination receives no bytes");
	Check(f.Tell() == 0, "file offset covers only the delivered bytes");
	ProtectPage(page, false);
	Check(page[0] == 0 && page[PageSize - 1] == 0, "unrecovered destination is unchanged");

	// A guest buffer larger than what is left in the file, with only the part the file fills
	// mapped: the caller passes the clamped size, so the read succeeds with the file's bytes.
	{
		uint8_t* two = nullptr;
#if defined(_WIN32)
		two = static_cast<uint8_t*>(
		    VirtualAlloc(nullptr, PageSize * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		Check(two != nullptr, "alloc two pages");
#else
		two = static_cast<uint8_t*>(mmap(nullptr, PageSize * 2, PROT_READ | PROT_WRITE,
		                                 MAP_PRIVATE | MAP_ANON, -1, 0));
		Check(two != MAP_FAILED, "alloc two pages");
#endif
		ProtectPage(two + PageSize, true);
		Check(f.Seek(0), "seek to start");
		g_tracked_page = two;
		g_check_calls  = 0;
		g_copy_calls   = 0;
		got            = 0;
		Check(ReadIntoGuest(f, two, PageSize, 0, &got, PastFirstPage, NotRecovered) == 0 &&
		          got == PageSize,
		      "read into a buffer with an unmapped tail past the file end succeeds");
		Check(g_check_calls == 1 && g_copy_calls == 0, "only the clamped size is checked");
		Check(two[0] == 3 && two[PageSize - 1] == Pattern(PageSize - 1), "clamped data matches");
		Check(f.Seek(0), "seek to start");
		got = 123;
		Check(ReadIntoGuest(f, two, PageSize * 2, 0, &got, PastFirstPage, NotRecovered) == EFAULT &&
		          got == 0,
		      "a range reaching the reserved page fails with EFAULT");
		ProtectPage(two + PageSize, false);
#if defined(_WIN32)
		VirtualFree(two, 0, MEM_RELEASE);
#else
		munmap(two, PageSize * 2);
#endif
	}

	// Tracked: the bounce copy delivers the whole read.
	Check(f.Seek(0), "seek to start");
	ProtectPage(page, true);
	g_tracked_page = page;
	g_check_calls  = 0;
	g_copy_calls   = 0;
	got            = 0;
	Check(ReadIntoGuest(f, page, PageSize, 0, &got, NotKnown, UnprotectTracked) == 0 &&
	          got == PageSize,
	      "read into a tracked page is retried in full");
	Check(g_check_calls == 1 && g_copy_calls == 1,
	      "destination is checked before the read and before the copy");
	Check(page[0] == 3 && page[PageSize - 1] == Pattern(PageSize - 1), "retried data matches");
	FreePage(page);
	f.Close();

#if defined(_WIN32)
	Common::File w;
	Check(w.Open(path, Common::File::Mode::Write), "open temp file for writing");
	got = 123;
	Check(ReadIntoGuest(w, twice, PageSize, 0, &got, NotKnown, NotRecovered) == EIO && got == 0,
	      "other read failures fail with EIO");
	w.Close();
#else
	const std::filesystem::path dir = "win_read_efault.dir";
	std::filesystem::create_directory(dir);
	Common::File d;
	Check(d.Open(dir, Common::File::Mode::Read), "open directory");
	got = 123;
	Check(ReadIntoGuest(d, twice, PageSize, 0, &got, NotKnown, NotRecovered) == EIO && got == 0,
	      "other read failures fail with EIO");
	d.Close();
	std::filesystem::remove(dir);
#endif
}

} // namespace

int main() {
	const std::filesystem::path path = "win_read_efault.tmp";
	{
		auto* w = SysFileCreate(path);
		Check(w != nullptr, "create temp file");
		uint8_t block[PageSize];
		for (uint32_t i = 0; i < PageSize; i++) {
			block[i] = Pattern(i);
		}
		uint32_t written = 0;
		SysFileWrite(block, sizeof(block), *w, &written);
		Check(written == sizeof(block), "write temp file");
		SysFileClose(w);
	}

	TestSysFileReadEx(path);
	TestReadIntoGuest(path);

	SysFileDeleteFile(path);
	std::printf("WinReadEfaultTests: ok\n");
	return 0;
}
