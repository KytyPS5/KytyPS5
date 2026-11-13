// Demon's Souls' resource loader (ResourceFactory's loading thread, 01.005.000 eboot+0x88fbc0) takes the first request
// of its priority lists (list nodes: the request at + 0, next at + 0x10) and hands it to the request handling. A request
// withdrawn by another thread leaves its node in two steps: its request cleared first ([node] = 0), then the node
// unlinked and linked to itself. The loader reads the lists without the lock of that removal: a node read in between
// went to the handling with no request, which crashed on it (eboot+0x895822, cmp dword [rdi + 0x5c], 0 with rdi 0).
// Two players of the European 01.005.000 crashed there within minutes of play, one of them also at start-up (issues
// #185, #204); neither the regions' languages nor the job workers' spin settings changed it, and 01.007.000 has the
// same code. Here a node without a request makes the loader wait until it is unlinked (or has a request again; a
// second at most), then scan its lists again from the start. A node already unlinked (linked to itself), or still
// without a request after that second, that a list still begins with stays so: the loader holds the lists' lock while
// it scans, so nothing would ever move it, and the scan found it again without end (issue #327, 01.005.000 at King
// Allant's fog: 268 million scans, the game frozen). That list is rebuilt from its last node backwards first (Repair).
#include "loader/demonsSoulsLoaderGuard.h"

#include "common/abi.h"
#include "common/guardedCopy.h"
#include "common/virtualMemory.h"
#include "loader/demonsSoulsTitle.h"
#include "kernel/memory.h"
#include "loader/guestCode.h"
#include "loader/runtimeLinker.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#ifndef XBYAK_NO_EXCEPTION
#define XBYAK_NO_EXCEPTION
#endif
#include <xbyak/xbyak.h>

namespace Loader::DemonsSoulsLoaderGuard {
namespace {
constexpr uint64_t PageSize = 0x4000, CaveOffset = 0x8000000 + 6 * PageSize;

// 01.005.000 eboot+0x8906b0: mov rax, [rbx + 0xe0]; cmp rax, [rbp - 0x2250]; je <no request>; then the node taken:
// mov r12, [rax]; cmp [rip + d], r12; je +0x75; mov rax, [rip + d]; mov [rip + d], r12; cmp byte [rax], 0; je +0x62.
constexpr const char* Take = "48 8b 83 e0 00 00 00 48 3b 85 b0 dd ff ff 0f 84 ?? ?? ?? ?? 4c 8b 20 4c 39 25 ?? ?? ?? ?? 74 75 "
                             "48 8b 05 ?? ?? ?? ?? 4c 89 25 ?? ?? ?? ?? 80 38 00 74 62";
constexpr uint64_t SiteOffset = 20; // mov r12, [rax] (3 bytes) and the cmp of the last request (7) move to the stub
constexpr uint32_t SiteSize   = 10;
// The scan's start, 0x3a1 bytes before the site: mov rax, [rbx + d] (the first list's first node); mov [rbp + d], r12d;
// then at the scan's entry cmp rax, [rbp + d]; jne <site> (the list not empty).
constexpr uint64_t ScanDistance = 0x3a1;

uint64_t              g_base = 0;
std::atomic<uint32_t> g_waits {0}, g_repairs {0};

// The loader's five priority lists: sentinel nodes at + 0xd0 + i * 0x28 (the owner, prev at + 0x8, next at + 0x10, the
// list at + 0x18, the node count at + 0x20, an int). The loader holds their lock (its mutex at + 0x50) while it scans.
constexpr uint64_t ListSentinels = 0xd0, ListStride = 0x28;
constexpr uint32_t Lists = 5, LongestWalk = 1u << 16;

bool ReadPointer(uint64_t address, uint64_t& value) {
	return address != 0 && (address & 7u) == 0 &&
	       Common::GuardedCopy(&value, reinterpret_cast<const void*>(address), sizeof(value)) == 0 && value != 0 &&
	       (value & 7u) == 0;
}

// `node`, a list's first node, links to itself (or has had no request for a second): it left the chain without the
// list's sentinel knowing (issue #327: the sentinel's next named it forever after, the guard waited out nothing and the
// loader scanned again without end). The list is rebuilt from the sentinel's prev back to that node (or to the
// sentinel): the chain its tail insertions linked, every node's next set to the one after it, the count recounted.
// Returns the list, or -1 (the node first in none, or a chain that does not end).
int Repair(uint64_t loader, uint64_t node, uint32_t& kept) {
	for (uint32_t i = 0; i < Lists; ++i) {
		const uint64_t sentinel = loader + ListSentinels + ListStride * i;
		uint64_t       first = 0, last = 0;
		if (!ReadPointer(sentinel + 0x10, first) || first != node || !ReadPointer(sentinel + 0x8, last)) continue;
		// (`after` is the node after `at` in the list.)
		uint64_t after = sentinel, at = last;
		uint32_t count = 0;
		while (at != sentinel && at != node) {
			uint64_t before = 0;
			if (count == LongestWalk || !ReadPointer(at + 0x8, before)) return -1;
			if (before == at) break; // (another node out of the chain: the list starts after it)
			*reinterpret_cast<uint64_t*>(at + 0x10) = after;
			after = at;
			at    = before;
			++count;
		}
		*reinterpret_cast<uint64_t*>(sentinel + 0x10) = after;
		*reinterpret_cast<uint64_t*>(after + 0x8)     = sentinel;
		*reinterpret_cast<int32_t*>(sentinel + 0x20)  = static_cast<int32_t>(count);
		kept                                          = count;
		return static_cast<int>(i);
	}
	return -1;
}

// A node whose request was cleared: wait until it has a request again (at most a second); the loader then scans its
// lists again. Linked to itself already, or the second out with no request, it stays so: the loader holds the lists' lock
// while it scans (its mutex, locked at eboot+0x890310), so no thread that takes it can move the node. That list is
// repaired first.
void KYTY_SYSV_ABI Withdrawn(uint64_t node, uint64_t loader) {
	const auto begin = std::chrono::steady_clock::now();
	bool       unlinked = false, stuck = false;
	for (uint32_t round = 0;; ++round) {
		uint64_t request = 0, next = 0;
		if (Common::GuardedCopy(&request, reinterpret_cast<const void*>(node), sizeof(request)) != 0 ||
		    Common::GuardedCopy(&next, reinterpret_cast<const void*>(node + 0x10), sizeof(next)) != 0 || request != 0)
			break;
		if (next == node) {
			unlinked = true;
			break;
		}
		if (std::chrono::steady_clock::now() - begin >= std::chrono::seconds(1)) {
			stuck = true;
			break;
		}
		if (round < 64) std::this_thread::yield();
		else std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	if (unlinked || stuck) {
		uint32_t kept = 0;
		if (const int list = Repair(loader, node, kept); list >= 0) {
			const auto repairs = g_repairs.fetch_add(1, std::memory_order_relaxed) + 1;
			if (repairs <= 16 || (repairs & (repairs - 1)) == 0)
				std::printf("Demon's Souls loader guard: list %d of the loader 0x%llx still began at the %s node 0x%llx; "
				            "rebuilt from its last node (%u node(s) kept; %u repair(s) so far)\n",
				            list, static_cast<unsigned long long>(loader), unlinked ? "unlinked" : "requestless",
				            static_cast<unsigned long long>(node), kept, repairs);
			return;
		}
	}
	const auto count = g_waits.fetch_add(1, std::memory_order_relaxed) + 1;
	if (count <= 8 || (count & (count - 1)) == 0)
		std::printf("Demon's Souls loader guard: a request being withdrawn waited out (node 0x%llx of the loader 0x%llx, "
		            "%lld us; %u so far), the lists scanned again\n",
		            static_cast<unsigned long long>(node), static_cast<unsigned long long>(loader),
		            static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
		                                       std::chrono::steady_clock::now() - begin)
		                                       .count()),
		            count);
}

// The scan's start before `site` as 01.005.000 has it; its first list's displacement from the loader (rbx), or 0.
uint32_t ScanStart(uint64_t site, uint64_t& scan) {
	scan            = site - ScanDistance;
	const auto* at  = reinterpret_cast<const uint8_t*>(scan - 14);
	constexpr uint8_t load[] {0x48, 0x8b, 0x83}, store[] {0x44, 0x89, 0xa5}, compare[] {0x48, 0x3b, 0x85}, jne[] {0x0f, 0x85};
	if (std::memcmp(at, load, 3) != 0 || std::memcmp(at + 7, store, 3) != 0 || std::memcmp(at + 14, compare, 3) != 0 ||
	    std::memcmp(at + 21, jne, 2) != 0)
		return 0;
	int32_t to = 0;
	std::memcpy(&to, at + 23, sizeof(to));
	if (scan + 13 + static_cast<int64_t>(to) != site) return 0;
	uint32_t list = 0;
	std::memcpy(&list, at + 3, sizeof(list));
	return list;
}
} // namespace

void Install(Program* program) {
#if defined(__x86_64__) || defined(_M_X64)
	if (program == nullptr || !Loader::DemonsSoulsTitle::IsSupportedGame() || program->file_name.filename() != "eboot.bin" ||
	    program->mapped_size > CaveOffset || program->base_vaddr > UINT64_MAX - CaveOffset - PageSize)
		return;
	const auto take = GuestCode::FindUnique(*program, GuestCode::Pattern(Take));
	if (!take) return; // (not 01.005.000)
	const auto site = *take + SiteOffset;
	uint64_t   scan = 0;
	const auto list = ScanStart(site, scan);
	if (list == 0) {
		std::printf("Demon's Souls loader guard: the resource loader's scan is not of a known build; retaining its guest code\n");
		return;
	}
	int32_t last = 0;
	std::memcpy(&last, reinterpret_cast<const void*>(site + 6), sizeof(last));
	const uint64_t last_request = site + SiteSize + static_cast<int64_t>(last);
	const auto     requested    = program->base_vaddr + CaveOffset;
	const auto     allocated    = Libs::LibKernel::Memory::AllocateRuntimeMemory(
        requested, PageSize, Common::VirtualMemory::Mode::ExecuteReadWrite, "demons_souls_loader_guard", true);
	if (allocated != requested) {
		if (allocated) Libs::LibKernel::Memory::FreeGuestMemory(allocated, PageSize);
		return;
	}
	g_base = program->base_vaddr;
	using namespace Xbyak::util;
	Xbyak::ClearError();
	Xbyak::CodeGenerator c(PageSize, reinterpret_cast<void*>(allocated));
	constexpr uint8_t save_fp[] {0x48, 0x0f, 0xae, 0x04, 0x24}; // FXSAVE64 [rsp]
	Xbyak::Label      withdrawn;
	// The node's request; with one, the moved compare of the last request (its address now absolute) and back. (rax, the
	// node, is read no more after it: the code after the site loads it again before a use, or calls first.)
	c.mov(r12, ptr[rax]);
	c.test(r12, r12);
	c.jz(withdrawn);
	c.mov(rax, last_request);
	c.cmp(ptr[rax], r12);
	c.jmp(reinterpret_cast<const void*>(site + SiteSize));
	// None: waited out (registers and FP state kept; below the red zone, on a realigned stack), then the scan from its
	// start, as the loader's loop enters it (rax: the first list's first node; the scan compares before it branches).
	c.L(withdrawn);
	c.lea(rsp, ptr[rsp - 128]);
	for (const auto& reg: {rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11, rbx})
		c.push(reg);
	c.mov(rdi, rax);
	c.mov(rsi, rbx);
	c.mov(rbx, rsp);
	c.and_(rsp, -16);
	c.sub(rsp, 512);
	c.db(save_fp, sizeof(save_fp));
	c.mov(rax, reinterpret_cast<uint64_t>(&Withdrawn));
	c.call(rax);
	c.fxrstor64(ptr[rsp]);
	c.mov(rsp, rbx);
	for (const auto& reg: {rbx, r11, r10, r9, r8, rdi, rsi, rdx, rcx, rax})
		c.pop(reg);
	c.lea(rsp, ptr[rsp + 128]);
	c.mov(rax, ptr[rbx + list]);
	c.jmp(reinterpret_cast<const void*>(scan));
	c.ready();
	if (Xbyak::GetError() || !Common::VirtualMemory::FlushInstructionCache(allocated, c.getSize()) ||
	    !Libs::LibKernel::Memory::ProtectGuestMemory(allocated, PageSize, Common::VirtualMemory::Mode::ExecuteRead, nullptr)) {
		Libs::LibKernel::Memory::FreeGuestMemory(allocated, PageSize);
		return;
	}
	// Installation occurs before module initializers or guest threads run; program memory is writable to the host.
	// (The bytes after the jump are never reached: nothing jumps into the moved instructions.)
	uint8_t code[SiteSize];
	std::memset(code, 0xcc, sizeof(code));
	code[0]                 = 0xe9;
	const auto displacement = static_cast<int32_t>(allocated - (site + 5));
	std::memcpy(code + 1, &displacement, sizeof(displacement));
	std::memcpy(reinterpret_cast<void*>(site), code, sizeof(code));
	Common::VirtualMemory::FlushInstructionCache(site, sizeof(code));
	std::printf("Demon's Souls loader guard: installed (the resource loader at eboot+0x%llx waits out a request being "
	            "withdrawn)\n",
	            static_cast<unsigned long long>(site - program->base_vaddr));
#endif
}

} // namespace Loader::DemonsSoulsLoaderGuard
