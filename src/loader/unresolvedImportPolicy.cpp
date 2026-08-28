#include "loader/unresolvedImportPolicy.h"

#include <fmt/format.h>

namespace Loader {

std::string FormatStrictUnresolvedImportError(uint64_t                             record_id,
                                              const StrictUnresolvedImportDetails* details) {
	if (details == nullptr) {
		return fmt::format("strict unresolved import: invalid record_id={}", record_id);
	}

	return fmt::format(
	    "strict unresolved import: symbol={} type={} bind={} program={} patch_vaddr=0x{:016x} "
	    "jmprela_index={} record_id={}",
	    details->symbol, details->type, details->bind, details->program, details->patch_vaddr,
	    details->relocation_index, record_id);
}

} // namespace Loader
