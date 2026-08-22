#include "graphics/host_gpu/vulkanDiagnostics.h"

#include "SDL.h"
#include "SDL_vulkan.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/host_gpu/vulkanInstance.h"

#include <algorithm>
#include <fmt/format.h>
#include <vector>

namespace Libs::Graphics {

namespace {

// The extensions this emulator actually looks for. Reporting the full list a
// driver exposes would bury the four lines that matter under two hundred that
// do not.
constexpr const char* INTERESTING_EXTENSIONS[] = {
    VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
    VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME,
    VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME,
    VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
    VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_QUERY_EXTENSION_NAME,
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

std::string ApiVersionToString(uint32_t version) {
	return fmt::format("{}.{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
	                   VK_API_VERSION_PATCH(version));
}

// Driver versions are vendor-encoded. NVIDIA and Intel-on-Windows pack them
// differently from the Vulkan convention, and printing 2298589184 helps nobody,
// so decode the two that are worth decoding and print the raw value alongside.
std::string DriverVersionToString(uint32_t vendor_id, uint32_t version) {
	constexpr uint32_t VENDOR_NVIDIA = 0x10DE;

	if (vendor_id == VENDOR_NVIDIA) {
		return fmt::format("{}.{}.{}.{} (raw {})", (version >> 22U) & 0x3FFU,
		                   (version >> 14U) & 0xFFU, (version >> 6U) & 0xFFU, version & 0x3FU,
		                   version);
	}
	return fmt::format("{} (raw {})", ApiVersionToString(version), version);
}

const char* DeviceTypeToString(vk::PhysicalDeviceType type) {
	switch (type) {
		case vk::PhysicalDeviceType::eIntegratedGpu: return "integrated GPU";
		case vk::PhysicalDeviceType::eDiscreteGpu: return "discrete GPU";
		case vk::PhysicalDeviceType::eVirtualGpu: return "virtual GPU";
		case vk::PhysicalDeviceType::eCpu: return "CPU";
		default: return "other";
	}
}

std::vector<vk::ExtensionProperties> DeviceExtensions(vk::PhysicalDevice device) {
	uint32_t count = 0;
	if (device.enumerateDeviceExtensionProperties(nullptr, &count, nullptr) !=
	        vk::Result::eSuccess ||
	    count == 0) {
		return {};
	}
	std::vector<vk::ExtensionProperties> extensions(count);
	if (device.enumerateDeviceExtensionProperties(nullptr, &count, extensions.data()) !=
	    vk::Result::eSuccess) {
		return {};
	}
	return extensions;
}

void AppendDevice(std::string& report, vk::PhysicalDevice device, uint32_t index) {
	vk::PhysicalDeviceProperties props {};
	device.getProperties(&props);

	report += fmt::format("  [{}] {}\n", index, props.deviceName.data());
	report += fmt::format("      type:   {}\n", DeviceTypeToString(props.deviceType));
	report += fmt::format("      ids:    vendor 0x{:04x}, device 0x{:04x}\n", props.vendorID,
	                      props.deviceID);
	report += fmt::format("      api:    {}\n", ApiVersionToString(props.apiVersion));
	report += fmt::format("      driver: {}\n", DriverVersionToString(props.vendorID,
	                                                                  props.driverVersion));

	vk::PhysicalDeviceMemoryProperties memory {};
	device.getMemoryProperties(&memory);
	for (uint32_t i = 0; i < memory.memoryHeapCount; i++) {
		const auto& heap = memory.memoryHeaps[i];
		const bool  local =
		    static_cast<bool>(heap.flags & vk::MemoryHeapFlagBits::eDeviceLocal);
		report += fmt::format("      heap {}: {} MiB{}\n", i, heap.size / (1024ULL * 1024ULL),
		                      local ? ", device local" : "");
	}

	const auto extensions = DeviceExtensions(device);
	for (const auto* wanted: INTERESTING_EXTENSIONS) {
		const bool present =
		    std::any_of(extensions.begin(), extensions.end(), [wanted](const auto& available) {
			    return std::string_view(available.extensionName.data()) == wanted;
		    });
		report += fmt::format("      {} {}\n", present ? "yes" : " no", wanted);
	}
	report += "\n";
}

} // namespace

std::string BuildVulkanReport() {
	std::string report = "Vulkan\n";

	// SDL will not hand out the Vulkan loader until its video subsystem is up.
	// On a headless box that init fails, which is itself worth reporting rather
	// than hiding: it means the emulator would not open a window here either.
	const bool video_was_up = SDL_WasInit(SDL_INIT_VIDEO) != 0;
	if (!video_was_up && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
		report += fmt::format("  no display available, so Vulkan cannot be queried: {}\n\n",
		                      SDL_GetError());
		return report;
	}

	auto release_video = [video_was_up] {
		if (!video_was_up) {
			SDL_QuitSubSystem(SDL_INIT_VIDEO);
		}
	};

	if (SDL_Vulkan_LoadLibrary(nullptr) != 0) {
		report += fmt::format("  no Vulkan loader on this machine: {}\n\n", SDL_GetError());
		release_video();
		return report;
	}

	auto* get_instance_proc_addr =
	    reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
	if (get_instance_proc_addr == nullptr) {
		report += fmt::format("  Vulkan loader present but unusable: {}\n\n", SDL_GetError());
		SDL_Vulkan_UnloadLibrary();
		release_video();
		return report;
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(get_instance_proc_addr);

	vk::ApplicationInfo app_info {};
	app_info.sType            = vk::StructureType::eApplicationInfo;
	app_info.pApplicationName = "Kyty";
	app_info.pEngineName      = "Kyty";
	// Deliberately not VULKAN_TARGET_API_VERSION: a machine whose loader is too
	// old to give us 1.3 is precisely the machine we want a report from, and
	// asking for 1.0 still enumerates every device and its real apiVersion.
	app_info.apiVersion = VK_API_VERSION_1_0;

	vk::InstanceCreateInfo info {};
	info.sType             = vk::StructureType::eInstanceCreateInfo;
	info.pApplicationInfo  = &app_info;

	vk::Instance instance = nullptr;
	const auto   result   = vk::createInstance(&info, nullptr, &instance);
	if (result != vk::Result::eSuccess) {
		report += fmt::format("  could not create a Vulkan instance: {}\n\n",
		                      VulkanToString(result));
		SDL_Vulkan_UnloadLibrary();
		release_video();
		return report;
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);

	uint32_t count = 0;
	if (instance.enumeratePhysicalDevices(&count, nullptr) != vk::Result::eSuccess || count == 0) {
		report += "  no Vulkan devices found\n\n";
		instance.destroy(nullptr);
		SDL_Vulkan_UnloadLibrary();
		release_video();
		return report;
	}

	std::vector<vk::PhysicalDevice> devices(count);
	if (instance.enumeratePhysicalDevices(&count, devices.data()) != vk::Result::eSuccess) {
		report += "  device enumeration failed\n\n";
		instance.destroy(nullptr);
		SDL_Vulkan_UnloadLibrary();
		release_video();
		return report;
	}

	report += fmt::format("  {} device(s)\n\n", count);
	for (uint32_t i = 0; i < count; i++) {
		AppendDevice(report, devices[i], i);
	}

	instance.destroy(nullptr);
	SDL_Vulkan_UnloadLibrary();
	release_video();
	return report;
}

} // namespace Libs::Graphics
