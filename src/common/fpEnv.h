#ifndef KYTY_COMMON_FP_ENV_H_
#define KYTY_COMMON_FP_ENV_H_

#include "common/common.h"

namespace Common {

struct FpEnv {
	uint32_t mxcsr;
	uint16_t control;
};

inline constexpr FpEnv GuestFpEnv {0x9fc0, 0x037f};

inline FpEnv ReadFpEnv() {
	FpEnv env {};
	asm volatile("stmxcsr %0\n\tfnstcw %1" : "=m"(env.mxcsr), "=m"(env.control) : : "memory");
	return env;
}

inline void WriteFpEnv(FpEnv env) {
	asm volatile("ldmxcsr %0\n\tfldcw %1" : : "m"(env.mxcsr), "m"(env.control) : "memory");
}

} // namespace Common

#endif /* KYTY_COMMON_FP_ENV_H_ */
