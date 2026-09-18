#ifndef KYTY_COMMON_DEBUG_H_
#define KYTY_COMMON_DEBUG_H_

#include "common/common.h"

#include <string>

namespace Common {

namespace Debug {
std::string GetCompiler();
std::string GetLinker();
} // namespace Debug

} // namespace Common

#endif /* KYTY_COMMON_DEBUG_H_ */
