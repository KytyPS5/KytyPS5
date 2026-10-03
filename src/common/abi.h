#ifndef KYTY_COMMON_ABI_H_
#define KYTY_COMMON_ABI_H_

#include "common/common.h"

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_MS_ABI __attribute__((ms_abi))

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS && (defined(__x86_64__) || defined(_M_X64))
#define KYTY_SYSV_ABI __attribute__((sysv_abi, force_align_arg_pointer))
#else
#define KYTY_SYSV_ABI __attribute__((sysv_abi))
#endif

#endif /* KYTY_COMMON_ABI_H_ */
