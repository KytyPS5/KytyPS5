#include "graphics/presentation/renderDoc.h"

#include "SDL_syswm.h"
#include "SDL_version.h"
#include "SDL_video.h"
#include "common/logging/log.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#else
#include <dlfcn.h>

// RenderDoc uses Windows-style names in its cross-platform API.
#define __cdecl
using HMODULE = void*;
#endif

namespace Libs::Graphics {

using RenderDocDevicePointer = void*;
using RenderDocWindowHandle  = void*;

enum RenderDocVersion {
	eRENDERDOC_API_Version_1_4_2 = 10402,
};

enum RenderDocInputButton {
	eRENDERDOC_Key_NonPrintable = 0x100,
	eRENDERDOC_Key_Divide,
	eRENDERDOC_Key_Multiply,
	eRENDERDOC_Key_Subtract,
	eRENDERDOC_Key_Plus,
	eRENDERDOC_Key_F1,
};

using pRENDERDOC_SetCaptureKeys             = void(__cdecl*)(RenderDocInputButton* keys, int num);
using pRENDERDOC_SetCaptureFilePathTemplate = void(__cdecl*)(const char* pathtemplate);
using pRENDERDOC_GetCaptureFilePathTemplate = const char*(__cdecl*)();
using pRENDERDOC_GetNumCaptures             = uint32_t(__cdecl*)();
using pRENDERDOC_GetCapture = uint32_t(__cdecl*)(uint32_t idx, char* filename, uint32_t* pathlength,
                                                 uint64_t* timestamp);
using pRENDERDOC_UnloadCrashHandler = void(__cdecl*)();
using pRENDERDOC_SetActiveWindow    = void(__cdecl*)(RenderDocDevicePointer device,
                                                     RenderDocWindowHandle  wndHandle);
using pRENDERDOC_StartFrameCapture  = void(__cdecl*)(RenderDocDevicePointer device,
                                                     RenderDocWindowHandle  wndHandle);
using pRENDERDOC_IsFrameCapturing   = uint32_t(__cdecl*)();
using pRENDERDOC_EndFrameCapture    = uint32_t(__cdecl*)(RenderDocDevicePointer device,
                                                         RenderDocWindowHandle  wndHandle);
using pRENDERDOC_GetAPI = int(__cdecl*)(RenderDocVersion version, void** out_api_pointers);

struct RenderDocApi {
	void*                                 GetAPIVersion;
	void*                                 SetCaptureOptionU32;
	void*                                 SetCaptureOptionF32;
	void*                                 GetCaptureOptionU32;
	void*                                 GetCaptureOptionF32;
	void*                                 SetFocusToggleKeys;
	pRENDERDOC_SetCaptureKeys             SetCaptureKeys;
	void*                                 GetOverlayBits;
	void*                                 MaskOverlayBits;
	void*                                 RemoveHooks;
	pRENDERDOC_UnloadCrashHandler         UnloadCrashHandler;
	pRENDERDOC_SetCaptureFilePathTemplate SetCaptureFilePathTemplate;
	pRENDERDOC_GetCaptureFilePathTemplate GetCaptureFilePathTemplate;
	pRENDERDOC_GetNumCaptures             GetNumCaptures;
	pRENDERDOC_GetCapture                 GetCapture;
	void*                                 TriggerCapture;
	void*                                 IsTargetControlConnected;
	void*                                 LaunchReplayUI;
	pRENDERDOC_SetActiveWindow            SetActiveWindow;
	pRENDERDOC_StartFrameCapture          StartFrameCapture;
	pRENDERDOC_IsFrameCapturing           IsFrameCapturing;
	pRENDERDOC_EndFrameCapture            EndFrameCapture;
	void*                                 TriggerMultiFrameCapture;
	void*                                 SetCaptureFileComments;
	void*                                 DiscardFrameCapture;
};

enum class RenderDocState : uint32_t {
	Idle,
	Requested,
	Capturing,
};

static RenderDocApi*               g_api             = nullptr;
static HMODULE                     g_module          = nullptr;
static RenderDocDevicePointer      g_device          = nullptr;
static RenderDocWindowHandle       g_window          = nullptr;
static std::atomic<RenderDocState> g_state           = RenderDocState::Idle;
static std::atomic_bool            g_init_done       = false;
static std::atomic_bool            g_unavailable_log = false;

static RenderDocDevicePointer GetRenderDocDevicePointer(vk::Instance instance) {
	if (instance == nullptr) {
		return nullptr;
	}

	// RenderDoc wants the Vulkan *dispatch key*, not the handle: its own header defines
	//     RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(inst) == (*((void **)(inst)))
	// i.e. the first pointer inside the dispatchable object. Passing the VkInstance handle
	// itself makes StartFrameCapture silently do nothing ("StartFrameCapture returned, but
	// RenderDoc is not capturing"), so no capture is ever produced.
	auto* dispatchable = static_cast<void*>(VulkanHandleToPointer(instance));
	if (dispatchable == nullptr) {
		return nullptr;
	}
	return *reinterpret_cast<void**>(dispatchable);
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

static bool BindRenderDocApi(HMODULE module) {
	auto* get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
	if (get_api == nullptr) {
		return false;
	}

	void* api = nullptr;
	if (get_api(eRENDERDOC_API_Version_1_4_2, &api) == 0 || api == nullptr) {
		return false;
	}

	g_module = module;
	g_api    = static_cast<RenderDocApi*>(api);

	g_api->SetCaptureFilePathTemplate("_RenderDoc/kyty");
	static RenderDocInputButton capture_keys[] = {eRENDERDOC_Key_F1};
	g_api->SetCaptureKeys(capture_keys, 1);
	g_api->UnloadCrashHandler();

	char module_path[MAX_PATH] = {};
	GetModuleFileNameA(module, module_path, sizeof(module_path));
	LOGF("RenderDoc: bound API from %s\n", module_path);
	return true;
}

static RenderDocWindowHandle GetRenderDocWindowHandle(SDL_Window* window) {
	if (window == nullptr) {
		return nullptr;
	}

	SDL_SysWMinfo info {};
	SDL_VERSION(&info.version);

	if (SDL_GetWindowWMInfo(window, &info) != SDL_TRUE || info.subsystem != SDL_SYSWM_WINDOWS) {
		return nullptr;
	}

	return info.info.win.window;
}

#else

static bool BindRenderDocApi(HMODULE module) {
	auto* get_api = reinterpret_cast<pRENDERDOC_GetAPI>(::dlsym(module, "RENDERDOC_GetAPI"));
	if (get_api == nullptr) {
		return false;
	}

	void* api = nullptr;
	if (get_api(eRENDERDOC_API_Version_1_4_2, &api) == 0 || api == nullptr) {
		return false;
	}

	g_module = module;
	g_api    = static_cast<RenderDocApi*>(api);

	g_api->SetCaptureFilePathTemplate("_RenderDoc/kyty");
	static RenderDocInputButton capture_keys[] = {eRENDERDOC_Key_F1};
	g_api->SetCaptureKeys(capture_keys, 1);
	g_api->UnloadCrashHandler();

	Dl_info info {};
	if (::dladdr(reinterpret_cast<void*>(get_api), &info) != 0 && info.dli_fname != nullptr) {
		LOGF("RenderDoc: bound API from %s\n", info.dli_fname);
	} else {
		LOGF("RenderDoc: bound API\n");
	}
	return true;
}

static RenderDocWindowHandle GetRenderDocWindowHandle(SDL_Window* window) {
	if (window == nullptr) {
		return nullptr;
	}

	SDL_SysWMinfo info {};
	SDL_VERSION(&info.version);

	if (SDL_GetWindowWMInfo(window, &info) != SDL_TRUE) {
		return nullptr;
	}

#if defined(SDL_VIDEO_DRIVER_X11)
	if (info.subsystem == SDL_SYSWM_X11) {
		// RenderDoc takes the raw xlib Window id in the pointer slot, not a Display*.
		return reinterpret_cast<RenderDocWindowHandle>(
		    static_cast<uintptr_t>(info.info.x11.window));
	}
#else
	if (info.subsystem == SDL_SYSWM_X11) {
		// SDL_VIDEO_DRIVER_X11 is not visible here: the source-tree SDL_config.h shadows the
		// generated one on the include path, so SDL_SysWMinfo::info has no x11 member at
		// compile time even though the SDL we link against was built with X11. Without this
		// branch every X11 session is misreported as Wayland and RenderDoc capture is refused
		// outright ("StartFrameCapture returned, but RenderDoc is not capturing").
		//
		// The union is padded to a fixed 64 bytes (SDL_syswm.h: `Uint8 dummy[64]`) and the
		// x11 member is `{ Display* display; Window window; }` at its start, so the window id
		// can be read positionally without the declaration.
		struct X11InfoLayout {
			void*         display;
			unsigned long window;
		};
		static_assert(sizeof(X11InfoLayout) <= sizeof(info.info),
		              "SDL_SysWMinfo union smaller than the X11 layout");
		X11InfoLayout x11 {};
		std::memcpy(&x11, &info.info, sizeof(x11));
		if (x11.window != 0) {
			return reinterpret_cast<RenderDocWindowHandle>(static_cast<uintptr_t>(x11.window));
		}
	}
#endif

	// Wayland capture works without an active-window handle.
	static std::atomic_bool logged = false;
	if (!logged.exchange(true)) {
		LOGF("RenderDoc: no native window handle for SDL subsystem %d (Wayland?); the in-app "
		     "overlay is unavailable, but --rd captures still work\n",
		     static_cast<int>(info.subsystem));
	}
	return nullptr;
}

#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

static bool IsAvailable() {
	return g_api != nullptr && g_device != nullptr && g_window != nullptr;
}

#else

static bool IsAvailable() {
	return g_api != nullptr && g_device != nullptr;
}

#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

void RenderDocInit() {
	bool expected = false;
	if (!g_init_done.compare_exchange_strong(expected, true)) {
		return;
	}

	HKEY h_reg_key;
	LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
	                            L"SOFTWARE\\Classes\\RenderDoc.RDCCapture.1\\DefaultIcon\\", 0,
	                            KEY_READ, &h_reg_key);
	if (result != ERROR_SUCCESS) {
		return;
	}
	std::array<wchar_t, MAX_PATH> key_str {};
	DWORD                         str_sz_out {key_str.size()};
	result = RegQueryValueExW(h_reg_key, L"", 0, NULL, (LPBYTE)key_str.data(), &str_sz_out);
	RegCloseKey(h_reg_key);
	if (result != ERROR_SUCCESS) {
		return;
	}

	std::filesystem::path path {key_str.cbegin(), key_str.cend()};
	path                   = path.parent_path().append("renderdoc.dll");
	const auto path_to_lib = path.generic_string();
	auto*      module      = LoadLibraryA(path_to_lib.c_str());
	if (module == nullptr) {
		return;
	}

	if (!BindRenderDocApi(module)) {
		LOGF("RenderDoc: API 1.4.2 is not available; in-app capture disabled\n");
		FreeLibrary(module);
		return;
	}
}

#else

void RenderDocInit() {
	bool expected = false;
	if (!g_init_done.compare_exchange_strong(expected, true)) {
		return;
	}

	// Prefer an injected RenderDoc instance.
	auto* module = ::dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
	if (module == nullptr) {
		module = ::dlopen("librenderdoc.so", RTLD_NOW);
	}
	if (module == nullptr) {
		LOGF("RenderDoc: librenderdoc.so was not found; in-app capture disabled\n");
		return;
	}

	if (!BindRenderDocApi(module)) {
		LOGF("RenderDoc: API 1.4.2 is not available; in-app capture disabled\n");
		::dlclose(module);
		return;
	}
}

#endif

void RenderDocSetActiveWindow(vk::Instance instance, SDL_Window* window) {
	if (g_api == nullptr) {
		return;
	}

	g_device = GetRenderDocDevicePointer(instance);
	g_window = GetRenderDocWindowHandle(window);

	if (g_device == nullptr || g_window == nullptr) {
		LOGF("RenderDoc: active Vulkan window was not registered\n");
		return;
	}

	g_api->SetActiveWindow(g_device, g_window);
	LOGF("RenderDoc: active Vulkan window registered\n");
}

void RenderDocRequestCapture() {
	if (!IsAvailable()) {
		if (!g_unavailable_log.exchange(true)) {
			LOGF("RenderDoc: capture requested, but RenderDoc is not available\n");
		}
		return;
	}

	RenderDocState expected = RenderDocState::Idle;
	if (g_state.compare_exchange_strong(expected, RenderDocState::Requested)) {
		LOGF("RenderDoc: capture requested; next complete presented frame will be captured\n");
	} else {
		LOGF("RenderDoc: capture request ignored because a capture is already pending\n");
	}
}

static void LogNewestCapture() {
	const auto count = g_api->GetNumCaptures();
	if (count == 0) {
		return;
	}

	char     filename[4096] = {};
	uint32_t path_length    = sizeof(filename);
	uint64_t timestamp      = 0;

	if (g_api->GetCapture(count - 1, filename, &path_length, &timestamp) != 0) {
		filename[sizeof(filename) - 1] = '\0';
		LOGF("RenderDoc: wrote capture %s\n", filename);
	}
}

void RenderDocOnPresent() {
	if (!IsAvailable()) {
		return;
	}

	switch (g_state.load()) {
		case RenderDocState::Idle: return;
		case RenderDocState::Requested:
			if (g_api->IsFrameCapturing() != 0) {
				// A capture is already open -- RenderDoc can begin one itself depending on how
				// the layer was activated. Dropping the request to Idle here means nothing ever
				// calls EndFrameCapture, so the capture stays open forever and no file is
				// written. Adopt it instead and let the next present close it out.
				LOGF("RenderDoc: a capture was already in progress; adopting it and ending it on "
				     "the next present\n");
				g_state.store(RenderDocState::Capturing);
				return;
			}

			g_api->StartFrameCapture(nullptr, nullptr);
			if (g_api->IsFrameCapturing() == 0) {
				LOGF("RenderDoc: StartFrameCapture returned, but RenderDoc is not capturing\n");
				g_state.store(RenderDocState::Idle);
				return;
			}
			g_state.store(RenderDocState::Capturing);
			LOGF("RenderDoc: capture started\n");
			return;
		case RenderDocState::Capturing: break;
	}

	const auto ok = g_api->EndFrameCapture(nullptr, nullptr);
	g_state.store(RenderDocState::Idle);

	if (ok != 0) {
		const auto count = g_api->GetNumCaptures();
		LOGF("RenderDoc: capture finished, count=%u\n", count);
		LogNewestCapture();
	} else {
		LOGF("RenderDoc: capture failed\n");
	}
}

} // namespace Libs::Graphics
