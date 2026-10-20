#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_ADDITIONAL_CONTENT_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_ADDITIONAL_CONTENT_H_

#include "common/additionalContent.h"

#include <cstddef>

namespace Loader {

using AddContentEntry = Common::AddContentEntry;

void AddContentLoad(const std::filesystem::path& dir, const std::string& title_id);
const std::vector<AddContentEntry>& AddContentGetAll();
const AddContentEntry*              AddContentFind(const char* label, size_t label_size);

} // namespace Loader

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_ADDITIONAL_CONTENT_H_ */
