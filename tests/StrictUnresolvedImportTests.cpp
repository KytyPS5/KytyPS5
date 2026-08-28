#include "common/emulatorConfig.h"
#include "loader/unresolvedImportPolicy.h"

#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "StrictUnresolvedImportTests: failed: %s\n", message);
		std::abort();
	}
}

void TestConfigurationIsOptIn() {
	Config::Initialize();
	Check(!Config::StrictUnresolvedImportsEnabled(), "strict mode was enabled by default");

	Config::ConfigOptions options;
	options.strict_unresolved_imports = true;
	Config::Load(options);
	Check(Config::StrictUnresolvedImportsEnabled(), "strict mode was not loaded");
	Config::Shutdown();
}

void TestDiagnosticIncludesRelocationContext() {
	Loader::StrictUnresolvedImportDetails details;
	details.symbol           = "EwB7Qa7y2Rk[libSceExample]";
	details.type             = "Func";
	details.bind             = "Global";
	details.program          = "/app0/eboot.bin";
	details.patch_vaddr      = 0x1234abcd;
	details.relocation_index = 17;

	const auto message = Loader::FormatStrictUnresolvedImportError(3, &details);
	Check(message == "strict unresolved import: symbol=EwB7Qa7y2Rk[libSceExample] "
	                 "type=Func "
	                 "bind=Global program=/app0/eboot.bin patch_vaddr=0x000000001234abcd "
	                 "jmprela_index=17 record_id=3",
	      "valid record diagnostic lost context");
}

void TestInvalidRecordDiagnostic() {
	const auto message = Loader::FormatStrictUnresolvedImportError(99, nullptr);
	Check(message == "strict unresolved import: invalid record_id=99",
	      "invalid record diagnostic was ambiguous");
}

} // namespace

int main() {
	TestConfigurationIsOptIn();
	TestDiagnosticIncludesRelocationContext();
	TestInvalidRecordDiagnostic();
	return 0;
}
