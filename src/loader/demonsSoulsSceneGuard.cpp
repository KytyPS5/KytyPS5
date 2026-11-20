// Demon's Souls' scene nodes keep their children in an eastl::vector (node + 0x30); three traversals of that tree
// read node + 0x98 / + 0xa0 first and crashed on a null child in job workers while areas streamed in (01.007.000
// eboot+0x8daff8 10-03 and 10-05, eboot+0xb9d1e1 10-05, eboot+0xc5cabf 10-06; the 10-06 parent held three null
// children that stayed null). What writes them is not known: the child list mutators (AddChild 0x8da6f0,
// RemoveChild 0x8dae50, InsertChild 0x8daee0) never ran concurrently on one parent nor left a null in an
// instrumented 1-1 walk. Each of the three treats a null node as an empty one here (a find returns nothing, a
// visit visits nothing) and the first skips are printed with the registers that locate the parent's vector.
// Three more pass a state bit of a node down its subtree (bits 0x2000, 0x8000 and 0x4000 of node + 8; 01.007.000
// eboot+0xc5a030, +0xc5a2e0 and +0xc5a570, 01.005.000 +0xc39640, +0xc398c0 and +0xc39b20): their loops over the
// children read each child's word at + 8 in place and crashed on the same null children when the world was torn
// down at "save and quit" (eboot+0xc5a3be 10-07, after the first guard had skipped them 32 times). Their loops
// skip a null child here (nothing to pass the bit to).
// The find by id has five siblings next to it (01.007.000 eboot+0x8db150, +0x8db350, +0x8db5c0, +0x8db650 and
// +0x8db780; 01.005.000 the same five after +0x8bf620), each reading node + 0x98 / + 0xa0 first and recursing into
// the children the same way: two collect a subtree's components matching a type into a vector, one finds a component
// by its 128-bit id, two visit them. The first crashed on a null child in a job worker at 1-1 (eboot+0x8db185 10-11),
// after the visit at +0xb9d1d0 had skipped 32 null children of the same parent, whose children vector changed size
// and null slots between the skips. A null node is an empty one in each of them too (a collector returns the count
// of its vector, unchanged).
// An event's dispatch calls its listeners (object and member function pairs, 0x40 bytes apart) under a spin lock of the
// object it is called on (01.007.000 eboot+0xc58440, and +0xc587f0 for another event, one template). A job worker called
// the first on a null object at Latria (eboot+0xc5845a 10-16, from eboot+0x421827): the world's record of an entity,
// looked up just before (eboot+0x1475a20), was missing, as the visits skipped null children. A null object has no
// listeners: nothing is dispatched.
// A physics job's step (Havok; 01.007.000 eboot+0x1ab5fe0, a virtual method of the job: its vtable eboot+0x269d2f0) reads
// the object it steps from the job (+ 0x38), then that object's state. Job workers read a null one (eboot+0x1ab6047
// reading 0x18) at least five times since 10-11: replaying the user's route at 1-1, in world tours and in long runs of
// world changes (11-08 loading the Shrine of Storms), the rest of the job whole (its vtables and other fields). The step
// has its own way out when what it builds there fails: an assert (int 0x41), its timer ended and back. A job with no
// object takes that way here, without the assert: nothing to step.
#include "loader/demonsSoulsSceneGuard.h"

#include "common/guardedCopy.h"
#include "common/logging/log.h"
#include "common/virtualMemory.h"
#include "graphics/host_gpu/hostMemory.h"
#include "loader/demonsSoulsTitle.h"
#include "kernel/memory.h"
#include "loader/demonsSoulsIdle.h"
#include "loader/guestCode.h"
#include "loader/runtimeLinker.h"
#include "local/local-platform.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Loader::DemonsSoulsSceneGuard {
namespace {
constexpr uint64_t PageSize = 0x4000, CaveOffset = 0x8000000 + 2 * PageSize;

// The traversals' first bytes as both builds seen have them (01.007.000 eboot+0x8dafd0, +0xb9d1d0 and +0xc5ca90;
// 01.005.000 +0x8bf620, +0xb7dd20 and +0xc3bf90; the find's five siblings), up to their reads of node + 0x98 /
// + 0xa0, with what a build moves (displacements, the frame size) left out; two of the siblings in each build's own
// register allocation.
// What a null node returns: nothing found (null) or visited, or for a collector the count of the vector it appends to
// (rcx: begin, end), of 8- or 32-byte elements, as its own end computes it (shr rax, 3 or 5).
enum class Empty : uint8_t { Zero, Count8, Count32 };
struct Traversal {
	const char*                what;
	std::array<const char*, 2> code; // (a second for a build whose code differs)
	Empty                      empty = Empty::Zero;
};
constexpr std::array<Traversal, 8> Traversals {{
    {"find a node by id (returns it or null)",
     {"55 48 89 e5 41 57 41 56 41 55 41 54 53 50 89 c8 89 d3 41 89 f6 49 89 ff 48 89 4d d0 83 e0 fd 83 f8 04 0f 84 ?? ?? "
      "?? ?? 4d 8b a7 98 00 00 00 4d 3b a7 a0 00 00 00 0f 84 ?? ?? ?? ?? 41 83 fe ff"}},
    {"visit a subtree", {"55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 58 48 8b 87 a0 00 00 00 4c 8b bf 98 00 00 00 89 75 "
                         "d4 48 89 7d c8 48 89 45 b0 49 39 c7 74 60 48 8d 45 10 48 8b 48 28 48 8b 10 4c 8b 60 08 4c 8b 68 10"}},
    {"visit a subtree", {"55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec ?? ?? ?? ?? 48 8b 05 ?? ?? ?? ?? 89 b5 5c ff ff ff 48 "
                         "8b 00 48 89 45 d0 48 89 bd 50 ff ff ff 4c 8b af 98 00 00 00 4c 8b a7 a0 00 00 00 4d 39 e5"}},
    {"collect a subtree's components (8-byte elements)",
     {"55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec ?? 44 89 c0 48 89 cb 41 89 d4 41 89 f5 48 89 7d ?? 4c 89 45 ?? "
      "83 e0 fd 83 f8 04 0f 84 ?? ?? ?? ?? 48 8b 45 ?? 4c 8b b0 98 00 00 00 4c 3b b0 a0 00 00 00",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec ?? 44 89 c0 49 89 cd 41 89 d4 48 89 7d ?? 4c 89 45 ?? 89 75 ?? "
      "83 e0 fd 83 f8 04 0f 84 ?? ?? ?? ?? 48 8b 45 ?? 4c 8b b0 98 00 00 00 4c 3b b0 a0 00 00 00"},
     Empty::Count8},
    {"collect a subtree's components (32-byte elements)",
     {"55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec ?? 44 89 c0 49 89 cc 41 89 d5 48 89 7d ?? 4c 89 45 ?? 89 75 ?? "
      "83 e0 fd 83 f8 04 0f 84 ?? ?? ?? ?? 48 8b 45 ?? 4c 8b b8 98 00 00 00 4c 3b b8 a0 00 00 00"},
     Empty::Count32},
    {"find a component by its 128-bit id (returns it or null)",
     {"55 48 89 e5 41 57 41 56 53 50 48 8b 8f 98 00 00 00 48 8b 97 a0 00 00 00 48 89 f3 48 39 d1 74 ?? c5 fa 6f 03",
      "55 48 89 e5 41 57 41 56 41 54 53 48 8b 87 98 00 00 00 48 8b 8f a0 00 00 00 48 89 f3 48 39 c8 74 ?? c5 fa 6f 03"}},
    {"visit a subtree's components",
     {"55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec ?? 48 8b 05 ?? ?? ?? ?? 48 89 f3 48 8b 00 48 89 45 ?? 48 89 7d ?? "
      "4c 8b b7 98 00 00 00 4c 8b bf a0 00 00 00"}},
    {"visit a subtree's components",
     {"55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec ?? 4c 8b af 98 00 00 00 48 8b 87 a0 00 00 00 4c 89 c3 49 89 ce "
      "49 89 d7"}},
}};
// Each begins with push rbp; mov rbp, rsp; push r15: its stub runs them, the jump to the stub replaces them.
constexpr std::array<uint8_t, 6> Prologue {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
// The event dispatches up to their spin on the lock (the lock's offset in the object left out): every one found.
constexpr const char* Dispatch = "55 48 89 e5 41 57 41 56 41 54 53 49 89 f6 48 89 fb 49 c7 c7 ff ff ff ff 31 c9 48 83 bb ?? ?? "
                                 "00 00 00 74 ?? 66 66 66 2e 0f 1f 84 00 00 00 00 00 48 81 f9 10 27 00 00";

// The children loops of the bit-passing traversals, as both builds have them: the increment and end test
// (add r12, 8; cmp r12, [parent + 0x38]; je with a short or a near displacement), then the child's load
// (mov REG, [r12]) and the read of its word (mov eax, [REG + 8], or 01.005.000's mov ecx, imm32 first).
constexpr std::array<const char*, 2> ChildLoops {"49 83 c4 08 4d 3b ?? 38 74 ?? 4d 8b ?? 24",
                                                 "49 83 c4 08 4d 3b ?? 38 0f 84 ?? ?? ?? ?? 4d 8b ?? 24"};
// The children loop of the traversal that sets a subtree's update group (01.007.000 eboot+0x8da540, 01.005.000 the
// same bytes; node + 8 bits 8..11 and its times): add r14, 8; cmp r14, r15; je; mov rdi, [r14]; mov eax, [rdi + 8].
// It crashed on a null child in a job worker at 1-1 (eboot+0x8da64c 10-11), 4 null children of one parent that the
// bit-passing loops had skipped just before.
constexpr const char* GroupLoop = "49 83 c6 08 4d 39 fe 74 ?? 49 8b 3e 8b 47 08";
// The children loop of the traversal that calls each child's virtual method 0x18, then visits the components
// (01.007.000 eboot+0x8dacd0, 01.005.000 eboot+0x8bf340): mov rdi, [r14]; mov rax, [rdi]; call [rax + 0x18]; add r14, 8;
// cmp r14, r15; jne. It crashed on a null child in a job worker at 1-1 (eboot+0x8dad03 10-11, the user's route).
constexpr const char* VirtualLoop = "49 8b 3e 48 8b 07 ff 50 ?? 49 83 c6 08 4d 39 fe 75 ee";
// The children loop of the traversal that hands each node's id (+0x58) to a table at +0x40 of its first argument, then
// recurses into the children (01.007.000 eboot+0x8d8be0, 01.005.000 eboot+0x8bd270): mov rsi, [r15]; mov rdi, r14; call
// itself; add r15, 8; cmp r15, r12; jne. A null child crashed in the table lookup (eboot+0x8e5448 reading 0x58, 10-11,
// twice on the user's route).
constexpr const char* IdLoop = "49 8b 37 4c 89 f7 e8 ?? ?? ?? ?? 49 83 c7 08 4d 39 e7 75 ec";
// The character manager's collection loop (01.007.000 eboot+0x3510e0, called with the manager at 0x9027cf378): for
// each of a list of slot indices (u16) it loads the character from the manager's array (+0x18), asks whether its id
// (+8) is still in use and removes it if not: inc r14; cmp r13, r14; je; mov rcx, [rbp - 0x30]; movzx eax, word [rbx +
// r14 * 2]; mov rdi, [rip + d]; mov rcx, [rcx + 0x18]; mov r12, [rcx + rax * 8]; mov esi, [r12 + 8]; call; test al, al;
// jne. A null slot crashed it when the Nexus loaded (eboot+0x351140 reading 0x8, 10-11 and 10-12, five times: slot 19
// of lists of 22-28 indices up to 114, as the world switch tool warps there from another world's save; past the null
// ones, slot 69 read 0x100000000 beyond the array's count, +0x24). An index at or past the count, or a null slot, has
// nothing to remove: the next index.
constexpr const char* CharacterLoop = "49 ff c6 4d 39 f5 74 ?? 48 8b 4d d0 42 0f b7 04 73 48 8b 3d ?? ?? ?? ?? 48 8b 49 18 "
                                      "4c 8b 24 c1 41 8b 74 24 08 e8 ?? ?? ?? ?? 84 c0 75 d2";
// The physics job step's load of its object, as both builds have it (01.007.000 eboot+0x1ab602b): the end of its timer's
// start (mov rdi, [rbx]; mov r13d, eax; test rdi, rdi; je; lea rsi, [rip + d]; call), then mov rcx, [r12 + 0x38]; mov rdx,
// [rcx + 0x18] (the two the jump to the stub replaces) and the reads after them. Within the next 0x100 bytes the test of
// what it builds (mov [r12 + 0x40], rax; test rax, rax; je rel32) leads to its way out: int 0x41, then the timer's end
// (mov rax, fs:[0], which the loader made a 9-byte call; lea rax, [rax - 0x38]; mov rdi, [rax]; test rdi, rdi; jne; jmp)
// and its epilogue.
constexpr const char* JobStep = "48 8b 3b 41 89 c5 48 85 ff 74 0c 48 8d 35 ?? ?? ?? ?? e8 ?? ?? ?? ?? 49 8b 4c 24 38 48 8b 51 18 "
                                "48 63 81 34 01 00 00 48 89 55 80 48 8b 52 60 4c 8b 34 c2";
constexpr uint64_t    JobStepLoad  = 23;
constexpr uint32_t    JobStepMoved = 9;
constexpr const char* JobStepTest  = "49 89 44 24 40 48 85 c0 0f 84 ?? ?? ?? ??";
constexpr const char* JobStepExit  = "cd 41 ?? ?? ?? ?? ?? ?? ?? ?? ?? 48 8d 80 c8 ff ff ff 48 8b 38 48 85 ff 0f 85 ?? ?? ?? ?? e9";
struct ChildLoop {
	uint64_t increment = 0;  // the loop's add ITER, 8: a null child continues there
	uint64_t load      = 0;  // mov REG, [ITER]
	uint32_t reg       = 0;  // REG
	uint32_t moved     = 0;  // the bytes the jump to the stub replaces: the load and the next instruction
	uint32_t load_size = 4;  // (mov REG, [r12] takes a SIB byte)
	uint32_t iter      = 12; // ITER: the element's address
	// Non-zero: ITER is an index, the element is skipped as a null one unless it is under the dword count at
	// [[rbp + owner_frame] + count_offset] (rdx holds it: the loop's call after the moved bytes clobbers rdx).
	int32_t  owner_frame  = 0;
	uint32_t count_offset = 0;
};

std::vector<ChildLoop> FindChildLoops(const Program& program) {
	std::vector<ChildLoop> loops;
	for (const auto* text: ChildLoops) {
		const GuestCode::Pattern pattern(text);
		for (const auto at: GuestCode::Find(program, pattern, 16)) {
			const auto* code = reinterpret_cast<const uint8_t*>(at);
			const auto* load = code + pattern.Size() - 4;
			// cmp r12, [r8..r15 + 0x38]; mov r8..r15 (not r12), [r12]
			const uint32_t reg = (load[2] >> 3u) & 7u;
			if ((code[6] & 0xf8u) != 0x60u || (load[2] & 0xc7u) != 0x04u || reg == 4) continue;
			uint32_t moved = 0;
			if (load[4] == 0x41 && load[5] == 0x8b && load[6] == (0x40u | reg) && load[7] == 0x08) moved = 8; // mov eax, [REG + 8]
			else if (load[4] == 0xb9) moved = 9; // mov ecx, imm32
			else continue;
			loops.push_back({at, at + pattern.Size() - 4, 8 + reg, moved});
		}
	}
	const GuestCode::Pattern group(GroupLoop);
	if (const auto at = GuestCode::FindUnique(program, group)) loops.push_back({*at, *at + 9, 7, 6, 3, 14});
	const GuestCode::Pattern virtual_loop(VirtualLoop);
	if (const auto at = GuestCode::FindUnique(program, virtual_loop)) loops.push_back({*at + 9, *at, 7, 6, 3, 14});
	const GuestCode::Pattern id_loop(IdLoop);
	if (const auto at = GuestCode::FindUnique(program, id_loop)) loops.push_back({*at + 11, *at, 6, 6, 3, 15});
	// (The slot's load and the read of its id move; the slot index, rax, is printed.)
	const GuestCode::Pattern character_loop(CharacterLoop);
	if (const auto at = GuestCode::FindUnique(program, character_loop))
		loops.push_back({*at, *at + 28, 12, 9, 4, 0, -0x30, 0x24});
	return loops;
}

uint64_t              g_base = 0;
std::atomic<uint32_t> g_skips {0};
std::atomic<uint32_t> g_child_skips {0};
std::atomic<uint32_t> g_job_skips {0};

// What leaves the null children is not known. The first null seen starts one dump (SCENE-NULL lines): every thread's
// frames then and 200 ms later (who is in the middle of what), and the null elements seen meanwhile read again 50 ms
// to 10 s later (filled later: a subtree being built; still null: written over).
std::array<std::atomic<uint64_t>, 16> g_null_elements {};
std::atomic<uint32_t>                 g_null_count {0};
std::atomic<bool>                     g_null_dump {false};

void DumpThreads(const char* when) {
	LocalPlatform::PrepareSampling();
	const auto self = LocalPlatform::ThreadId();
	std::string out = std::string("SCENE-NULL threads ") + when + "\n";
	char        line[160];
	for (const auto& [tid, name]: LocalPlatform::ProcessThreads()) {
		if (tid == self || name.rfind("SDLAudio", 0) == 0) continue;
		const auto handle = LocalPlatform::OpenThreadForSampling(tid);
		if (handle == 0) continue;
		uint64_t   words[24] {};
		const bool sampled = LocalPlatform::SampleThread(handle, 0, 0, words, std::size(words));
		LocalPlatform::CloseThreadForSampling(handle);
		if (!sampled) continue;
		std::snprintf(line, sizeof(line), "SCENE-NULL   %u %s:", tid, name.empty() ? "-" : name.c_str());
		out += line;
		for (const auto word: words) {
			if (word == 0) break;
			const auto guest = DescribeGuestAddressForDiagnostics(word);
			if (!guest.empty()) out += " " + guest;
			else {
				std::snprintf(line, sizeof(line), " %llx", static_cast<unsigned long long>(word));
				out += line;
			}
		}
		out += "\n";
	}
	std::fputs(out.c_str(), stdout);
	std::fflush(stdout);
}

void DumpNullElements(int after_ms) {
	std::string out;
	char        line[64];
	std::snprintf(line, sizeof(line), "SCENE-NULL +%d ms:", after_ms);
	out += line;
	const auto count = std::min<uint32_t>(g_null_count.load(std::memory_order_acquire), g_null_elements.size());
	for (uint32_t i = 0; i < count; i++) {
		const auto element = g_null_elements[i].load(std::memory_order_relaxed);
		uint64_t   value   = 0;
		const bool read    = element != 0 && Common::GuardedCopy(&value, reinterpret_cast<const void*>(element), sizeof(value)) == 0;
		std::snprintf(line, sizeof(line), read ? " %llx=%llx" : " %llx=?", static_cast<unsigned long long>(element),
		              static_cast<unsigned long long>(value));
		out += line;
	}
	out += "\n";
	std::fputs(out.c_str(), stdout);
	std::fflush(stdout);
}

void NullDump() {
	LocalPlatform::SetThreadName("Kyty.SceneNull");
	const auto start = std::chrono::steady_clock::now();
	DumpThreads("at the first null");
	int dumped = 0;
	for (const int after_ms: {50, 200, 1000, 3000, 10000}) {
		std::this_thread::sleep_until(start + std::chrono::milliseconds(after_ms));
		DumpNullElements(after_ms);
		if (after_ms == 200 && dumped++ == 0) DumpThreads("200 ms later");
	}
}

// A null element of a children vector at `element` (0: not known).
void NullSeen(uint64_t element) {
	if (element != 0 && (element & 7u) == 0) {
		bool known = false;
		const auto count = std::min<uint32_t>(g_null_count.load(std::memory_order_acquire), g_null_elements.size());
		for (uint32_t i = 0; i < count && !known; i++) known = g_null_elements[i].load(std::memory_order_relaxed) == element;
		if (!known) {
			if (const auto at = g_null_count.fetch_add(1, std::memory_order_acq_rel); at < g_null_elements.size())
				g_null_elements[at].store(element, std::memory_order_release);
		}
	}
	if (!g_null_dump.exchange(true, std::memory_order_acq_rel)) std::thread(NullDump).detach();
}

// The register that points at the null element of the caller's loop: a readable, aligned null word.
uint64_t NullElementOf(std::initializer_list<uint64_t> candidates) {
	for (const auto candidate: candidates) {
		uint64_t value = 1;
		if (candidate >= 0x10000 && (candidate & 7u) == 0 &&
		    Common::GuardedCopy(&value, reinterpret_cast<const void*>(candidate), sizeof(value)) == 0 && value == 0)
			return candidate;
	}
	return 0;
}

void KYTY_SYSV_ABI Skipped(uint64_t offset, uint64_t caller, uint64_t rbx, uint64_t r14, uint64_t r12, uint64_t r15) {
	NullSeen(NullElementOf({rbx, r14, r12, r15}));
	if (g_skips.fetch_add(1, std::memory_order_relaxed) >= 32) return;
	// (At the recursive visits' call sites rbx points at the null element of the parent's vector, r14 at its end.)
	std::printf("Demon's Souls scene guard: a null node skipped by eboot+0x%llx (caller eboot+0x%llx, rbx 0x%llx, r14 "
	            "0x%llx)\n",
	            static_cast<unsigned long long>(offset), static_cast<unsigned long long>(caller - g_base),
	            static_cast<unsigned long long>(rbx), static_cast<unsigned long long>(r14));
}

void KYTY_SYSV_ABI SkippedChild(uint64_t offset, uint64_t element, uint64_t r14, uint64_t r15) {
	NullSeen(element);
	if (g_child_skips.fetch_add(1, std::memory_order_relaxed) >= 32) return;
	// (r12 points at the null element of the parent's vector; the parent is r14 in 01.007.000, r15 in 01.005.000.)
	std::printf("Demon's Souls scene guard: a null child skipped at eboot+0x%llx (element 0x%llx, r14 0x%llx, r15 0x%llx)\n",
	            static_cast<unsigned long long>(offset), static_cast<unsigned long long>(element),
	            static_cast<unsigned long long>(r14), static_cast<unsigned long long>(r15));
}

void KYTY_SYSV_ABI SkippedJob(uint64_t offset, uint64_t job) {
	if (g_job_skips.fetch_add(1, std::memory_order_relaxed) >= 32) return;
	uint64_t vtable = 0;
	(void)Common::GuardedCopy(&vtable, reinterpret_cast<const void*>(job), sizeof(vtable));
	std::printf("Demon's Souls scene guard: a physics job with no object left its step at eboot+0x%llx (job 0x%llx, vtable "
	            "0x%llx)\n",
	            static_cast<unsigned long long>(offset), static_cast<unsigned long long>(job),
	            static_cast<unsigned long long>(vtable));
	std::fflush(stdout);
}
} // namespace

void Install(Program* program) {
#if defined(__x86_64__) || defined(_M_X64)
	if (program == nullptr || !Loader::DemonsSoulsTitle::IsSupportedGame() || program->file_name.filename() != "eboot.bin" ||
	    program->mapped_size > CaveOffset || program->base_vaddr > UINT64_MAX - CaveOffset - PageSize)
		return;
	// The null node traversals: all of a known build, or none of them.
	std::array<uint64_t, Traversals.size()> entries {};
	bool                                    traversals = true;
	for (size_t i = 0; i < Traversals.size() && traversals; i++) {
		std::vector<uint64_t> found;
		for (const auto* code: Traversals[i].code)
			if (code != nullptr)
				for (const auto at: GuestCode::Find(*program, GuestCode::Pattern(code), 2)) found.push_back(at);
		const std::optional<uint64_t> entry = found.size() == 1 ? std::optional {found.front()} : std::nullopt;
		if (!entry || std::memcmp(reinterpret_cast<const void*>(*entry), Prologue.data(), Prologue.size()) != 0) {
			std::printf("Demon's Souls scene guard: no single '%s' traversal of a known build found; retaining their guest "
			            "code\n",
			            Traversals[i].what);
			traversals = false;
		} else {
			entries[i] = *entry;
		}
	}
	const auto loops = FindChildLoops(*program);
	if (loops.empty()) std::printf("Demon's Souls scene guard: no children loop of a known build found; retaining their guest code\n");
	std::vector<uint64_t> dispatches;
	for (const auto at: GuestCode::Find(*program, GuestCode::Pattern(Dispatch), 4))
		if (std::memcmp(reinterpret_cast<const void*>(at), Prologue.data(), Prologue.size()) == 0) dispatches.push_back(at);
	if (dispatches.empty()) std::printf("Demon's Souls scene guard: no event dispatch of a known build found; retaining their guest code\n");
	// The physics job step: its object's load and its way out.
	uint64_t job_load = 0, job_exit = 0;
	if (const auto at = GuestCode::FindUnique(*program, GuestCode::Pattern(JobStep))) {
		const GuestCode::Pattern test(JobStepTest), way_out(JobStepExit);
		const uint64_t           end = program->base_vaddr + program->mapped_size;
		for (uint64_t i = 0; i < 0x100 && *at + i + test.Size() <= end; i++) {
			const auto* code = reinterpret_cast<const uint8_t*>(*at + i);
			if (!test.Matches(code)) continue;
			int32_t displacement = 0;
			std::memcpy(&displacement, code + 10, sizeof(displacement));
			const uint64_t target = *at + i + test.Size() + static_cast<int64_t>(displacement);
			if (target >= program->base_vaddr && target + way_out.Size() <= end &&
			    way_out.Matches(reinterpret_cast<const uint8_t*>(target))) {
				job_load = *at + JobStepLoad;
				job_exit = target + 2; // (past the int 0x41)
			}
			break;
		}
	}
	if (job_load == 0) std::printf("Demon's Souls scene guard: no physics job step of a known build found; retaining its guest code\n");
	if (!traversals && loops.empty() && dispatches.empty() && job_load == 0) return;
	const auto requested = program->base_vaddr + CaveOffset;
	const auto allocated = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    requested, PageSize, Common::VirtualMemory::Mode::ExecuteReadWrite, "demons_souls_scene_guard", true);
	if (allocated != requested) {
		if (allocated) Libs::LibKernel::Memory::FreeGuestMemory(allocated, PageSize);
		return;
	}
	g_base = program->base_vaddr;
	using namespace Xbyak::util;
	Xbyak::ClearError();
	Xbyak::CodeGenerator c(PageSize, reinterpret_cast<void*>(allocated));
	constexpr uint8_t save_fp[] {0x48, 0x0f, 0xae, 0x04, 0x24}; // FXSAVE64 [rsp]
	// A function called on a null node or object: its prologue and on, or for a null one a printed skip returning `empty`.
	const auto null_stub = [&](uint64_t entry, Empty empty) {
		Xbyak::Label skip;
		c.align(16);
		const auto* stub = c.getCurr();
		c.test(rdi, rdi); // (flags are not preserved across a call)
		c.jz(skip);
		c.db(Prologue.data(), Prologue.size());
		c.jmp(reinterpret_cast<const void*>(entry + Prologue.size()));
		// A null one: printed (registers, flags and FP state kept), then nothing found, visited or dispatched.
		c.L(skip);
		c.pushfq();
		for (const auto& reg: {rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11})
			c.push(reg);
		c.mov(rsi, ptr[rsp + 80]); // the return address
		c.mov(rdi, entry - program->base_vaddr);
		c.mov(rdx, rbx);
		c.mov(rcx, r14);
		c.mov(r8, r12);
		c.mov(r9, r15);
		c.sub(rsp, 520); // entry rsp is 8 mod 16: 80 + 520 bytes align it for FXSAVE
		c.db(save_fp, sizeof(save_fp));
		c.mov(rax, reinterpret_cast<uint64_t>(&Skipped));
		c.call(rax);
		c.fxrstor64(ptr[rsp]);
		c.add(rsp, 520);
		for (const auto& reg: {r11, r10, r9, r8, rdi, rsi, rdx, rcx, rax})
			c.pop(reg);
		c.popfq();
		if (empty == Empty::Zero) {
			c.xor_(eax, eax);
		} else {
			c.mov(rax, ptr[rcx + 8]);
			c.sub(rax, ptr[rcx]);
			c.shr(rax, empty == Empty::Count8 ? 3 : 5);
		}
		c.ret();
		return stub;
	};
	std::array<const uint8_t*, Traversals.size()> stubs {};
	for (size_t i = 0; i < Traversals.size() && traversals; i++)
		stubs[i] = null_stub(entries[i], Traversals[i].empty);
	std::vector<const uint8_t*> dispatch_stubs;
	for (const auto entry: dispatches)
		dispatch_stubs.push_back(null_stub(entry, Empty::Zero));
	// A children loop: the child's load, a null child continues with the next one, else the moved instruction and back.
	std::vector<const uint8_t*> loop_stubs;
	for (const auto& loop: loops) {
		const auto*        load = reinterpret_cast<const uint8_t*>(loop.load);
		const Xbyak::Reg64 child(static_cast<int>(loop.reg));
		Xbyak::Label       null_child;
		c.align(16);
		loop_stubs.push_back(c.getCurr());
		if (loop.owner_frame != 0) {
			c.mov(rdx, ptr[rbp + loop.owner_frame]);
			c.movsxd(rdx, dword[rdx + loop.count_offset]);
			c.cmp(Xbyak::Reg64(static_cast<int>(loop.iter)), rdx);
			c.jae(null_child, Xbyak::CodeGenerator::T_NEAR);
		}
		c.db(load, loop.load_size); // mov REG, [ITER]
		c.test(child, child);
		c.jz(null_child);
		c.db(load + loop.load_size, loop.moved - loop.load_size);
		c.jmp(reinterpret_cast<const void*>(loop.load + loop.moved));
		// Printed (registers, flags and FP state kept; below the red zone, on a realigned stack), then the next child.
		c.L(null_child);
		c.lea(rsp, ptr[rsp - 128]);
		c.pushfq();
		for (const auto& reg: {rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11, rbx})
			c.push(reg);
		c.mov(rbx, rsp);
		c.and_(rsp, -16);
		c.sub(rsp, 512);
		c.db(save_fp, sizeof(save_fp));
		c.mov(rdi, loop.load - program->base_vaddr);
		c.mov(rsi, Xbyak::Reg64(static_cast<int>(loop.iter)));
		c.mov(rdx, r14);
		c.mov(rcx, r15);
		c.mov(rax, reinterpret_cast<uint64_t>(&SkippedChild));
		c.call(rax);
		c.fxrstor64(ptr[rsp]);
		c.mov(rsp, rbx);
		for (const auto& reg: {rbx, r11, r10, r9, r8, rdi, rsi, rdx, rcx, rax})
			c.pop(reg);
		c.popfq();
		c.lea(rsp, ptr[rsp + 128]);
		c.jmp(reinterpret_cast<const void*>(loop.increment));
	}
	// The physics job step: its object's load, and a job with no object printed, then out through the step's own way out,
	// which reads only its frame (rbp, and r15: the stack guard's address). (The step's rsp is 16-byte aligned there.)
	const uint8_t* job_stub = nullptr;
	if (job_load != 0) {
		Xbyak::Label no_object;
		c.align(16);
		job_stub = c.getCurr();
		c.mov(rcx, ptr[r12 + 0x38]);
		c.test(rcx, rcx);
		c.jz(no_object);
		c.mov(rdx, ptr[rcx + 0x18]);
		c.jmp(reinterpret_cast<const void*>(job_load + JobStepMoved));
		c.L(no_object);
		c.mov(rdi, job_load - program->base_vaddr);
		c.mov(rsi, r12);
		c.mov(rax, reinterpret_cast<uint64_t>(&SkippedJob));
		c.call(rax);
		c.jmp(reinterpret_cast<const void*>(job_exit));
	}
	c.ready();
	if (Xbyak::GetError() || !Common::VirtualMemory::FlushInstructionCache(allocated, c.getSize()) ||
	    !Libs::LibKernel::Memory::ProtectGuestMemory(allocated, PageSize, Common::VirtualMemory::Mode::ExecuteRead, nullptr)) {
		Libs::LibKernel::Memory::FreeGuestMemory(allocated, PageSize);
		return;
	}
	// Installation occurs before module initializers or guest worker threads run.
	const auto patch = [](uint64_t at, const uint8_t* stub, uint32_t size) {
		std::array<uint8_t, 16> code {};
		code.fill(0xcc); // (the moved bytes after the jump are never reached: nothing jumps into them)
		code[0]                 = 0xe9;
		const auto displacement = static_cast<int32_t>(reinterpret_cast<uint64_t>(stub) - (at + 5));
		std::memcpy(code.data() + 1, &displacement, sizeof(displacement));
		std::memcpy(reinterpret_cast<void*>(at), code.data(), size);
		Common::VirtualMemory::FlushInstructionCache(at, size);
	};
	for (size_t i = 0; i < Traversals.size() && traversals; i++)
		patch(entries[i], stubs[i], 5);
	for (size_t i = 0; i < loops.size(); i++)
		patch(loops[i].load, loop_stubs[i], loops[i].moved);
	for (size_t i = 0; i < dispatches.size(); i++)
		patch(dispatches[i], dispatch_stubs[i], 5);
	if (job_load != 0) patch(job_load, job_stub, JobStepMoved);
	if (traversals) {
		std::string at;
		for (const auto entry: entries) {
			char text[24];
			std::snprintf(text, sizeof(text), "%s0x%llx", at.empty() ? "" : ", ", static_cast<unsigned long long>(entry - program->base_vaddr));
			at += text;
		}
		std::printf("Demon's Souls scene guard: installed (traversals at eboot+%s skip a null node)\n", at.c_str());
	}
	for (const auto& loop: loops)
		std::printf("Demon's Souls scene guard: installed (the children loop at eboot+0x%llx skips a null child)\n",
		            static_cast<unsigned long long>(loop.load - program->base_vaddr));
	for (const auto entry: dispatches)
		std::printf("Demon's Souls scene guard: installed (the event dispatch at eboot+0x%llx skips a null object)\n",
		            static_cast<unsigned long long>(entry - program->base_vaddr));
	if (job_load != 0)
		std::printf("Demon's Souls scene guard: installed (the physics job step at eboot+0x%llx leaves a job with no object)\n",
		            static_cast<unsigned long long>(job_load - program->base_vaddr));
#endif
}
} // namespace Loader::DemonsSoulsSceneGuard

