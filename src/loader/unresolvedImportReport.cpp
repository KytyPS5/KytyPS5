#include "loader/unresolvedImportReport.h"

#include "common/file.h"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <tuple>

namespace Loader {
namespace {

auto ImportIdentity(const UnresolvedImportReportEntry& entry) {
	return std::tie(entry.program, entry.nid, entry.symbol, entry.type, entry.bind);
}

} // namespace

std::string SerializeUnresolvedImportReport(
	const std::vector<UnresolvedImportReportEntry>& unresolved_imports) {
	auto sorted = unresolved_imports;
	std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
		return ImportIdentity(left) < ImportIdentity(right);
	});

	std::vector<UnresolvedImportReportEntry> grouped;
	grouped.reserve(sorted.size());
	for (const auto& entry: sorted) {
		if (!grouped.empty() && ImportIdentity(grouped.back()) == ImportIdentity(entry)) {
			if (entry.relocation_sites >
			    std::numeric_limits<uint64_t>::max() - grouped.back().relocation_sites) {
				return {};
			}
			grouped.back().relocation_sites += entry.relocation_sites;
		} else {
			grouped.push_back(entry);
		}
	}

	nlohmann::ordered_json imports          = nlohmann::ordered_json::array();
	uint64_t               relocation_sites = 0;
	for (const auto& entry: grouped) {
		if (entry.relocation_sites > std::numeric_limits<uint64_t>::max() - relocation_sites) {
			return {};
		}
		relocation_sites += entry.relocation_sites;
		imports.push_back({{"program", entry.program},
		                   {"nid", entry.nid},
		                   {"symbol", entry.symbol},
		                   {"type", entry.type},
		                   {"bind", entry.bind},
		                   {"relocation_sites", entry.relocation_sites}});
	}

	const nlohmann::ordered_json root = {{"schema_version", 1},
	                                     {"unique_imports", grouped.size()},
	                                     {"relocation_sites", relocation_sites},
	                                     {"imports", std::move(imports)}};
	return root.dump(2) + "\n";
}

bool WriteUnresolvedImportReportFile(
	const std::filesystem::path&                    path,
	const std::vector<UnresolvedImportReportEntry>& unresolved_imports) {
	if (path.empty()) {
		return false;
	}

	const auto output = SerializeUnresolvedImportReport(unresolved_imports);
	if (output.empty() || output.size() > std::numeric_limits<uint32_t>::max()) {
		return false;
	}

	const auto parent = path.parent_path();
	if (!parent.empty() && !Common::File::CreateDirectories(parent)) {
		return false;
	}

	Common::File file;
	if (!file.Create(path)) {
		return false;
	}

	uint32_t written = 0;
	file.Write(output.data(), static_cast<uint32_t>(output.size()), &written);
	const bool flushed = file.Flush();
	file.Close();
	return written == output.size() && flushed;
}

} // namespace Loader
