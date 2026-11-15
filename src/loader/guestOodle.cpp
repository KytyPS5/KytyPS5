#include "loader/guestOodle.h"

#include "common/abi.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/platform/sysDbg.h"
#include "common/stringUtils.h"
#include "loader/elf.h"
#include "loader/guestCode.h"
#include "loader/guestInstructionPatcher.h"
#include "loader/runtimeLinker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fmt/format.h>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace Loader::GuestOodle {
namespace {

// OodleLZ_Decompress(comp, comp_size, raw, raw_size, fuzz_safe, check_crc, verbosity,
// dictionary_base, dictionary_size, callback, callback_data, decoder_memory,
// decoder_memory_size, thread_phase): the bytes decoded, raw_size when the whole stream decoded.
using DecompressFunc = int64_t(KYTY_SYSV_ABI*)(const void*, int64_t, void*, int64_t, int32_t,
                                               int32_t, int32_t, void*, int64_t, void*, void*,
                                               void*, int64_t, int32_t);

constexpr int32_t FuzzSafe = 1, CheckCrc = 0, Quiet = 0, Unthreaded = 3;
// The decoder neither allocates nor sizes its memory when given this much (it needs less than
// 1 MB for a block of any codec).
constexpr size_t DecoderMemory = 2u << 20u;
// Guest code has no Windows stack probes and may jump over the guard page (see
// RuntimeLinker::Execute()): the calling thread commits this much of its stack first.
constexpr size_t StackBytes = 256u << 10u;

// Two messages of OodleLZ_Decompress itself, in the Oodle 2.9 builds of Unreal Engine 5 games:
// the one function that refers to both is the decoder.
constexpr std::string_view Messages[] = {
    "OodleLZ_Decompress: asked for len 0",
    "OodleLZ_Decompress : decoder memory given, but too small!",
};

std::vector<const Program*> g_programs;
std::atomic<bool>           g_ready = false;
std::once_flag              g_find_once;
DecompressFunc              g_decompress = nullptr;

struct Segment {
	uint64_t address;
	uint64_t size;
	bool     code;
};

std::vector<Segment> Segments(const Program& program) {
	std::vector<Segment> segments;
	const auto*          elf = program.elf.get();
	if (elf == nullptr || elf->GetEhdr() == nullptr || elf->GetPhdr() == nullptr) {
		return segments;
	}
	const auto* phdr = elf->GetPhdr();
	for (size_t i = 0; i < elf->GetEhdr()->e_phnum; i++) {
		const auto address = program.base_vaddr + phdr[i].p_vaddr;
		if (phdr[i].p_type == PT_LOAD && phdr[i].p_filesz != 0 &&
		    GuestCode::Readable(address, phdr[i].p_filesz)) {
			segments.push_back({address, phdr[i].p_filesz, (phdr[i].p_flags & PF_X) != 0});
		}
	}
	return segments;
}

std::span<const uint8_t> Bytes(const Segment& segment) {
	return {reinterpret_cast<const uint8_t*>(segment.address), segment.size};
}

// The start of the one C string in the program that holds `text`; 0 when none or several.
uint64_t FindString(const std::vector<Segment>& segments, std::string_view text) {
	const std::boyer_moore_horspool_searcher searcher(text.begin(), text.end());
	uint64_t                                 found = 0;
	for (const auto& segment: segments) {
		const auto bytes = Bytes(segment);
		const auto begin = reinterpret_cast<const char*>(bytes.data());
		const auto end   = begin + bytes.size();
		for (auto at = std::search(begin, end, searcher); at != end;
		     at      = std::search(at + 1, end, searcher)) {
			if (found != 0) {
				return 0;
			}
			auto start = at;
			while (start > begin && start[-1] != '\0' && at - start < 256) {
				start--;
			}
			found = segment.address + static_cast<uint64_t>(start - begin);
		}
	}
	return found;
}

// The functions (from the start of each, in `starts`) with a RIP-relative lea of `target`.
std::vector<uint64_t> FunctionsReferring(const std::vector<Segment>&   segments,
                                         const std::vector<uintptr_t>& starts, uint64_t target) {
	std::vector<uint64_t> functions;
	for (const auto& segment: segments) {
		if (!segment.code) {
			continue;
		}
		const auto bytes = Bytes(segment);
		for (size_t i = 0; i + 7 <= bytes.size(); i++) {
			// lea r64, [rip + disp32]
			if ((bytes[i] & 0xfbu) != 0x48 || bytes[i + 1] != 0x8d ||
			    (bytes[i + 2] & 0xc7u) != 0x05) {
				continue;
			}
			int32_t displacement = 0;
			std::memcpy(&displacement, bytes.data() + i + 3, sizeof(displacement));
			const auto at = segment.address + i;
			if (at + 7 + static_cast<int64_t>(displacement) != target) {
				continue;
			}
			const auto next = std::upper_bound(starts.begin(), starts.end(), at);
			if (next != starts.begin() &&
			    std::ranges::find(functions, *(next - 1)) == functions.end()) {
				functions.push_back(*(next - 1));
			}
		}
	}
	return functions;
}

DecompressFunc FindIn(const Program& program) {
	const auto  segments = Segments(program);
	const auto* elf      = program.elf.get();
	if (segments.empty()) {
		return nullptr;
	}
	std::vector<uintptr_t> starts;
	const auto*            phdr = elf->GetPhdr();
	for (size_t i = 0; i < elf->GetEhdr()->e_phnum; i++) {
		if (phdr[i].p_type == PT_GNU_EH_FRAME &&
		    !DecodeEhFrameFunctionStarts(program.base_vaddr + phdr[i].p_vaddr, phdr[i].p_memsz,
		                                 &starts)) {
			starts.clear();
		}
	}
	if (starts.empty()) {
		return nullptr;
	}
	std::ranges::sort(starts);
	uint64_t function = 0;
	for (const auto message: Messages) {
		const auto text = FindString(segments, message);
		if (text == 0) {
			return nullptr;
		}
		const auto functions = FunctionsReferring(segments, starts, text);
		if (functions.size() != 1 || (function != 0 && functions[0] != function)) {
			return nullptr;
		}
		function = functions[0];
	}
	Log::WriteToConsoleAndLog(fmt::format("Oodle decoder of the game: {} +{:#x}\n",
	                                      Common::PathToString(program.file_name.filename()),
	                                      function - program.base_vaddr));
	return reinterpret_cast<DecompressFunc>(function);
}

void CommitStack() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	thread_local bool committed = false;
	if (committed) {
		return;
	}
	committed       = true;
	size_t expanded = 0;
	while (expanded < StackBytes) {
		const auto stack                                        = SysStackUsage();
		*reinterpret_cast<volatile uint32_t*>(stack.guard_addr) = 0;
		expanded += stack.guard_size;
	}
#endif
}

} // namespace

void SetPrograms(std::span<Program* const> programs) {
	g_programs.assign(programs.begin(), programs.end());
	g_ready.store(true, std::memory_order_release);
}

bool Decompress(std::span<const uint8_t> src, std::span<uint8_t> dst, const std::stop_token& stop) {
	// A block header: 0xc in the low bits of the first byte, then the codec (Kraken 6, Mermaid
	// and Selkie 10, Leviathan 12).
	if (src.size() < 2 || dst.empty() || (src[0] & 0x0fu) != 0x0c) {
		return false;
	}
	if (const auto codec = src[1] & 0x7fu; codec != 6 && codec != 10 && codec != 12) {
		return false;
	}
	while (!g_ready.load(std::memory_order_acquire)) {
		if (stop.stop_requested()) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	std::call_once(g_find_once, [] {
		for (const auto* program: g_programs) {
			if ((g_decompress = FindIn(*program)) != nullptr) {
				return;
			}
		}
		Log::WriteToConsoleAndLog("Oodle decoder of the game: not found\n");
	});
	if (g_decompress == nullptr) {
		return false;
	}
	CommitStack();
	thread_local std::vector<uint8_t> memory(DecoderMemory);
	const auto done = g_decompress(src.data(), static_cast<int64_t>(src.size()), dst.data(),
	                               static_cast<int64_t>(dst.size()), FuzzSafe, CheckCrc, Quiet,
	                               nullptr, 0, nullptr, nullptr, memory.data(),
	                               static_cast<int64_t>(memory.size()), Unthreaded);
	return done == static_cast<int64_t>(dst.size());
}

} // namespace Loader::GuestOodle
