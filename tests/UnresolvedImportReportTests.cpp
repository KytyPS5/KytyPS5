#include "loader/unresolvedImportReport.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>

namespace {

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "UnresolvedImportReportTests: failed: %s\n", text);
		std::abort();
	}
}

void TestDeterministicGrouping() {
	const std::vector<Loader::UnresolvedImportReportEntry> entries = {
	    {"z.prx", "NID-B", "NID-B[lib]", "Function", "Weak", 1},
	    {"a.prx", "NID-A", "NID-A[lib]", "Function", "Global", 2},
	    {"a.prx", "NID-A", "NID-A[lib]", "Function", "Global", 3},
	};

	const auto output = Loader::SerializeUnresolvedImportReport(entries);
	const auto json   = nlohmann::ordered_json::parse(output);
	Check(json["schema_version"] == 1, "schema version");
	Check(json["unique_imports"] == 2, "unique import count");
	Check(json["relocation_sites"] == 6, "total relocation-site count");
	Check(json["imports"][0]["program"] == "a.prx", "deterministic ordering");
	Check(json["imports"][0]["relocation_sites"] == 5, "duplicate grouping");
	Check(output == Loader::SerializeUnresolvedImportReport(entries),
	      "serialization must be deterministic");
}

void TestEmptyAndOverflowReports() {
	const auto empty = nlohmann::ordered_json::parse(Loader::SerializeUnresolvedImportReport({}));
	Check(empty["unique_imports"] == 0, "empty report unique count");
	Check(empty["relocation_sites"] == 0, "empty report relocation count");
	Check(empty["imports"].empty(), "empty report import list");

	const std::vector<Loader::UnresolvedImportReportEntry> overflow = {
	    {"a.prx", "NID", "NID[lib]", "Function", "Global",
	     std::numeric_limits<uint64_t>::max()},
	    {"a.prx", "NID", "NID[lib]", "Function", "Global", 1},
	};
	Check(Loader::SerializeUnresolvedImportReport(overflow).empty(),
	      "counter overflow must fail serialization");
}

void TestFileOutput() {
	const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto root = std::filesystem::current_path() /
	                  ("kyty_unresolved_import_report_" + std::to_string(unique));
	const auto report = root / "nested" / "imports.json";

	Check(Loader::WriteUnresolvedImportReportFile(
	          report, {{"eboot.bin", "NID", "NID[lib]", "Function", "Global", 1}}),
	      "write report and create parent directories");
	std::ifstream input(report, std::ios::binary);
	Check(input.good(), "open written report");
	const std::string contents((std::istreambuf_iterator<char>(input)),
	                           std::istreambuf_iterator<char>());
	input.close();
	Check(contents == Loader::SerializeUnresolvedImportReport(
	                      {{"eboot.bin", "NID", "NID[lib]", "Function", "Global", 1}}),
	      "written report contents");
	Check(!Loader::WriteUnresolvedImportReportFile({}, {}), "empty output path must fail");

	std::error_code error;
	std::filesystem::remove_all(root, error);
	Check(!error, "remove test directory");
}

} // namespace

int main() {
	TestDeterministicGrouping();
	TestEmptyAndOverflowReports();
	TestFileOutput();
	return 0;
}
