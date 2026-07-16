#include "common/debug.h"

#include "common/platform/sysDbg.h"

namespace Common {

void DebugStack::Print(int from, [[maybe_unused]] bool with_name) const {
	for (int i = from; i < depth; i++) {
		printf("[%d] %016" PRIx64 "\n", i - from, static_cast<uint64_t>(GetAddr(i)));
	}
}

void DebugStack::Trace(DebugStack* stack) {
	stack->depth = DEBUG_MAX_STACK_DEPTH;
	SysStackWalk(stack->stack.data(), &stack->depth);
}

std::string Debug::GetCompiler() {
#if KYTY_COMPILER == KYTY_COMPILER_CLANG
	return "clang";
#elif KYTY_COMPILER == KYTY_COMPILER_GCC
	return "gcc";
#else
	return "????";
#endif
}

std::string Debug::GetLinker() {
#if KYTY_LINKER == KYTY_LINKER_LD
	return "ld";
#elif KYTY_LINKER == KYTY_LINKER_LLD
	return "lld";
#elif KYTY_LINKER == KYTY_LINKER_LLD_LINK
	return "lld_link";
#else
	return "??";
#endif
}

std::string Debug::GetBitness() {
	return "64";
}

} // namespace Common
