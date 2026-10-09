#include "graphics/presentation/renderDoc.h"

#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <renderdoc_app.h>
#include <string>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#else
#include <dlfcn.h>
#endif

namespace Libs::Graphics {

enum class RenderDocState : uint32_t {
	Idle,
	Requested,
	Capturing,
};

static RENDERDOC_API_1_6_0*        g_api                  = nullptr;
static std::atomic<RenderDocState> g_state                = RenderDocState::Idle;
static uint32_t                    g_captured_flips       = 0;
static uint32_t                    g_capture_count_before = 0;
static std::atomic_bool            g_unavailable_log      = false;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

static bool BindRenderDocApi(HMODULE module) {
	auto* get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
#else
static bool BindRenderDocApi(void* module) {
	auto* get_api = reinterpret_cast<pRENDERDOC_GetAPI>(::dlsym(module, "RENDERDOC_GetAPI"));
#endif
	if (get_api == nullptr) {
		LOGF("RenderDoc: RENDERDOC_GetAPI export missing\n");
		return false;
	}

	void* api = nullptr;
	LOGF("RenderDoc: requesting in-application API 1.6.0\n");
	Log::Flush();
	if (get_api(eRENDERDOC_API_Version_1_6_0, &api) != 1 || api == nullptr) {
		return false;
	}

	g_api = static_cast<RENDERDOC_API_1_6_0*>(api);
	LOGF("RenderDoc: API returned; disabling RenderDoc capture keys\n");
	Log::Flush();
	g_api->SetCaptureKeys(nullptr, 0);
	LOGF("RenderDoc: capture keys disabled; unloading RenderDoc crash handler\n");
	Log::Flush();
	g_api->UnloadCrashHandler();
	LOGF("RenderDoc: crash handler unloaded\n");
	Log::Flush();
	int major = 0, minor = 0, patch = 0;
	g_api->GetAPIVersion(&major, &minor, &patch);
	LOGF("RenderDoc: API %d.%d.%d bound (requested 1.6.0; not the RenderDoc product version)\n",
	     major, minor, patch);
	return true;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

static bool PrependEnvironmentValue(const wchar_t* name, const std::wstring& value) {
	std::wstring combined = value;
	if (const auto* existing = _wgetenv(name); existing != nullptr && existing[0] != L'\0') {
		combined += L';';
		combined += existing;
	}
	// Update both the Windows environment and the CRT copy used by compatibility detection.
	const auto result = _wputenv_s(name, combined.c_str());
	if (result != 0) {
		LOGF("RenderDoc: cannot update the portable layer environment (CRT error %d)\n", result);
	}
	return result == 0;
}

static bool ConfigurePortableLayer(const std::filesystem::path& directory) {
	// VK_LAYER_PATH overrides VK_ADD_LAYER_PATH. Preserve whichever search path is active,
	// including installed layers when no override exists. These changes affect only this process.
	const auto* search_variable =
	    _wgetenv(L"VK_LAYER_PATH") != nullptr ? L"VK_LAYER_PATH" : L"VK_ADD_LAYER_PATH";
	const auto*        previous_search = _wgetenv(search_variable);
	const std::wstring saved_search    = previous_search != nullptr ? previous_search : L"";
	if (!PrependEnvironmentValue(search_variable, directory.native())) {
		return false;
	}
	if (!PrependEnvironmentValue(L"VK_INSTANCE_LAYERS", L"VK_LAYER_RENDERDOC_Capture")) {
		if (_wputenv_s(search_variable, saved_search.c_str()) != 0) {
			LOGF("RenderDoc: failed to restore the previous Vulkan layer search path\n");
		}
		return false;
	}
	LOGF("RenderDoc: portable Vulkan capture layer enabled for this process\n");
	return true;
}

static HMODULE LoadPortableRenderDoc(std::filesystem::path& directory) {
	std::array<wchar_t, 32768> executable_path {};
	const auto length = GetModuleFileNameW(nullptr, executable_path.data(),
	                                       static_cast<DWORD>(executable_path.size()));
	if (length == 0 || length >= executable_path.size()) {
		LOGF("RenderDoc: cannot locate the executable (Windows error %lu)\n", GetLastError());
		return nullptr;
	}
	directory                = std::filesystem::path(executable_path.data()).parent_path();
	const auto      dll_path = directory / "renderdoc.dll";
	std::error_code error;
	if (!std::filesystem::is_regular_file(dll_path, error) ||
	    !std::filesystem::is_regular_file(directory / "renderdoc.json", error)) {
		LOGF("RenderDoc: portable loading requires renderdoc.dll and renderdoc.json beside the "
		     "executable: %s\n",
		     Common::PathToString(directory).c_str());
		return nullptr;
	}
	LOGF("RenderDoc: loading portable DLL %s\n", Common::PathToString(dll_path).c_str());
	Log::Flush();
	auto* module =
	    LoadLibraryExW(dll_path.c_str(), nullptr,
	                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (module == nullptr) {
		LOGF("RenderDoc: portable LoadLibraryExW failed (Windows error %lu)\n", GetLastError());
	}
	return module;
}

static HMODULE LoadInstalledRenderDoc() {
	HKEY       key         = nullptr;
	const auto open_result = RegOpenKeyExW(
	    HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\RenderDoc.RDCCapture.1\\DefaultIcon\\", 0,
	    KEY_READ, &key);
	if (open_result != ERROR_SUCCESS) {
		LOGF("RenderDoc: registry lookup failed (Windows error %ld); trying portable copy\n",
		     open_result);
		return nullptr;
	}

	std::array<wchar_t, MAX_PATH> path_buffer {};
	DWORD      path_size = static_cast<DWORD>(path_buffer.size() * sizeof(wchar_t));
	const auto result = RegQueryValueExW(key, L"", nullptr, nullptr,
	                                     reinterpret_cast<LPBYTE>(path_buffer.data()), &path_size);
	RegCloseKey(key);
	if (result != ERROR_SUCCESS) {
		LOGF("RenderDoc: registry path read failed (Windows error %ld)\n", result);
		return nullptr;
	}

	auto path = std::filesystem::path(path_buffer.data()).parent_path() / "renderdoc.dll";
	LOGF("RenderDoc: loading %s\n", Common::PathToString(path).c_str());
	Log::Flush();
	auto* module = LoadLibraryW(path.c_str());
	if (module == nullptr) {
		LOGF("RenderDoc: LoadLibraryW failed (Windows error %lu)\n", GetLastError());
	}
	return module;
}

void RenderDocInit() {
	if (g_api != nullptr) {
		return;
	}

	auto* module = GetModuleHandleA("renderdoc.dll");
	LOGF("RenderDoc: initialization; already loaded=%u\n", module != nullptr ? 1u : 0u);
	if (module == nullptr) {
		module = LoadInstalledRenderDoc();
	}
	std::filesystem::path portable_directory;
	if (module == nullptr) {
		module = LoadPortableRenderDoc(portable_directory);
	}
	if (module == nullptr) {
		LOGF("RenderDoc: unavailable; install RenderDoc, add its portable DLL and manifest beside "
		     "the executable, or launch through RenderDoc with --rd\n");
		return;
	}
	std::array<wchar_t, 32768> module_path {};
	if (GetModuleFileNameW(module, module_path.data(), static_cast<DWORD>(module_path.size())) !=
	    0) {
		LOGF("RenderDoc: loaded module %s\n", Common::PathToString(module_path.data()).c_str());
	}

	if (!BindRenderDocApi(module)) {
		LOGF("RenderDoc: API 1.6.0 is unavailable\n");
		return;
	}
	if (!portable_directory.empty() && !ConfigurePortableLayer(portable_directory)) {
		g_api = nullptr;
		LOGF("RenderDoc: portable Vulkan layer setup failed; capture disabled\n");
	}
}

#else

void RenderDocInit() {
	if (g_api != nullptr) {
		return;
	}

	auto* module = ::dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
	if (module == nullptr) {
		module = ::dlopen("librenderdoc.so", RTLD_NOW);
	}
	if (module == nullptr) {
		LOGF("RenderDoc: dlopen failed: %s\n", ::dlerror());
		return;
	}

	if (!BindRenderDocApi(module)) {
		LOGF("RenderDoc: API 1.6.0 is unavailable\n");
		::dlclose(module);
	}
}

#endif

void RenderDocRequestCapture() {
	if (g_api == nullptr) {
		if (!g_unavailable_log.exchange(true)) {
			LOGF("RenderDoc: capture requested, but RenderDoc is unavailable\n");
		}
		return;
	}

	RenderDocState expected = RenderDocState::Idle;
	if (g_state.compare_exchange_strong(expected, RenderDocState::Requested)) {
		LOGF("RenderDoc: capture requested\n");
	}
}

static void StartCapture() {
	if (g_api->IsFrameCapturing() != 0) {
		g_state.store(RenderDocState::Idle, std::memory_order_release);
		LOGF("RenderDoc: capture request ignored because a capture is already active\n");
		return;
	}

	const auto  capture_id  = std::chrono::duration_cast<std::chrono::microseconds>(
	                              std::chrono::system_clock::now().time_since_epoch())
	                              .count();
	const char* capture_dir = std::getenv("KYTY_RENDERDOC_DIR");
	const auto  capture_path =
	    std::string(capture_dir != nullptr && capture_dir[0] != '\0' ? capture_dir : "_RenderDoc") +
	    "/kyty_" + std::to_string(capture_id);
	g_capture_count_before = g_api->GetNumCaptures();
	std::error_code path_error;
	const auto      absolute_path =
	    std::filesystem::absolute(Common::PathFromUtf8(capture_path), path_error);
	LOGF("RenderDoc: StartFrameCapture; template=%s; previous captures=%u\n",
	     path_error ? capture_path.c_str() : Common::PathToString(absolute_path).c_str(),
	     g_capture_count_before);
	Log::Flush();
	g_api->SetCaptureFilePathTemplate(capture_path.c_str());
	g_api->StartFrameCapture(nullptr, nullptr);
	if (g_api->IsFrameCapturing() == 0) {
		g_state.store(RenderDocState::Idle, std::memory_order_release);
		LOGF("RenderDoc: capture failed to start\n");
		Log::Flush();
		return;
	}
	g_captured_flips = 0;
	g_state.store(RenderDocState::Capturing, std::memory_order_release);
	LOGF("RenderDoc: capture started\n");
	Log::Flush();
}

void RenderDocOnGuestFlip(RenderContext& renderer) {
	const auto state = g_state.load(std::memory_order_acquire);
	if (g_api == nullptr || state == RenderDocState::Idle) {
		return;
	}
	if (state == RenderDocState::Capturing) {
		LOGF("RenderDoc: captured guest flip %u/2\n", ++g_captured_flips);
		if (g_captured_flips < 2) {
			return;
		}
	}

	// Capture boundaries follow presentation and exclude concurrent queue access.
	Common::LockGuard render_lock(renderer.GetMutex());
	Common::LockGuard queue_lock(renderer.GetGraphics().queue_mutex);
	if (state == RenderDocState::Requested) {
		StartCapture();
	} else {
		LOGF("RenderDoc: EndFrameCapture; active before=%u\n", g_api->IsFrameCapturing());
		Log::Flush();
		const auto ok = g_api->EndFrameCapture(nullptr, nullptr);
		g_state.store(RenderDocState::Idle, std::memory_order_release);
		const auto count = g_api->GetNumCaptures();
		LOGF("RenderDoc: EndFrameCapture result=%u; active after=%u; captures=%u\n", ok,
		     g_api->IsFrameCapturing(), count);
		Log::Flush();
		for (auto index = g_capture_count_before; index < count; ++index) {
			uint32_t length = 0;
			if (g_api->GetCapture(index, nullptr, &length, nullptr) == 0 || length == 0) {
				LOGF("RenderDoc: cannot query capture %u\n", index);
				continue;
			}
			std::vector<char> path(length + 1, '\0');
			if (g_api->GetCapture(index, path.data(), &length, nullptr) == 0) {
				LOGF("RenderDoc: cannot read capture path %u\n", index);
				continue;
			}
			std::error_code error;
			const auto size = std::filesystem::file_size(Common::PathFromUtf8(path.data()), error);
			if (error) {
				LOGF("RenderDoc: capture file=%s; size unavailable: %s\n", path.data(),
				     error.message().c_str());
			} else {
				LOGF("RenderDoc: capture file=%s; bytes=%llu\n", path.data(),
				     static_cast<unsigned long long>(size));
			}
		}
		Log::Flush();
	}
}

} // namespace Libs::Graphics
