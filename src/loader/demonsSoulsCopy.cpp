#include "loader/demonsSoulsCopy.h"

#include "common/logging/log.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "loader/guestCode.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <xxhash.h>

extern "C" {
// KYTY_GUEST_MOVE_COMPARE (default on; 0 for an A/B): a large move writes only the destination pages whose bytes
// differ (CompareMove).
[[gnu::used]] volatile std::atomic<uint32_t> kyty_local_move_compare {[] {
	const char* value = std::getenv("KYTY_GUEST_MOVE_COMPARE");
	return value != nullptr && value[0] == '0' ? 0u : 1u;
}()};
}

namespace Loader::DemonsSoulsCopy {
namespace {
// Demon's Souls (PPSA01340, PPSA01341, PPSA01342).
bool IsSupportedGame() {
	static const bool demons_souls = [] {
		std::string title;
		return SystemContentParamSfoGetString("TITLE_ID", &title) &&
		       (title == "PPSA01340" || title == "PPSA01341" || title == "PPSA01342");
	}();
	return demons_souls;
}
// What the compared moves did (bytes moved, left alone as equal, GPU-written), printed every 30 s.
std::atomic<uint64_t> g_move_bytes {0}, g_move_same {0}, g_move_gpu {0}, g_moves {0};
void Report() {
	static std::atomic<uint64_t> last {0};
	const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
	                                           std::chrono::steady_clock::now().time_since_epoch()).count());
	auto old = last.load(std::memory_order_relaxed);
	if (now < old + 30 || !last.compare_exchange_strong(old, now)) return;
	std::printf("Demon's Souls copy: %llu large moves, %.1f MiB moved, %.1f MiB equal and left alone, %.1f MiB GPU-written\n",
	            static_cast<unsigned long long>(g_moves.load()), g_move_bytes.load() / 1048576.0, g_move_same.load() / 1048576.0,
	            g_move_gpu.load() / 1048576.0);
	std::fflush(stdout);
}
uint64_t                site = 0;
std::array<uint8_t, 13> original {}, patch {};

// A large move into memory the GPU caches, page by page: a page the GPU wrote (its bytes here are stale, and reading them
// would fault) or whose bytes differ is written (runs prepared as overwritten, Memory::TryPrepareHostOverwrite); a page
// that already holds the source's bytes is left alone, clean: no upload of it again, no protection change.
void* CompareMove(uint64_t to, uint64_t from, size_t size) {
	constexpr uint64_t Page = 4096;
	// (The images over all of it as before: a GPU-written image's bytes here are stale, equal or not.)
	(void)Libs::LibKernel::Memory::TryPrepareHostImageWrite(to, size);
	uint64_t           run = 0, run_end = 0, same = 0, gpu = 0;
	const auto         write = [&] {
        if (run_end == run) return;
        (void)Libs::LibKernel::Memory::TryPrepareHostOverwrite(to + run, run_end - run);
        std::memmove(reinterpret_cast<void*>(to + run), reinterpret_cast<const void*>(from + run), run_end - run);
	};
	for (uint64_t at = 0; at < size;) {
		const auto next  = std::min<uint64_t>(size, ((to + at) / Page + 1) * Page - to);
		const auto bytes = next - at;
		bool       differs = Libs::LibKernel::Memory::MayFaultOnGpuRead(to + at, bytes);
		if (differs) gpu += bytes;
		else differs = std::memcmp(reinterpret_cast<const void*>(to + at), reinterpret_cast<const void*>(from + at), bytes) != 0;
		if (!differs) {
			same += bytes;
		} else {
			if (run_end != at) {
				write();
				run = at;
			}
			run_end = next;
		}
		at = next;
	}
	write();
	g_moves.fetch_add(1, std::memory_order_relaxed);
	g_move_bytes.fetch_add(size, std::memory_order_relaxed);
	g_move_same.fetch_add(same, std::memory_order_relaxed);
	g_move_gpu.fetch_add(gpu, std::memory_order_relaxed);
	Report();
	return reinterpret_cast<void*>(to);
}

void* KYTY_SYSV_ABI     CoherentMove(void* destination, const void* source, size_t size) {
	// A move within one range reads bytes it then writes: their GPU data is read back first. Else the destination's is
	// replaced whole and dropped (Memory::InvalidateMemoryForWrite).
	const auto to = reinterpret_cast<uint64_t>(destination), from = reinterpret_cast<uint64_t>(source);
	const bool overlap = to < from + size && from < to + size;
	if (!overlap && size >= 256 * 1024 && kyty_local_move_compare.load(std::memory_order_relaxed) != 0)
		return CompareMove(to, from, size);
	if (size >= 256 * 1024) g_move_bytes.fetch_add(size, std::memory_order_relaxed);
	return Move(destination, source, size,
	            overlap ? &Libs::LibKernel::Memory::TryPrepareHostWrite : &Libs::LibKernel::Memory::TryPrepareHostOverwrite);
}
// libc.prx's memmove/bcopy/memcpy body, shared by the three entries, which jump to it after the guest's
// argument/stack checks with one saved RBP. Hashed whole, not just the prologue, in each build audited; nothing
// else branches into the replaced bytes (the body's size jump table neither). Other builds, relocations or
// platform loader patches fail closed.
struct Body {
	const char* build;
	uint64_t    offset, size, hash;
};
constexpr std::array<Body, 2> Bodies {{
    {"01.007.000", 0x3c36, 2048, 0xe654325b8be848a9ull},
    {"01.005.000", 0x3d06, 1705, 0xc4be692ee02e9f77ull}, // an older body (another small-size table), the same entries
}};
} // namespace

void Install(Program* program) {
#if defined(__x86_64__) || defined(_M_X64)
	if (site || !program || !IsSupportedGame() || program->file_name.filename() != "libc.prx")
		return;
	const auto body = std::find_if(Bodies.begin(), Bodies.end(), [&](const Body& b) {
		const auto address = program->base_vaddr + b.offset;
		return program->mapped_size >= b.offset + b.size && program->base_vaddr <= UINT64_MAX - b.offset - b.size &&
		       GuestCode::Readable(address, b.size) &&
		       XXH3_64bits(reinterpret_cast<const void*>(address), b.size) == b.hash;
	});
	if (body == Bodies.end()) {
		std::printf("Demon's Souls copy: libc.prx of an unknown build; retaining its memmove\n");
		return;
	}
	const auto address = program->base_vaddr + body->offset;
	std::memcpy(original.data(), reinterpret_cast<const void*>(address), original.size());
	patch = Tailcall(reinterpret_cast<uint64_t>(&CoherentMove));
	std::memcpy(reinterpret_cast<void*>(address), patch.data(), patch.size());
	if (!Common::VirtualMemory::FlushInstructionCache(address, patch.size())) {
		std::memcpy(reinterpret_cast<void*>(address), original.data(), original.size());
		Common::VirtualMemory::FlushInstructionCache(address, original.size());
		return;
	}
	site = address;
	std::printf("Demon's Souls copy: installed verified coherent memmove (libc.prx of %s)\n", body->build);
#endif
}

void Clear() {
	if (!site) return;
	if (std::memcmp(reinterpret_cast<const void*>(site), patch.data(), patch.size()) == 0) {
		std::memcpy(reinterpret_cast<void*>(site), original.data(), original.size());
		Common::VirtualMemory::FlushInstructionCache(site, original.size());
	}
	site = 0;
}
} // namespace Loader::DemonsSoulsCopy
