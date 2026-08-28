#ifndef KYTY_LOADER_UNRESOLVED_IMPORT_REPORT_H_
#define KYTY_LOADER_UNRESOLVED_IMPORT_REPORT_H_

#include "common/common.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Loader {

// One currently stubbed import. Entries with the same identity are combined when the report is
// serialized, so relocation_sites may describe more than one patch location.
struct UnresolvedImportReportEntry {
	std::string program;
	std::string nid;
	std::string symbol;
	std::string type;
	std::string bind;
	uint64_t    relocation_sites = 1;
};

// Returns a deterministic, human-readable JSON document. An empty result indicates that a
// relocation-site counter overflowed; a valid report is never empty, even when it has no imports.
[[nodiscard]] std::string SerializeUnresolvedImportReport(
	const std::vector<UnresolvedImportReportEntry>& unresolved_imports);

// Writes schema version 1 of the unresolved-import report. Parent directories are created when
// necessary. The function returns false for an empty path, serialization failure, or I/O failure.
[[nodiscard]] bool WriteUnresolvedImportReportFile(
	const std::filesystem::path&                    path,
	const std::vector<UnresolvedImportReportEntry>& unresolved_imports);

} // namespace Loader

#endif /* KYTY_LOADER_UNRESOLVED_IMPORT_REPORT_H_ */
