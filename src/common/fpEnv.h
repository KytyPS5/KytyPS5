#ifndef KYTY_COMMON_FP_ENV_H_
#define KYTY_COMMON_FP_ENV_H_

#include "common/common.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace Common {

struct FpEnv {
	uint32_t mxcsr;
	uint16_t control;
};

inline constexpr FpEnv GuestFpEnv {0x9fc0, 0x037f};

#if defined(__x86_64__) || defined(_M_X64)
inline FpEnv ReadFpEnv() {
	FpEnv env {_mm_getcsr(), 0};
	asm volatile("fnstcw %0" : "=m"(env.control));
	return env;
}

inline void WriteFpEnv(FpEnv env) {
	_mm_setcsr(env.mxcsr);
	asm volatile("fldcw %0" : : "m"(env.control) : "memory");
}
#else
inline FpEnv ReadFpEnv() {
	return {};
}

inline void WriteFpEnv(FpEnv /*env*/) {}
#endif

} // namespace Common

#endif /* KYTY_COMMON_FP_ENV_H_ */
