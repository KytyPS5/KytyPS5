#include "loader/demonsSoulsPageFree.h"

#include "common/abi.h"
#include "common/virtualMemory.h"
#include "graphics/host_gpu/hostMemory.h"
#include "loader/demonsSoulsTitle.h"
#include "kernel/memory.h"
#include "loader/guestCode.h"
#include "loader/runtimeLinker.h"
#include "local/local-platform.h"
#include "local/slow-log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#ifndef XBYAK_NO_EXCEPTION
#define XBYAK_NO_EXCEPTION
#endif
#include <xbyak/xbyak.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Loader::DemonsSoulsPageFree {
namespace {
constexpr uint64_t PageSize = 0x4000, CaveOffset = 0x8000000 + 4 * PageSize;

enum class Mode { Off, Count, Walk, GiveBack };

// KYTY_ENGINE_FREE=giveback|walk|count|0 (default: giveback with 16 GiB or less for the emulator on Windows and Linux
// (LocalPlatform::EmulatorMemory), where the RAM the engine's free blocks hold presses on the rest; off with more).
Mode GetMode() {
	static const Mode mode = [] {
		if (const char* value = std::getenv("KYTY_ENGINE_FREE"); value != nullptr && *value != '\0') {
			const std::string_view text = value;
			return text == "count"                    ? Mode::Count
			       : text == "walk"                   ? Mode::Walk
			       : text == "giveback" || text == "1" ? Mode::GiveBack
			                                           : Mode::Off;
		}
#if defined(_WIN32) || defined(__linux__)
		if (const auto ram = LocalPlatform::EmulatorMemory(); ram != 0 && ram <= (uint64_t {16} << 30u)) return Mode::GiveBack;
#endif
		return Mode::Off;
	}();
	return mode;
}

uint64_t cave = 0, site = 0;
// KYTY_ENGINE_FREE_BUFFERS (on unless 0): each block the free keeps mapped told to the GPU caches (LibKernel::Memory::
// NoteGameFree), whose buffers over it go; the free's hook is installed for that on any PC.
const bool g_drop_buffers = [] {
	const char* value = std::getenv("KYTY_ENGINE_FREE_BUFFERS");
	return value == nullptr || value[0] != '0';
}();
std::array<uint8_t, MovedPrologue> installed {}, original {};
// The allocator's lock word and the global holding its statistics object's address (from the free's operands).
uint64_t          g_lock = 0, g_stats_global = 0;
std::atomic<bool> g_stop {false};

int64_t NowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t Load(uint64_t address) {
	return __atomic_load_n(reinterpret_cast<const uint64_t*>(address), __ATOMIC_RELAXED);
}

uint32_t Load32(uint64_t address) {
	return __atomic_load_n(reinterpret_cast<const uint32_t*>(address), __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------------------------------------------------------------
// KYTY_ENGINE_FREE=count: the free's hook
// ---------------------------------------------------------------------------------------------------------------------

// What the frees were, since the last report (any thread; reported every 30 s by the thread that finds it due).
struct Stats {
	std::atomic<uint64_t>                kept {0}, kept_bytes {0}, unmapped {0}, unmapped_bytes {0};
	std::atomic<uint64_t>                writable_bytes {0}, read_only_bytes {0}, no_access_bytes {0};
	std::atomic<uint64_t>                hook_ns {0}, hook_max_ns {0};
	std::array<std::atomic<uint64_t>, 8> sizes {}; // < 128 KiB, < 512 KiB, < 2 MiB, < 8 MiB, < 32 MiB, < 128 MiB, < 512 MiB, more
	std::atomic<int64_t>                 next_report {0};
};
Stats g_stats;

size_t SizeClass(uint64_t size) {
	size_t index = 0;
	for (uint64_t limit = uint64_t {128} << 10u; index + 1 < g_stats.sizes.size() && size >= limit; limit <<= 2u)
		index++;
	return index;
}

void Report() {
	const int64_t now  = NowNs();
	int64_t       next = g_stats.next_report.load(std::memory_order_relaxed);
	if (next == 0) {
		g_stats.next_report.compare_exchange_strong(next, now + 30'000'000'000);
		return;
	}
	if (now < next || !g_stats.next_report.compare_exchange_strong(next, now + 30'000'000'000)) return;
	const auto take = [](std::atomic<uint64_t>& value) { return value.exchange(0, std::memory_order_relaxed); };
	const auto mib  = [](uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };
	const auto kept = take(g_stats.kept), kept_bytes = take(g_stats.kept_bytes);
	const auto unmapped = take(g_stats.unmapped), unmapped_bytes = take(g_stats.unmapped_bytes);
	const auto writable = take(g_stats.writable_bytes), read_only = take(g_stats.read_only_bytes);
	const auto no_access = take(g_stats.no_access_bytes), hook_ns = take(g_stats.hook_ns);
	const auto hook_max = take(g_stats.hook_max_ns);
	std::array<uint64_t, 8> sizes {};
	for (size_t i = 0; i < sizes.size(); i++)
		sizes[i] = take(g_stats.sizes[i]);
	std::printf("Engine frees: 30 s: %llu blocks %.0f MiB left mapped by the engine (guest view read-write %.0f, read-only %.0f, "
	            "no access %.0f MiB; sizes <128K %llu <512K %llu <2M %llu <8M %llu <32M %llu <128M %llu <512M %llu more %llu), "
	            "%llu blocks %.0f MiB unmapped by it; %.1f ms in the hook (at most %.2f ms a block)\n",
	            static_cast<unsigned long long>(kept), mib(kept_bytes), mib(writable), mib(read_only), mib(no_access),
	            static_cast<unsigned long long>(sizes[0]), static_cast<unsigned long long>(sizes[1]),
	            static_cast<unsigned long long>(sizes[2]), static_cast<unsigned long long>(sizes[3]),
	            static_cast<unsigned long long>(sizes[4]), static_cast<unsigned long long>(sizes[5]),
	            static_cast<unsigned long long>(sizes[6]), static_cast<unsigned long long>(sizes[7]),
	            static_cast<unsigned long long>(unmapped), mib(unmapped_bytes), static_cast<double>(hook_ns) / 1e6,
	            static_cast<double>(hook_max) / 1e6);
	std::fflush(stdout);
}

#if defined(_WIN32)
// The guest view's protection runs over [address, address + size): (begin, end, writable, no access) each, to `visit`.
template <typename Visit>
void ForProtections(uint64_t address, uint64_t size, Visit&& visit) {
	for (uint64_t at = address; at < address + size;) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) == 0) return;
		const uint64_t end     = std::min(address + size, reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize);
		const DWORD    protect = info.Protect & 0xffu;
		const bool     guard   = (info.Protect & PAGE_GUARD) != 0;
		visit(at, end, !guard && (protect == PAGE_READWRITE || protect == PAGE_EXECUTE_READWRITE),
		      guard || protect == PAGE_NOACCESS);
		at = end;
	}
}
#endif

// The engine's free of the block descriptor `block` begins (on the freeing thread, before the allocator's lock).
void KYTY_SYSV_ABI OnFree(uint64_t block) {
	const int64_t  begin   = NowNs();
	const uint64_t size    = Load(block + SizeAt);
	const uint64_t mapped  = Load(block + MappedAt);
	const uint64_t address = Load(block + AddressAt);
	// (A block partly mapped: the engine unmaps it, and the unmap lets the buffers there go.)
	if (g_drop_buffers && size != 0 && mapped == size) Libs::LibKernel::Memory::NoteGameFree(address, size);
	if (GetMode() != Mode::Count) return;
	if (size == 0 || mapped != size) {
		// (Partly mapped: the engine unmaps it.)
		g_stats.unmapped.fetch_add(1, std::memory_order_relaxed);
		g_stats.unmapped_bytes.fetch_add(size, std::memory_order_relaxed);
		Report();
		return;
	}
	g_stats.kept.fetch_add(1, std::memory_order_relaxed);
	g_stats.kept_bytes.fetch_add(size, std::memory_order_relaxed);
	g_stats.sizes[SizeClass(size)].fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
	ForProtections(address, size, [](uint64_t at, uint64_t end, bool writable, bool no_access) {
		(writable ? g_stats.writable_bytes : no_access ? g_stats.no_access_bytes : g_stats.read_only_bytes)
		    .fetch_add(end - at, std::memory_order_relaxed);
	});
#endif
	const auto took = static_cast<uint64_t>(NowNs() - begin);
	g_stats.hook_ns.fetch_add(took, std::memory_order_relaxed);
	for (uint64_t max = g_stats.hook_max_ns.load(std::memory_order_relaxed);
	     took > max && !g_stats.hook_max_ns.compare_exchange_weak(max, took, std::memory_order_relaxed);) {}
	Report();
}

#if defined(__x86_64__) || defined(_M_X64)
// The thunk the free's first instructions jump to: the free's registers and flags kept around OnFree (only rdi, the
// block, is an argument: the vector registers are the caller's to lose at a call), then the moved prologue and back.
void EmitThunk(Xbyak::CodeGenerator& c, uint64_t resume) {
	using namespace Xbyak::util;
	c.pushfq();
	for (const auto& reg: {rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11})
		c.push(reg);
	// Entry RSP is 8 mod 16 (the call's return address); the 80 bytes saved keep that, 8 more make it 0 for the call.
	c.sub(rsp, 8);
	c.mov(rax, reinterpret_cast<uint64_t>(&OnFree));
	c.call(rax);
	c.add(rsp, 8);
	for (const auto& reg: {r11, r10, r9, r8, rdi, rsi, rdx, rcx, rax})
		c.pop(reg);
	c.popfq();
	c.push(rbp);
	c.mov(rbp, rsp);
	c.push(r15);
	c.jmp(reinterpret_cast<const void*>(resume), Xbyak::CodeGenerator::T_NEAR);
}
#endif

// ---------------------------------------------------------------------------------------------------------------------
// KYTY_ENGINE_FREE=walk, =giveback: the free lists walked under the allocator's lock
// ---------------------------------------------------------------------------------------------------------------------

// The allocator's lock as the engine takes it (0 free, all ones held; the engine's waiters spin, then sleep): false
// when the engine held it for `patience_ms` on end.
bool LockAllocator(int patience_ms) {
	const int64_t until = NowNs() + int64_t {patience_ms} * 1'000'000;
	auto*         word  = reinterpret_cast<uint64_t*>(g_lock);
	for (uint32_t spins = 0;; spins++) {
		uint64_t expected = 0;
		if (__atomic_load_n(word, __ATOMIC_RELAXED) == 0 &&
		    __atomic_compare_exchange_n(word, &expected, ~uint64_t {0}, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			return true;
		_mm_pause();
		if ((spins & 1023u) == 1023u) {
			if (NowNs() > until || g_stop.load(std::memory_order_relaxed)) return false;
			std::this_thread::yield();
		}
	}
}

void UnlockAllocator() {
	__atomic_store_n(reinterpret_cast<uint64_t*>(g_lock), 0, __ATOMIC_RELEASE);
}

// A pointer into guest memory as the allocator's structures hold them (8-byte aligned, inside the guest's 1 TiB);
// `checked` asks the address space too (the first walks: the layout read from the code is confirmed).
bool GuestPointer(uint64_t pointer, bool checked) {
	if (pointer < 0x100000000ull || pointer >= (uint64_t {1} << 40u) || (pointer & 7u) != 0) return false;
	return !checked || Libs::Graphics::HostMemoryRangeIsReadable(pointer, 0x40);
}

struct FreeRun {
	uint64_t descriptor = 0, address = 0, size = 0, head = 0;
};

struct WalkTotals {
	std::array<uint64_t, Pools> blocks {}, pages {}, counted_blocks {}, counted_pages {};
};

// (The allocator's lock held) its pools' free blocks whose pages are all mapped, to `runs`; false when a pointer or a link
// is not as the layout read from the code has it (nothing is given back from then on).
bool WalkFreeLocked(std::vector<FreeRun>& runs, WalkTotals& totals, bool checked) {
	const uint64_t stats = Load(g_stats_global);
	if (!GuestPointer(stats, checked)) return false;
	for (uint64_t pool = 0; pool < Pools; pool++) {
		const uint64_t record = stats + pool * PoolStride, head = record + FreeHeadAt;
		totals.counted_blocks[pool] = Load32(record + FreeCountAt);
		totals.counted_pages[pool]  = Load32(record + FreePagesAt);
		uint64_t node               = Load(record + FreeFirstAt);
		for (uint64_t steps = 0; node != head; steps++) {
			if (steps > 1'000'000 || !GuestPointer(node, checked)) return false;
			const uint64_t descriptor = Load(node);
			if (!GuestPointer(descriptor, checked) || descriptor + NodeAt != node || Load(descriptor + ListAt) != head) return false;
			const uint64_t size = Load(descriptor + SizeAt), mapped = Load(descriptor + MappedAt);
			totals.blocks[pool]++;
			totals.pages[pool] += mapped >> 16u;
			if (mapped == size && size != 0) runs.push_back({descriptor, Load(descriptor + AddressAt), size, head});
			node = Load(descriptor + NextAt);
		}
	}
	return true;
}

#if defined(_WIN32)
// (The allocator's lock held, the block checked free) the bytes of [address, address + size) dropped: discarded through
// the guest view where it is all writable, else through the backing view (zeros in every view either way). A page leaves
// RAM when no view keeps it: true when the guest view still holds some (the page manager watches them, read-only or no
// access), for TrimGuest after the lock.
bool DiscardLocked(uint64_t address, uint64_t size, uint64_t* given) {
	thread_local std::vector<std::pair<const uint8_t*, uint64_t>> pieces;
	pieces.clear();
	if (!Libs::LibKernel::Memory::TryGetBackingPieces(address, size, pieces) || pieces.empty()) return false;
	for (const auto& [pointer, length]: pieces)
		Libs::LibKernel::Memory::DropHotReadsBacking(reinterpret_cast<uint64_t>(pointer), length);
	// The guest view's discard drops the pages the views share (zeros through both) in ~0.07 ms a 256 KiB chunk, and the
	// backing view then only lets go of them (VirtualUnlock: out of its working set; a page no view holds is freed). The
	// backing view's own discard took ~2.8 ms a chunk (up to 15) on a 3 GiB view alone, 4-9 ms in play on the 13.5 GiB
	// one, all of it under the allocator's lock. (scratchpad discardbench: same zeros, both views agree after writes.)
	if (DiscardVirtualMemory(reinterpret_cast<void*>(address), size) == ERROR_SUCCESS) {
		for (const auto& [pointer, length]: pieces) {
			(void)VirtualUnlock(const_cast<uint8_t*>(pointer), length);
			*given += length;
		}
		return false;
	}
	// A guest view not all writable (the page manager watches it, read-only or no access) refuses the discard: the
	// backing view's, and the guest view's pages trimmed after the lock (TrimGuest).
	for (const auto& [pointer, length]: pieces)
		if (DiscardVirtualMemory(const_cast<uint8_t*>(pointer), length) == ERROR_SUCCESS) *given += length;
	return true;
}

// (The lock released) the guest view's pages of the range taken out of its working set: dropped already, they leave RAM;
// a page the engine took and wrote again meanwhile only faults back in (no byte lost: a trim keeps contents).
void TrimGuest(uint64_t address, uint64_t size) {
	(void)VirtualUnlock(reinterpret_cast<void*>(address), size);
}

// How long the process's working set lock takes now, timed outside the allocator's lock on a page of the walker's own
// (written, so it faults back in when trimmed, and discarded). Every discard waits for that lock, which Windows holds
// for long stretches while it trims the process on a PC short of memory: issue #5's 16 GB PC (~2 GB left all session)
// had its 256 KiB discards hold the allocator's lock 17 ms on average, up to 131 ms, 20-37 s in every 30 s.
uint64_t ProbeWorkingSetNs() {
	static auto* const page = static_cast<volatile uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
	if (page == nullptr) return 0;
	const int64_t start = NowNs();
	page[0]             = static_cast<uint8_t>(page[0] + 1u);
	(void)DiscardVirtualMemory(const_cast<uint8_t*>(page), 4096);
	return static_cast<uint64_t>(NowNs() - start);
}
#elif defined(__linux__)
// (The allocator's lock held, the block checked free) the bytes of [address, address + size) dropped: a hole punched in
// the backing store over each piece (DiscardBacking: the pages leave every view, and the swap where they were swapped
// out; zeros after). Not where the GPU caches' trackers watch a page: their userfaultfd protections (minor mode's dropped
// PTEs, write protection) do not fault on a page punched under them, whose next access maps a new page. And no block is
// made the host's first, as Windows does: that invalidated the caches over freed blocks, and the images and buffers that
// also held live bytes were uploaded again on the render thread. A Steam Deck's world tour (worlds 2-5, Nexus, 1-1; 15 s
// in each from 40 s after its load) had 1 and 9 frames over 50 ms in all without it, 17-25 with it (Valley 7-13), 7-8
// with the giving back off; 3.1-5.5 GB given back, the emulator's swap 2.7-3.0 GB at the end instead of 5.2-5.35 and
// 1.0-1.2 GB available instead of 0.3-0.55.
bool DiscardLocked(uint64_t address, uint64_t size, uint64_t* given) {
	thread_local std::vector<std::pair<const uint8_t*, uint64_t>> pieces;
	pieces.clear();
	if (Libs::LibKernel::Memory::GuestRangeWatched(address, size) ||
	    !Libs::LibKernel::Memory::TryGetBackingPieces(address, size, pieces))
		return false;
	for (const auto& [pointer, length]: pieces)
		if (Libs::LibKernel::Memory::DiscardBacking(reinterpret_cast<uint64_t>(pointer), length)) *given += length;
	return false;
}

void TrimGuest(uint64_t /*address*/, uint64_t /*size*/) {}

// (No working set lock: a punch waits only for faults on its own pages, and they for it.)
uint64_t ProbeWorkingSetNs() {
	return 0;
}
#endif

void WalkerThread() {
	LocalPlatform::SetThreadName("Kyty.EngineFree");
	const bool give_back = GetMode() == Mode::GiveBack;
	constexpr int64_t  PassNs = 2'000'000'000;
	// 64 KiB under the lock at a time (a guest page the page manager watches still takes the backing view's slow discard:
	// 4-23 ms a 256 KiB chunk while a world streams in), 128 MiB a walk, and only blocks free on FreeWalks + 1 walks in a
	// row (~30 s): blocks free for less were often the engine's again soon after (issue #5: 13.5 GB given back in a
	// session where at most 2-4 GB were free at once, each page given back and faulted in again several times).
	constexpr uint64_t Chunk = uint64_t {64} << 10u, PassBudget = uint64_t {128} << 20u, FreeWalks = 14;
	// A pass's discards hold the allocator's lock LockBudgetNs at most in all (2.5% of the time), a chunk at a time. Before
	// it, the working set lock is timed (ProbeWorkingSetNs); a chunk that held the lock SlowChunkNs or more ends it. A pass
	// whose probe took SlowProbeNs or more or whose chunks held the lock SlowAverageNs or more on average is a slow one:
	// two in a row pause the giving back for PauseWalks walks (30 s), twice as long each time again up to MaxPauseWalks
	// (2 min). The engine's threads wait for that lock whenever they allocate or free, and on a PC short of memory every
	// discard waits for Windows: issue #5's chunks held it 17 ms on average, 20-37 s in every 30 s. (Here, with 100 GB
	// free, a chunk takes ~0.15 ms, and 4-9 ms while a world streams in: the backing view's discards, of a section the
	// game's mappings change all the time.)
	constexpr uint64_t LockBudgetNs = 50'000'000, SlowChunkNs = 20'000'000, SlowAverageNs = 8'000'000;
	constexpr uint64_t SlowProbeNs  = 2'000'000;
	constexpr uint64_t PauseWalks = 15, MaxPauseWalks = 60;
	[[maybe_unused]] uint64_t pause = PauseWalks, resume_walk = 0, slow_passes = 0;
	uint64_t                  paused = 0, slowest_ns = 0, prepare_ns = 0, prepare_max_ns = 0; // (since the last report)
	// (address, size) of a free block -> the walk that first found it free, and whether its pages were given back.
	struct Seen {
		uint64_t first = 0;
		bool     given = false;
	};
	std::map<std::pair<uint64_t, uint64_t>, Seen> seen, now_seen;
	std::vector<FreeRun>                          runs;
	uint64_t                                      walk = 0, given_total = 0, given_report = 0, hold_ns = 0, hold_max_ns = 0;
	int64_t                                       report_at = NowNs() + 30'000'000'000;
	for (;;) {
		std::this_thread::sleep_for(std::chrono::nanoseconds(PassNs));
		if (g_stop.load(std::memory_order_relaxed)) return;
		if (Load(g_stats_global) == 0) continue; // (the engine's allocator not made yet)
		walk++;
		runs.clear();
		WalkTotals totals;
		const bool checked = walk <= 3;
		if (!LockAllocator(50)) continue;
		const int64_t locked = NowNs();
		const bool    walked = WalkFreeLocked(runs, totals, checked);
		UnlockAllocator();
		const auto walk_ns = static_cast<uint64_t>(NowNs() - locked);
		hold_max_ns        = std::max(hold_max_ns, walk_ns);
		const bool counts_match =
		    walked && totals.blocks == totals.counted_blocks && totals.pages == totals.counted_pages;
		if (!walked || (checked && !counts_match)) {
			std::printf("Engine frees: the allocator's free lists are not as read from its code (walked %d: %llu+%llu blocks "
			            "%llu+%llu pages; its counts %llu+%llu blocks %llu+%llu pages): nothing given back\n",
			            walked ? 1 : 0, static_cast<unsigned long long>(totals.blocks[0]),
			            static_cast<unsigned long long>(totals.blocks[1]), static_cast<unsigned long long>(totals.pages[0]),
			            static_cast<unsigned long long>(totals.pages[1]), static_cast<unsigned long long>(totals.counted_blocks[0]),
			            static_cast<unsigned long long>(totals.counted_blocks[1]),
			            static_cast<unsigned long long>(totals.counted_pages[0]),
			            static_cast<unsigned long long>(totals.counted_pages[1]));
			std::fflush(stdout);
			return;
		}
		// Free on the walk before too: candidates (biggest first), up to PassBudget a walk.
		now_seen.clear();
		std::vector<std::pair<FreeRun, bool*>> candidates;
		for (const auto& run: runs) {
			const auto key   = std::make_pair(run.address, run.size);
			const auto found = seen.find(key);
			auto&      entry = now_seen[key];
			entry            = found != seen.end() ? found->second : Seen {walk, false};
		}
		for (const auto& run: runs) {
			auto& entry = now_seen[std::make_pair(run.address, run.size)];
			if (give_back && entry.first + FreeWalks <= walk && !entry.given) candidates.emplace_back(run, &entry.given);
		}
		std::sort(candidates.begin(), candidates.end(),
		          [](const auto& a, const auto& b) { return a.first.size > b.first.size; });
		uint64_t budget = PassBudget;
#if defined(_WIN32) || defined(__linux__)
		uint64_t      pass_hold_ns = 0, chunks = 0;
		bool          slow = false, ended = false;
		const int64_t pass_begin = NowNs();
		const auto    given_before = given_total;
		if (!candidates.empty() && walk >= resume_walk) {
			const auto probe = ProbeWorkingSetNs();
			slowest_ns       = std::max(slowest_ns, probe);
			slow             = probe >= SlowProbeNs;
		}
		for (auto& [run, given_flag]: candidates) {
			if (walk < resume_walk || slow || ended || budget == 0 || pass_hold_ns >= LockBudgetNs ||
			    g_stop.load(std::memory_order_relaxed))
				break;
#if defined(_WIN32)
			// Its pages the page manager watches (images' and buffers', read-only in the guest view) made the host's first, as
			// before a host write of all of it that reads none (its bytes become zeros): the caches over it invalidated, the
			// GPU-written bytes dropped (until 10-28 read back first, through the render thread), the pages writable. The
			// guest view's discard then takes them, ~0.07 ms a 256 KiB chunk: the backing view's took ~0.9 ms (and more while
			// a world streams), and freed textures are most of what a trip leaves free (a 16 GB-policy trip gave back ~170 MiB
			// in 30 s under the lock budget while 1.3-2.8 GiB stayed free). Without the allocator's lock: this waits for the
			// caches' locks. (A block the engine takes again meanwhile only has its caches invalidated, as a write would.)
			// Linux skips watched pages instead (DiscardLocked).
			const int64_t prepared = NowNs();
			(void)Libs::LibKernel::Memory::TryPrepareHostOverwrite(run.address, run.size);
			const auto prepare_took = static_cast<uint64_t>(NowNs() - prepared);
			prepare_ns += prepare_took;
			prepare_max_ns = std::max(prepare_max_ns, prepare_took);
#endif
			bool whole = true;
			for (uint64_t at = 0; at < run.size; at += Chunk) {
				if (budget == 0 || pass_hold_ns >= LockBudgetNs) {
					whole = false;
					break;
				}
				// The working set lock timed again before each chunk, outside the allocator's lock: a PC paging hard had a
				// chunk hold it 0.5-1.3 s waiting for Windows (issue #174: 16 GB, 2 GB left), the game's allocating threads
				// with it, where the probe once a pass had found Windows quick.
				if (const auto probe = ProbeWorkingSetNs(); probe >= SlowProbeNs) {
					slowest_ns = std::max(slowest_ns, probe);
					whole      = false;
					ended      = true;
					slow       = true;
					break;
				}
				if (!LockAllocator(20)) {
					whole = false;
					break;
				}
				const int64_t held = NowNs();
				// Still the free block it was: in its pool's free list with the same address and size, all mapped.
				const bool free = Load(run.descriptor + ListAt) == run.head && Load(run.descriptor + AddressAt) == run.address &&
				                  Load(run.descriptor + SizeAt) == run.size && Load(run.descriptor + MappedAt) == run.size &&
				                  Load(run.descriptor + NodeAt) == run.descriptor;
				uint64_t   given = 0;
				const auto bytes = std::min(Chunk, run.size - at);
				const bool trim  = free && DiscardLocked(run.address + at, bytes, &given);
				UnlockAllocator();
				const auto took = static_cast<uint64_t>(NowNs() - held);
				hold_ns += took;
				pass_hold_ns += took;
				chunks++;
				hold_max_ns = std::max(hold_max_ns, took);
				slowest_ns  = std::max(slowest_ns, took);
				if (!free) {
					whole = false;
					break;
				}
				if (trim) TrimGuest(run.address + at, bytes);
				given_total += given;
				given_report += given;
				budget -= std::min(budget, bytes);
				if (took >= SlowChunkNs) {
					whole = at + bytes >= run.size;
					ended = true;
					break;
				}
				std::this_thread::yield();
			}
			if (whole) *given_flag = true;
		}
		// (KYTY_SLOW_LOG_MS: each pass that gave pages back, at its wall-clock time, to place it among the SLOW Frame lines.)
		if (SlowLog::Threshold() > 0.0 && chunks != 0) {
			std::printf("Engine frees pass: %.1f MiB given in %llu chunks, the allocator's lock held %.1f ms, %.1f ms in all "
			            "unix_ms=%lld\n",
			            static_cast<double>(given_total - given_before) / 1048576.0, static_cast<unsigned long long>(chunks),
			            static_cast<double>(pass_hold_ns) / 1e6, static_cast<double>(NowNs() - pass_begin) / 1e6,
			            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
			                                       std::chrono::system_clock::now().time_since_epoch())
			                                       .count()));
			std::fflush(stdout);
		}
		slow = slow || (chunks != 0 && pass_hold_ns >= SlowAverageNs * chunks);
		if (slow && ++slow_passes >= 2) {
			resume_walk = walk + pause;
			pause       = std::min(pause * 2, MaxPauseWalks);
			slow_passes = 0;
			paused++;
		} else if (!slow && chunks != 0) {
			slow_passes = 0;
			pause       = PauseWalks;
		}
#endif
		seen.swap(now_seen);
		if (const int64_t now = NowNs(); now >= report_at) {
			uint64_t free_bytes = 0;
			for (const auto& run: runs)
				free_bytes += run.size;
			std::printf("Engine frees: %llu free blocks, %.0f MiB, on the last walk (%llu+%llu pages, the allocator counts "
			            "%llu+%llu: %s); %s %.0f MiB in 30 s (%.0f MiB since start), the allocator's lock held %.1f ms (at most "
			            "%.2f ms), %.1f ms making blocks the host's (at most %.2f ms a block); paused %llu time(s) while the system's memory "
			            "manager was slow "
			            "(the slowest step %.2f ms)\n",
			            static_cast<unsigned long long>(runs.size()), static_cast<double>(free_bytes) / 1048576.0,
			            static_cast<unsigned long long>(totals.pages[0]), static_cast<unsigned long long>(totals.pages[1]),
			            static_cast<unsigned long long>(totals.counted_pages[0]),
			            static_cast<unsigned long long>(totals.counted_pages[1]), counts_match ? "the same" : "DIFFERENT",
			            give_back ? "given back" : "walked only, would give back", static_cast<double>(given_report) / 1048576.0,
			            static_cast<double>(given_total) / 1048576.0, static_cast<double>(hold_ns) / 1e6,
			            static_cast<double>(hold_max_ns) / 1e6, static_cast<double>(prepare_ns) / 1e6,
			            static_cast<double>(prepare_max_ns) / 1e6, static_cast<unsigned long long>(paused),
			            static_cast<double>(slowest_ns) / 1e6);
			std::fflush(stdout);
			given_report = hold_ns = hold_max_ns = paused = slowest_ns = prepare_ns = prepare_max_ns = 0;
			report_at                                                  = now + 30'000'000'000;
		}
	}
}
} // namespace

bool FreeBlocksIn(uint64_t begin, uint64_t end, std::vector<std::pair<uint64_t, uint64_t>>* blocks) {
	// (The first lists checked against the allocator's own counts, as the walker's first walks are; lists not as read
	// from the code stop them for good. The GPU thread waits 2 ms at most for that lock, as an engine thread holding it
	// may itself wait for the GPU thread: the buffer then stays as it is.)
	static std::atomic<uint32_t> lists {0};
	static std::atomic<bool>     wrong {false};
	if (cave == 0 || blocks == nullptr || begin >= end || wrong.load(std::memory_order_relaxed) || Load(g_stats_global) == 0)
		return false;
	thread_local std::vector<FreeRun> runs;
	runs.clear();
	WalkTotals totals;
	const bool checked = lists.load(std::memory_order_relaxed) < 3;
	if (!LockAllocator(2)) return false;
	const bool walked = WalkFreeLocked(runs, totals, checked);
	UnlockAllocator();
	lists.fetch_add(1, std::memory_order_relaxed);
	if (!walked || (checked && (totals.blocks != totals.counted_blocks || totals.pages != totals.counted_pages))) {
		if (!wrong.exchange(true, std::memory_order_relaxed)) {
			std::printf("Engine frees: the allocator's free lists are not as read from its code: buffers over several free "
			            "blocks stay (KYTY_ENGINE_FREE_BUFFERS)\n");
			std::fflush(stdout);
		}
		return false;
	}
	for (const auto& run: runs)
		if (run.address < end && run.address + run.size > begin) blocks->emplace_back(run.address, run.size);
	return true;
}

void Install(Program* program) {
#if defined(__x86_64__) || defined(_M_X64)
	if ((GetMode() == Mode::Off && !g_drop_buffers) || site != 0 || program == nullptr || !Loader::DemonsSoulsTitle::IsSupportedGame() ||
	    program->file_name.filename() != "eboot.bin" || program->mapped_size > CaveOffset ||
	    program->base_vaddr > UINT64_MAX - CaveOffset - PageSize)
		return;
	const auto found = GuestCode::FindUnique(*program, GuestCode::Pattern(FreeBlock));
	const GuestCode::Pattern body(FreeBlockBody);
	if (!found || !Libs::Graphics::HostMemoryRangeIsReadable(*found + FreeBlockBodyAt, body.Size()) ||
	    !body.Matches(reinterpret_cast<const uint8_t*>(*found + FreeBlockBodyAt))) {
		std::printf("Engine frees: no page allocator free of a known build found; retaining guest code\n");
		return;
	}
	int32_t lock_displacement = 0, stats_displacement = 0;
	std::memcpy(&lock_displacement, reinterpret_cast<const void*>(*found + LockOperandAt), sizeof(lock_displacement));
	std::memcpy(&stats_displacement, reinterpret_cast<const void*>(*found + StatsOperandAt), sizeof(stats_displacement));
	g_lock         = *found + LockOperandEnd + static_cast<int64_t>(lock_displacement);
	g_stats_global = *found + StatsOperandEnd + static_cast<int64_t>(stats_displacement);
	site           = *found;
	if (GetMode() == Mode::Walk || GetMode() == Mode::GiveBack) {
		std::thread(WalkerThread).detach();
		std::printf("Engine frees: the page allocator (free at eboot+0x%llx, lock eboot+0x%llx): free blocks %s (KYTY_ENGINE_FREE)\n",
		            static_cast<unsigned long long>(site - program->base_vaddr),
		            static_cast<unsigned long long>(g_lock - program->base_vaddr),
		            GetMode() == Mode::GiveBack ? "free for ~30 s given back" : "walked");
	}
	if (GetMode() != Mode::Count && !g_drop_buffers) return;
	const auto requested = program->base_vaddr + CaveOffset;
	const auto allocated = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    requested, PageSize, Common::VirtualMemory::Mode::ExecuteReadWrite, "demons_souls_page_free", true);
	if (allocated != requested) {
		if (allocated) Libs::LibKernel::Memory::FreeGuestMemory(allocated, PageSize);
		site = 0;
		return;
	}
	Xbyak::ClearError();
	Xbyak::CodeGenerator code(PageSize, reinterpret_cast<void*>(allocated));
	EmitThunk(code, *found + MovedPrologue);
	code.ready();
	if (Xbyak::GetError() || !Common::VirtualMemory::FlushInstructionCache(allocated, code.getSize()) ||
	    !Libs::LibKernel::Memory::ProtectGuestMemory(allocated, PageSize, Common::VirtualMemory::Mode::ExecuteRead,
	                                                 nullptr)) {
		Libs::LibKernel::Memory::FreeGuestMemory(allocated, PageSize);
		site = 0;
		return;
	}
	// jmp thunk; nop over push rbp; mov rbp, rsp; push r15 (installed before guest threads run).
	installed[0]            = 0xe9;
	const auto displacement = static_cast<int32_t>(allocated - (*found + 5));
	std::memcpy(installed.data() + 1, &displacement, sizeof(displacement));
	installed[5] = 0x90;
	std::memcpy(original.data(), reinterpret_cast<const void*>(*found), original.size());
	std::memcpy(reinterpret_cast<void*>(*found), installed.data(), installed.size());
	if (!Common::VirtualMemory::FlushInstructionCache(*found, installed.size())) {
		std::memcpy(reinterpret_cast<void*>(*found), original.data(), original.size());
		Common::VirtualMemory::FlushInstructionCache(*found, original.size());
		Libs::LibKernel::Memory::FreeGuestMemory(allocated, PageSize);
		site = 0;
		return;
	}
	cave = allocated;
	std::printf("Engine frees: the page allocator's free at eboot+0x%llx hooked:%s%s\n",
	            static_cast<unsigned long long>(site - program->base_vaddr), GetMode() == Mode::Count ? " counted (KYTY_ENGINE_FREE)" : "",
	            g_drop_buffers ? " the buffers over a freed block let go (KYTY_ENGINE_FREE_BUFFERS)" : "");
#else
	(void)program;
#endif
}

void Clear() {
	g_stop.store(true, std::memory_order_relaxed);
	if (!cave) return;
	std::array<uint8_t, MovedPrologue> bytes {};
	std::memcpy(bytes.data(), reinterpret_cast<const void*>(site), bytes.size());
	if (bytes == installed) {
		std::memcpy(reinterpret_cast<void*>(site), original.data(), bytes.size());
		Common::VirtualMemory::FlushInstructionCache(site, bytes.size());
	}
	Libs::LibKernel::Memory::FreeGuestMemory(cave, PageSize);
	cave = site = 0;
}
} // namespace Loader::DemonsSoulsPageFree
