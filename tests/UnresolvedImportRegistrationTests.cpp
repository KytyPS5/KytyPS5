#include "loader/symbolDatabase.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace Libs {
namespace LibRazorCpu {
void InitRazorCpu_1(Loader::SymbolDatabase* s);
} // namespace LibRazorCpu
void InitConvertKeycode_1(Loader::SymbolDatabase* s);
} // namespace Libs

namespace {

int g_failures = 0;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "UnresolvedImportRegistrationTests: failed: %s\n", text);
		g_failures++;
	}
}

// RE9 imports these by NID under an exact library/module/version triple; the loader matches on the
// whole generated string, so a wrong module name or version leaves the import unresolved.
void CheckRegistered(const Loader::SymbolDatabase& db, const char* nid, const char* expected_name,
                     const char* text) {
	const auto* record = db.FindByNid(nid, Loader::SymbolType::Func);
	Check(record != nullptr, text);
	if (record != nullptr) {
		Check(record->name == expected_name, text);
		Check(record->vaddr != 0, text);
	}
}

void TestRazorCpuImportsAreRegistered() {
	Loader::SymbolDatabase db;
	Libs::LibRazorCpu::InitRazorCpu_1(&db);

	CheckRegistered(db, "EboejOQvLL4", "EboejOQvLL4[RazorCpu_v1][RazorCpu_v1.1][Func]",
	                "sceRazorCpu capture query is not registered as RE9 imports it");
	CheckRegistered(db, "Ax7NjOzctIM", "Ax7NjOzctIM[RazorCpu_v1][RazorCpu_v1.1][Func]",
	                "RazorCpu Ax7NjOzctIM is not registered as RE9 imports it");
}

void TestConvertKeycodeImportsAreRegistered() {
	Loader::SymbolDatabase db;
	Libs::InitConvertKeycode_1(&db);

	CheckRegistered(db, "mUuUOWI-C+0", "mUuUOWI-C+0[ConvertKeycode_v1][ConvertKeycode_v1.0][Func]",
	                "ConvertKeycode keyboard-type query is not registered as RE9 imports it");
	CheckRegistered(db, "QjGCaJbRib4", "QjGCaJbRib4[ConvertKeycode_v1][ConvertKeycode_v1.0][Func]",
	                "ConvertKeycode QjGCaJbRib4 is not registered as RE9 imports it");
}

} // namespace

int main() {
	TestRazorCpuImportsAreRegistered();
	TestConvertKeycodeImportsAreRegistered();

	if (g_failures != 0) {
		std::fprintf(stderr, "UnresolvedImportRegistrationTests: %d failure(s)\n", g_failures);
		std::abort();
	}
	std::printf("UnresolvedImportRegistrationTests: all cases passed\n");
	return 0;
}
