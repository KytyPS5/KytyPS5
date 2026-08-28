#ifndef LOADER_INCLUDE_LOADER_UNRESOLVED_IMPORT_POLICY_H_
#define LOADER_INCLUDE_LOADER_UNRESOLVED_IMPORT_POLICY_H_

#include "common/common.h"

#include <cstdint>
#include <string>

namespace Loader {

// Host-side context captured for a guest import that could not be resolved.
// Keeping this independent of RuntimeLinker makes strict-mode diagnostics testable.
struct StrictUnresolvedImportDetails {
	std::string symbol;
	std::string type;
	std::string bind;
	std::string program;
	uint64_t    patch_vaddr      = 0;
	uint32_t    relocation_index = 0;
};

// Returns a stable, single-line diagnostic. A null details pointer represents a
// corrupt or stale thunk whose record id no longer exists.
[[nodiscard]] std::string
FormatStrictUnresolvedImportError(uint64_t record_id, const StrictUnresolvedImportDetails* details);

} // namespace Loader

#endif /* LOADER_INCLUDE_LOADER_UNRESOLVED_IMPORT_POLICY_H_ */
