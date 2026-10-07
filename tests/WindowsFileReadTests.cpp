#include "common/file.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {
uint8_t* g_destination = nullptr;
size_t g_destination_size = 0;
unsigned g_write_faults = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "WindowsFileReadTests: %s\n", message);
		std::exit(1);
	}
}

LONG CALLBACK HandleWriteFault(EXCEPTION_POINTERS* exception) {
	const auto& record = *exception->ExceptionRecord;
	if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record.NumberParameters < 2 ||
	    record.ExceptionInformation[0] != 1) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	const auto address = record.ExceptionInformation[1];
	const auto begin = reinterpret_cast<uintptr_t>(g_destination);
	if (address < begin || address - begin >= g_destination_size) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	DWORD previous = 0;
	if (!VirtualProtect(g_destination, g_destination_size, PAGE_READWRITE, &previous)) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	++g_write_faults;
	return EXCEPTION_CONTINUE_EXECUTION;
}
}

int main() {
	const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto path = std::filesystem::temp_directory_path() /
	                  ("kyty_windows_read_" + std::to_string(unique) + ".bin");
	std::vector<uint8_t> expected((2u << 20u) + 123u);
	for (size_t i = 0; i < expected.size(); ++i) {
		expected[i] = static_cast<uint8_t>((i * 37u + i / 251u) & 255u);
	}
	Common::File output;
	Check(output.Create(path), "create fixture");
	uint32_t written = 0;
	output.Write(expected.data(), static_cast<uint32_t>(expected.size()), &written);
	Check(written == expected.size(), "write fixture");
	output.Close();

	g_destination_size = expected.size() + 4096u;
	g_destination = static_cast<uint8_t*>(VirtualAlloc(nullptr, g_destination_size,
	                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
	Check(g_destination != nullptr, "allocate destination");
	void* handler = AddVectoredExceptionHandler(1, HandleWriteFault);
	Check(handler != nullptr, "install write-fault handler");
	Common::File input(path, Common::File::Mode::Read);
	Check(!input.IsInvalid(), "open fixture");

	// Verify ordinary reads, then reads crossing several chunks from a nonzero offset.
	uint32_t received = 0;
	input.Read(g_destination, 31, &received);
	Check(received == 31 && std::memcmp(g_destination, expected.data(), 31) == 0,
	      "ordinary read preserves data");
	for (const uint32_t requested : {73u, static_cast<uint32_t>(expected.size() + 1024u)}) {
		Check(input.Seek(17), "seek to nonzero offset");
		DWORD previous = 0;
		Check(VirtualProtect(g_destination, g_destination_size, PAGE_READONLY, &previous) != 0,
		      "protect destination");
		const unsigned before = g_write_faults;
		input.Read(g_destination, requested, &received);
		const auto wanted = std::min<size_t>(requested, expected.size() - 17u);
		Check(received == wanted, "read into protected destination returns actual bytes");
		Check(g_write_faults > before, "user-mode copy invokes write-fault handler");
		Check(std::memcmp(g_destination, expected.data() + 17, wanted) == 0,
		      "protected read preserves data");
		Check(input.Tell() == 17u + wanted, "protected read preserves file position");
	}
	input.Read(g_destination, 16, &received);
	Check(received == 0, "read at EOF returns zero");
	input.Close();
	RemoveVectoredExceptionHandler(handler);
	VirtualFree(g_destination, 0, MEM_RELEASE);
	Check(std::filesystem::remove(path), "remove fixture");
	std::puts("WindowsFileReadTests: all cases passed");
}
