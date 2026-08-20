#ifndef KYTY_COMMON_DIAGNOSTICS_H_
#define KYTY_COMMON_DIAGNOSTICS_H_

#include <string>

namespace Common::Diagnostics {

// The build identification line: build type, compiler, version, git hash, date.
// Used both by --help and by the diagnostics report, so there is one definition
// of what this build calls itself.
[[nodiscard]] std::string BuildString();

// The host half of the report printed by --diagnostics: build string, OS, and
// CPU. The Vulkan half lives in the graphics layer, which is the only place
// that may include Vulkan headers - see Libs::Graphics::BuildVulkanReport().
//
// Meant to be pasted into a bug report, so it deliberately contains no file
// paths, no usernames and no game titles - a reporter should not have to check
// what they are about to post in public.
[[nodiscard]] std::string BuildReport();

} // namespace Common::Diagnostics

#endif /* KYTY_COMMON_DIAGNOSTICS_H_ */
