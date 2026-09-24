#include "common/systemInfo.h"

#include "common/assert.h"
#include "common/common.h"

#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <intrin.h>
#elif defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#else
#error "unsupported host architecture for CPU brand-string detection"
#endif

#include <array>
#include <cstring>

namespace Common {

#if defined(__APPLE__)

SystemInfo GetSystemInfo() {
	char   buffer[256] = {};
	size_t size        = sizeof(buffer);
	EXIT_IF(sysctlbyname("machdep.cpu.brand_string", buffer, &size, nullptr, 0) != 0);
	return {std::string(buffer)};
}

#else

static void CpuId(uint32_t leaf, std::array<uint32_t, 4>& regs) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	__cpuid(reinterpret_cast<int*>(regs.data()), static_cast<int>(leaf));
#else
	__get_cpuid(leaf, &regs[0], &regs[1], &regs[2], &regs[3]);
#endif
}

SystemInfo GetSystemInfo() {
	std::array<uint32_t, 4> regs {};
	CpuId(0x80000000u, regs);
	EXIT_IF(regs[0] < 0x80000004u);

	char brand[3 * sizeof(regs) + 1] = {};
	for (uint32_t leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
		CpuId(leaf, regs);
		std::memcpy(brand + (leaf - 0x80000002u) * sizeof(regs), regs.data(), sizeof(regs));
	}

	// The brand string is fixed-width and vendors pad it with spaces.
	std::string name(brand);
	const auto  begin = name.find_first_not_of(' ');
	const auto  end   = name.find_last_not_of(' ');
	if (begin == std::string::npos) {
		return {std::string()};
	}
	return {name.substr(begin, end - begin + 1)};
}

#endif

} // namespace Common
