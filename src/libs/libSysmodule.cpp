#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <mutex>
#include <unordered_map>

namespace Libs {

LIB_VERSION("Sysmodule", 1, "Sysmodule", 1, 1);

namespace LibKernel {
struct ModuleInfoForUnwind;
int KYTY_SYSV_ABI KernelGetModuleInfoForUnwind(uint64_t addr, int flags, ModuleInfoForUnwind* info);
} // namespace LibKernel

namespace Sysmodule {

constexpr int SYSMODULE_ERROR_UNLOADED = static_cast<int32_t>(0x805a1001u);

static std::mutex                             g_modules_mutex;
static std::unordered_map<uint16_t, uint32_t> g_module_ref_counts;

static int LoadModule(uint16_t id) {
	std::scoped_lock lock(g_modules_mutex);
	g_module_ref_counts[id]++;
	return OK;
}

static int UnloadModule(uint16_t id) {
	std::scoped_lock lock(g_modules_mutex);
	const auto       module = g_module_ref_counts.find(id);
	if (module == g_module_ref_counts.end()) {
		return SYSMODULE_ERROR_UNLOADED;
	}
	if (module->second <= 1u) {
		g_module_ref_counts.erase(module);
	} else {
		module->second--;
	}
	return OK;
}

static KYTY_SYSV_ABI int SysmoduleGetModuleInfoForUnwind(uint64_t addr, int flags,
                                                         LibKernel::ModuleInfoForUnwind* info) {
	return LibKernel::KernelGetModuleInfoForUnwind(addr, flags, info);
}

static KYTY_SYSV_ABI int SysmoduleLoadModule(uint16_t id) {
	PRINT_NAME();

	LOGF("\t id = %d\n", static_cast<int>(id));

	return LoadModule(id);
}

static KYTY_SYSV_ABI int SysmoduleUnloadModule(uint16_t id) {
	PRINT_NAME();

	LOGF("\t id = %d\n", static_cast<int>(id));

	return UnloadModule(id);
}

static KYTY_SYSV_ABI int SysmoduleLoadModuleInternalWithArg(uint16_t id, int arg1, int arg2,
                                                            int arg3, int* ret) {
	PRINT_NAME();

	LOGF("\t id = %d\n", static_cast<int>(id));

	if (arg1 != 0 || arg2 != 0 || arg3 != 0 || ret == nullptr) {
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	const auto result = LoadModule(id);
	*ret              = result;

	return result;
}

static KYTY_SYSV_ABI int SysmoduleIsLoaded(uint16_t id) {
	PRINT_NAME();

	LOGF("\t id = %d\n", static_cast<int>(id));

	std::scoped_lock lock(g_modules_mutex);
	const auto       module = g_module_ref_counts.find(id);
	return module != g_module_ref_counts.end() && module->second != 0u ? OK
	                                                                   : SYSMODULE_ERROR_UNLOADED;
}

} // namespace Sysmodule

LIB_DEFINE(InitSysmodule_1) {
	LIB_FUNC("4fU5yvOkVG4", Sysmodule::SysmoduleGetModuleInfoForUnwind);
	LIB_FUNC("eR2bZFAAU0Q", Sysmodule::SysmoduleUnloadModule);
	LIB_FUNC("hHrGoGoNf+s", Sysmodule::SysmoduleLoadModuleInternalWithArg);
	LIB_FUNC("g8cM39EUZ6o", Sysmodule::SysmoduleLoadModule);
	LIB_FUNC("fMP5NHUOaMk", Sysmodule::SysmoduleIsLoaded);
}

} // namespace Libs
