#include "libs/libcStdio.h"
#include "loader/symbolDatabase.h"

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <vector>
#endif
#endif

namespace {

using Libs::LibcStdio::BindGuestLibc;
using Libs::LibcStdio::Fflush;
using Libs::LibcStdio::HasGuestFflush;
using Libs::LibcStdio::RefreshGuestLibc;
using Libs::LibcStdio::ResetGuestLibc;
using Libs::LibcStdio::Slots;
using Loader::SymbolDatabase;
using Loader::SymbolResolve;
using Loader::SymbolType;

// Addresses that cannot collide with each other or with a null default.
constexpr uint64_t STDIN_ADDRESS  = 0x7000'0000'1000;
constexpr uint64_t STDOUT_ADDRESS = 0x7000'0000'2000;
constexpr uint64_t STDERR_ADDRESS = 0x7000'0000'3000;

// Values a libc.prx would store in its own __stdinp, __stdoutp and __stderrp variables.
constexpr uint64_t DEFINED_STDIN_VALUE  = 0x5000'0000'1000;
constexpr uint64_t DEFINED_STDOUT_VALUE = 0x5000'0000'2000;

// Owners are only compared by identity, so any distinct addresses do.
int g_owner_a = 0;
int g_owner_b = 0;

constexpr int GUEST_FFLUSH_RESULT = 1234;
// Exit status that DbgExit gives a fatal error.
constexpr unsigned      FATAL_EXIT_STATUS = 321;
constexpr unsigned long DEATH_TIMEOUT_MS  = 10000;

int   g_guest_fflush_calls = 0;
FILE* g_guest_fflush_arg   = nullptr;

// Variables standing in for guest memory that libc.prx defines.
uint64_t g_defined_stdinp  = DEFINED_STDIN_VALUE;
uint64_t g_defined_stdoutp = DEFINED_STDOUT_VALUE;
uint64_t g_zero_stdoutp    = 0;
uint64_t g_late_stdoutp    = 0;

/// Aborts the test run with a message when a condition does not hold.
void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "LibcStdioTests: failed: %s\n", text);
		std::abort();
	}
}

/// Stands in for the guest libc fflush and records how it was called.
KYTY_SYSV_ABI int FakeGuestFflush(FILE* stream) {
	g_guest_fflush_calls++;
	g_guest_fflush_arg = stream;
	return GUEST_FFLUSH_RESULT;
}

/// Adds one export to a fake libc.prx export table.
void AddExport(SymbolDatabase& exports, const char* nid, SymbolType type, uint64_t vaddr) {
	SymbolResolve sr {};
	sr.name                 = nid;
	sr.library              = "libc";
	sr.library_version      = 1;
	sr.module               = "libc";
	sr.module_version_major = 1;
	sr.module_version_minor = 1;
	sr.type                 = type;
	exports.Add(sr, vaddr);
}

/// Builds a libc.prx export table with all three FILE objects and a fflush.
void FillFullExports(SymbolDatabase& exports) {
	AddExport(exports, "1TDo-ImqkJc", SymbolType::Object, STDIN_ADDRESS);
	AddExport(exports, "2sWzhYqFH4E", SymbolType::Object, STDOUT_ADDRESS);
	AddExport(exports, "H8AprKeZtNg", SymbolType::Object, STDERR_ADDRESS);
	AddExport(exports, "MUjC4lbHrK4", SymbolType::Func,
	          reinterpret_cast<uint64_t>(&FakeGuestFflush));
}

/// Leaves non-zero slots, a guest fflush and an owner behind, as a previous run would.
/// Tests that expect nulls would otherwise pass on a fresh process without proving anything.
void SeedStaleState() {
	static SymbolDatabase stale;
	static const bool     filled = (FillFullExports(stale), true);
	(void)filled;
	BindGuestLibc(&stale, &g_owner_a);
	Check(Slots().stdout_file != 0 && HasGuestFflush(), "stale state was seeded");
}

/// Binding a full export table fills every slot with the matching FILE object address.
void TestBindFillsSlots() {
	SeedStaleState();
	SymbolDatabase exports;
	FillFullExports(exports);
	BindGuestLibc(&exports, &g_owner_a);

	Check(Slots().stdin_file == STDIN_ADDRESS, "stdin slot holds _Stdin address");
	Check(Slots().stdout_file == STDOUT_ADDRESS, "stdout slot holds _Stdout address");
	Check(Slots().stderr_file == STDERR_ADDRESS, "stderr slot holds _Stderr address");
	Check(HasGuestFflush(), "guest fflush is bound");
}

/// A missing export leaves only its own slot null.
void TestBindKeepsMissingExportNull() {
	SeedStaleState();
	// Only stdout is exported, so the other two must not borrow its address.
	SymbolDatabase exports;
	AddExport(exports, "2sWzhYqFH4E", SymbolType::Object, STDOUT_ADDRESS);
	BindGuestLibc(&exports, &g_owner_a);

	Check(Slots().stdout_file == STDOUT_ADDRESS, "present export is bound");
	Check(Slots().stdin_file == 0, "missing _Stdin stays null");
	Check(Slots().stderr_file == 0, "missing _Stderr stays null");
	Check(!HasGuestFflush(), "missing fflush stays unbound");
}

/// An export of the wrong symbol type must not satisfy the Object lookup.
void TestBindIgnoresWrongSymbolType() {
	SeedStaleState();
	SymbolDatabase exports;
	AddExport(exports, "2sWzhYqFH4E", SymbolType::Func, STDOUT_ADDRESS);
	BindGuestLibc(&exports, &g_owner_a);

	Check(Slots().stdout_file == 0, "Func export does not bind an Object slot");
}

/// Binding nothing clears slots and fflush that an earlier bind set.
void TestBindWithoutLibcClearsState() {
	SeedStaleState();
	BindGuestLibc(nullptr, nullptr);

	Check(Slots().stdout_file == 0, "no libc.prx leaves stdout null");
	Check(Slots().stdin_file == 0, "no libc.prx leaves stdin null");
	Check(Slots().stderr_file == 0, "no libc.prx leaves stderr null");
	Check(!HasGuestFflush(), "no libc.prx leaves no guest fflush");
}

/// A slot variable defined by libc.prx supplies its value, and the others fall back to FILE.
void TestBindHonoursDefinedVariable() {
	SeedStaleState();
	SymbolDatabase exports;
	FillFullExports(exports);
	AddExport(exports, "zqJhBxAKfsc", SymbolType::Object,
	          reinterpret_cast<uint64_t>(&g_defined_stdoutp));
	AddExport(exports, "bgAcsbcEznc", SymbolType::Object,
	          reinterpret_cast<uint64_t>(&g_defined_stdinp));
	BindGuestLibc(&exports, &g_owner_a);

	Check(Slots().stdout_file == DEFINED_STDOUT_VALUE, "defined __stdoutp value is copied");
	Check(Slots().stdin_file == DEFINED_STDIN_VALUE, "defined __stdinp value is copied");
	Check(Slots().stderr_file == STDERR_ADDRESS, "undefined __stderrp falls back to _Stderr");
}

/// A defined variable that still holds null must not hide the FILE object.
void TestBindIgnoresUnsetDefinedVariable() {
	SeedStaleState();
	SymbolDatabase exports;
	FillFullExports(exports);
	AddExport(exports, "zqJhBxAKfsc", SymbolType::Object,
	          reinterpret_cast<uint64_t>(&g_zero_stdoutp));
	BindGuestLibc(&exports, &g_owner_a);

	Check(Slots().stdout_file == STDOUT_ADDRESS, "null defined variable falls back to _Stdout");
}

/// A variable that is null at bind time and set by libc init is picked up by the refresh,
/// and a refresh for another module or without exports changes nothing.
void TestRefreshPicksUpLateVariable() {
	SeedStaleState();
	SymbolDatabase exports;
	FillFullExports(exports);
	AddExport(exports, "zqJhBxAKfsc", SymbolType::Object,
	          reinterpret_cast<uint64_t>(&g_late_stdoutp));
	g_late_stdoutp = 0;
	BindGuestLibc(&exports, &g_owner_a);
	Check(Slots().stdout_file == STDOUT_ADDRESS, "null variable falls back at bind time");

	g_late_stdoutp = DEFINED_STDOUT_VALUE;
	RefreshGuestLibc(&exports, &g_owner_b);
	Check(Slots().stdout_file == STDOUT_ADDRESS, "refresh for another module changes nothing");

	RefreshGuestLibc(&exports, &g_owner_a);
	Check(Slots().stdout_file == DEFINED_STDOUT_VALUE, "late variable value is copied");
	Check(Slots().stdin_file == STDIN_ADDRESS, "slots without a defined variable are kept");
	g_late_stdoutp = 0;
}

/// Resetting for another module keeps the binding and resetting for the owner clears it.
void TestResetFollowsOwner() {
	SeedStaleState();
	ResetGuestLibc(&g_owner_b);
	Check(Slots().stdout_file != 0 && HasGuestFflush(), "other module unload keeps the binding");

	ResetGuestLibc(&g_owner_a);
	Check(Slots().stdin_file == 0 && Slots().stdout_file == 0 && Slots().stderr_file == 0,
	      "owner unload clears the slots");
	Check(!HasGuestFflush(), "owner unload clears fflush");

	SeedStaleState();
	ResetGuestLibc(nullptr);
	Check(Slots().stdout_file == 0 && !HasGuestFflush(), "null owner clears unconditionally");
}

/// The host stdout stays on the host and every other stream goes to the guest fflush.
void TestFflushRouting() {
	SeedStaleState();
	SymbolDatabase exports;
	FillFullExports(exports);
	BindGuestLibc(&exports, &g_owner_a);

	g_guest_fflush_calls = 0;
	Check(Fflush(stdout, stdout) == 0, "host stdout flushes on the host");
	Check(g_guest_fflush_calls == 0, "host stdout never reaches the guest fflush");

	auto* guest_file = reinterpret_cast<FILE*>(static_cast<uintptr_t>(STDERR_ADDRESS));
	Check(Fflush(guest_file, stdout) == GUEST_FFLUSH_RESULT, "guest stream uses guest fflush");
	Check(g_guest_fflush_calls == 1, "guest fflush called once");
	Check(g_guest_fflush_arg == guest_file, "guest fflush receives the same stream");

	Check(Fflush(nullptr, stdout) == GUEST_FFLUSH_RESULT, "fflush(NULL) goes to the guest");
	Check(g_guest_fflush_calls == 2 && g_guest_fflush_arg == nullptr,
	      "guest fflush receives the null stream");
}

/// Child process body. With no guest fflush a guest stream must hit the fatal exit.
[[noreturn]] void RunFflushDeathCase() {
	BindGuestLibc(nullptr, nullptr);
	auto* guest_file = reinterpret_cast<FILE*>(static_cast<uintptr_t>(STDERR_ADDRESS));
	Fflush(guest_file, stdout);
	// Reaching here means Fflush returned without the fatal exit.
	std::_Exit(0x7f);
}

/// Runs this binary again with --death and returns its exit status, or -1 on a launch failure.
long RunDeathChild() {
#if defined(_WIN32)
	char path[MAX_PATH] {};
	if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0) {
		return -1;
	}
	std::string         command = std::string("\"") + path + "\" --death";
	STARTUPINFOA        startup {sizeof(startup)};
	PROCESS_INFORMATION process {};
	if (CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
	                   nullptr, &startup, &process) == 0) {
		return -1;
	}
	long exit_code = -1;
	if (WaitForSingleObject(process.hProcess, DEATH_TIMEOUT_MS) != WAIT_OBJECT_0) {
		// A hung child must not outlive the test.
		TerminateProcess(process.hProcess, 1);
	} else {
		DWORD code = 0;
		exit_code = GetExitCodeProcess(process.hProcess, &code) != 0 ? static_cast<long>(code) : -1;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return exit_code;
#else
#if defined(__APPLE__)
	// macOS has no /proc, so ask dyld for the path of this binary.
	std::vector<char> path(PATH_MAX);
	uint32_t          path_size = static_cast<uint32_t>(path.size());
	if (_NSGetExecutablePath(path.data(), &path_size) != 0) {
		path.resize(path_size);
		if (_NSGetExecutablePath(path.data(), &path_size) != 0) {
			return -1;
		}
	}
#endif
	const pid_t pid = ::fork();
	if (pid == 0) {
#if defined(__APPLE__)
		::execl(path.data(), "LibcStdioTests", "--death", nullptr);
#else
		::execl("/proc/self/exe", "LibcStdioTests", "--death", nullptr);
#endif
		std::_Exit(0x7e);
	}
	int status = 0;
	if (pid < 0 || ::waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
		return -1;
	}
	return WEXITSTATUS(status) == static_cast<int>(FATAL_EXIT_STATUS & 0xff)
	           ? static_cast<long>(FATAL_EXIT_STATUS)
	           : WEXITSTATUS(status);
#endif
}

/// Without a guest fflush, a guest stream is a clean fatal exit and not a null call.
void TestFflushWithoutGuestAborts() {
	Check(RunDeathChild() == static_cast<long>(FATAL_EXIT_STATUS),
	      "fflush of a guest stream without guest fflush exits through the fatal path");
}

} // namespace

int main(int argc, char** argv) {
	if (argc == 2 && std::strcmp(argv[1], "--death") == 0) {
		RunFflushDeathCase();
	}

	TestBindFillsSlots();
	TestBindKeepsMissingExportNull();
	TestBindIgnoresWrongSymbolType();
	TestBindWithoutLibcClearsState();
	TestBindHonoursDefinedVariable();
	TestBindIgnoresUnsetDefinedVariable();
	TestRefreshPicksUpLateVariable();
	TestResetFollowsOwner();
	TestFflushRouting();
	TestFflushWithoutGuestAborts();
	std::puts("LibcStdioTests: ok");
	return 0;
}
