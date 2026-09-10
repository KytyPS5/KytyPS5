#define VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL 1

#include "graphics/rt/accelStruct.h"
#include "graphics/rt/bvhNode.h"

#include "common/assert.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-variable"
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

// This standalone test executable doesn't link vulkanCommon.cpp (the emulator's usual home for
// this), so it needs its own storage for the global dynamic dispatcher.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace {

using Libs::Graphics::BlasBuildResources;
using Libs::Graphics::BvhTriangleNode;
using Libs::Graphics::DestroyBlasBuildResources;
using Libs::Graphics::RecordBlasBuild;

void Check(bool value, const char* text) {
if (!value) {
std::fprintf(stderr, "AccelStructBuilderTests: failed: %s\n", text);
std::abort();
}
}

void CheckVk(vk::Result result, const char* text) {
if (result != vk::Result::eSuccess) {
std::fprintf(stderr, "AccelStructBuilderTests: failed: %s (VkResult %d)\n", text,
             static_cast<int>(result));
std::abort();
}
}

BvhTriangleNode MakeTriangle(float x0, float y0, float z0, float x1, float y1, float z1, float x2,
                              float y2, float z2) {
BvhTriangleNode triangle {};
triangle.vertices[0][0] = x0;
triangle.vertices[0][1] = y0;
triangle.vertices[0][2] = z0;
triangle.vertices[1][0] = x1;
triangle.vertices[1][1] = y1;
triangle.vertices[1][2] = z1;
triangle.vertices[2][0] = x2;
triangle.vertices[2][1] = y2;
triangle.vertices[2][2] = z2;
return triangle;
}

void TestFlattenTrianglesToVertexBufferPacksASingleTriangle() {
using namespace Libs::Graphics;

const BvhTriangleNode triangles[] = {MakeTriangle(1, 2, 3, 4, 5, 6, 7, 8, 9)};
const auto            flattened   = FlattenTrianglesToVertexBuffer(triangles);

Check(flattened.size() == 9, "single triangle did not flatten to 9 floats");
const float expected[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
for (size_t i = 0; i < 9; i++) {
Check(flattened[i] == expected[i],
      "flattened vertex value did not match the source triangle");
}
}

void TestFlattenTrianglesToVertexBufferPacksMultipleTrianglesInOrder() {
using namespace Libs::Graphics;

const BvhTriangleNode triangles[] = {
    MakeTriangle(1, 0, 0, 0, 1, 0, 0, 0, 1),
    MakeTriangle(2, 0, 0, 0, 2, 0, 0, 0, 2),
};
const auto flattened = FlattenTrianglesToVertexBuffer(triangles);

Check(flattened.size() == 18, "two triangles did not flatten to 18 floats");
Check(flattened[0] == 1.0f && flattened[9] == 2.0f,
      "second triangle's first vertex was not at offset 9 (triangles not packed in order?)");
}

void TestFlattenTrianglesToVertexBufferHandlesNoTriangles() {
using namespace Libs::Graphics;

const std::vector<BvhTriangleNode> triangles;
const auto                         flattened = FlattenTrianglesToVertexBuffer(triangles);

Check(flattened.empty(), "no triangles should flatten to an empty vertex buffer");
}

constexpr const char* RequiredRtExtensions[] = {
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_QUERY_EXTENSION_NAME,
};

// Bootstraps its own instance/device exactly like BvhAccelStructDeviceTests (deliberately not
// shared: see that file for why this stays decoupled from the production GraphicContext), then
// goes further by building a real BLAS from a single triangle and checking it out.
void TestBuildsABlasFromASingleTriangle() {
static vk::detail::DynamicLoader loader;
const auto                       get_instance_proc_addr =
    loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
Check(get_instance_proc_addr != nullptr, "could not load the Vulkan loader");
VULKAN_HPP_DEFAULT_DISPATCHER.init(get_instance_proc_addr);

vk::ApplicationInfo app {};
app.sType            = vk::StructureType::eApplicationInfo;
app.pApplicationName = "AccelStructBuilderTests";
app.apiVersion       = VK_API_VERSION_1_3;

vk::InstanceCreateInfo instance_info {};
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

vk::PhysicalDevice physical_device = physical_devices[0];
for (const auto& candidate: physical_devices) {
vk::PhysicalDeviceProperties properties {};
candidate.getProperties(&properties);
if (properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
physical_device = candidate;
break;
}
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
vk::DeviceQueueCreateInfo queue_info {};
queue_info.sType            = vk::StructureType::eDeviceQueueCreateInfo;
queue_info.queueFamilyIndex = queue_family;
queue_info.queueCount       = 1;
queue_info.pQueuePriorities = &queue_priority;

vk::PhysicalDeviceRayQueryFeaturesKHR ray_query {};
ray_query.sType    = vk::StructureType::ePhysicalDeviceRayQueryFeaturesKHR;
ray_query.rayQuery = true;

vk::PhysicalDeviceAccelerationStructureFeaturesKHR accel_structure {};
accel_structure.sType                 = vk::StructureType::ePhysicalDeviceAccelerationStructureFeaturesKHR;
accel_structure.accelerationStructure = true;
accel_structure.pNext                 = &ray_query;

vk::PhysicalDeviceVulkan12Features features12 {};
features12.sType               = vk::StructureType::ePhysicalDeviceVulkan12Features;
features12.bufferDeviceAddress = true;
features12.pNext               = &accel_structure;

vk::PhysicalDeviceFeatures2 features2 {};
features2.sType = vk::StructureType::ePhysicalDeviceFeatures2;
features2.pNext = &features12;

vk::DeviceCreateInfo device_info {};
device_info.sType                   = vk::StructureType::eDeviceCreateInfo;
device_info.pNext                   = &features2;
device_info.queueCreateInfoCount    = 1;
device_info.pQueueCreateInfos       = &queue_info;
device_info.enabledExtensionCount   = static_cast<uint32_t>(std::size(RequiredRtExtensions));
device_info.ppEnabledExtensionNames = RequiredRtExtensions;

vk::Device device = nullptr;
CheckVk(physical_device.createDevice(&device_info, nullptr, &device), "vkCreateDevice");
VULKAN_HPP_DEFAULT_DISPATCHER.init(device);

const vk::Queue queue = device.getQueue(queue_family, 0);

VmaVulkanFunctions vma_functions {};
vma_functions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
vma_functions.vkGetDeviceProcAddr   = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

VmaAllocatorCreateInfo allocator_info {};
allocator_info.instance         = instance;
allocator_info.physicalDevice   = physical_device;
allocator_info.device           = device;
allocator_info.pVulkanFunctions = &vma_functions;
allocator_info.vulkanApiVersion = VK_API_VERSION_1_3;
allocator_info.flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

VmaAllocator allocator = nullptr;
CheckVk(static_cast<vk::Result>(vmaCreateAllocator(&allocator_info, &allocator)),
        "vmaCreateAllocator");

vk::CommandPoolCreateInfo pool_info {};
pool_info.sType            = vk::StructureType::eCommandPoolCreateInfo;
pool_info.queueFamilyIndex = queue_family;
vk::CommandPool command_pool = nullptr;
CheckVk(device.createCommandPool(&pool_info, nullptr, &command_pool), "vkCreateCommandPool");

vk::CommandBufferAllocateInfo cmd_alloc_info {};
cmd_alloc_info.sType              = vk::StructureType::eCommandBufferAllocateInfo;
cmd_alloc_info.commandPool        = command_pool;
cmd_alloc_info.level              = vk::CommandBufferLevel::ePrimary;
cmd_alloc_info.commandBufferCount = 1;
vk::CommandBuffer command_buffer  = nullptr;
CheckVk(device.allocateCommandBuffers(&cmd_alloc_info, &command_buffer),
        "vkAllocateCommandBuffers");

vk::CommandBufferBeginInfo begin_info {};
begin_info.sType = vk::StructureType::eCommandBufferBeginInfo;
begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
CheckVk(command_buffer.begin(&begin_info), "vkBeginCommandBuffer");

// One triangle with distinct, non-degenerate coordinates so a bug that swapped or dropped a
// vertex would be visible if this test grows position assertions later.
const BvhTriangleNode triangles[] = {MakeTriangle(0, 0, 0, 1, 0, 0, 0, 1, 0)};

auto resources = RecordBlasBuild(physical_device, device, allocator, command_buffer, triangles);

CheckVk(command_buffer.end(), "vkEndCommandBuffer");

vk::FenceCreateInfo fence_info {};
fence_info.sType = vk::StructureType::eFenceCreateInfo;
vk::Fence fence   = nullptr;
CheckVk(device.createFence(&fence_info, nullptr, &fence), "vkCreateFence");

vk::SubmitInfo submit_info {};
submit_info.sType              = vk::StructureType::eSubmitInfo;
submit_info.commandBufferCount = 1;
submit_info.pCommandBuffers    = &command_buffer;
CheckVk(queue.submit(1, &submit_info, fence), "vkQueueSubmit");
CheckVk(device.waitForFences(1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");

Check(bool(resources.handle), "acceleration structure handle was null after a successful build");

vk::AccelerationStructureDeviceAddressInfoKHR as_address_info {};
as_address_info.sType                 = vk::StructureType::eAccelerationStructureDeviceAddressInfoKHR;
as_address_info.accelerationStructure = resources.handle;
const auto as_address = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetAccelerationStructureDeviceAddressKHR(
	    device, static_cast<const VkAccelerationStructureDeviceAddressInfoKHR*>(as_address_info));
Check(as_address != 0, "built acceleration structure has a null device address");

device.destroyFence(fence);
DestroyBlasBuildResources(allocator, device, resources);
device.destroyCommandPool(command_pool);
vmaDestroyAllocator(allocator);
device.destroy();
instance.destroy();
std::puts("AccelStructBuilderTests: built a BLAS from a single triangle on the GPU");
}

} // namespace

namespace Common {

int  DbgExitHandler(const char*, int, std::string_view) { std::abort(); }
int  DbgExitHandler(const char*, int, fmt::text_style, std::string_view) { std::abort(); }
int  DbgExitIfHandler(const char*, const char*, int) { return 1; }
void DbgExit(int) { std::abort(); }

} // namespace Common

int main(int argc, char** argv) {
TestFlattenTrianglesToVertexBufferPacksASingleTriangle();
TestFlattenTrianglesToVertexBufferPacksMultipleTrianglesInOrder();
TestFlattenTrianglesToVertexBufferHandlesNoTriangles();

const bool flatten_only = argc > 1 && std::string_view(argv[1]) == "--flatten-only";
if (flatten_only) {
std::puts("AccelStructBuilderTests: flatten-only cases passed");
return 0;
}

TestBuildsABlasFromASingleTriangle();
std::puts("AccelStructBuilderTests: all cases passed");
return 0;
}
