#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define VK_NO_PROTOTYPES
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC    1
#define VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL 1
#define VULKAN_HPP_NO_CONSTRUCTORS
#define VULKAN_HPP_NO_EXCEPTIONS

#include <vulkan/vulkan.hpp>

// This standalone test executable doesn't link vulkanCommon.cpp (the emulator's usual home for
// this), so it needs its own storage for the global dynamic dispatcher.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace {

void Check(bool value, const char* text) {
if (!value) {
std::fprintf(stderr, "BvhAccelStructDeviceTests: failed: %s\n", text);
std::abort();
}
}

void CheckVk(vk::Result result, const char* text) {
if (result != vk::Result::eSuccess) {
std::fprintf(stderr, "BvhAccelStructDeviceTests: failed: %s (VkResult %d)\n", text,
             static_cast<int>(result));
std::abort();
}
}

// The extensions hardware.cpp's AppendHardwareRayTracingDeviceExtensions negotiates in the real
// emulator. Duplicated here rather than reused because that function is a GraphicContext method
// tied to the full production device-creation flow; this test only needs the four names.
constexpr const char* RequiredRtExtensions[] = {
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_QUERY_EXTENSION_NAME,
};

bool ExtensionAvailable(const std::vector<vk::ExtensionProperties>& available, const char* name) {
return std::any_of(available.begin(), available.end(), [name](const auto& extension) {
return std::strcmp(extension.extensionName, name) == 0;
});
}

// Proves the environment can actually do hardware ray tracing before any acceleration-structure
// building code gets written on top of that assumption: creates its own minimal instance and
// device (deliberately not the shared VulkanHarness, whose device is created in its constructor
// before RT extensions could be requested), enables the required extensions and their feature
// flags, and confirms every RT function pointer this feature will need actually loads.
void TestCreatesRtCapableDeviceAndLoadsFunctionPointers() {
static vk::detail::DynamicLoader loader;
const auto get_instance_proc_addr =
    loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
Check(get_instance_proc_addr != nullptr, "could not load the Vulkan loader");
VULKAN_HPP_DEFAULT_DISPATCHER.init(get_instance_proc_addr);

vk::ApplicationInfo app{};
app.sType         = vk::StructureType::eApplicationInfo;
app.pApplicationName = "BvhAccelStructDeviceTests";
app.apiVersion    = VK_API_VERSION_1_3;

vk::InstanceCreateInfo instance_info{};
instance_info.sType            = vk::StructureType::eInstanceCreateInfo;
instance_info.pApplicationInfo = &app;

vk::Instance instance = nullptr;
CheckVk(vk::createInstance(&instance_info, nullptr, &instance), "vkCreateInstance");
VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);

uint32_t physical_count = 0;
CheckVk(instance.enumeratePhysicalDevices(&physical_count, nullptr),
        "vkEnumeratePhysicalDevices (count)");
Check(physical_count > 0, "no Vulkan physical devices found");
std::vector<vk::PhysicalDevice> physical_devices(physical_count);
CheckVk(instance.enumeratePhysicalDevices(&physical_count, physical_devices.data()),
        "vkEnumeratePhysicalDevices");

// Prefer a discrete GPU: on a multi-GPU machine, device 0 isn't necessarily the one that
// actually supports RT.
vk::PhysicalDevice physical_device = physical_devices[0];
for (const auto& candidate: physical_devices) {
vk::PhysicalDeviceProperties properties{};
candidate.getProperties(&properties);
if (properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
physical_device = candidate;
break;
}
}

uint32_t extension_count = 0;
CheckVk(physical_device.enumerateDeviceExtensionProperties(nullptr, &extension_count, nullptr),
        "vkEnumerateDeviceExtensionProperties (count)");
std::vector<vk::ExtensionProperties> available_extensions(extension_count);
CheckVk(physical_device.enumerateDeviceExtensionProperties(nullptr, &extension_count,
                                                            available_extensions.data()),
        "vkEnumerateDeviceExtensionProperties");

for (const auto* extension: RequiredRtExtensions) {
Check(ExtensionAvailable(available_extensions, extension), extension);
}

uint32_t queue_family_count = 0;
physical_device.getQueueFamilyProperties(&queue_family_count, nullptr);
std::vector<vk::QueueFamilyProperties> queue_families(queue_family_count);
physical_device.getQueueFamilyProperties(&queue_family_count, queue_families.data());
uint32_t queue_family = UINT32_MAX;
for (uint32_t i = 0; i < queue_families.size(); i++) {
if ((queue_families[i].queueFlags & vk::QueueFlagBits::eCompute) ==
    vk::QueueFlagBits::eCompute) {
queue_family = i;
break;
}
}
Check(queue_family != UINT32_MAX, "no compute-capable queue family found");

const float               queue_priority = 1.0f;
vk::DeviceQueueCreateInfo queue_info{};
queue_info.sType            = vk::StructureType::eDeviceQueueCreateInfo;
queue_info.queueFamilyIndex = queue_family;
queue_info.queueCount       = 1;
queue_info.pQueuePriorities = &queue_priority;

vk::PhysicalDeviceRayQueryFeaturesKHR ray_query{};
ray_query.sType    = vk::StructureType::ePhysicalDeviceRayQueryFeaturesKHR;
ray_query.rayQuery = true;

vk::PhysicalDeviceAccelerationStructureFeaturesKHR accel_structure{};
accel_structure.sType = vk::StructureType::ePhysicalDeviceAccelerationStructureFeaturesKHR;
accel_structure.accelerationStructure = true;
accel_structure.pNext                 = &ray_query;

vk::PhysicalDeviceVulkan12Features features12{};
features12.sType               = vk::StructureType::ePhysicalDeviceVulkan12Features;
features12.bufferDeviceAddress = true;
features12.pNext               = &accel_structure;

vk::PhysicalDeviceFeatures2 features2{};
features2.sType = vk::StructureType::ePhysicalDeviceFeatures2;
features2.pNext = &features12;

vk::DeviceCreateInfo device_info{};
device_info.sType                   = vk::StructureType::eDeviceCreateInfo;
device_info.pNext                   = &features2;
device_info.queueCreateInfoCount    = 1;
device_info.pQueueCreateInfos       = &queue_info;
device_info.enabledExtensionCount   = static_cast<uint32_t>(std::size(RequiredRtExtensions));
device_info.ppEnabledExtensionNames = RequiredRtExtensions;

vk::Device device = nullptr;
CheckVk(physical_device.createDevice(&device_info, nullptr, &device), "vkCreateDevice");
VULKAN_HPP_DEFAULT_DISPATCHER.init(device);

Check(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetAccelerationStructureBuildSizesKHR != nullptr,
      "vkGetAccelerationStructureBuildSizesKHR did not load");
Check(VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateAccelerationStructureKHR != nullptr,
      "vkCreateAccelerationStructureKHR did not load");
Check(VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyAccelerationStructureKHR != nullptr,
      "vkDestroyAccelerationStructureKHR did not load");
Check(VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBuildAccelerationStructuresKHR != nullptr,
      "vkCmdBuildAccelerationStructuresKHR did not load");
Check(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetAccelerationStructureDeviceAddressKHR != nullptr,
      "vkGetAccelerationStructureDeviceAddressKHR did not load");
Check(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetBufferDeviceAddressKHR != nullptr ||
          VULKAN_HPP_DEFAULT_DISPATCHER.vkGetBufferDeviceAddress != nullptr,
      "vkGetBufferDeviceAddress(KHR) did not load");

device.destroy(nullptr);
instance.destroy(nullptr);
std::puts("BvhAccelStructDeviceTests: RT-capable device created, function pointers loaded");
}

} // namespace

int main() {
TestCreatesRtCapableDeviceAndLoadsFunctionPointers();
return 0;
}
