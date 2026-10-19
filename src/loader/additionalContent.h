#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_ADDITIONAL_CONTENT_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_ADDITIONAL_CONTENT_H_

#include "common/additionalContent.h"

#include <cstddef>

namespace Loader {

using AddContentEntry = Common::AddContentEntry;

/// Reload installed packages and optional app0/dlc_emu.ini license-only declarations.
void AddContentLoad(const std::filesystem::path& dir, const std::string& title_id,
                    const std::filesystem::path& app0_dir = {});
const std::vector<AddContentEntry>& AddContentGetAll();
/// Return current local declarations; these entries do not have mountable content.
const std::vector<AddContentEntry>& LocalEntitlementsGetAll();
const AddContentEntry*              AddContentFind(const char* label, size_t label_size);

} // namespace Loader

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_ADDITIONAL_CONTENT_H_ */
