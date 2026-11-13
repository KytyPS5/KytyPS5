#include "local-platform.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iterator>
#include <map>
#include <set>
#include <tuple>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <softpub.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <wintrust.h>
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#include <atomic>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <thread>
#else
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <x86intrin.h>
#if defined(__linux__)
#include <linux/membarrier.h>
#include <malloc.h>
#include <sys/vfs.h>
#endif
#endif

namespace LocalPlatform {

// "1,2,3" -> bit mask of CPUs 0..63; 0 for an empty or invalid list.
static uint64_t CpuListMask(const char* list) {
	uint64_t mask = 0;
	if (list == nullptr) return 0;
	for (const char* p = list; *p != '\0';) {
		char*      end = nullptr;
		const long cpu = std::strtol(p, &end, 10);
		if (end == p) break;
		if (cpu >= 0 && cpu < 64) mask |= uint64_t(1) << cpu;
		p = (*end == ',') ? end + 1 : end;
	}
	return mask;
}

#if defined(_WIN32)

static double FileTimeSeconds(const FILETIME& time) {
	const uint64_t ticks = (uint64_t(time.dwHighDateTime) << 32u) | time.dwLowDateTime;
	return static_cast<double>(ticks) / 1e7;
}

static double HandleCpuSeconds(HANDLE thread) {
	FILETIME creation {}, exit {}, kernel {}, user {};
	if (GetThreadTimes(thread, &creation, &exit, &kernel, &user) == 0) return 0;
	return FileTimeSeconds(kernel) + FileTimeSeconds(user);
}

void SetThreadName(const char* name) {
	wchar_t wide[64] {};
	for (size_t i = 0; name[i] != '\0' && i + 1 < std::size(wide); ++i) wide[i] = static_cast<wchar_t>(name[i]);
	(void)SetThreadDescription(GetCurrentThread(), wide);
}

void ReleaseRedirectedConsole() {
	if (GetConsoleWindow() != nullptr && GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_DISK &&
	    GetFileType(GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_DISK)
		(void)FreeConsole();
}

static HANDLE                g_stdout_done = nullptr; // set once the writer has written all the pipe held

void AsyncStdout() {
	const HANDLE original = GetStdHandle(STD_OUTPUT_HANDLE);
	if (original == nullptr || original == INVALID_HANDLE_VALUE || GetFileType(original) != FILE_TYPE_DISK) return;
	// (Its own handle to the file: replacing descriptor 1 closes the one the C runtime holds.)
	HANDLE file = nullptr;
	if (DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &file, 0, FALSE, DUPLICATE_SAME_ACCESS) == 0)
		return;
	HANDLE read_end = nullptr, write_end = nullptr;
	if (CreatePipe(&read_end, &write_end, nullptr, 4u << 20u) == 0) {
		CloseHandle(file);
		return;
	}
	const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(write_end), _O_TEXT);
	if (fd < 0) {
		CloseHandle(read_end);
		CloseHandle(write_end);
		CloseHandle(file);
		return;
	}
	std::fflush(stdout);
	if (_dup2(fd, _fileno(stdout)) != 0) {
		_close(fd);
		CloseHandle(read_end);
		CloseHandle(file);
		return;
	}
	_close(fd);
	(void)SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))));
	g_stdout_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	std::thread([read_end, file] {
		SetThreadName("Kyty.Stdout");
		std::vector<char> buffer(256u << 10u);
		for (DWORD read = 0; ReadFile(read_end, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) != 0 && read != 0;)
			for (DWORD written = 0, at = 0; at < read; at += written)
				if (WriteFile(file, buffer.data() + at, read - at, &written, nullptr) == 0 || written == 0) break;
		SetEvent(g_stdout_done);
	}).detach();
}

void DrainStdout() {
	static std::atomic<bool> closed {false};
	std::fflush(stdout);
	if (g_stdout_done == nullptr) return;
	// The pipe cannot be polled: a request on its read end waits behind the writer's pending read (a crash report
	// hung there, 10-06). Standard output goes to NUL instead, which closes the pipe's only write end: the writer's
	// read ends once it has taken all the pipe held (not _close: a later write to a closed descriptor would end
	// the process before the writer is done). Bounded: a crash must not hang the end.
	if (!closed.exchange(true)) {
		if (const int nul = _open("NUL", _O_WRONLY); nul >= 0) {
			(void)_dup2(nul, _fileno(stdout));
			_close(nul);
		}
	}
	WaitForSingleObject(g_stdout_done, 2000);
}

uint32_t ThreadId() {
	return static_cast<uint32_t>(GetCurrentThreadId());
}

void PinThreadToCpuList(const char* list) {
	if (const auto mask = CpuListMask(list); mask != 0) (void)SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(mask));
}

bool PinThreadHandleToCpuList(uint64_t handle, const char* list) {
	const auto mask = CpuListMask(list);
	return handle != 0 && mask != 0 &&
	       SetThreadAffinityMask(reinterpret_cast<HANDLE>(handle), DWORD_PTR(mask)) != 0;
}

// CPU set ids of the logical processors in the mask (all of them for mask 0 = no restriction).
static bool SetProcessDefaultCpuMask(uint64_t mask) {
	if (mask == 0) return SetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0) != 0;
	ULONG length = 0;
	(void)GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
	std::vector<uint8_t> buffer(length);
	auto* first = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data());
	if (length == 0 || !GetSystemCpuSetInformation(first, length, &length, GetCurrentProcess(), 0)) return false;
	std::vector<ULONG> ids;
	for (ULONG offset = 0; offset < length;) {
		const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
		if (info->Type == CpuSetInformation && info->CpuSet.Group == 0 && info->CpuSet.LogicalProcessorIndex < 64 &&
		    (mask >> info->CpuSet.LogicalProcessorIndex) & 1u)
			ids.push_back(info->CpuSet.Id);
		offset += info->Size;
	}
	return !ids.empty() && SetProcessDefaultCpuSets(GetCurrentProcess(), ids.data(), static_cast<ULONG>(ids.size())) != 0;
}

bool SetProcessDefaultCpuList(const char* list) {
	return SetProcessDefaultCpuMask(CpuListMask(list));
}

std::string SpeculationCpuList() {
	DWORD_PTR process = 0, system = 0;
	if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) == 0) process = ~DWORD_PTR(0);
	ULONG length = 0;
	(void)GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
	std::vector<uint8_t> buffer(length);
	auto* first = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data());
	if (length == 0 || !GetSystemCpuSetInformation(first, length, &length, GetCurrentProcess(), 0)) return {};
	struct Cpu {
		uint32_t index, core, efficiency;
	};
	std::vector<Cpu> cpus;
	for (ULONG offset = 0; offset < length;) {
		const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
		if (info->Type == CpuSetInformation && info->CpuSet.Group == 0 && info->CpuSet.LogicalProcessorIndex < 64 &&
		    ((process >> info->CpuSet.LogicalProcessorIndex) & 1u) != 0)
			cpus.push_back({info->CpuSet.LogicalProcessorIndex, info->CpuSet.CoreIndex, info->CpuSet.EfficiencyClass});
		offset += info->Size;
	}
	// The performance cores (the highest efficiency class; every core where all are alike) without CPU 0's core (most
	// interrupts on Windows), as run-windows.ps1's "auto" picks the render threads' CPUs; then the upper half of them.
	uint32_t fastest = 0, interrupts = UINT32_MAX;
	for (const auto& cpu: cpus) {
		fastest = std::max(fastest, cpu.efficiency);
		if (cpu.index == 0) interrupts = cpu.core;
	}
	std::vector<uint32_t> cores;
	for (const auto& cpu: cpus)
		if (cpu.efficiency == fastest && cpu.core != interrupts && std::find(cores.begin(), cores.end(), cpu.core) == cores.end())
			cores.push_back(cpu.core);
	std::sort(cores.begin(), cores.end());
	cores.erase(cores.begin(), cores.begin() + static_cast<std::ptrdiff_t>(cores.size() / 2));
	std::sort(cpus.begin(), cpus.end(), [](const Cpu& a, const Cpu& b) { return a.index < b.index; });
	std::string list;
	for (const auto& cpu: cpus)
		if (std::find(cores.begin(), cores.end(), cpu.core) != cores.end())
			list += (list.empty() ? "" : ",") + std::to_string(cpu.index);
	return list;
}

uint32_t PerformanceLogicalCpus() {
	if (const char* simulate = std::getenv("KYTY_SIMULATE_PCORES"); simulate != nullptr && *simulate != '\0')
		return static_cast<uint32_t>(std::strtoul(simulate, nullptr, 10));
	DWORD_PTR process = 0, system = 0;
	if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) == 0) process = ~DWORD_PTR(0);
	ULONG length = 0;
	(void)GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
	std::vector<uint8_t> buffer(length);
	if (length == 0 || !GetSystemCpuSetInformation(reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data()), length,
	                                               &length, GetCurrentProcess(), 0))
		return 0;
	uint32_t fastest = 0, count = 0;
	for (int pass = 0; pass < 2; ++pass)
		for (ULONG offset = 0; offset < length;) {
			const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
			if (info->Type == CpuSetInformation && info->CpuSet.Group == 0 && info->CpuSet.LogicalProcessorIndex < 64 &&
			    ((process >> info->CpuSet.LogicalProcessorIndex) & 1u) != 0) {
				if (pass == 0) fastest = std::max<uint32_t>(fastest, info->CpuSet.EfficiencyClass);
				else if (info->CpuSet.EfficiencyClass == fastest) ++count;
			}
			offset += info->Size;
		}
	return count;
}

uint64_t CurrentThreadHandle() {
	HANDLE handle = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION | THREAD_SUSPEND_RESUME |
	                               THREAD_GET_CONTEXT,
	                           FALSE, GetCurrentThreadId());
	return reinterpret_cast<uint64_t>(handle);
}

uint64_t OpenThreadForSampling(uint32_t thread_id) {
	HANDLE handle = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION | THREAD_SUSPEND_RESUME |
	                               THREAD_GET_CONTEXT,
	                           FALSE, thread_id);
	return reinterpret_cast<uint64_t>(handle);
}

void CloseThreadForSampling(uint64_t handle) {
	if (handle != 0) CloseHandle(reinterpret_cast<HANDLE>(handle));
}

// Unwind tables of the loaded images, collected before sampling: the sampler must not take
// loader or function-table locks (RtlLookupFunctionEntry does) while the sampled thread,
// which may hold them, is suspended. RtlVirtualUnwind itself takes no lock.
namespace {
struct UnwindImage {
	uint64_t                base = 0, end = 0;
	const RUNTIME_FUNCTION* table = nullptr;
	size_t                  count = 0;
};
std::vector<UnwindImage> g_unwind_images;

const RUNTIME_FUNCTION* FindUnwindEntry(uint64_t pc, uint64_t* image_base) {
	for (const auto& image: g_unwind_images) {
		if (pc < image.base || pc >= image.end) continue;
		const auto rva = static_cast<DWORD>(pc - image.base);
		size_t     lo = 0, hi = image.count;
		while (lo < hi) {
			const size_t mid = (lo + hi) / 2;
			if (image.table[mid].EndAddress <= rva) lo = mid + 1;
			else hi = mid;
		}
		if (lo == image.count || image.table[lo].BeginAddress > rva) return nullptr;
		*image_base = image.base;
		// Chained entries (UNW_FLAG_CHAININFO) are resolved by UnwindFrame.
		return &image.table[lo];
	}
	return nullptr;
}

// One frame of RtlVirtualUnwind's algorithm (epilogue emulation, else the unwind codes of the
// function and its chain) with every stack read inside [low, high): a sample can catch a thread at
// an instruction its unwind data does not describe, and RtlVirtualUnwind then reads wherever a
// stale frame register points (a profiling run died reading 3.6 KiB above the render stack's top).
// XMM saves are not read (they never steer the unwind). False ends the walk.
bool UnwindFrame(uint64_t image_base, const RUNTIME_FUNCTION* entry, CONTEXT* context, uint64_t low, uint64_t high) {
	// x64 unwind operation codes (not in the SDK headers).
	enum : uint32_t {
		UWOP_PUSH_NONVOL      = 0,
		UWOP_ALLOC_LARGE      = 1,
		UWOP_ALLOC_SMALL      = 2,
		UWOP_SET_FPREG        = 3,
		UWOP_SAVE_NONVOL      = 4,
		UWOP_SAVE_NONVOL_FAR  = 5,
		UWOP_SAVE_XMM128      = 8,
		UWOP_SAVE_XMM128_FAR  = 9,
		UWOP_PUSH_MACHFRAME   = 10,
	};
	DWORD64*   registers = &context->Rax;
	const auto read      = [&](uint64_t address, uint64_t* value) {
        if (address < low || high < 8 || address > high - 8) return false;
        *value = *reinterpret_cast<const uint64_t*>(address);
        return true;
	};
	const uint64_t pc    = context->Rip;
	const auto*    info  = reinterpret_cast<const uint8_t*>(image_base + entry->UnwindData);
	const uint32_t count = info[2], frame_register = info[3] & 0x0fu;
	const auto*    codes = reinterpret_cast<const uint16_t*>(info + 4);
	if ((info[0] & 7u) != 1 && (info[0] & 7u) != 2) return false;
	const auto slots = [](uint32_t op, uint32_t op_info) -> uint32_t {
		switch (op) {
			case UWOP_ALLOC_LARGE: return op_info != 0 ? 3 : 2;
			case UWOP_SAVE_NONVOL:
			case UWOP_SAVE_XMM128: return 2;
			case UWOP_SAVE_NONVOL_FAR:
			case UWOP_SAVE_XMM128_FAR: return 3;
			default: return 1; // version 2 epilogue descriptors take one slot each
		}
	};
	auto prolog_offset = static_cast<uint32_t>(pc - (image_base + entry->BeginAddress));
	uint64_t frame     = context->Rsp;
	if (frame_register != 0) {
		bool established = prolog_offset >= info[1] || ((info[0] >> 3) & UNW_FLAG_CHAININFO) != 0;
		for (uint32_t i = 0; !established && i < count; i += slots((codes[i] >> 8) & 0x0fu, codes[i] >> 12)) {
			if (((codes[i] >> 8) & 0x0fu) == UWOP_SET_FPREG) {
				established = prolog_offset >= (codes[i] & 0xffu);
				break;
			}
		}
		if (established) frame = registers[frame_register] - (info[3] >> 4u) * 16ull;
	}
	// An epilogue (add rsp / lea rsp,[fp], pops, then ret or a jump out) is emulated forward.
	const auto* next          = reinterpret_cast<const uint8_t*>(pc);
	const auto  rex           = [](uint8_t byte) { return (byte & 0xf0u) == 0x40u; };
	uint64_t    epilogue_rsp  = context->Rsp;
	if (next[0] == 0x48 && next[1] == 0x83 && next[2] == 0xc4) {
		epilogue_rsp += static_cast<int8_t>(next[3]);
		next += 4;
	} else if (next[0] == 0x48 && next[1] == 0x81 && next[2] == 0xc4) {
		int32_t displacement = 0;
		std::memcpy(&displacement, next + 3, 4);
		epilogue_rsp += displacement;
		next += 7;
	} else if ((next[0] & 0xfeu) == 0x48 && next[1] == 0x8d) {
		const uint32_t base = ((next[0] & 1u) << 3u) | (next[2] & 7u);
		if (base != 0 && base == frame_register) {
			if ((next[2] & 0xf8u) == 0x60) {
				epilogue_rsp = registers[base] + static_cast<int8_t>(next[3]);
				next += 4;
			} else if ((next[2] & 0xf8u) == 0xa0) {
				int32_t displacement = 0;
				std::memcpy(&displacement, next + 3, 4);
				epilogue_rsp = registers[base] + displacement;
				next += 7;
			}
		}
	}
	const auto* pops = next;
	while ((next[0] & 0xf8u) == 0x58 || (rex(next[0]) && (next[1] & 0xf8u) == 0x58)) next += (next[0] & 0xf8u) == 0x58 ? 1 : 2;
	bool epilogue = next[0] == 0xc3 || next[0] == 0xc2 || (next[0] == 0xf3 && next[1] == 0xc3) ||
	                (next[0] == 0xff && next[1] == 0x25) || (rex(next[0]) && next[1] == 0xff && (next[2] & 0x38u) == 0x20);
	if (next[0] == 0xeb || next[0] == 0xe9) {
		// A jump out of the function (or to its start) is a tail call.
		int32_t displacement = 0;
		if (next[0] == 0xeb) displacement = static_cast<int8_t>(next[1]);
		else std::memcpy(&displacement, next + 1, 4);
		const uint64_t target = reinterpret_cast<uint64_t>(next) + (next[0] == 0xeb ? 2 : 5) + displacement - image_base;
		epilogue = target < entry->BeginAddress || target >= entry->EndAddress ||
		           (target == entry->BeginAddress && ((info[0] >> 3) & UNW_FLAG_CHAININFO) == 0);
	}
	if (epilogue) {
		uint64_t rsp = epilogue_rsp;
		for (next = pops; (next[0] & 0xf8u) == 0x58 || (rex(next[0]) && (next[1] & 0xf8u) == 0x58);) {
			const bool     extended = (next[0] & 0xf8u) != 0x58;
			const uint32_t reg      = (extended ? ((next[0] & 1u) << 3u) | (next[1] & 7u) : (next[0] & 7u));
			uint64_t       value    = 0;
			if (!read(rsp, &value)) return false;
			registers[reg] = value;
			rsp += 8;
			next += extended ? 2 : 1;
		}
		uint64_t ret = 0;
		if (!read(rsp, &ret)) return false;
		context->Rip = ret;
		context->Rsp = rsp + 8;
		return true;
	}
	// The unwind codes the thread has executed, through the chain of the function's fragments.
	bool machine_frame = false;
	for (int chain = 0; chain < 32; ++chain) {
		info                 = reinterpret_cast<const uint8_t*>(image_base + entry->UnwindData);
		codes                = reinterpret_cast<const uint16_t*>(info + 4);
		const uint32_t total = info[2];
		prolog_offset        = static_cast<uint32_t>(pc - (image_base + entry->BeginAddress));
		for (uint32_t i = 0; i < total;) {
			const uint32_t op = (codes[i] >> 8) & 0x0fu, op_info = codes[i] >> 12;
			if (prolog_offset < (codes[i] & 0xffu) || op == UWOP_SAVE_XMM128 || op == UWOP_SAVE_XMM128_FAR ||
			    op == 6 /* version 2 epilogue descriptor */) {
				i += slots(op, op_info);
				continue;
			}
			uint64_t value = 0;
			switch (op) {
				case UWOP_PUSH_NONVOL:
					if (!read(context->Rsp, &value)) return false;
					registers[op_info] = value;
					context->Rsp += 8;
					break;
				case UWOP_ALLOC_LARGE:
					if (i + slots(op, op_info) > total) return false;
					context->Rsp += op_info != 0 ? codes[i + 1] | (uint64_t(codes[i + 2]) << 16u) : codes[i + 1] * 8ull;
					break;
				case UWOP_ALLOC_SMALL: context->Rsp += op_info * 8ull + 8; break;
				case UWOP_SET_FPREG: context->Rsp = registers[info[3] & 0x0fu] - (info[3] >> 4u) * 16ull; break;
				case UWOP_SAVE_NONVOL:
				case UWOP_SAVE_NONVOL_FAR: {
					if (i + slots(op, op_info) > total) return false;
					const uint64_t offset = op == UWOP_SAVE_NONVOL ? codes[i + 1] * 8ull
					                                               : codes[i + 1] | (uint64_t(codes[i + 2]) << 16u);
					if (!read(frame + offset, &value)) return false;
					registers[op_info] = value;
					break;
				}
				case UWOP_PUSH_MACHFRAME: {
					const uint64_t base = context->Rsp + (op_info != 0 ? 8 : 0);
					uint64_t       rsp  = 0;
					if (!read(base, &value) || !read(base + 24, &rsp)) return false;
					context->Rip  = value;
					context->Rsp  = rsp;
					machine_frame = true;
					break;
				}
				default: return false;
			}
			i += slots(op, op_info);
		}
		if (((info[0] >> 3) & UNW_FLAG_CHAININFO) == 0) break;
		entry = reinterpret_cast<const RUNTIME_FUNCTION*>(codes + ((total + 1) & ~1u));
	}
	if (machine_frame) return true;
	uint64_t ret = 0;
	if (!read(context->Rsp, &ret)) return false;
	context->Rip = ret;
	context->Rsp += 8;
	return true;
}
} // namespace

void PrepareSampling() {
	g_unwind_images.clear();
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	if (snapshot == INVALID_HANDLE_VALUE) return;
	MODULEENTRY32W entry {};
	entry.dwSize = sizeof(entry);
	for (BOOL ok = Module32FirstW(snapshot, &entry); ok; ok = Module32NextW(snapshot, &entry)) {
		const auto base = reinterpret_cast<uint64_t>(entry.modBaseAddr);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE ||
		    nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION)
			continue;
		const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
		if (directory.VirtualAddress == 0 || directory.Size < sizeof(RUNTIME_FUNCTION)) continue;
		g_unwind_images.push_back({base, base + entry.modBaseSize,
		                           reinterpret_cast<const RUNTIME_FUNCTION*>(base + directory.VirtualAddress),
		                           directory.Size / sizeof(RUNTIME_FUNCTION)});
	}
	CloseHandle(snapshot);
}

bool SampleThread(uint64_t handle, uint64_t stack_low, uint64_t stack_high, uint64_t* out, size_t words) {
	auto* thread = reinterpret_cast<HANDLE>(handle);
	if (thread == nullptr || words < 2 || SuspendThread(thread) == static_cast<DWORD>(-1)) return false;
	CONTEXT context {};
	context.ContextFlags = CONTEXT_FULL;
	const bool got       = GetThreadContext(thread, &context) != 0;
	if (got && stack_high == 0) {
		// Unknown stack (a guest thread on its own stack): walk only inside the committed
		// region that holds rsp, so no read can fault while the thread is suspended.
		MEMORY_BASIC_INFORMATION region {};
		if (VirtualQuery(reinterpret_cast<const void*>(context.Rsp), &region, sizeof(region)) != 0 &&
		    region.State == MEM_COMMIT) {
			stack_low  = context.Rsp;
			stack_high = reinterpret_cast<uint64_t>(region.BaseAddress) + region.RegionSize;
		}
	}
	if (got) {
		// Layout of the Linux SIGPROF sampler: pc, then callers. With unwind tables
		// (PrepareSampling) the callers come from the unwind codes (UnwindFrame), so frames
		// without a frame pointer (ntdll, the driver, leaf code) are walked too; code without
		// unwind data (guest code) ends the walk with the word at rsp and the frame-pointer chain.
		// Only [rsp, stack top) is read: the pages below rsp may be uncommitted.
		stack_low = std::max(stack_low, static_cast<uint64_t>(context.Rsp));
		for (size_t i = 0; i < words; ++i) out[i] = 0;
		out[0]      = context.Rip;
		size_t next = 1;
		for (int depth = 0; next < words && depth < 64; ++depth) {
			const uint64_t rsp = context.Rsp;
			if (rsp < stack_low || rsp + 8 > stack_high) break;
			uint64_t    image_base = 0;
			const auto* entry      = g_unwind_images.empty() ? nullptr : FindUnwindEntry(context.Rip, &image_base);
			if (entry == nullptr) {
				// No unwind data: the word at rsp (a leaf's caller), then frame pointers.
				if (next < words) out[next++] = *reinterpret_cast<const uint64_t*>(rsp);
				uint64_t rbp = context.Rbp;
				while (next < words) {
					if (rbp < rsp || rbp >= stack_high || stack_high - rbp < 16 || (rbp & 7u) != 0) break;
					const auto* frame = reinterpret_cast<const uint64_t*>(rbp);
					out[next++]       = frame[1];
					rbp               = frame[0] <= rbp ? 0 : frame[0];
				}
				break;
			}
			if (!UnwindFrame(image_base, entry, &context, stack_low, stack_high) || context.Rip == 0 ||
			    context.Rsp <= rsp)
				break;
			out[next++] = context.Rip;
		}
	}
	ResumeThread(thread);
	return got;
}

bool ModuleOf(const void* address, uint64_t* base, char* path, size_t path_size) {
	HMODULE module = nullptr;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       static_cast<LPCSTR>(address), &module) == 0)
		return false;
	*base = reinterpret_cast<uint64_t>(module);
	if (path_size != 0 && GetModuleFileNameA(module, path, static_cast<DWORD>(path_size)) == 0) path[0] = '\0';
	return true;
}

double ThreadCpuSeconds(uint64_t handle) {
	return handle != 0 ? HandleCpuSeconds(reinterpret_cast<HANDLE>(handle)) : 0;
}

uint64_t CurrentThreadCycles() {
	ULONG64 cycles = 0;
	return QueryThreadCycleTime(GetCurrentThread(), &cycles) != 0 ? cycles : 0;
}

int CurrentThreadPriority() {
	return GetThreadPriority(GetCurrentThread());
}

bool ProcessCpusIdleTime(uint64_t* idle_100ns, uint64_t* cpus) {
	// NtQuerySystemInformation's SystemProcessorPerformanceInformation (8): each processor of the calling thread's group.
	struct ProcessorTimes {
		LARGE_INTEGER idle, kernel, user, dpc, interrupt;
		ULONG         interrupts;
	};
	using Query             = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
	static const auto query = reinterpret_cast<Query>(
	    reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
	DWORD_PTR process = 0, system = 0;
	if (query == nullptr || GetProcessAffinityMask(GetCurrentProcess(), &process, &system) == 0) return false;
	ProcessorTimes times[64];
	ULONG          returned = 0;
	if (query(8, times, sizeof(times), &returned) < 0) return false;
	uint64_t idle = 0, mask = 0;
	for (size_t i = 0; i < std::min<size_t>(returned / sizeof(ProcessorTimes), 64); ++i) {
		if (((process >> i) & 1u) == 0) continue;
		idle += static_cast<uint64_t>(times[i].idle.QuadPart);
		mask |= uint64_t {1} << i;
	}
	*idle_100ns = idle;
	*cpus       = mask;
	return mask != 0;
}

void KeptFileCloser();

namespace {
struct KeptFile {
	HANDLE   handle     = INVALID_HANDLE_VALUE;
	uint64_t size       = 0;
	bool     prefetched = false;
	bool     ahead      = false; // opened by FollowKeptFiles's threads, not read by the game yet
	uint32_t readers    = 0;     // OpenKeptReadFile calls whose DoneKeptReadFile has not come yet
	uint64_t stamp      = 0;     // (g_kept_stamp when last queued to close: an older entry of a queue is stale)
};
std::mutex                                g_kept_files_mutex;
std::unordered_map<std::string, KeptFile> g_kept_files; // (the game's files, read-only)

// Kept files are closed once unread for a while (KeptFileCloser). While a file is open, the system file cache's view of
// what was read through it stays mapped, in the system's working set: memory in use, not available. The game reads tens
// of thousands of small files whole (~28,500 to load 1-1), each kept open: after 1-1 and four world trips the "\Memory\Cache
// Bytes" counter stood at 6.6-8 GB more than before the boot (back at exit), on top of the emulator's own memory. Closed,
// a file's pages go to the standby list: still cached (a later read of them waits for no disk) but available memory,
// which Windows reuses before it trims any program's working set. (A big file read in pieces keeps ~100 MiB mapped
// however much is read: the ZArchive's one file.)
struct IdleKeptFile {
	uint64_t    at    = 0; // (GetTickCount64)
	uint64_t    stamp = 0;
	std::string path;
};
std::deque<IdleKeptFile> g_idle_read, g_idle_ahead; // reads done and files opened ahead, oldest first (g_kept_files_mutex)
uint64_t                 g_kept_stamp = 0;          // (g_kept_files_mutex)

// After how long unread a file read by the game closes: 10 s (an audio stream being played reads a piece every second
// or so and stays open), KYTY_FILE_IDLE_CLOSE=<seconds> (0: never, as before). A file opened ahead and not read by the
// game closes after 2 minutes (no cache view: only its handle).
uint64_t IdleCloseMs() {
	static const uint64_t ms = [] {
		const char* value = std::getenv("KYTY_FILE_IDLE_CLOSE");
		return value != nullptr && *value != '\0' ? std::strtoull(value, nullptr, 10) * 1000 : uint64_t {10000};
	}();
	return ms;
}

uint64_t AheadCloseMs() {
	return std::max<uint64_t>(IdleCloseMs(), 120000);
}

// (g_kept_files_mutex held) `kept`, the file of `path`, closes once unread for the queue's wait.
void QueueIdleLocked(std::deque<IdleKeptFile>& queue, KeptFile& kept, const std::string& path) {
	if (IdleCloseMs() == 0) return;
	kept.stamp = ++g_kept_stamp;
	queue.push_back({GetTickCount64(), kept.stamp, path});
	static std::once_flag started;
	std::call_once(started, [] { std::thread(KeptFileCloser).detach(); });
}

std::wstring WidePath(const std::string& path) {
	const int wide_size = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
	if (wide_size <= 0) return {};
	std::wstring wide(static_cast<size_t>(wide_size), L' ');
	MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), wide_size);
	return wide;
}

// PrefetchKeptFile's thread: the files queued, each read through once in 1 MiB pieces at background priority (low I/O
// priority too: the game's own reads go first) and by a handle of its own (a synchronous handle runs one read at a
// time, so the game's next read would wait behind a piece of this one; the cache is the file's, whatever the handle).
std::mutex              g_prefetch_mutex;
std::condition_variable g_prefetch_ready;
std::vector<std::wstring> g_prefetch_queue;

void PrefetchThread() {
	SetThreadName("Kyty.FilePrefetch");
	SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
	std::vector<uint8_t> piece(1u << 20u);
	for (;;) {
		std::wstring path;
		{
			std::unique_lock lock(g_prefetch_mutex);
			g_prefetch_ready.wait(lock, [] { return !g_prefetch_queue.empty(); });
			path = std::move(g_prefetch_queue.front());
			g_prefetch_queue.erase(g_prefetch_queue.begin());
		}
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		                          FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (file == INVALID_HANDLE_VALUE) continue;
		DWORD      got   = 0;
		uint64_t   total = 0;
		const auto begin = std::chrono::steady_clock::now();
		while (ReadFile(file, piece.data(), static_cast<DWORD>(piece.size()), &got, nullptr) != 0 && got != 0) total += got;
		CloseHandle(file);
		// KYTY_APR_LOG (libAmpr.cpp's): each file read through.
		static const bool log_reads = std::getenv("KYTY_APR_LOG") != nullptr;
		if (log_reads)
			std::printf("APRPREFETCH %llu bytes %.1f ms %ls\n", static_cast<unsigned long long>(total),
			            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count(), path.c_str());
	}
}
} // namespace

// FollowKeptFiles: the recorded opens and the files queued for the openers. A 16 GB laptop loading 1-1 opened 28,528
// files for 31,390 reads, one after another on the game's file thread (0.9 ms a read at the median, 11 ms at p90:
// metadata and the antivirus's scan of each first open); its file thread was 60% in CreateFileW over the ~100 s
// loading fog.
struct FileFollow {
	std::mutex                                mutex; // (never held with g_kept_files_mutex)
	std::condition_variable                   ready;
	std::unordered_map<std::string, uint32_t> index; // host path -> place in the recording
	std::vector<const std::string*>           paths; // index's keys in recorded order
	std::vector<uint8_t>                      queued; // per place: queued, opened or opened by the game
	std::vector<uint64_t>                     closed; // per place: when KeptFileCloser closed it (GetTickCount64), or 0
	std::deque<uint32_t>                      queue;
	std::string                               root; // (with its separator)
	FILE*                                     out       = nullptr;
	uint32_t                                  unflushed = 0;
	bool                                      on        = false;
};
FileFollow g_follow;
// The opens wait (metadata, the antivirus), each thread one at a time: the 1-1 fog took 28.5-30.7 s with 4 threads and
// windows of 256, 24.3 s with 8 and 512, 17.6-20.6 s with 16 and 1024, 18.8 s with 32 and 2048. (KYTY_FILE_FOLLOW_WINDOW,
// KYTY_FILE_FOLLOW_THREADS: other counts.)
uint32_t g_follow_window = 1024, g_follow_threads = 16;
// A file closed unread for a while is opened ahead again only this long after (a world visited again): the game reads in
// an order of its own, and windows of files it read a moment before opened again those just closed (25-60 thousand
// opens a minute through a tour of four worlds, 45-90 thousand files open at once).
constexpr uint64_t FollowAgainMs = 5 * 60 * 1000;
// The files kept open at most before the openers stop opening ahead (the game's own opens are never held back). A
// player's loads stuck in the fog for good after a world change with 64,481 files open, about 2^16 (the game's folder
// on a drive whose file system or driver takes no more, it seems), and went through with the read-ahead off (issue
// #249); this desktop peaked at 42 thousand. Lowered when an open of the game's fails (CloseFilesOpenedAhead).
std::atomic<size_t> g_kept_open_max {48 * 1024};

void FollowOpenThread() {
	SetThreadName("Kyty.FileOpen");
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	for (;;) {
		std::string path;
		{
			std::unique_lock lock(g_follow.mutex);
			g_follow.ready.wait(lock, [] { return !g_follow.queue.empty(); });
			path = *g_follow.paths[g_follow.queue.front()];
			g_follow.queue.pop_front();
		}
		{
			const std::lock_guard lock(g_kept_files_mutex);
			if (g_kept_files.contains(path) || g_kept_files.size() >= g_kept_open_max.load(std::memory_order_relaxed)) continue;
		}
		const auto wide = WidePath(path);
		if (wide.empty()) continue;
		HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		                            FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE) continue; // (a file of an earlier session gone)
		LARGE_INTEGER length {};
		if (GetFileSizeEx(handle, &length) == 0) {
			CloseHandle(handle);
			continue;
		}
		const std::lock_guard lock(g_kept_files_mutex);
		const auto [kept, added] =
		    g_kept_files.try_emplace(path, KeptFile {handle, static_cast<uint64_t>(length.QuadPart), false, true});
		if (!added) {
			CloseHandle(handle);
			continue;
		}
		QueueIdleLocked(g_idle_ahead, kept->second, path);
	}
}

void KeptFileCloser() {
	SetThreadName("Kyty.FileClose");
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	std::vector<HANDLE>      handles;
	std::vector<std::string> paths;
	uint64_t                 read = 0, ahead = 0, reported = GetTickCount64();
	for (;;) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		const uint64_t now  = GetTickCount64();
		size_t         open = 0;
		{
			const std::lock_guard lock(g_kept_files_mutex);
			const auto expire = [&](std::deque<IdleKeptFile>& queue, uint64_t wait) {
				for (; !queue.empty() && now - queue.front().at >= wait; queue.pop_front()) {
					const auto found = g_kept_files.find(queue.front().path);
					// (Being read, or queued again since: a later entry has it.)
					if (found == g_kept_files.end() || found->second.readers != 0 || found->second.stamp != queue.front().stamp)
						continue;
					handles.push_back(found->second.handle);
					g_kept_files.erase(found);
					paths.push_back(std::move(queue.front().path));
				}
			};
			expire(g_idle_read, IdleCloseMs());
			const size_t after_reads = handles.size();
			expire(g_idle_ahead, AheadCloseMs());
			read += after_reads;
			ahead += handles.size() - after_reads;
			open = g_kept_files.size();
		}
		if (handles.empty()) continue;
		for (const auto handle: handles)
			CloseHandle(handle);
		{
			// (Followed again after FollowAgainMs.)
			const std::lock_guard lock(g_follow.mutex);
			for (const auto& path: paths)
				if (const auto found = g_follow.index.find(path); found != g_follow.index.end()) g_follow.closed[found->second] = now;
		}
		handles.clear();
		paths.clear();
		if (now - reported >= 60000) {
			std::printf("Files: in the last %.0f s %llu of the game's closed %.0f s after its last read (the data stays cached "
			            "as available memory), %llu opened ahead and not read; %zu open\n",
			            static_cast<double>(now - reported) / 1000.0, static_cast<unsigned long long>(read),
			            static_cast<double>(IdleCloseMs()) / 1000.0, static_cast<unsigned long long>(ahead), open);
			read = ahead = 0;
			reported     = now;
		}
	}
}

// The game opened (or first read) `path`: the recorded files after it queued, or it recorded.
void FollowFrom(const std::string& path) {
	std::lock_guard lock(g_follow.mutex);
	if (!g_follow.on) return;
	const auto found = g_follow.index.find(path);
	if (found == g_follow.index.end()) {
		const auto place = static_cast<uint32_t>(g_follow.paths.size());
		const auto added = g_follow.index.emplace(path, place).first;
		g_follow.paths.push_back(&added->first);
		g_follow.queued.push_back(1);
		g_follow.closed.push_back(0);
		if (g_follow.out != nullptr) {
			const bool under = !g_follow.root.empty() && path.starts_with(g_follow.root);
			std::fprintf(g_follow.out, "%s\n", path.c_str() + (under ? g_follow.root.size() : 0));
			if (++g_follow.unflushed >= 32) {
				std::fflush(g_follow.out);
				g_follow.unflushed = 0;
			}
		}
		return;
	}
	g_follow.queued[found->second] = 1;
	g_follow.closed[found->second] = 0;
	// Ahead of the windows queued before (the game left them, or is past them): the next file first.
	const auto     end = std::min<size_t>(size_t {found->second} + 1 + g_follow_window, g_follow.paths.size());
	const uint64_t now = GetTickCount64();
	bool           any = false;
	for (auto place = end; place-- > size_t {found->second} + 1;)
		if (g_follow.queued[place] == 0 || (g_follow.closed[place] != 0 && now - g_follow.closed[place] >= FollowAgainMs)) {
			g_follow.queued[place] = 1;
			g_follow.closed[place] = 0;
			g_follow.queue.push_front(static_cast<uint32_t>(place));
			any = true;
		}
	if (any) g_follow.ready.notify_all();
}

// An open of the game's failed for want of something other than the file (issue #249: the loads of a player whose
// drive seems to take about 2^16 open files stopped): the files opened ahead and not read yet close, the openers keep
// at most three quarters of what was open from then on, and the first few such failures go in the log. The number
// closed (0: none to give back).
size_t CloseFilesOpenedAhead(const std::string& path, DWORD error) {
	std::vector<HANDLE> handles;
	size_t              open = 0;
	{
		const std::lock_guard lock(g_kept_files_mutex);
		open = g_kept_files.size();
		for (auto it = g_kept_files.begin(); it != g_kept_files.end();) {
			if (it->second.ahead && it->second.readers == 0) {
				handles.push_back(it->second.handle);
				it = g_kept_files.erase(it);
			} else {
				++it;
			}
		}
		g_kept_open_max.store(std::min(g_kept_open_max.load(std::memory_order_relaxed), std::max<size_t>(open * 3 / 4, 1024)),
		                      std::memory_order_relaxed);
	}
	for (const auto handle: handles)
		CloseHandle(handle);
	static std::atomic<uint32_t> logged {0};
	if (logged.fetch_add(1, std::memory_order_relaxed) < 5) {
		std::printf("Files: opening %s failed (error %lu) with %zu files open: %zu opened ahead closed, the read-ahead keeps at "
		            "most %zu open from now on (issue #249)\n",
		            path.c_str(), static_cast<unsigned long>(error), open, handles.size(),
		            g_kept_open_max.load(std::memory_order_relaxed));
		std::fflush(stdout);
	}
	return handles.size();
}

uint64_t OpenKeptReadFile(const std::string& path, uint64_t* size) {
	{
		std::unique_lock lock(g_kept_files_mutex);
		if (const auto found = g_kept_files.find(path); found != g_kept_files.end()) {
			*size              = found->second.size;
			const auto handle  = reinterpret_cast<uint64_t>(found->second.handle);
			const bool first   = std::exchange(found->second.ahead, false);
			found->second.readers++;
			lock.unlock();
			if (first) FollowFrom(path);
			return handle;
		}
	}
	FollowFrom(path);
	// Opened outside the lock: a 16 GB laptop's opens took 100-390 ms while the game streamed a level in, and each
	// read of a file kept open already (an audio stream's next piece) waited for them behind it (open=150-250 ms).
	const auto wide = WidePath(path);
	if (wide.empty()) return 0;
	const auto open = [&] {
		return CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		                   FILE_ATTRIBUTE_NORMAL, nullptr);
	};
	HANDLE handle = open();
	if (handle == INVALID_HANDLE_VALUE) {
		// (Not for a file that is not there: the game asks for some.)
		if (const DWORD error = GetLastError();
		    error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND && CloseFilesOpenedAhead(path, error) != 0)
			handle = open();
		if (handle == INVALID_HANDLE_VALUE) return 0;
	}
	LARGE_INTEGER length {};
	if (GetFileSizeEx(handle, &length) == 0) {
		CloseHandle(handle);
		return 0;
	}
	const std::lock_guard lock(g_kept_files_mutex);
	const auto [kept, added] = g_kept_files.try_emplace(path, KeptFile {handle, static_cast<uint64_t>(length.QuadPart)});
	if (!added) CloseHandle(handle); // (another thread opened it meanwhile: its handle is the one kept)
	kept->second.ahead = false;
	kept->second.readers++;
	*size = kept->second.size;
	return reinterpret_cast<uint64_t>(kept->second.handle);
}

void DoneKeptReadFile(const std::string& path) {
	const std::lock_guard lock(g_kept_files_mutex);
	const auto            found = g_kept_files.find(path);
	if (found != g_kept_files.end() && found->second.readers != 0 && --found->second.readers == 0)
		QueueIdleLocked(g_idle_read, found->second, path);
}

uint32_t ReadOpenFileAt(uint64_t file, uint64_t offset, void* buffer, uint32_t size) {
	// A positional read (the handle is synchronous: it returns when done), so threads share the handle.
	OVERLAPPED at {};
	at.Offset     = static_cast<DWORD>(offset);
	at.OffsetHigh = static_cast<DWORD>(offset >> 32u);
	DWORD got     = 0;
	if (ReadFile(reinterpret_cast<HANDLE>(file), buffer, size, &got, &at) == 0) return 0;
	return got;
}

void FollowKeptFiles(const std::string& recording, const std::string& root) {
	static std::once_flag once;
	std::call_once(once, [&] {
		if (const char* value = std::getenv("KYTY_FILE_FOLLOW"); value != nullptr && std::string_view(value) == "0") return;
		if (const char* value = std::getenv("KYTY_FILE_FOLLOW_WINDOW"); value != nullptr && *value != '\0')
			g_follow_window = std::clamp(static_cast<uint32_t>(std::strtoul(value, nullptr, 10)), 1u, 65536u);
		if (const char* value = std::getenv("KYTY_FILE_FOLLOW_THREADS"); value != nullptr && *value != '\0')
			g_follow_threads = std::clamp(static_cast<uint32_t>(std::strtoul(value, nullptr, 10)), 1u, 64u);
		// KYTY_FILE_OPEN_MAX=<n>: the files kept open before the openers stop (g_kept_open_max).
		if (const char* value = std::getenv("KYTY_FILE_OPEN_MAX"); value != nullptr && *value != '\0')
			g_kept_open_max.store(std::max<size_t>(std::strtoull(value, nullptr, 10), 256), std::memory_order_relaxed);
		std::lock_guard lock(g_follow.mutex);
		g_follow.root = root;
		if (!g_follow.root.empty() && g_follow.root.back() != '/' && g_follow.root.back() != '\\') g_follow.root += '/';
		if (FILE* in = std::fopen(recording.c_str(), "rb")) {
			std::string line;
			for (int c; (c = std::fgetc(in)) != EOF;) {
				if (c != '\n') {
					line += static_cast<char>(c);
					continue;
				}
				if (!line.empty() && line.back() == '\r') line.pop_back();
				if (!line.empty()) {
					// (Relative: under the root. Absolute: a drive or a separator first.)
					const bool absolute = line.size() > 1 && (line[1] == ':' || line[0] == '/' || line[0] == '\\');
					auto       full     = absolute ? line : g_follow.root + line;
					const auto place    = static_cast<uint32_t>(g_follow.paths.size());
					if (const auto [added, inserted] = g_follow.index.emplace(std::move(full), place); inserted)
						g_follow.paths.push_back(&added->first);
				}
				line.clear();
			}
			std::fclose(in);
		}
		g_follow.queued.assign(g_follow.paths.size(), 0);
		g_follow.closed.assign(g_follow.paths.size(), 0);
		std::error_code error;
		std::filesystem::create_directories(std::filesystem::path(recording).parent_path(), error);
		g_follow.out = std::fopen(recording.c_str(), "ab");
		g_follow.on  = true;
		for (uint32_t t = 0; t < g_follow_threads; ++t) std::thread(FollowOpenThread).detach();
	});
}

void PrefetchKeptFile(const std::string& path) {
	{
		const std::lock_guard lock(g_kept_files_mutex);
		const auto            found = g_kept_files.find(path);
		if (found == g_kept_files.end() || std::exchange(found->second.prefetched, true)) return;
	}
	auto wide = WidePath(path);
	if (wide.empty()) return;
	static std::once_flag started;
	std::call_once(started, [] { std::thread(PrefetchThread).detach(); });
	{
		const std::lock_guard lock(g_prefetch_mutex);
		g_prefetch_queue.push_back(std::move(wide));
	}
	g_prefetch_ready.notify_one();
}

double NamedThreadsCpuSeconds(const char* name) {
	std::wstring wanted;
	for (const char* p = name; *p != '\0'; ++p) wanted.push_back(static_cast<wchar_t>(*p));
	double       total    = 0;
	const DWORD  process  = GetCurrentProcessId();
	HANDLE       snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return 0;
	THREADENTRY32 entry {};
	entry.dwSize = sizeof(entry);
	for (BOOL more = Thread32First(snapshot, &entry); more != 0; more = Thread32Next(snapshot, &entry)) {
		if (entry.th32OwnerProcessID != process) continue;
		HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
		if (thread == nullptr) continue;
		PWSTR description = nullptr;
		if (SUCCEEDED(GetThreadDescription(thread, &description)) && description != nullptr) {
			if (wanted == description) total += HandleCpuSeconds(thread);
			LocalFree(description);
		}
		CloseHandle(thread);
	}
	CloseHandle(snapshot);
	return total;
}

std::vector<std::pair<uint32_t, std::string>> ProcessThreads() {
	std::vector<std::pair<uint32_t, std::string>> threads;
	const DWORD process  = GetCurrentProcessId();
	HANDLE      snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return threads;
	THREADENTRY32 entry {};
	entry.dwSize = sizeof(entry);
	for (BOOL more = Thread32First(snapshot, &entry); more != 0; more = Thread32Next(snapshot, &entry)) {
		if (entry.th32OwnerProcessID != process) continue;
		HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
		if (thread == nullptr) continue;
		PWSTR       description = nullptr;
		std::string name;
		if (SUCCEEDED(GetThreadDescription(thread, &description)) && description != nullptr) {
			for (const wchar_t* c = description; *c != L'\0'; ++c) name.push_back(*c < 128 ? static_cast<char>(*c) : '?');
			LocalFree(description);
		}
		threads.emplace_back(entry.th32ThreadID, std::move(name));
		CloseHandle(thread);
	}
	CloseHandle(snapshot);
	return threads;
}

bool FlushProcessWriteBuffers() {
	::FlushProcessWriteBuffers();
	return true;
}

bool CurrentThreadStack(uint64_t* low, uint64_t* high) {
	ULONG_PTR stack_low = 0, stack_high = 0;
	GetCurrentThreadStackLimits(&stack_low, &stack_high);
	*low  = stack_low;
	*high = stack_high;
	return stack_high > stack_low;
}

void AvoidCpuList(const char* avoid_cpus) {
	DWORD_PTR process = 0, system = 0;
	if (const auto avoid = static_cast<DWORD_PTR>(CpuListMask(avoid_cpus));
	    avoid != 0 && GetProcessAffinityMask(GetCurrentProcess(), &process, &system) != 0 && (process & ~avoid) != 0)
		(void)SetThreadAffinityMask(GetCurrentThread(), process & ~avoid);
}

void MakeBackgroundThread(const char* avoid_cpus) {
	(void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	AvoidCpuList(avoid_cpus);
}

void MakeCriticalThread() {
	(void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}

NormalPriority::NormalPriority() {
	m_saved   = GetThreadPriority(GetCurrentThread());
	m_changed = m_saved != THREAD_PRIORITY_ERROR_RETURN && m_saved > THREAD_PRIORITY_NORMAL &&
	          SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL) != 0;
}

NormalPriority::~NormalPriority() {
	if (m_changed) (void)SetThreadPriority(GetCurrentThread(), m_saved);
}

int KeepRenderCoreAlone() {
	return -1;
}

namespace {
// d3dkmthk.h (WDK) structures for the display's vertical blank, taken from gdi32 at run time.
using KmtHandle = UINT;
struct KmtOpenAdapterFromGdiDisplayName {
	WCHAR     DeviceName[32];
	KmtHandle hAdapter;
	LUID      AdapterLuid;
	UINT      VidPnSourceId;
};
struct KmtWaitForVerticalBlankEvent {
	KmtHandle hAdapter;
	KmtHandle hDevice;
	UINT      VidPnSourceId;
};
struct KmtCloseAdapter {
	KmtHandle hAdapter;
};
using KmtOpenFn  = LONG(APIENTRY*)(KmtOpenAdapterFromGdiDisplayName*);
using KmtWaitFn  = LONG(APIENTRY*)(const KmtWaitForVerticalBlankEvent*);
using KmtCloseFn = LONG(APIENTRY*)(const KmtCloseAdapter*);

struct DisplayVblanks {
	std::mutex              mutex;
	std::condition_variable changed;
	uint64_t                count = 0;
	std::atomic<HWND>       window {nullptr};
	std::atomic<bool>       started {false};
};
DisplayVblanks g_display_vblanks;

bool MonitorDevice(HWND window, HMONITOR& monitor, MONITORINFOEXW& info) {
	monitor     = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
	info        = {};
	info.cbSize = sizeof(info);
	return monitor != nullptr && GetMonitorInfoW(monitor, &info) != FALSE;
}

void CountDisplayVblanks(KmtOpenFn open, KmtWaitFn wait, KmtCloseFn close) {
	SetThreadName("Kyty.DisplayVblank");
	MakeCriticalThread();
	HMONITOR  monitor = nullptr;
	KmtHandle adapter = 0;
	UINT      source  = 0;
	uint32_t  waits   = 0;
	const auto release = [&] {
		if (adapter != 0) {
			const KmtCloseAdapter request {adapter};
			(void)close(&request);
			adapter = 0;
		}
	};
	for (;;) {
		// The window's display: at the start, after a failed wait and every 120 vblanks (the window may have moved).
		if (adapter == 0 || ++waits >= 120) {
			waits = 0;
			HMONITOR       now = nullptr;
			MONITORINFOEXW info {};
			if (!MonitorDevice(g_display_vblanks.window.load(), now, info)) {
				release();
				Sleep(100);
				continue;
			}
			if (adapter == 0 || now != monitor) {
				release();
				KmtOpenAdapterFromGdiDisplayName request {};
				wcsncpy_s(request.DeviceName, info.szDevice, _TRUNCATE);
				if (open(&request) != 0) {
					Sleep(100);
					continue;
				}
				monitor = now;
				adapter = request.hAdapter;
				source  = request.VidPnSourceId;
			}
		}
		const KmtWaitForVerticalBlankEvent request {adapter, 0, source};
		if (wait(&request) != 0) {
			release();
			Sleep(16);
			continue;
		}
		{
			std::lock_guard lock(g_display_vblanks.mutex);
			g_display_vblanks.count++;
		}
		g_display_vblanks.changed.notify_all();
	}
}
} // namespace

int DisplayRefreshHz(void* window) {
	HMONITOR       monitor = nullptr;
	MONITORINFOEXW info {};
	if (!MonitorDevice(static_cast<HWND>(window), monitor, info)) return 0;
	DEVMODEW mode {};
	mode.dmSize = sizeof(mode);
	if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) == FALSE) return 0;
	return mode.dmDisplayFrequency > 1 ? static_cast<int>(mode.dmDisplayFrequency) : 0;
}

// The largest visible top-level window of process `pid` or of a process it started: Steam's windows, Big Picture's full
// screen one among them, belong to the steamwebhelper.exe it starts.
static HWND LargestWindowOf(DWORD pid) {
	struct Search {
		std::vector<DWORD> pids;
		HWND               best = nullptr;
		LONG               area = 0;
	} search {{pid}};
	if (HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snapshot != INVALID_HANDLE_VALUE) {
		PROCESSENTRY32W entry {};
		entry.dwSize = sizeof(entry);
		for (BOOL more = Process32FirstW(snapshot, &entry); more != FALSE; more = Process32NextW(snapshot, &entry)) {
			if (entry.th32ParentProcessID == pid) search.pids.push_back(entry.th32ProcessID);
		}
		CloseHandle(snapshot);
	}
	EnumWindows(
	    [](HWND window, LPARAM parameter) -> BOOL {
		    auto& s     = *reinterpret_cast<Search*>(parameter);
		    DWORD owner = 0;
		    RECT  rect {};
		    if (GetWindowThreadProcessId(window, &owner) != 0 &&
		        std::find(s.pids.begin(), s.pids.end(), owner) != s.pids.end() && IsWindowVisible(window) != FALSE &&
		        GetWindow(window, GW_OWNER) == nullptr && GetWindowRect(window, &rect) != FALSE) {
			    const LONG area = (rect.right - rect.left) * (rect.bottom - rect.top);
			    if (area > s.area) {
				    s.area = area;
				    s.best = window;
			    }
		    }
		    return TRUE;
	    },
	    reinterpret_cast<LPARAM>(&search));
	return search.best;
}

// A window for the log: its class and its program ("'SDL_app' of steamwebhelper.exe").
static std::string DescribeWindow(HWND window) {
	char  name[64] = {};
	DWORD pid      = 0;
	GetClassNameA(window, name, sizeof(name));
	GetWindowThreadProcessId(window, &pid);
	char program[MAX_PATH] = "?";
	if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); process != nullptr) {
		DWORD size = sizeof(program);
		if (QueryFullProcessImageNameA(process, 0, program, &size) == FALSE) std::strcpy(program, "?");
		CloseHandle(process);
	}
	const char* file = std::strrchr(program, '\\');
	return std::string("'") + name + "' of " + (file != nullptr ? file + 1 : program);
}

void ReturnFocus(void* window) {
	const char* value = std::getenv("KYTY_RETURN_FOCUS");
	if (value == nullptr || *value == '\0') return;
	const auto own     = static_cast<HWND>(window);
	const auto started = reinterpret_cast<HWND>(static_cast<uintptr_t>(std::strtoull(value, nullptr, 10)));
	auto       target  = started;
	// The window that had the focus as the launcher started can be gone or hidden by now (issue #217: Big Picture still
	// lost it): the largest visible window of the process that started the launcher (Steam) or of one Steam started.
	if (target == nullptr || target == own || IsWindow(target) == FALSE || IsWindowVisible(target) == FALSE) {
		std::printf("Focus: the window in front as the game started (%s) is gone or hidden\n", value);
		const char* pid = std::getenv("KYTY_RETURN_FOCUS_PID");
		target          = pid != nullptr ? LargestWindowOf(static_cast<DWORD>(std::strtoul(pid, nullptr, 10))) : nullptr;
	}
	if (target == nullptr || target == own) {
		std::printf("Focus: no window to give it back to\n");
		std::fflush(stdout);
		return;
	}
	// Handed over while the game's window has the focus (Windows lets the process that has it give it away), then the
	// game's window hidden: hidden first, Windows would give the focus to a window of its own choosing. Refused (the
	// focus already elsewhere, say Steam's menu over the game), with the input of the foreground window's thread
	// attached to this one's.
	if (IsIconic(target) != FALSE) ShowWindow(target, SW_RESTORE);
	bool given = SetForegroundWindow(target) != FALSE && GetForegroundWindow() == target;
	if (!given) {
		const DWORD foreground = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
		const DWORD self       = GetCurrentThreadId();
		if (foreground != 0 && foreground != self && AttachThreadInput(self, foreground, TRUE) != FALSE) {
			BringWindowToTop(target);
			given = SetForegroundWindow(target) != FALSE && GetForegroundWindow() == target;
			AttachThreadInput(self, foreground, FALSE);
		}
	}
	// Also as Alt+Tab switches to it: Big Picture, in front again and "given back", still showed no focus and took no pad
	// input until an Alt+Tab (issue #217).
	if (given) SwitchToThisWindow(target, TRUE);
	std::printf("Focus: %s window %p, %s (%s)\n", given ? "given back to" : "not taken by", static_cast<void*>(target),
	            DescribeWindow(target).c_str(),
	            target == started ? "the one in front as the game started" : "the largest of the launcher's parent's");
	std::fflush(stdout);
	if (own != nullptr) ShowWindow(own, SW_HIDE);
	// (Which window is in front a moment later, the game's hidden: one that took the focus after the hand-off shows here.)
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	const HWND later = GetForegroundWindow();
	std::printf("Focus: 0.3 s later in front: %s\n",
	            later == target ? "that window" : later == nullptr ? "no window" : DescribeWindow(later).c_str());
	std::fflush(stdout);
}

bool StartDisplayVblanks(void* window) {
	g_display_vblanks.window.store(static_cast<HWND>(window));
	if (g_display_vblanks.started.load()) return true;
	HMODULE    gdi   = GetModuleHandleW(L"gdi32.dll");
	if (gdi == nullptr) gdi = LoadLibraryW(L"gdi32.dll");
	const auto open  = gdi != nullptr ? reinterpret_cast<KmtOpenFn>(reinterpret_cast<void*>(
	                                       GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName")))
	                                  : nullptr;
	const auto wait  = gdi != nullptr ? reinterpret_cast<KmtWaitFn>(reinterpret_cast<void*>(
	                                       GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent")))
	                                  : nullptr;
	const auto close = gdi != nullptr ? reinterpret_cast<KmtCloseFn>(reinterpret_cast<void*>(
	                                        GetProcAddress(gdi, "D3DKMTCloseAdapter")))
	                                  : nullptr;
	if (open == nullptr || wait == nullptr || close == nullptr) return false;
	if (!g_display_vblanks.started.exchange(true)) std::thread(CountDisplayVblanks, open, wait, close).detach();
	return true;
}

bool WaitDisplayVblank(uint64_t& seen, uint32_t timeout_us) {
	if (!g_display_vblanks.started.load()) return false;
	std::unique_lock lock(g_display_vblanks.mutex);
	if (seen == 0) seen = g_display_vblanks.count; // (a caller's first wait: the next vblank)
	if (!g_display_vblanks.changed.wait_for(lock, std::chrono::microseconds(timeout_us),
	                                        [&] { return g_display_vblanks.count > seen; })) {
		return false;
	}
	// One vblank a call: those that came while the caller was away are caught up, four at most.
	seen = std::max(seen + 1, g_display_vblanks.count > 4 ? g_display_vblanks.count - 4 : uint64_t {0});
	return true;
}

uint64_t OpenScratchFile() {
	wchar_t directory[MAX_PATH + 1] {}, path[MAX_PATH + 1] {};
	if (GetTempPathW(MAX_PATH + 1, directory) == 0 || GetTempFileNameW(directory, L"kyt", 0, path) == 0) return 0;
	HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
	                          FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		(void)DeleteFileW(path);
		return 0;
	}
	return reinterpret_cast<uint64_t>(file);
}

// ReadFile or WriteFile at an offset, in pieces a DWORD can count.
template <typename Call, typename Byte>
static bool ScratchTransfer(uint64_t file, uint64_t offset, Byte* data, size_t size, Call call) {
	while (size != 0) {
		OVERLAPPED at {};
		at.Offset         = static_cast<DWORD>(offset);
		at.OffsetHigh     = static_cast<DWORD>(offset >> 32u);
		DWORD      done   = 0;
		const auto length = static_cast<DWORD>(size < (1u << 30u) ? size : (1u << 30u));
		if (call(reinterpret_cast<HANDLE>(file), data, length, &done, &at) == 0 || done == 0) return false;
		data += done;
		offset += done;
		size -= done;
	}
	return true;
}

bool WriteScratchFile(uint64_t file, uint64_t offset, const void* data, size_t size) {
	return file != 0 && ScratchTransfer(file, offset, static_cast<const uint8_t*>(data), size, WriteFile);
}

bool ReadScratchFile(uint64_t file, uint64_t offset, void* data, size_t size) {
	return file != 0 && ScratchTransfer(file, offset, static_cast<uint8_t*>(data), size, ReadFile);
}

void CloseScratchFile(uint64_t file) {
	if (file != 0) CloseHandle(reinterpret_cast<HANDLE>(file));
}

uint64_t OpenFileForReading(const char* path) {
	wchar_t wide[MAX_PATH * 2] {};
	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH * 2) == 0) return 0;
	HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
	                          FILE_FLAG_RANDOM_ACCESS, nullptr);
	return file == INVALID_HANDLE_VALUE ? 0 : reinterpret_cast<uint64_t>(file);
}

uint64_t OpenDataFile(const char* path) {
	wchar_t wide[MAX_PATH * 2] {};
	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH * 2) == 0) return 0;
	// (Not shared: another process opening it fails.)
	HANDLE file = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_FLAG_RANDOM_ACCESS, nullptr);
	return file == INVALID_HANDLE_VALUE ? 0 : reinterpret_cast<uint64_t>(file);
}

uint64_t ScratchFileSize(uint64_t file) {
	LARGE_INTEGER size {};
	return file != 0 && GetFileSizeEx(reinterpret_cast<HANDLE>(file), &size) != 0 ? static_cast<uint64_t>(size.QuadPart)
	                                                                              : 0;
}

bool ResizeScratchFile(uint64_t file, uint64_t size) {
	FILE_END_OF_FILE_INFO end {};
	end.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
	return file != 0 &&
	       SetFileInformationByHandle(reinterpret_cast<HANDLE>(file), FileEndOfFileInfo, &end, sizeof(end)) != 0;
}

bool FlushScratchFile(uint64_t file) {
	return file != 0 && FlushFileBuffers(reinterpret_cast<HANDLE>(file)) != 0;
}

std::string ExecutablePath() {
	std::vector<wchar_t> path(MAX_PATH);
	for (;;) {
		const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
		if (length == 0) return {};
		if (length < path.size()) {
			const int   bytes = WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(length), nullptr, 0, nullptr, nullptr);
			std::string utf8(bytes > 0 ? static_cast<size_t>(bytes) : 0, '\0');
			if (bytes <= 0 || WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(length), utf8.data(), bytes,
			                                      nullptr, nullptr) != bytes)
				return {};
			return utf8;
		}
		if (path.size() >= 32768) return {};
		path.resize(path.size() * 2);
	}
}

std::string FileSystemName(const std::string& path) {
	const int            length = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
	std::vector<wchar_t> wide(length > 0 ? static_cast<size_t>(length) : 1, L'\0');
	if (length <= 0 || MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), length) != length) return {};
	std::array<wchar_t, MAX_PATH + 1> volume {};
	std::array<wchar_t, MAX_PATH + 1> name {};
	if (GetVolumePathNameW(wide.data(), volume.data(), static_cast<DWORD>(volume.size())) == FALSE ||
	    GetVolumeInformationW(volume.data(), nullptr, 0, nullptr, nullptr, nullptr, name.data(),
	                          static_cast<DWORD>(name.size())) == FALSE)
		return {};
	std::string result;
	for (const wchar_t c: name) {
		if (c == L'\0') break;
		result += static_cast<char>(c < 0x80 ? c : '?');
	}
	return result;
}

bool WriteHeapHistogram(const char* path) {
	std::vector<HANDLE> heaps(256);
	const DWORD         count = GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data());
	if (count == 0 || count > heaps.size()) return false;
	// Nothing is allocated from a heap while it is walked: blocks under 16 bytes are counted by size in arrays made
	// before, larger ones are recorded (size, first qword) in pages of their own and grouped after.
	constexpr uint64_t Small = 16;
	struct Block {
		uint32_t heap;
		uint64_t size;
		uint64_t first;
	};
	constexpr size_t Capacity = size_t {16} << 20u;
	auto* blocks = static_cast<Block*>(VirtualAlloc(nullptr, Capacity * sizeof(Block), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
	if (blocks == nullptr) return false;
	std::vector<std::pair<uint64_t, uint64_t>> by_size(static_cast<size_t>(count) * Small); // heap, size -> blocks, bytes
	size_t recorded = 0, dropped = 0;
	// KYTY_HEAP_SAMPLE=<size>: the bytes of some busy blocks of that size (the first 32, then every 32768th) in
	// <path>.samples.txt, to tell what they are.
	const char*       sample_value = std::getenv("KYTY_HEAP_SAMPLE");
	const uint64_t    sample_size  = sample_value != nullptr ? std::strtoull(sample_value, nullptr, 10) : 0;
	constexpr size_t  SampleCount = 128, SampleBytes = 256;
	struct Sample {
		uint64_t address;
		uint64_t size;
		uint8_t  bytes[SampleBytes];
	};
	auto*  samples = sample_size != 0 ? static_cast<Sample*>(VirtualAlloc(nullptr, SampleCount * sizeof(Sample),
	                                                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
	                                  : nullptr;
	size_t sampled = 0, sample_seen = 0;
	// Where the heaps' memory lies: their regions (segments) and blocks of a megabyte or more (each its own
	// allocation): the private allocations of <path>.vm.tsv that are the heaps'.
	constexpr size_t      MaxPlaces = 65536;
	std::vector<uint64_t> places(MaxPlaces);
	size_t                place_count = 0;
	for (DWORD i = 0; i < count; ++i) {
		if (!HeapLock(heaps[i])) continue;
		PROCESS_HEAP_ENTRY entry {};
		while (HeapWalk(heaps[i], &entry)) {
			if ((entry.wFlags & PROCESS_HEAP_REGION) != 0 && place_count < MaxPlaces)
				places[place_count++] = reinterpret_cast<uint64_t>(entry.lpData);
			if ((entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) == 0) continue;
			if (samples != nullptr && entry.cbData == sample_size && sampled < SampleCount &&
			    (sample_seen++ < 32 || sample_seen % 32768 == 0)) {
				samples[sampled].address = reinterpret_cast<uint64_t>(entry.lpData);
				samples[sampled].size    = entry.cbData;
				std::memcpy(samples[sampled].bytes, entry.lpData, std::min<size_t>(entry.cbData, SampleBytes));
				sampled++;
			}
			if (entry.cbData >= (uint64_t {1} << 20u) && place_count < MaxPlaces)
				places[place_count++] = reinterpret_cast<uint64_t>(entry.lpData);
			if (entry.cbData < Small) {
				auto& use = by_size[i * Small + entry.cbData];
				use.first++;
				use.second += entry.cbData;
			} else if (recorded < Capacity) {
				uint64_t first = 0;
				std::memcpy(&first, entry.lpData, sizeof(first));
				blocks[recorded++] = {i, entry.cbData, first};
			} else {
				dropped++;
			}
		}
		HeapUnlock(heaps[i]);
	}
	std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> sizes; // block size -> blocks, bytes
	for (size_t i = 0; i < by_size.size(); i++) {
		if (by_size[i].first == 0) continue;
		auto& use = sizes[i % Small];
		use.first += by_size[i].first;
		use.second += by_size[i].second;
	}
	for (size_t i = 0; i < recorded; i++) {
		auto& use = sizes[blocks[i].size];
		use.first++;
		use.second += blocks[i].size;
	}
	FILE* file = std::fopen(path, "w");
	if (file == nullptr) {
		VirtualFree(blocks, 0, MEM_RELEASE);
		return false;
	}
	std::fprintf(file, "size\tcount\tbytes\n");
	for (const auto& [size, use]: sizes)
		std::fprintf(file, "%llu\t%llu\t%llu\n", static_cast<unsigned long long>(size),
		             static_cast<unsigned long long>(use.first), static_cast<unsigned long long>(use.second));
	std::fclose(file);
	if (samples != nullptr) {
		if (FILE* out = std::fopen((std::string(path) + ".samples.txt").c_str(), "w"); out != nullptr) {
			for (size_t i = 0; i < sampled; i++) {
				std::fprintf(out, "%llx %llu:", static_cast<unsigned long long>(samples[i].address),
				             static_cast<unsigned long long>(samples[i].size));
				for (size_t at = 0; at + 8 <= std::min<size_t>(samples[i].size, SampleBytes); at += 8) {
					uint64_t qword = 0;
					std::memcpy(&qword, samples[i].bytes + at, sizeof(qword));
					std::fprintf(out, " %llx", static_cast<unsigned long long>(qword));
				}
				std::fprintf(out, "\n");
			}
			std::fclose(out);
		}
		VirtualFree(samples, 0, MEM_RELEASE);
	}
	// What the blocks are, per heap: <path>.sig.tsv groups blocks of 16 bytes or more by size and first qword: SPIR-V's
	// magic, an address in a module (a vtable: module+offset), zero, else data; groups under 64 KiB and 16 blocks go
	// to "rest" per heap.
	struct Module {
		uint64_t    begin;
		uint64_t    end;
		std::string name;
	};
	std::vector<Module>  modules;
	std::vector<HMODULE> handles(2048);
	DWORD                needed = 0;
	if (EnumProcessModules(GetCurrentProcess(), handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)),
	                       &needed) != 0) {
		handles.resize(std::min<size_t>(handles.size(), needed / sizeof(HMODULE)));
		for (HMODULE handle: handles) {
			MODULEINFO module {};
			char       name[MAX_PATH] {};
			if (GetModuleInformation(GetCurrentProcess(), handle, &module, sizeof(module)) != 0 &&
			    GetModuleBaseNameA(GetCurrentProcess(), handle, name, MAX_PATH) != 0) {
				const auto begin = reinterpret_cast<uint64_t>(module.lpBaseOfDll);
				modules.push_back({begin, begin + module.SizeOfImage, name});
			}
		}
	}
	std::sort(modules.begin(), modules.end(), [](const Module& a, const Module& b) { return a.begin < b.begin; });
	constexpr int64_t Data = -1, Spirv = -2, Zero = -3;
	// heap, size, kind (module index or Data/Spirv/Zero), offset in the module -> blocks, bytes
	std::map<std::tuple<uint32_t, uint64_t, int64_t, uint64_t>, std::pair<uint64_t, uint64_t>> groups;
	for (size_t i = 0; i < recorded; i++) {
		const uint64_t first  = blocks[i].first;
		int64_t        kind   = Data;
		uint64_t       offset = 0;
		if (static_cast<uint32_t>(first) == 0x07230203u) {
			kind = Spirv;
		} else if (first == 0) {
			kind = Zero;
		} else {
			auto module = std::upper_bound(modules.begin(), modules.end(), first,
			                               [](uint64_t value, const Module& m) { return value < m.begin; });
			if (module != modules.begin() && first < std::prev(module)->end) {
				kind   = std::prev(module) - modules.begin();
				offset = first - std::prev(module)->begin;
			}
		}
		auto& use = groups[{blocks[i].heap, blocks[i].size, kind, offset}];
		use.first++;
		use.second += blocks[i].size;
	}
	VirtualFree(blocks, 0, MEM_RELEASE);
	const std::string sig_path = std::string(path) + ".sig.tsv";
	file                       = std::fopen(sig_path.c_str(), "w");
	if (file == nullptr) return false;
	std::fprintf(file, "heap\tsize\tkind\tcount\tbytes\n");
	std::vector<std::pair<uint64_t, uint64_t>> rest(count); // per heap: blocks, bytes
	for (const auto& [key, use]: groups) {
		const auto& [heap, size, kind, offset] = key;
		if (use.first < 16 && use.second < (uint64_t {64} << 10u)) {
			rest[heap].first += use.first;
			rest[heap].second += use.second;
			continue;
		}
		char text[320];
		if (kind >= 0) {
			std::snprintf(text, sizeof(text), "%s+0x%llx", modules[kind].name.c_str(), static_cast<unsigned long long>(offset));
		} else {
			std::snprintf(text, sizeof(text), "%s", kind == Spirv ? "spirv" : kind == Zero ? "zero" : "data");
		}
		std::fprintf(file, "%lu\t%llu\t%s\t%llu\t%llu\n", static_cast<unsigned long>(heap), static_cast<unsigned long long>(size),
		             text, static_cast<unsigned long long>(use.first), static_cast<unsigned long long>(use.second));
	}
	for (DWORD i = 0; i < count; ++i) {
		uint64_t blocks_small = 0, bytes_small = 0;
		for (uint64_t size = 0; size < Small; size++) {
			blocks_small += by_size[i * Small + size].first;
			bytes_small += by_size[i * Small + size].second;
		}
		if (rest[i].first != 0)
			std::fprintf(file, "%lu\t0\trest\t%llu\t%llu\n", static_cast<unsigned long>(i),
			             static_cast<unsigned long long>(rest[i].first), static_cast<unsigned long long>(rest[i].second));
		if (blocks_small != 0)
			std::fprintf(file, "%lu\t0\tsmall\t%llu\t%llu\n", static_cast<unsigned long>(i),
			             static_cast<unsigned long long>(blocks_small), static_cast<unsigned long long>(bytes_small));
	}
	if (dropped != 0) std::fprintf(file, "-1\t0\tdropped\t%llu\t0\n", static_cast<unsigned long long>(dropped));
	std::fclose(file);
	// Committed private memory per allocation (VirtualAlloc'd pools, heap segments, driver memory): <path>.vm.tsv.
	const std::string vm_path = std::string(path) + ".vm.tsv";
	file                      = std::fopen(vm_path.c_str(), "w");
	if (file == nullptr) return false;
	std::fprintf(file, "base\tcommitted\tregions\theap\n");
	std::set<uint64_t> heap_bases;
	for (size_t i = 0; i < place_count; i++) {
		MEMORY_BASIC_INFORMATION place {};
		if (VirtualQuery(reinterpret_cast<void*>(places[i]), &place, sizeof(place)) == sizeof(place))
			heap_bases.insert(reinterpret_cast<uint64_t>(place.AllocationBase));
	}
	std::map<uint64_t, std::pair<uint64_t, uint64_t>> bases; // allocation base -> committed bytes, regions
	MEMORY_BASIC_INFORMATION                          info {};
	for (uint64_t address = 0x10000; VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == sizeof(info);) {
		if (info.State == MEM_COMMIT && info.Type == MEM_PRIVATE) {
			auto& base = bases[reinterpret_cast<uint64_t>(info.AllocationBase)];
			base.first += info.RegionSize;
			base.second++;
		}
		const uint64_t next = reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
		if (next <= address) break;
		address = next;
	}
	for (const auto& [base, use]: bases)
		std::fprintf(file, "%llx\t%llu\t%llu\t%d\n", static_cast<unsigned long long>(base),
		             static_cast<unsigned long long>(use.first), static_cast<unsigned long long>(use.second),
		             heap_bases.contains(base) ? 1 : 0);
	std::fclose(file);
	return true;
}

namespace AllocTrace {

// Live blocks of the traced sizes: open addressing by address (tombstones on free), stacks interned by hash. All in
// pages of their own: the hooks never allocate from a heap.
constexpr size_t   MaxSize    = 1u << 20u;
constexpr size_t   BlockSlots = size_t {1} << 22u;
constexpr size_t   StackSlots = size_t {1} << 18u;
constexpr int      Frames     = 20;
constexpr uint64_t Empty = 0, Tombstone = 1;

struct BlockSlot {
	uint64_t address;
	uint32_t stack;
	uint32_t size;
};
struct StackSlot {
	uint64_t hash;
	void*    frames[Frames];
	uint32_t count;
};

using Allocate   = void*(NTAPI*)(void*, ULONG, SIZE_T);
using Free       = BOOLEAN(NTAPI*)(void*, ULONG, void*);
using Reallocate = void*(NTAPI*)(void*, ULONG, void*, SIZE_T);

std::atomic_flag  g_lock       = ATOMIC_FLAG_INIT;
uint8_t*          g_sizes      = nullptr; // MaxSize flags
BlockSlot*        g_blocks     = nullptr;
BlockSlot*        g_scratch    = nullptr; // a rehash's copy of the live blocks
size_t            g_used       = 0;       // slots not empty (live and tombstones)
size_t            g_live       = 0;
StackSlot*        g_stacks     = nullptr;
Allocate          g_allocate   = nullptr;
Free              g_free       = nullptr;
Reallocate        g_reallocate = nullptr;
std::atomic<bool> g_full {false};

struct Guard {
	Guard() {
		while (g_lock.test_and_set(std::memory_order_acquire)) YieldProcessor();
	}
	~Guard() { g_lock.clear(std::memory_order_release); }
};

uint64_t Mix(uint64_t value) {
	value ^= value >> 33u;
	value *= 0xff51afd7ed558ccdull;
	value ^= value >> 33u;
	return value;
}

void Record(void* block, size_t size) {
	if (g_full.load(std::memory_order_relaxed)) return;
	void*      frames[Frames] {};
	const auto captured = RtlCaptureStackBackTrace(2, Frames, frames, nullptr);
	uint64_t   hash     = size;
	for (USHORT i = 0; i < captured; i++) hash = Mix(hash ^ reinterpret_cast<uint64_t>(frames[i]));
	if (hash <= Tombstone) hash += 2;
	Guard    guard;
	uint32_t stack = 0;
	for (size_t i = hash % StackSlots, probes = 0; probes < StackSlots; i = (i + 1) % StackSlots, probes++) {
		if (g_stacks[i].hash == hash) {
			stack = static_cast<uint32_t>(i);
			break;
		}
		if (g_stacks[i].hash == Empty) {
			g_stacks[i].hash = hash;
			std::memcpy(g_stacks[i].frames, frames, sizeof(frames));
			stack = static_cast<uint32_t>(i);
			break;
		}
	}
	g_stacks[stack].count++;
	// Half the slots used (tombstones of freed blocks mostly): the live blocks go in again from scratch. (A free racing
	// with it may miss its block's record and leave it listed.)
	if ((g_used + 1) * 2 > BlockSlots) {
		size_t live = 0;
		for (size_t i = 0; i < BlockSlots; i++)
			if (g_blocks[i].address > Tombstone) g_scratch[live++] = g_blocks[i];
		std::memset(g_blocks, 0, BlockSlots * sizeof(BlockSlot));
		for (size_t k = 0; k < live; k++) {
			size_t i = Mix(g_scratch[k].address) % BlockSlots;
			while (g_blocks[i].address != Empty) i = (i + 1) % BlockSlots;
			g_blocks[i] = g_scratch[k];
		}
		g_used = g_live = live;
		// Half the slots live blocks: no more records (each would go in from scratch again; a trace from start-up of
		// 32-176 bytes livelocked so).
		if ((live + 1) * 2 > BlockSlots) {
			g_full = true;
			return;
		}
	}
	const uint64_t address = reinterpret_cast<uint64_t>(block);
	for (size_t i = Mix(address) % BlockSlots, probes = 0; probes < BlockSlots; i = (i + 1) % BlockSlots, probes++) {
		if (g_blocks[i].address <= Tombstone) {
			if (g_blocks[i].address == Empty) g_used++;
			g_live++;
			g_blocks[i] = {address, stack, static_cast<uint32_t>(size)};
			return;
		}
	}
	g_full = true;
}

void Forget(void* block) {
	const uint64_t address = reinterpret_cast<uint64_t>(block);
	// Most blocks freed were never traced: an empty first slot says so without the lock (slots never become empty
	// again, and a traced block's record was written before the block could be freed).
	if (std::atomic_ref<uint64_t>(g_blocks[Mix(address) % BlockSlots].address).load(std::memory_order_relaxed) == Empty)
		return;
	Guard guard;
	for (size_t i = Mix(address) % BlockSlots, probes = 0; probes < BlockSlots; i = (i + 1) % BlockSlots, probes++) {
		if (g_blocks[i].address == address) {
			g_stacks[g_blocks[i].stack].count--;
			g_blocks[i].address = Tombstone;
			g_live--;
			return;
		}
		if (g_blocks[i].address == Empty) return;
	}
}

// Sizes of MaxSize and more: a few ranges beside the flags (set before the hooks go in).
constexpr uint32_t                  LargeRanges = 16;
std::pair<uint64_t, uint64_t>       g_large[LargeRanges] {};
uint32_t                            g_large_count = 0;

bool Traced(size_t size) {
	if (size < MaxSize) return g_sizes[size] != 0;
	for (uint32_t i = 0; i < g_large_count; i++)
		if (size >= g_large[i].first && size <= g_large[i].second) return true;
	return false;
}

void* NTAPI HookAllocate(void* heap, ULONG flags, SIZE_T size) {
	void* block = g_allocate(heap, flags, size);
	if (block != nullptr && Traced(size)) Record(block, size);
	return block;
}

BOOLEAN NTAPI HookFree(void* heap, ULONG flags, void* block) {
	if (block != nullptr) Forget(block);
	return g_free(heap, flags, block);
}

void* NTAPI HookReallocate(void* heap, ULONG flags, void* block, SIZE_T size) {
	void* result = g_reallocate(heap, flags, block, size);
	if (result != nullptr) {
		if (block != nullptr) Forget(block);
		if (Traced(size)) Record(result, size);
	}
	return result;
}

// Every loaded module's import slots that hold `from` now hold `to`.
int Patch(void* from, void* to) {
	std::vector<HMODULE> handles(2048);
	DWORD                needed = 0;
	if (EnumProcessModules(GetCurrentProcess(), handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)),
	                       &needed) == 0)
		return 0;
	handles.resize(std::min<size_t>(handles.size(), needed / sizeof(HMODULE)));
	int patched = 0;
	for (HMODULE handle: handles) {
		auto*       base = reinterpret_cast<uint8_t*>(handle);
		const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) continue;
		const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
		if (dir.VirtualAddress == 0 || dir.Size == 0) continue;
		auto* slots = reinterpret_cast<void**>(base + dir.VirtualAddress);
		for (size_t i = 0; i < dir.Size / sizeof(void*); i++) {
			if (slots[i] != from) continue;
			DWORD old = 0;
			if (VirtualProtect(&slots[i], sizeof(void*), PAGE_READWRITE, &old) == 0) continue;
			slots[i] = to;
			VirtualProtect(&slots[i], sizeof(void*), old, &old);
			patched++;
		}
	}
	return patched;
}

} // namespace AllocTrace

bool StartAllocTrace(const char* sizes) {
	using namespace AllocTrace;
	if (g_sizes == nullptr) {
		HMODULE ntdll = GetModuleHandleA("ntdll.dll");
		g_allocate    = reinterpret_cast<Allocate>(GetProcAddress(ntdll, "RtlAllocateHeap"));
		g_free        = reinterpret_cast<Free>(GetProcAddress(ntdll, "RtlFreeHeap"));
		g_reallocate  = reinterpret_cast<Reallocate>(GetProcAddress(ntdll, "RtlReAllocateHeap"));
		if (g_allocate == nullptr || g_free == nullptr || g_reallocate == nullptr) return false;
		const auto reserve = [](size_t bytes) {
			return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		};
		g_sizes  = static_cast<uint8_t*>(reserve(MaxSize));
		g_blocks  = static_cast<BlockSlot*>(reserve(BlockSlots * sizeof(BlockSlot)));
		g_scratch = static_cast<BlockSlot*>(reserve(BlockSlots * sizeof(BlockSlot)));
		g_stacks = static_cast<StackSlot*>(reserve(StackSlots * sizeof(StackSlot)));
		if (g_sizes == nullptr || g_blocks == nullptr || g_scratch == nullptr || g_stacks == nullptr) return false;
	}
	for (const char* text = sizes; *text != '\0';) {
		char*      end  = nullptr;
		const auto size = std::strtoull(text, &end, 10);
		if (end == text) break;
		auto last = size; // "first-last": a range
		if (*end == '-') {
			text = end + 1;
			last = std::strtoull(text, &end, 10);
		}
		for (auto at = size; at <= last && at < MaxSize; at++) g_sizes[at] = 1;
		if (last >= MaxSize && g_large_count < LargeRanges) g_large[g_large_count++] = {std::max<uint64_t>(size, MaxSize), last};
		text = *end == ',' || *end == ';' ? end + 1 : end; // (';' too: a list in a comma-separated list of settings)
	}
	// Frees first: a block traced from here on is forgotten whichever module frees it.
	const int patched = Patch(reinterpret_cast<void*>(g_free), reinterpret_cast<void*>(&HookFree)) +
	                    Patch(reinterpret_cast<void*>(g_reallocate), reinterpret_cast<void*>(&HookReallocate)) +
	                    Patch(reinterpret_cast<void*>(g_allocate), reinterpret_cast<void*>(&HookAllocate));
	std::printf("Alloc trace: %d import slots hooked\n", patched);
	std::fflush(stdout);
	return patched != 0;
}

bool WriteAllocTrace(const char* path) {
	using namespace AllocTrace;
	if (g_stacks == nullptr) return false;
	struct Module {
		uint64_t    begin;
		uint64_t    end;
		std::string name;
	};
	std::vector<Module>  modules;
	std::vector<HMODULE> handles(2048);
	DWORD                needed = 0;
	if (EnumProcessModules(GetCurrentProcess(), handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)),
	                       &needed) != 0) {
		handles.resize(std::min<size_t>(handles.size(), needed / sizeof(HMODULE)));
		for (HMODULE handle: handles) {
			MODULEINFO module {};
			char       name[MAX_PATH] {};
			if (GetModuleInformation(GetCurrentProcess(), handle, &module, sizeof(module)) != 0 &&
			    GetModuleBaseNameA(GetCurrentProcess(), handle, name, MAX_PATH) != 0) {
				const auto begin = reinterpret_cast<uint64_t>(module.lpBaseOfDll);
				modules.push_back({begin, begin + module.SizeOfImage, name});
			}
		}
	}
	// The live stacks with their blocks' bytes, copied under the lock (no allocation there), written after.
	struct Live {
		uint32_t stack;
		uint64_t count;
		uint64_t bytes;
		uint32_t size;
	};
	std::vector<uint64_t> bytes(StackSlots), counts(StackSlots), sizes(StackSlots);
	std::vector<void*>    frames(StackSlots * Frames);
	{
		Guard guard;
		for (size_t i = 0; i < BlockSlots; i++) {
			if (g_blocks[i].address <= Tombstone) continue;
			bytes[g_blocks[i].stack] += g_blocks[i].size;
			counts[g_blocks[i].stack]++;
			sizes[g_blocks[i].stack] = g_blocks[i].size;
		}
		for (size_t i = 0; i < StackSlots; i++)
			if (counts[i] != 0) std::memcpy(&frames[i * Frames], g_stacks[i].frames, sizeof(g_stacks[i].frames));
	}
	std::vector<Live> live;
	for (uint32_t i = 0; i < StackSlots; i++)
		if (counts[i] != 0) live.push_back({i, counts[i], bytes[i], static_cast<uint32_t>(sizes[i])});
	std::sort(live.begin(), live.end(), [](const Live& a, const Live& b) { return a.bytes > b.bytes; });
	FILE* file = std::fopen(path, "w");
	if (file == nullptr) return false;
	std::fprintf(file, "count\tbytes\tsize\tframes%s\n", g_full ? " (table full: some blocks missing)" : "");
	for (const auto& entry: live) {
		std::fprintf(file, "%llu\t%llu\t%u\t", static_cast<unsigned long long>(entry.count),
		             static_cast<unsigned long long>(entry.bytes), entry.size);
		for (int f = 0; f < Frames; f++) {
			const auto address = reinterpret_cast<uint64_t>(frames[static_cast<size_t>(entry.stack) * Frames + f]);
			if (address == 0) break;
			const Module* found = nullptr;
			for (const auto& module: modules)
				if (address >= module.begin && address < module.end) found = &module;
			if (found != nullptr) {
				std::fprintf(file, "%s%s+0x%llx", f == 0 ? "" : " ", found->name.c_str(),
				             static_cast<unsigned long long>(address - found->begin));
			} else {
				std::fprintf(file, "%s0x%llx", f == 0 ? "" : " ", static_cast<unsigned long long>(address));
			}
		}
		std::fprintf(file, "\n");
	}
	std::fclose(file);
	return true;
}

MemoryUse ProcessMemory() {
	PROCESS_MEMORY_COUNTERS_EX counters {};
	counters.cb = sizeof(counters);
	if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
	                            sizeof(counters)) == 0)
		return {};
	return {counters.PrivateUsage, counters.WorkingSetSize};
}

uint64_t PhysicalMemory() {
	if (const char* simulate = std::getenv("KYTY_SIMULATE_RAM_MB"); simulate != nullptr && *simulate != '\0')
		return std::strtoull(simulate, nullptr, 10) << 20u;
	MEMORYSTATUSEX status {};
	status.dwLength = sizeof(status);
	return GlobalMemoryStatusEx(&status) != 0 ? status.ullTotalPhys : 0;
}

uint64_t AvailablePhysicalMemory() {
	MEMORYSTATUSEX status {};
	status.dwLength = sizeof(status);
	return GlobalMemoryStatusEx(&status) != 0 ? status.ullAvailPhys : 0;
}

CommitState SystemCommit() {
	MEMORYSTATUSEX status {};
	status.dwLength = sizeof(status);
	if (GlobalMemoryStatusEx(&status) == 0) return {};
	return {status.ullAvailPageFile, status.ullTotalPageFile};
}

static IDXGIAdapter3* g_video_memory_adapter = nullptr;

bool OpenVideoMemoryAdapter(const uint8_t* luid) {
	HMODULE dxgi = LoadLibraryA("dxgi.dll");
	if (dxgi == nullptr || g_video_memory_adapter != nullptr) return g_video_memory_adapter != nullptr;
	using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
	const auto     create  = reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
	IDXGIFactory4* factory = nullptr;
	if (create == nullptr || FAILED(create(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)))) return false;
	LUID id {};
	std::memcpy(&id, luid, sizeof(id));
	if (FAILED(factory->EnumAdapterByLuid(id, __uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&g_video_memory_adapter))))
		g_video_memory_adapter = nullptr;
	factory->Release();
	return g_video_memory_adapter != nullptr;
}

bool QueryVideoMemory(uint64_t* usage, uint64_t* budget) {
	DXGI_QUERY_VIDEO_MEMORY_INFO info {};
	if (g_video_memory_adapter == nullptr ||
	    FAILED(g_video_memory_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
		return false;
	*usage  = info.CurrentUsage;
	*budget = info.Budget;
	return true;
}

bool QuerySharedGpuMemory(uint64_t* usage) {
	DXGI_QUERY_VIDEO_MEMORY_INFO info {};
	if (g_video_memory_adapter == nullptr ||
	    FAILED(g_video_memory_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info)))
		return false;
	*usage = info.CurrentUsage;
	return true;
}

static bool IsSignedBy(const std::wstring& path, const wchar_t* signer_name);

bool IsNvidiaSigned(const std::wstring& path) {
	return IsSignedBy(path, L"NVIDIA Corporation");
}

bool IsAmdSigned(const std::wstring& path) {
	return IsSignedBy(path, L"Advanced Micro Devices");
}

static bool IsSignedBy(const std::wstring& path, const wchar_t* signer_name) {
	WINTRUST_FILE_INFO file {};
	file.cbStruct      = sizeof(file);
	file.pcwszFilePath = path.c_str();
	WINTRUST_DATA data {};
	data.cbStruct            = sizeof(data);
	data.dwUIChoice          = WTD_UI_NONE;
	data.fdwRevocationChecks = WTD_REVOKE_NONE;
	data.dwUnionChoice       = WTD_CHOICE_FILE;
	data.pFile               = &file;
	data.dwStateAction       = WTD_STATEACTION_VERIFY;
	data.dwProvFlags         = WTD_CACHE_ONLY_URL_RETRIEVAL;
	GUID       action        = WINTRUST_ACTION_GENERIC_VERIFY_V2;
	const LONG status        = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
	bool       nvidia        = false;
	if (status == ERROR_SUCCESS) {
		// The signer: the leaf certificate of the first signature's chain.
		auto* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
		auto* signer   = provider != nullptr ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
		auto* cert     = signer != nullptr ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
		wchar_t name[256] {};
		if (cert != nullptr && cert->pCert != nullptr &&
		    CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256) > 1)
			nvidia = std::wstring(name) == signer_name;
	}
	data.dwStateAction = WTD_STATEACTION_CLOSE;
	(void)WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
	return nvidia;
}

// (StartHeapTrimmer: the process heap's free pages decommitted where possible; it is the CRT's
// malloc's, the static CRT's too. Windows 8.1 and later. 0, else GetLastError.)
static uint32_t TrimHeap() {
	HEAP_OPTIMIZE_RESOURCES_INFORMATION info {};
	info.Version = HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION;
	if (HeapSetInformation(GetProcessHeap(), HeapOptimizeResources, &info, sizeof(info)) != 0)
		return 0;
	return static_cast<uint32_t>(GetLastError());
}

#else

// pthread_t is an integer on Linux and a pointer on macOS: handles hold either.
static uint64_t ThreadToHandle(pthread_t thread) {
	return (uint64_t)(uintptr_t)thread;
}

static pthread_t HandleToThread(uint64_t handle) {
	return (pthread_t)(uintptr_t)handle;
}

void SetThreadName(const char* name) {
#if defined(__APPLE__)
	// macOS names only the calling thread.
	(void)pthread_setname_np(name);
#else
	(void)pthread_setname_np(pthread_self(), name);
#endif
}

uint32_t ThreadId() {
#if defined(__APPLE__)
	uint64_t id = 0;
	(void)pthread_threadid_np(nullptr, &id);
	return static_cast<uint32_t>(id);
#else
	return static_cast<uint32_t>(syscall(SYS_gettid));
#endif
}

#if defined(__linux__)
static bool MaskToSet(uint64_t mask, cpu_set_t* set) {
	CPU_ZERO(set);
	for (int cpu = 0; cpu < 64; ++cpu)
		if ((mask >> cpu) & 1u) CPU_SET(cpu, set);
	return mask != 0;
}
#endif

// (macOS has no CPU affinity: the pinning calls do nothing there.)
void PinThreadToCpuList(const char* list) {
#if defined(__linux__)
	cpu_set_t set;
	if (MaskToSet(CpuListMask(list), &set)) (void)sched_setaffinity(0, sizeof(set), &set);
#else
	(void)list;
#endif
}

std::string SpeculationCpuList() {
	return {};
}

uint32_t PerformanceLogicalCpus() {
	if (const char* simulate = std::getenv("KYTY_SIMULATE_PCORES"); simulate != nullptr && *simulate != '\0')
		return static_cast<uint32_t>(std::strtoul(simulate, nullptr, 10));
	return 0;
}

bool PinThreadHandleToCpuList(uint64_t handle, const char* list) {
#if defined(__linux__)
	cpu_set_t set;
	return handle != 0 && MaskToSet(CpuListMask(list), &set) &&
	       pthread_setaffinity_np(HandleToThread(handle), sizeof(set), &set) == 0;
#else
	(void)handle;
	(void)list;
	return false;
#endif
}

bool SetProcessDefaultCpuList(const char* /*list*/) {
	return false;
}

static double ClockSeconds(clockid_t clock) {
	timespec now {};
	if (clock_gettime(clock, &now) != 0) return 0;
	return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
}

uint64_t CurrentThreadHandle() {
	return ThreadToHandle(pthread_self());
}

double ThreadCpuSeconds(uint64_t handle) {
#if defined(__APPLE__)
	// macOS has no CPU clock of another thread: only the calling thread's is known.
	return handle == CurrentThreadHandle() ? ClockSeconds(CLOCK_THREAD_CPUTIME_ID) : 0;
#else
	clockid_t clock {};
	if (pthread_getcpuclockid(HandleToThread(handle), &clock) != 0) return 0;
	return ClockSeconds(clock);
#endif
}

bool ProcessCpusIdleTime(uint64_t* idle_100ns, uint64_t* cpus) {
#if defined(__linux__)
	cpu_set_t set;
	CPU_ZERO(&set);
	if (sched_getaffinity(getpid(), sizeof(set), &set) != 0) return false;
	FILE* stat = std::fopen("/proc/stat", "r");
	if (stat == nullptr) return false;
	static const long ticks = sysconf(_SC_CLK_TCK);
	uint64_t          idle = 0, mask = 0;
	char              line[512];
	while (std::fgets(line, sizeof(line), stat) != nullptr) {
		// "cpuN user nice system idle iowait ...": the processors' lines (not the "cpu " total).
		unsigned           cpu = 0;
		unsigned long long user = 0, nice = 0, system = 0, idle_ticks = 0, iowait = 0;
		if (std::strncmp(line, "cpu", 3) != 0 || line[3] < '0' || line[3] > '9' ||
		    std::sscanf(line, "cpu%u %llu %llu %llu %llu %llu", &cpu, &user, &nice, &system, &idle_ticks, &iowait) != 6 ||
		    cpu >= 64 || !CPU_ISSET(cpu, &set))
			continue;
		idle += (idle_ticks + iowait) * 10'000'000ull / static_cast<uint64_t>(ticks > 0 ? ticks : 100);
		mask |= uint64_t {1} << cpu;
	}
	std::fclose(stat);
	*idle_100ns = idle;
	*cpus       = mask;
	return mask != 0;
#else
	(void)idle_100ns;
	(void)cpus;
	return false;
#endif
}

// The thread's CPU time in TSC ticks, as Windows' thread cycle count is (the slow-frame lines' cpu=: busy wall time well
// over it is time the thread was blocked or not running). The TSC rate is measured once.
uint64_t CurrentThreadCycles() {
	static const double ticks_per_ns = [] {
		const auto     start = std::chrono::steady_clock::now();
		const uint64_t first = __rdtsc();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
		return ns > 0 ? static_cast<double>(__rdtsc() - first) / static_cast<double>(ns) : 0.0;
	}();
	timespec time {};
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) return 0;
	return static_cast<uint64_t>((static_cast<double>(time.tv_sec) * 1e9 + static_cast<double>(time.tv_nsec)) * ticks_per_ns);
}

int CurrentThreadPriority() {
	return 0;
}

uint64_t OpenKeptReadFile(const std::string& /*path*/, uint64_t* /*size*/) {
	return 0;
}

uint32_t ReadOpenFileAt(uint64_t /*file*/, uint64_t /*offset*/, void* /*buffer*/, uint32_t /*size*/) {
	return 0;
}

void DoneKeptReadFile(const std::string& /*path*/) {}

void PrefetchKeptFile(const std::string& /*path*/) {}

void FollowKeptFiles(const std::string& /*recording*/, const std::string& /*root*/) {}

std::vector<std::pair<uint32_t, std::string>> ProcessThreads() {
	return {};
}

// Windows samples another thread's stack (SuspendThread + GetThreadContext + unwind data). Linux has no such
// cross-thread sampler here: ProcessThreads() is empty, so the diagnostic's per-thread dump stays empty and these
// are never reached; they exist so the callers compile and link.
void PrepareSampling() {}

uint64_t OpenThreadForSampling(uint32_t /*thread_id*/) {
	return 0;
}

void CloseThreadForSampling(uint64_t /*handle*/) {}

bool SampleThread(uint64_t /*handle*/, uint64_t /*stack_low*/, uint64_t /*stack_high*/, uint64_t* /*out*/,
                  size_t /*words*/) {
	return false;
}

double NamedThreadsCpuSeconds(const char* name) {
	double     total = 0;
	const long ticks = sysconf(_SC_CLK_TCK);
	if (auto* dir = opendir("/proc/self/task")) {
		while (auto* entry = readdir(dir)) {
			if (entry->d_name[0] == '.') continue;
			char path[64], comm[32] {};
			std::snprintf(path, sizeof(path), "/proc/self/task/%s/comm", entry->d_name);
			if (auto* f = std::fopen(path, "re")) {
				if (std::fgets(comm, sizeof(comm), f)) comm[std::strcspn(comm, "\n")] = 0;
				std::fclose(f);
			}
			if (std::strcmp(comm, name) != 0) continue;
			std::snprintf(path, sizeof(path), "/proc/self/task/%s/stat", entry->d_name);
			if (auto* f = std::fopen(path, "re")) {
				char line[1024] {};
				if (std::fgets(line, sizeof(line), f)) {
					const char*        rest  = std::strrchr(line, ')');
					unsigned long long utime = 0, stime = 0;
					if (rest && std::sscanf(rest + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
					                        &utime, &stime) == 2)
						total += static_cast<double>(utime + stime) / static_cast<double>(ticks);
				}
				std::fclose(f);
			}
		}
		closedir(dir);
	}
	return total;
}

bool FlushProcessWriteBuffers() {
#if defined(__linux__)
	// membarrier's expedited private command: what FlushProcessWriteBuffers does on Windows, so RenderMutex's owner
	// takes its fast path here too (KYTY_LINUX_FAST_LOCKS=0: false, the mutex always).
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_LINUX_FAST_LOCKS");
		return (value == nullptr || value[0] != '0') &&
		       syscall(SYS_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0) == 0;
	}();
	return enabled && syscall(SYS_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0) == 0;
#else
	return false;
#endif
}

bool CurrentThreadStack(uint64_t* low, uint64_t* high) {
#if defined(__APPLE__)
	// The stack address macOS gives is its top.
	const pthread_t self = pthread_self();
	*high                = reinterpret_cast<uint64_t>(pthread_get_stackaddr_np(self));
	*low                 = *high - pthread_get_stacksize_np(self);
	return *high != 0;
#else
	pthread_attr_t attr;
	bool           found = false;
	if (pthread_getattr_np(pthread_self(), &attr) == 0) {
		void*  stack = nullptr;
		size_t size  = 0;
		if (pthread_attr_getstack(&attr, &stack, &size) == 0) {
			*low  = reinterpret_cast<uint64_t>(stack);
			*high = *low + size;
			found = true;
		}
		pthread_attr_destroy(&attr);
	}
	return found;
#endif
}

// KeepRenderCoreAlone: the render thread's core (bit n: CPU n), 0 when it shares its core.
static std::atomic<uint64_t> g_render_core {0};

void AvoidCpuList(const char* avoid_cpus) {
#if !defined(__linux__)
	(void)avoid_cpus;
#else
	cpu_set_t  set;
	const auto core = g_render_core.load(std::memory_order_relaxed);
	// With the render thread's core kept for it, that core is what other threads avoid, not the list (KYTY_RENDER_CPUS,
	// e.g. "1-7": what the render thread may run on, which left a helper born on its CPU only CPU 0).
	if (const auto avoid = core != 0 ? core : CpuListMask(avoid_cpus);
	    avoid != 0 && sched_getaffinity(0, sizeof(set), &set) == 0) {
		for (int cpu = 0; cpu < 64; ++cpu)
			if ((avoid >> cpu) & 1u) CPU_CLR(cpu, &set);
		// (A thread the render thread made inherits its one CPU: the process's other CPUs instead.)
		if (CPU_COUNT(&set) == 0 && core != 0 && sched_getaffinity(getpid(), sizeof(set), &set) == 0)
			for (int cpu = 0; cpu < 64; ++cpu)
				if ((avoid >> cpu) & 1u) CPU_CLR(cpu, &set);
		if (CPU_COUNT(&set) != 0) (void)sched_setaffinity(0, sizeof(set), &set);
	}
#endif
}

int KeepRenderCoreAlone() {
#if defined(__linux__)
	const char* value = std::getenv("KYTY_RENDER_CORE");
#if defined(KYTY_STEAM_DECK)
	if (value == nullptr || *value == '\0') value = "2";
#endif
	if (value == nullptr || *value == '\0' || std::strcmp(value, "off") == 0) return -1;
	char*      end = nullptr;
	const auto cpu = static_cast<int>(std::strtol(value, &end, 10));
	if (*end != '\0' || cpu <= 0 || cpu >= 64) return -1;
	// The core: the CPU's SMT siblings ("2-3", "2,3"); the CPU alone where the file is missing.
	uint64_t core = uint64_t {1} << cpu;
	char     path[96];
	std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
	if (FILE* file = std::fopen(path, "r"); file != nullptr) {
		char list[64] = {};
		if (std::fgets(list, sizeof(list), file) != nullptr) {
			list[std::strcspn(list, "\n")] = '\0';
			char* save = nullptr;
			for (char* range = strtok_r(list, ",", &save); range != nullptr; range = strtok_r(nullptr, ",", &save)) {
				int first = 0, last = 0;
				const int n = std::sscanf(range, "%d-%d", &first, &last);
				for (int c = first; n >= 1 && c <= (n == 2 ? last : first) && c < 64; ++c) core |= uint64_t {1} << c;
			}
		}
		std::fclose(file);
	}
	// The process may run on more CPUs than the core (else nothing is kept off it).
	cpu_set_t process;
	if (sched_getaffinity(getpid(), sizeof(process), &process) != 0 || !CPU_ISSET(cpu, &process)) return -1;
	uint64_t allowed = 0;
	for (int c = 0; c < 64; ++c)
		if (CPU_ISSET(c, &process)) allowed |= uint64_t {1} << c;
	if ((allowed & ~core) == 0) return -1;
	cpu_set_t own;
	MaskToSet(uint64_t {1} << cpu, &own);
	if (sched_setaffinity(0, sizeof(own), &own) != 0) return -1;
	g_render_core.store(core, std::memory_order_relaxed);
	const auto render = static_cast<pid_t>(ThreadId());
	std::thread([render, core, allowed] {
		SetThreadName("Kyty.CoreKeeper");
		(void)setpriority(PRIO_PROCESS, static_cast<id_t>(ThreadId()), 10);
		cpu_set_t others;
		MaskToSet(allowed & ~core, &others);
		(void)sched_setaffinity(0, sizeof(others), &others);
		for (;;) {
			if (DIR* tasks = opendir("/proc/self/task"); tasks != nullptr) {
				while (const dirent* entry = readdir(tasks)) {
					const auto tid = static_cast<pid_t>(std::strtol(entry->d_name, nullptr, 10));
					cpu_set_t  set;
					if (tid <= 0 || tid == render || sched_getaffinity(tid, sizeof(set), &set) != 0) continue;
					uint64_t mask = 0;
					for (int c = 0; c < 64; ++c)
						if (CPU_ISSET(c, &set)) mask |= uint64_t {1} << c;
					if ((mask & core) == 0) continue;
					MaskToSet((mask & ~core) != 0 ? mask & ~core : allowed & ~core, &set);
					(void)sched_setaffinity(tid, sizeof(set), &set);
				}
				closedir(tasks);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
		}
	}).detach();
	return cpu;
#else
	return -1;
#endif
}

void MakeBackgroundThread(const char* avoid_cpus) {
	(void)setpriority(PRIO_PROCESS, static_cast<id_t>(ThreadId()), 10);
	AvoidCpuList(avoid_cpus);
}

void MakeCriticalThread() {
#if defined(__linux__)
	// nice -10 where the nice limit allows it (RLIMIT_NICE), else unchanged.
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_LINUX_FAST_LOCKS");
		return value == nullptr || value[0] != '0';
	}();
	if (enabled) (void)setpriority(PRIO_PROCESS, static_cast<id_t>(ThreadId()), -10);
#endif
}

// (Linux: back to a nice value below 0 is allowed again, RLIMIT_NICE having allowed it the first time.)
NormalPriority::NormalPriority() {
	errno           = 0;
	const int value = getpriority(PRIO_PROCESS, static_cast<id_t>(ThreadId()));
	m_saved         = value;
	m_changed = errno == 0 && value < 0 && setpriority(PRIO_PROCESS, static_cast<id_t>(ThreadId()), 0) == 0;
}

NormalPriority::~NormalPriority() {
	if (m_changed) (void)setpriority(PRIO_PROCESS, static_cast<id_t>(ThreadId()), m_saved);
}

int DisplayRefreshHz(void* /*window*/) {
	return 0;
}

void ReturnFocus(void* /*window*/) {}

bool StartDisplayVblanks(void* /*window*/) {
	return false;
}

bool WaitDisplayVblank(uint64_t& /*seen*/, uint32_t /*timeout_us*/) {
	return false;
}

// Handles are the file descriptor + 1 (0: none).
uint64_t OpenScratchFile() {
	const char* directory = std::getenv("TMPDIR");
	std::string path      = std::string(directory != nullptr && *directory != '\0' ? directory : "/tmp") + "/kyty-XXXXXX";
	const int   fd        = mkstemp(path.data());
	if (fd < 0) return 0;
	(void)unlink(path.c_str());
	return static_cast<uint64_t>(fd) + 1;
}

bool WriteScratchFile(uint64_t file, uint64_t offset, const void* data, size_t size) {
	for (const auto* bytes = static_cast<const uint8_t*>(data); file != 0 && size != 0;) {
		const auto done = pwrite(static_cast<int>(file - 1), bytes, size, static_cast<off_t>(offset));
		if (done <= 0) return false;
		bytes += done;
		offset += static_cast<uint64_t>(done);
		size -= static_cast<size_t>(done);
	}
	return file != 0;
}

bool ReadScratchFile(uint64_t file, uint64_t offset, void* data, size_t size) {
	for (auto* bytes = static_cast<uint8_t*>(data); file != 0 && size != 0;) {
		const auto done = pread(static_cast<int>(file - 1), bytes, size, static_cast<off_t>(offset));
		if (done <= 0) return false;
		bytes += done;
		offset += static_cast<uint64_t>(done);
		size -= static_cast<size_t>(done);
	}
	return file != 0;
}

void CloseScratchFile(uint64_t file) {
	if (file != 0) close(static_cast<int>(file - 1));
}

uint64_t OpenFileForReading(const char* path) {
	const int fd = open(path, O_RDONLY | O_CLOEXEC);
	return fd < 0 ? 0 : static_cast<uint64_t>(fd) + 1;
}

uint64_t OpenDataFile(const char* path) {
	const int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0) return 0;
	// (Locked: another process opening it fails.)
	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		close(fd);
		return 0;
	}
	return static_cast<uint64_t>(fd) + 1;
}

uint64_t ScratchFileSize(uint64_t file) {
	struct stat status {};
	return file != 0 && fstat(static_cast<int>(file - 1), &status) == 0 ? static_cast<uint64_t>(status.st_size) : 0;
}

bool ResizeScratchFile(uint64_t file, uint64_t size) {
	return file != 0 && ftruncate(static_cast<int>(file - 1), static_cast<off_t>(size)) == 0;
}

bool FlushScratchFile(uint64_t file) {
	return file != 0 && fsync(static_cast<int>(file - 1)) == 0;
}

std::string ExecutablePath() {
	std::error_code error;
	const auto      path = std::filesystem::read_symlink("/proc/self/exe", error);
	return error ? std::string() : path.string();
}

std::string FileSystemName(const std::string& path) {
#if defined(__linux__)
	struct statfs info {};
	if (statfs(path.c_str(), &info) != 0) return {};
	return info.f_type == 0x4d44 ? "FAT32" : ""; // (MSDOS_SUPER_MAGIC: a vfat mount)
#else
	(void)path;
	return {};
#endif
}

bool WriteHeapHistogram(const char* path) {
#if defined(__GLIBC__)
	// glibc's account of its heap: the totals of all arenas (mallinfo2), then malloc_info's XML
	// (each arena's free chunks by size, the mmapped blocks).
	FILE* file = std::fopen(path, "w");
	if (file == nullptr) return false;
	const auto info = mallinfo2();
	std::fprintf(file,
	             "arenas %zu bytes from the system, %zu in use, %zu free (%zu at the tops, %zu in "
	             "fastbins)\n"
	             "mmapped blocks %zu bytes (%zu)\n\n",
	             info.arena, info.uordblks, info.fordblks, info.keepcost, info.fsmblks, info.hblkhd,
	             info.hblks);
	const bool ok = malloc_info(0, file) == 0;
	return std::fclose(file) == 0 && ok;
#else
	(void)path;
	return false;
#endif
}

bool StartAllocTrace(const char*) {
	return false;
}

bool WriteAllocTrace(const char*) {
	return false;
}

MemoryUse ProcessMemory() {
	// statm: total, resident, shared pages; private resident = resident - shared.
	MemoryUse use;
	if (FILE* file = std::fopen("/proc/self/statm", "r"); file != nullptr) {
		unsigned long long total = 0, resident = 0, shared = 0;
		if (std::fscanf(file, "%llu %llu %llu", &total, &resident, &shared) == 3) {
			const auto page   = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
			use.working_set   = resident * page;
			use.private_bytes = (resident - std::min(resident, shared)) * page;
		}
		std::fclose(file);
	}
	return use;
}

uint64_t PhysicalMemory() {
	if (const char* simulate = std::getenv("KYTY_SIMULATE_RAM_MB"); simulate != nullptr && *simulate != '\0')
		return std::strtoull(simulate, nullptr, 10) << 20u;
	const long pages = sysconf(_SC_PHYS_PAGES);
	const long size  = sysconf(_SC_PAGESIZE);
	return pages > 0 && size > 0 ? static_cast<uint64_t>(pages) * static_cast<uint64_t>(size) : 0;
}

uint64_t AvailablePhysicalMemory() {
	uint64_t available = 0;
	if (FILE* file = std::fopen("/proc/meminfo", "r")) {
		char line[256];
		while (std::fgets(line, sizeof(line), file) != nullptr) {
			unsigned long long kib = 0;
			if (std::sscanf(line, "MemAvailable: %llu kB", &kib) == 1) {
				available = static_cast<uint64_t>(kib) << 10u;
				break;
			}
		}
		std::fclose(file);
	}
	return available;
}

CommitState SystemCommit() {
	uint64_t limit = 0, committed = 0;
	if (FILE* file = std::fopen("/proc/meminfo", "r")) {
		char line[256];
		while (std::fgets(line, sizeof(line), file) != nullptr) {
			unsigned long long kib = 0;
			if (std::sscanf(line, "CommitLimit: %llu kB", &kib) == 1) limit = static_cast<uint64_t>(kib) << 10u;
			if (std::sscanf(line, "Committed_AS: %llu kB", &kib) == 1) committed = static_cast<uint64_t>(kib) << 10u;
		}
		std::fclose(file);
	}
	return {limit - std::min(limit, committed), limit};
}

// (StartHeapTrimmer: glibc's free pages inside its arenas given back, not only the tops'. 0, else
// 1: no malloc_trim.)
static uint32_t TrimHeap() {
#if defined(__GLIBC__)
	malloc_trim(0);
	return 0;
#else
	return 1;
#endif
}

bool OpenVideoMemoryAdapter(const uint8_t* /*luid*/) {
	return false;
}

bool QueryVideoMemory(uint64_t* /*usage*/, uint64_t* /*budget*/) {
	return false;
}

bool QuerySharedGpuMemory(uint64_t* /*usage*/) {
	return false;
}

bool IsNvidiaSigned(const std::wstring& /*path*/) {
	return false;
}

bool IsAmdSigned(const std::wstring& /*path*/) {
	return false;
}

#endif

uint64_t EmulatorMemory() {
	static const uint64_t memory = [] {
		const uint64_t total     = PhysicalMemory();
		const uint64_t available = AvailablePhysicalMemory();
		const uint64_t own       = ProcessMemory().working_set;
		const uint64_t usable    = total != 0 && available != 0 ? std::min(total, available + own) : total;
		std::printf("PC memory: %llu MiB of the PC's %llu MiB for the emulator at start (other programs held %llu MiB); the "
		            "memory savings of PCs short of RAM %s\n",
		            static_cast<unsigned long long>(usable >> 20u), static_cast<unsigned long long>(total >> 20u),
		            static_cast<unsigned long long>((total - usable) >> 20u),
		            usable != 0 && usable <= (uint64_t {16} << 30u) ? "on (16 GiB or less)" : "off (more than 16 GiB)");
		std::fflush(stdout);
		return usable;
	}();
	return memory;
}

void StartHeapTrimmer() {
	const char* value = std::getenv("KYTY_HEAP_TRIM");
	if (value == nullptr || value[0] != '1') return;
	std::thread([] {
		SetThreadName("Kyty.HeapTrim");
		// The private memory after the last trim, or less since: the next trim waits for 64 MiB of
		// growth over it (a steady heap is left alone). Private memory: Linux resident minus
		// shared, Windows the commit charge; the working set shows the RAM.
		uint64_t low = 0;
		for (uint32_t round = 0;;) {
			std::this_thread::sleep_for(std::chrono::seconds(30));
			const auto before = ProcessMemory();
			low               = std::min(low, before.private_bytes);
			if (round != 0 && before.private_bytes < low + (uint64_t {64} << 20u)) continue;
			const auto start = std::chrono::steady_clock::now();
			if (const auto error = TrimHeap(); error != 0) {
				std::printf("Heap trim: failed (error %u), stopped\n", error);
				std::fflush(stdout);
				return;
			}
			const double ms =
			    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
			        .count();
			const auto after = ProcessMemory();
			low              = after.private_bytes;
			if (round++ < 10) {
				std::printf(
				    "Heap trim: private memory %llu -> %llu MiB, working set %llu -> %llu MiB "
				    "(%.1f ms)\n",
				    static_cast<unsigned long long>(before.private_bytes >> 20u),
				    static_cast<unsigned long long>(after.private_bytes >> 20u),
				    static_cast<unsigned long long>(before.working_set >> 20u),
				    static_cast<unsigned long long>(after.working_set >> 20u), ms);
				std::fflush(stdout);
			}
		}
	}).detach();
}

} // namespace LocalPlatform
