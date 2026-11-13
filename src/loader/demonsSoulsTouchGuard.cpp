// Demon's Souls' TouchManager keeps its touch interval nodes in a swept list (next at node + 0x50, prev at + 0x58; the
// list head is a node too, so a node in the list always has a prev). A job worker's loop over a list of entries
// removes each entry's node from it with STouchIntervalNode::RemoveFromSweptList(), inlined. 01.005.000 unlinks
// without a check and crashed on a node with prev and next 0 (eboot+0xd8c1f5, mov [rcx + 0x50], rdx with rcx 0: a
// write to 0x50, "BPE JobWorkerThread", after 10 minutes at 1-1; issue #10). 01.007.000 checks prev first: a node in
// no list is reported ("TouchManager: Attempted to remove a node that was not in the swept list.", an assert of
// debug builds) and the loop goes on with the next entry (eboot+0xdabbb6). The same check is patched into 01.005.000's
// loop here: the 35 bytes from the load of next, the 8-byte nop that aligns the block after it making room.
#include "loader/demonsSoulsTouchGuard.h"

#include "common/virtualMemory.h"
#include "loader/demonsSoulsTitle.h"
#include "loader/guestCode.h"
#include "loader/runtimeLinker.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace Loader::DemonsSoulsTouchGuard {
namespace {

// 01.005.000: L: mov rbx, [rbx + 0x18]; cmp rbx, r13; je <end>; mov rax, [rbx + 8]; cmp rbx, rax; je +0x23 (an entry
// without a node); mov rdx, [rax + 0x50]; mov rcx, [rax + 0x58]; mov [rcx + 0x50], rdx; mov rax, [rax + 0x50];
// test rax, rax; je L; mov [rax + 0x58], rcx; jmp L; nop8; then the entry without a node: cmp dword [rbx + 0x44], 0;
// mov r15, [rbx + 0x48].
constexpr const char* Loop = "48 8b 5b 18 4c 39 eb 0f 84 ?? ?? ?? ?? 48 8b 43 08 48 39 c3 74 23 48 8b 50 50 48 8b 48 58 48 89 51 50 "
                             "48 8b 40 50 48 85 c0 74 d5 48 89 48 58 eb cf 0f 1f 84 00 00 00 00 00 83 7b 44 00 4c 8b 7b 48";
constexpr uint64_t RemovalOffset = 22; // the load of next

// mov rdx, [rax + 0x50]; mov rcx, [rax + 0x58]; test rcx, rcx; je L; mov [rcx + 0x50], rdx; mov rax, [rax + 0x50];
// test rax, rax; je L; mov [rax + 0x58], rcx; jmp L; nop3: the removal as it was, after a check of prev.
constexpr std::array<uint8_t, 35> CheckedRemoval {0x48, 0x8b, 0x50, 0x50, 0x48, 0x8b, 0x48, 0x58, 0x48, 0x85, 0xc9, 0x74,
                                                  0xdd, 0x48, 0x89, 0x51, 0x50, 0x48, 0x8b, 0x40, 0x50, 0x48, 0x85, 0xc0,
                                                  0x74, 0xd0, 0x48, 0x89, 0x48, 0x58, 0xeb, 0xca, 0x0f, 0x1f, 0x00};

} // namespace

void Install(Program* program) {
#if defined(__x86_64__) || defined(_M_X64)
	if (program == nullptr || !Loader::DemonsSoulsTitle::IsSupportedGame() || program->file_name.filename() != "eboot.bin")
		return;
	// 01.007.000 has the check: no match there.
	const auto loop = GuestCode::FindUnique(*program, GuestCode::Pattern(Loop));
	if (!loop) return;
	// Installation occurs before module initializers or guest threads run; program memory is writable to the host.
	const auto removal = *loop + RemovalOffset;
	std::memcpy(reinterpret_cast<void*>(removal), CheckedRemoval.data(), CheckedRemoval.size());
	Common::VirtualMemory::FlushInstructionCache(removal, CheckedRemoval.size());
	std::printf("Demon's Souls touch guard: installed (the swept list removal at eboot+0x%llx skips a node in no list, as "
	            "01.007.000 does)\n",
	            static_cast<unsigned long long>(removal - program->base_vaddr));
#endif
}

} // namespace Loader::DemonsSoulsTouchGuard
