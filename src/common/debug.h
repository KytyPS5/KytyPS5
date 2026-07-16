#ifndef KYTY_COMMON_DEBUG_H_
#define KYTY_COMMON_DEBUG_H_

#include "common/common.h"
#include "common/stringUtils.h"

#include <array>

namespace Common {

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS && KYTY_BUILD == KYTY_BUILD_DEBUG &&                    \
    KYTY_COMPILER == KYTY_COMPILER_CLANG
constexpr int DEBUG_MAX_STACK_DEPTH = 20;
#else
constexpr int DEBUG_MAX_STACK_DEPTH = 15;
#endif

namespace Debug {
std::string GetCompiler();
std::string GetLinker();
std::string GetBitness();
} // namespace Debug

struct DebugStack {
	int                                      depth {0};
	std::array<void*, DEBUG_MAX_STACK_DEPTH> stack {};

	[[nodiscard]] uintptr_t GetAddr(int i) const { return reinterpret_cast<uintptr_t>(stack[i]); }

	void Print(int from, bool with_name = true) const;

	void CopyTo(DebugStack* s) const {
		std::memcpy(s->stack.data(), stack.data(), sizeof(void*) * depth);
		s->depth = depth;
	}

	static void Trace(DebugStack* stack);
};

} // namespace Common

#endif /* KYTY_COMMON_DEBUG_H_ */
