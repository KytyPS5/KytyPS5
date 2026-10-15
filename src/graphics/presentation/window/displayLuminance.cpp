#include "common/common.h"
#include "graphics/presentation/window.h"
#include "graphics/presentation/window/windowInternal.h"

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_6.h>
#endif

namespace Libs::Graphics {

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

// DXGI reports the values Windows settled on for the monitor: a calibration profile (e.g. from
// the Windows HDR Calibration app) overrides the driver and EDID values.
bool HostGetDisplayLuminance(void* native_window, DisplayLuminance* luminance) {
	auto* const monitor =
	    MonitorFromWindow(static_cast<HWND>(native_window), MONITOR_DEFAULTTOPRIMARY);
	IDXGIFactory1* factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
		return false;
	}
	bool           found   = false;
	IDXGIAdapter1* adapter = nullptr;
	for (UINT a = 0; !found && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
		IDXGIOutput* output = nullptr;
		for (UINT o = 0; !found && adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
			IDXGIOutput6* output6 = nullptr;
			if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6),
			                                     reinterpret_cast<void**>(&output6)))) {
				DXGI_OUTPUT_DESC1 desc {};
				if (SUCCEEDED(output6->GetDesc1(&desc)) && desc.Monitor == monitor &&
				    desc.MaxLuminance > 0.0f && desc.MaxFullFrameLuminance > 0.0f) {
					luminance->min_luminance            = desc.MinLuminance;
					luminance->max_luminance            = desc.MaxLuminance;
					luminance->max_full_frame_luminance = desc.MaxFullFrameLuminance;
					found                               = true;
				}
				output6->Release();
			}
			output->Release();
		}
		adapter->Release();
	}
	factory->Release();
	return found;
}

#else

bool HostGetDisplayLuminance([[maybe_unused]] void*             native_window,
                             [[maybe_unused]] DisplayLuminance* luminance) {
	return false;
}

#endif

} // namespace Libs::Graphics
