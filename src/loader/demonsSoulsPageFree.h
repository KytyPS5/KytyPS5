#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace Loader {
struct Program;
// The game engine's page allocator (BPE PlatformPageAllocator, 01.007.000) takes all the direct memory the game is told
// it has at start-up and hands it out in blocks of 64 KiB pages. A freed block whose pages are all mapped stays mapped
// in its pool's free list for the next allocation: its pages stay in RAM, the game's whether or not it uses them
// (2.1-2.8 GiB of free blocks at Stonefang, Latria, Shrine and Valley after Boletaria; 0.6 GiB in Boletaria, where the
// texture streamer fills the memory to its 400 MiB floor). On a PS5 that is free memory; here it is RAM the emulator
// holds. The engine frees and takes 1.5-7.5 GB of blocks every 30 s (textures streamed in and out, 0.5-8 MiB each), so
// most free blocks are taken again within seconds: a free block's pages are worth giving back only once it stays free.
// KYTY_ENGINE_FREE=giveback (default with 16 GiB or less for the emulator, Windows and Linux): a thread of the emulator's
// takes the allocator's lock every 2 s, walks the free lists, and gives the pages of blocks free for ~30 s back to the
// system (a chunk at a time, each under the lock after a check that the block is still free: no allocation can take it
// meanwhile; its bytes become zeros, a free block's bytes being no one's; Linux: a hole punched in the guest memory's
// file, pages a GPU cache watches left alone). Boletaria and four worlds (16 GB policy): the
// PC's memory in use 1.7-2.0 GB lower in the other worlds, 1.2 GB back in Boletaria, 0.2-0.6 GB at the end of its
// route; 2-2's tour at the same frame rate (93.9 fps walking against 92.7). =walk: the walks only, checked
// against the allocator's own counts (diagnostics); =count: each free counted (sizes, the guest view's protection over
// the block); =0: off.
namespace DemonsSoulsPageFree {
void Install(Program* program);
void Clear();
// The free blocks whose pages are all mapped, those over [begin, end) added to `blocks` as (address, size): the free lists
// walked under the allocator's lock (waited for 2 ms at most). False when the free is not hooked, the lock stayed taken,
// or the lists were not as read from the code (from then on always). For the GPU caches (BufferCache::DropFreedRange): a
// buffer over a freed block and others free too.
bool FreeBlocksIn(uint64_t begin, uint64_t end, std::vector<std::pair<uint64_t, uint64_t>>* blocks);

// The engine's free of a block as 01.007.000 has it (01.005.000, built by another compiler, has other code): the prologue
// (push rbp; mov rbp, rsp; push r15; push r14; push rbx; sub rsp, 0x18), the stack guard load, the block descriptor kept
// in rbx, the spin on the allocator's lock (its address the cmp's RIP-relative operand at LockOperandAt); at StatsLoadAt
// the load of the allocator's statistics object (RIP-relative), then the descriptor's unlink and its pool's index.
inline constexpr std::string_view FreeBlock = "55 48 89 e5 41 57 41 56 53 48 83 ec 18 4c 8b 35 ?? ?? ?? ?? 48 89 fb 49 c7 c7 ff "
                                              "ff ff ff 31 c9 49 8b 06 48 89 45 e0 48 83 3d ?? ?? ?? ?? 00 74";
inline constexpr std::string_view FreeBlockBody =
    "48 8b 05 ?? ?? ?? ?? 48 8d 73 20 0f b6 4b 70 48 c7 43 20 00 00 00 00 4c 8b 4b 28 4c 8b 43 30 48 8d 14 89";
inline constexpr size_t FreeBlockBodyAt = 0x97;
inline constexpr size_t LockOperandAt = 0x2a, LockOperandEnd = 0x2f; // cmp qword [rip + d], 0: d at +0x2a, next at +0x2f
inline constexpr size_t StatsOperandAt = 0x9a, StatsOperandEnd = 0x9e; // mov rax, [rip + d]
inline constexpr size_t MovedPrologue = 6; // push rbp; mov rbp, rsp; push r15: run by the count hook's thunk
// A block descriptor: size at +0x50, mapped bytes at +0x58, address at +0x60; its allocator list node at +0x20 (the node's
// owner, the descriptor, at +0x20; the next node at +0x30; the list's head at +0x38).
inline constexpr uint32_t SizeAt = 0x50, MappedAt = 0x58, AddressAt = 0x60, NodeAt = 0x20, NextAt = 0x30, ListAt = 0x38;
// The statistics object's pools, PoolStride apart: the free list's head node at +0x28 (no owner; the first node at +0x38),
// the free blocks at +0x48, the free blocks' mapped 64 KiB pages at +0x70.
inline constexpr uint64_t Pools = 2, PoolStride = 0xa0, FreeHeadAt = 0x28, FreeFirstAt = 0x38, FreeCountAt = 0x48,
                          FreePagesAt = 0x70;
} // namespace DemonsSoulsPageFree
} // namespace Loader
