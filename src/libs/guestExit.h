#ifndef KYTY_LIBS_GUEST_EXIT_H_
#define KYTY_LIBS_GUEST_EXIT_H_

#include "common/abi.h"
#include "common/assert.h"

namespace Libs {

inline KYTY_SYSV_ABI void GuestExit(int status) {
	Common::DbgExit(status);
}

} // namespace Libs

#endif
