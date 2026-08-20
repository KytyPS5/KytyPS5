#include "common/diagnostics.h"
#include "graphics/host_gpu/vulkanDiagnostics.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "DiagnosticsTests: failed: %s\n", text);
		std::abort();
	}
}

bool Contains(std::string_view haystack, std::string_view needle) {
	return haystack.find(needle) != std::string_view::npos;
}

void TestTheReportSaysWhoAndWhat() {
	const auto report = Common::Diagnostics::BuildReport();

	Check(!report.empty(), "the report is not empty");
	Check(Contains(report, "KytyPS5 diagnostics"), "the report identifies itself");
	Check(Contains(report, "Build"), "the report has a build section");
	Check(Contains(report, "Host"), "the report has a host section");
	Check(Contains(report, Common::Diagnostics::BuildString()),
	      "the report carries the same build string --help prints");
}

// The report exists to be pasted into a public issue. If it leaked a home
// directory or a login name, every reporter would have to audit it first, and
// most would not. So this is a test, not a comment.
void TestTheReportLeaksNothingPersonal() {
	const auto report = Common::Diagnostics::BuildReport();

	for (const char* variable: {"HOME", "USER", "USERNAME", "LOGNAME", "USERPROFILE"}) {
		const char* value = std::getenv(variable);
		if (value == nullptr || std::strlen(value) < 3) {
			continue;
		}
		if (!Contains(report, value)) {
			continue;
		}
		std::fprintf(stderr, "DiagnosticsTests: the report contains $%s (\"%s\")\n", variable,
		             value);
		std::abort();
	}

	// No absolute paths either: the build string carries a date and a git hash,
	// never a source directory.
	Check(!Contains(report, "/home/"), "no POSIX home path");
	Check(!Contains(report, "/Users/"), "no macOS home path");
	Check(!Contains(report, "C:\\Users"), "no Windows home path");
}

// A machine that cannot run the emulator is exactly the machine a report is
// wanted from, so this must produce something useful with no GPU, no driver and
// no Vulkan loader at all rather than failing.
void TestTheVulkanSectionAlwaysReports() {
	const auto report = Libs::Graphics::BuildVulkanReport();

	Check(!report.empty(), "the Vulkan section is not empty");
	Check(Contains(report, "Vulkan"), "the Vulkan section is labelled");

	const bool says_something =
	    Contains(report, "device(s)") || Contains(report, "no Vulkan devices") ||
	    Contains(report, "no Vulkan loader") || Contains(report, "could not create") ||
	    Contains(report, "unusable") || Contains(report, "enumeration failed");
	Check(says_something, "the Vulkan section states an outcome either way");
}

void TestItIsStable() {
	// Two calls in a row must agree; a report that changes under the reporter is
	// not evidence of anything.
	Check(Common::Diagnostics::BuildReport() == Common::Diagnostics::BuildReport(),
	      "the host report is stable");
}

} // namespace

int main() {
	TestTheReportSaysWhoAndWhat();
	TestTheReportLeaksNothingPersonal();
	TestTheVulkanSectionAlwaysReports();
	TestItIsStable();

	// Printed so a human can read what a reporter would actually paste.
	std::printf("--- what --diagnostics prints ---\n%s%s",
	            Common::Diagnostics::BuildReport().c_str(),
	            Libs::Graphics::BuildVulkanReport().c_str());
	std::printf("DiagnosticsTests: all passed\n");
	return 0;
}
