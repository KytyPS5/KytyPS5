#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/stringUtils.h"
#include "kernel/fileSystem.h"
#include "libs/libs.h"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace Libs {

LIB_VERSION("Sysmodule", 1, "Sysmodule", 1, 1);

namespace LibKernel {
struct ModuleInfoForUnwind;
int KYTY_SYSV_ABI KernelGetModuleInfoForUnwind(uint64_t addr, int flags, ModuleInfoForUnwind* info);
} // namespace LibKernel

namespace Sysmodule {

constexpr int SYSMODULE_ERROR_UNLOADED = static_cast<int>(0x805a1001u);

static std::mutex                             g_module_mutex;
static std::unordered_map<uint16_t, uint32_t> g_module_refs;
static std::recursive_mutex                   g_game_module_mutex;

struct GameModule {
	uint16_t    id;
	const char* name;
};

// Modules that libSceSysmodule loads from /app0/sce_module instead of the system library
// directories. They are shipped with the game, so the runtime does not load them at startup.
constexpr GameModule GAME_MODULES[] = {
    {0x0038, "libSceFace"},          {0x0039, "libSceSmart"},
    {0x0086, "libSceS3DConversion"}, {0x0093, "libSceHand"},
    {0x00cb, "libSceHeadTracker"},   {0x00d8, "libSceFaceTracker"},
    {0x00d9, "libSceHandTracker"},   {0x00e8, "libSceAudioLatencyEstimation"},
    {0x00f7, "libSceNpToolkit"},     {0x00fe, "libSceJobManager"},
    {0x0102, "libSceNpToolkit2"},    {0x0115, "libSceNpCppWebApi"},
    {0x0135, "libSceFontGsm"},
};

static const char* FindGameModule(uint16_t id) {
	for (const auto& module: GAME_MODULES) {
		if (module.id == id) {
			return module.name;
		}
	}
	return nullptr;
}

static void LoadGameModule(uint16_t id) {
	const auto* name = FindGameModule(id);
	if (name == nullptr) {
		return;
	}

	const auto path =
	    LibKernel::FileSystem::GetRealFilename(std::string("/app0/sce_module/") + name + ".prx");
	if (!Common::File::IsFileExisting(path)) {
		LOGF("\t game module missing = %s\n", Common::PathToString(path).c_str());
		return;
	}

	std::scoped_lock lock(g_game_module_mutex);

	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
	if (rt->FindProgramByFileName(path) != nullptr) {
		return;
	}

	auto* program = rt->LoadProgram(path);
	rt->RelocateProgram(program);
	if (program->dynamic_info->init_vaddr != 0) {
		int result = rt->StartModule(program, 0, nullptr, nullptr);
		LOGF("\t %s module_start() result = %d\n", name, result);
	}
}

static void LoadModule(uint16_t id) {
	LoadGameModule(id);

	std::scoped_lock lock(g_module_mutex);
	++g_module_refs[id];
}

static KYTY_SYSV_ABI int SysmoduleGetModuleInfoForUnwind(uint64_t addr, int flags,
                                                         LibKernel::ModuleInfoForUnwind* info) {
	return LibKernel::KernelGetModuleInfoForUnwind(addr, flags, info);
}

static KYTY_SYSV_ABI int SysmoduleLoadModule(uint16_t id) {
	PRINT_NAME();

	LOGF("\t id = %d\n", static_cast<int>(id));

	LoadModule(id);
	return 0;
}

static KYTY_SYSV_ABI int SysmoduleUnloadModule(uint16_t id) {
	PRINT_NAME();

	LOGF("\t id = %d\n", static_cast<int>(id));

	std::scoped_lock lock(g_module_mutex);
	const auto       module = g_module_refs.find(id);
	if (module == g_module_refs.end()) {
		return SYSMODULE_ERROR_UNLOADED;
	}
	if (--module->second == 0) {
		g_module_refs.erase(module);
	}
	return 0;
}

static KYTY_SYSV_ABI int SysmoduleLoadModuleInternalWithArg(uint16_t id, int arg1, int arg2,
                                                            int arg3, int* ret) {
	PRINT_NAME();

	LOGF("\t id = %d\n", static_cast<int>(id));

	EXIT_IF(arg1 != 0);
	EXIT_IF(arg2 != 0);
	EXIT_IF(arg3 != 0);
	EXIT_IF(ret == nullptr);

	LoadModule(id);
	*ret = 0;

	return 0;
}

static KYTY_SYSV_ABI int SysmoduleIsLoaded(uint16_t id) {
	PRINT_NAME();

	LOGF("\t id = %d\n", static_cast<int>(id));

	std::scoped_lock lock(g_module_mutex);
	return g_module_refs.contains(id) ? 0 : SYSMODULE_ERROR_UNLOADED;
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
