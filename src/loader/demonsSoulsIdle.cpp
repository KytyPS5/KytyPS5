#include "loader/demonsSoulsIdle.h"

#include "common/hackNotice.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "loader/guestCode.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"

#include <array>
#include <cstring>
#include <fmt/format.h>
#include <string>

namespace Loader::DemonsSoulsIdle {
namespace {

constexpr uint64_t PageSize = 0x4000, CaveOffset = 0x8000000;
// The sleep of a worker that found no work. Spinning, ~12 workers take a CPU each all through
// play, as many as the host has cores beside the emulator's own threads.
constexpr uint32_t SleepMicros = 50;

void KYTY_SYSV_ABI Sleep() {
	Common::Thread::SleepMicro(SleepMicros);
}

// Demon's Souls (PPSA01340, PPSA01341, PPSA01342).
bool IsDemonsSouls() {
	static const bool demons_souls = [] {
		std::string title;
		return SystemContentParamSfoGetString("TITLE_ID", &title) &&
		       (title == "PPSA01340" || title == "PPSA01341" || title == "PPSA01342");
	}();
	return demons_souls;
}

// The call at `call`'s target, when it lies in the program.
uint64_t CallTarget(const Program& program, uint64_t call) {
	int32_t displacement = 0;
	std::memcpy(&displacement, reinterpret_cast<const void*>(call + 1), sizeof(displacement));
	const uint64_t target = call + 5 + static_cast<int64_t>(displacement);
	return target >= program.base_vaddr && target < program.base_vaddr + program.mapped_size
	           ? target
	           : 0;
}

// The idle loop's call of the poll (one such call site in the game's code) and the poll it calls.
bool FindPollCall(const Program& program, uint64_t* call, uint64_t* poll) {
	const auto site = GuestCode::FindUnique(program, GuestCode::Pattern(IdleCall));
	if (!site) {
		return false;
	}
	*call = *site + IdleCallAt;
	*poll = CallTarget(program, *call);
	const GuestCode::Pattern prologue(PollPrologue);
	return *poll != 0 && *poll + prologue.Size() <= program.base_vaddr + program.mapped_size &&
	       GuestCode::Readable(*poll, prologue.Size()) &&
	       prologue.Matches(reinterpret_cast<const uint8_t*>(*poll));
}

// The wait for work's call of the TSC helper (one such call in the game's code), and the helper.
bool FindSpinCall(const Program& program, uint64_t* call, uint64_t* helper) {
	const auto site = GuestCode::FindUnique(program, GuestCode::Pattern(SpinCall));
	if (!site) {
		return false;
	}
	*call   = *site + SpinCallAt;
	*helper = CallTarget(program, *call);
	return *helper != 0;
}

// Points the call at `call` to `target`.
bool Redirect(uint64_t call, uint64_t target) {
	std::array<uint8_t, 5> bytes {0xe8};
	const auto             displacement = static_cast<int32_t>(target - (call + 5));
	std::memcpy(bytes.data() + 1, &displacement, sizeof(displacement));
	std::array<uint8_t, 5> original {};
	std::memcpy(original.data(), reinterpret_cast<const void*>(call), original.size());
	// The main program's code stays writable for loader patches; no guest thread runs yet.
	std::memcpy(reinterpret_cast<void*>(call), bytes.data(), bytes.size());
	if (Common::VirtualMemory::FlushInstructionCache(call, bytes.size())) {
		return true;
	}
	std::memcpy(reinterpret_cast<void*>(call), original.data(), original.size());
	(void)Common::VirtualMemory::FlushInstructionCache(call, original.size());
	return false;
}

} // namespace

void Install(Program* program) {
#if defined(__x86_64__) || defined(_M_X64)
	if (program == nullptr || !IsDemonsSouls() || program->file_name.filename() != "eboot.bin" ||
	    program->mapped_size > CaveOffset ||
	    program->base_vaddr > UINT64_MAX - CaveOffset - PageSize) {
		return;
	}
	uint64_t   poll_call = 0, poll = 0, spin_call = 0, helper = 0;
	const bool has_poll = FindPollCall(*program, &poll_call, &poll);
	const bool has_spin = FindSpinCall(*program, &spin_call, &helper);
	if (!has_poll && !has_spin) {
		Log::WriteToConsoleAndLog(
		    "Demon's Souls idle job workers: no idle loop of a known build found\n");
		return;
	}
	// The thunks lie within a call's reach of the code.
	const auto requested = program->base_vaddr + CaveOffset;
	const auto cave      = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    requested, PageSize, Common::VirtualMemory::Mode::ExecuteReadWrite,
	    "demons_souls_idle_workers", true);
	if (cave != requested) {
		if (cave != 0) {
			(void)Libs::LibKernel::Memory::FreeGuestMemory(cave, PageSize);
		}
		return;
	}
	Xbyak::ClearError();
	Xbyak::CodeGenerator code(PageSize, reinterpret_cast<void*>(cave));
	uint64_t             poll_thunk = 0, spin_thunk = 0;
	if (has_poll) {
		poll_thunk = reinterpret_cast<uint64_t>(code.getCurr());
		EmitPollThunk(code, reinterpret_cast<const void*>(poll),
		              reinterpret_cast<const void*>(&Sleep));
	}
	if (has_spin) {
		code.align(16);
		spin_thunk = reinterpret_cast<uint64_t>(code.getCurr());
		EmitSpinThunk(code, reinterpret_cast<const void*>(&Sleep),
		              reinterpret_cast<const void*>(helper));
	}
	code.ready();
	if (Xbyak::GetError() != 0 ||
	    !Common::VirtualMemory::FlushInstructionCache(cave, code.getSize()) ||
	    !Libs::LibKernel::Memory::ProtectGuestMemory(cave, PageSize,
	                                                 Common::VirtualMemory::Mode::ExecuteRead)) {
		(void)Libs::LibKernel::Memory::FreeGuestMemory(cave, PageSize);
		return;
	}
	const bool poll_installed = has_poll && Redirect(poll_call, poll_thunk);
	const bool spin_installed = has_spin && Redirect(spin_call, spin_thunk);
	if (!poll_installed && !spin_installed) {
		(void)Libs::LibKernel::Memory::FreeGuestMemory(cave, PageSize);
		return;
	}
	Log::WriteToConsoleAndLog(
	    fmt::format("Demon's Souls idle job workers: sleep {} us when idle (poll at eboot+0x{:x}: "
	                "{}, TSC read at eboot+0x{:x}: {})\n",
	                SleepMicros, poll_call - program->base_vaddr, poll_installed ? "yes" : "no",
	                spin_call - program->base_vaddr, spin_installed ? "yes" : "no"));
	Common::HackNotice::Report("Demon's Souls idle job workers sleep instead of spinning",
	                           "game code", 0, poll_installed ? poll_call : spin_call,
	                           "job workers' wait for work");
#else
	(void)program;
#endif
}

} // namespace Loader::DemonsSoulsIdle
