#include "common/debug.h"

namespace Common {

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
