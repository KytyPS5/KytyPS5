#include "common/threads.h"
#include "libs/libcStdio.h"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace Libs::LibC {
void InitLibC_1(Loader::SymbolDatabase* s);
} // namespace Libs::LibC

namespace {

using Libs::LibcStdio::Slots;
using Loader::SymbolDatabase;
using Loader::SymbolResolve;
using Loader::SymbolType;

constexpr uint64_t FAKE_STDOUT_FILE = 0x7000'0000'2000;

/// Aborts the test run with a message when a condition does not hold.
void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "LibcRegistrationTests: failed: %s\n", text);
		std::abort();
	}
}

/// Returns the address that the HLE libc registered for an Object NID, or 0 when absent.
uint64_t RegisteredObject(const SymbolDatabase& db, const char* nid) {
	const auto* record = db.FindByNid(nid, SymbolType::Object);
	return record != nullptr ? record->vaddr : 0;
}

/// The HLE export must be the address of the slot, because the game's import slot holds
/// that address and the game dereferences twice. A copy of the value would break it.
void TestSlotsAreRegisteredByAddress() {
	SymbolDatabase db;
	Libs::LibC::InitLibC_1(&db);

	Check(RegisteredObject(db, "bgAcsbcEznc") == reinterpret_cast<uint64_t>(&Slots().stdin_file),
	      "__stdinp is registered as the address of the stdin slot");
	Check(RegisteredObject(db, "zqJhBxAKfsc") == reinterpret_cast<uint64_t>(&Slots().stdout_file),
	      "__stdoutp is registered as the address of the stdout slot");
	Check(RegisteredObject(db, "as8Od-tH1BI") == reinterpret_cast<uint64_t>(&Slots().stderr_file),
	      "__stderrp is registered as the address of the stderr slot");
}

/// The linker's named bind step fills the registered slots from a program's exports, and
/// binding no program leaves them null again.
void TestLinkerBindStep() {
	Loader::RuntimeLinker linker;
	Loader::Program       libc;
	libc.export_symbols = std::make_unique<SymbolDatabase>();
	SymbolResolve sr {};
	sr.name                 = "2sWzhYqFH4E";
	sr.library              = "libc";
	sr.library_version      = 1;
	sr.module               = "libc";
	sr.module_version_major = 1;
	sr.module_version_minor = 1;
	sr.type                 = SymbolType::Object;
	libc.export_symbols->Add(sr, FAKE_STDOUT_FILE);

	linker.BindGuestLibc(&libc);
	Check(Slots().stdout_file == FAKE_STDOUT_FILE, "linker bind step fills the stdout slot");

	linker.BindGuestLibc(nullptr);
	Check(Slots().stdout_file == 0, "linker bind step with no libc clears the slot");
}

} // namespace

int main() {
	// The linker destructor asserts that it runs on the main thread.
	Common::InitializeThreads();
	TestSlotsAreRegisteredByAddress();
	TestLinkerBindStep();
	std::puts("LibcRegistrationTests: ok");
	return 0;
}
