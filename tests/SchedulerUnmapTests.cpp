#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/sync.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <semaphore>

using namespace Libs::Graphics;

namespace {

void Require(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		std::exit(EXIT_FAILURE);
	}
}

void RequireVk(vk::Result result, const char* operation) {
	if (result != vk::Result::eSuccess) {
		std::fprintf(stderr, "%s: %s\n", operation, vk::to_string(result).c_str());
		std::exit(EXIT_FAILURE);
	}
}

class VulkanContext {
public:
	VulkanContext() {
		const auto get_proc =
		    m_loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
		Require(get_proc != nullptr, "Vulkan loader unavailable");
		VULKAN_HPP_DEFAULT_DISPATCHER.init(get_proc);
		vk::ApplicationInfo app {};
		app.apiVersion = VULKAN_TARGET_API_VERSION;
		vk::InstanceCreateInfo instance_info {};
		instance_info.pApplicationInfo = &app;
		RequireVk(vk::createInstance(&instance_info, nullptr, &graphics.instance),
		          "create instance");
		VULKAN_HPP_DEFAULT_DISPATCHER.init(graphics.instance);
		const auto devices = EnumerateVulkan<vk::PhysicalDevice>(
		    "enumerate physical devices", [&](uint32_t* count, vk::PhysicalDevice* values) {
			    return graphics.instance.enumeratePhysicalDevices(count, values);
		    });
		Require(!devices.empty(), "no Vulkan device");
		graphics.physical_device = devices.front();
		graphics.physical_device.getProperties(&graphics.physical_device_properties);
		graphics.physical_device.getMemoryProperties(&graphics.physical_device_memory_properties);
		std::printf("Device: %s\n", graphics.physical_device_properties.deviceName.data());
		uint32_t count = 0;
		graphics.physical_device.getQueueFamilyProperties(&count, nullptr);
		std::vector<vk::QueueFamilyProperties> families(count);
		graphics.physical_device.getQueueFamilyProperties(&count, families.data());
		for (uint32_t i = 0; i < count; ++i) {
			if (families[i].queueFlags & vk::QueueFlagBits::eGraphics) {
				graphics.queue_family = i;
				break;
			}
		}
		Require(graphics.queue_family != uint32_t(-1), "no graphics queue");
		vk::PhysicalDeviceVulkan13Features features13 {};
		vk::PhysicalDeviceVulkan12Features features12 {};
		features12.pNext = &features13;
		vk::PhysicalDeviceFeatures2 features {};
		features.pNext = &features12;
		graphics.physical_device.getFeatures2(&features);
		Require(features12.timelineSemaphore && features12.bufferDeviceAddress,
		        "timeline semaphores and buffer device addresses are required");
		const float               priority = 1.0f;
		vk::DeviceQueueCreateInfo queue_info {};
		queue_info.queueFamilyIndex = graphics.queue_family;
		queue_info.queueCount       = 1;
		queue_info.pQueuePriorities = &priority;
		const char*          extensions[] {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
		vk::DeviceCreateInfo device_info {};
		device_info.pNext                   = &features;
		device_info.queueCreateInfoCount    = 1;
		device_info.pQueueCreateInfos       = &queue_info;
		device_info.enabledExtensionCount   = std::size(extensions);
		device_info.ppEnabledExtensionNames = extensions;
		RequireVk(graphics.physical_device.createDevice(&device_info, nullptr, &graphics.device),
		          "create device");
		VULKAN_HPP_DEFAULT_DISPATCHER.init(graphics.device);
		graphics.device.getQueue(graphics.queue_family, 0, &graphics.queue);
		Require(graphics.CreateAllocator(), "create allocator");
	}

	~VulkanContext() {
		graphics.DestroyAllocator();
		graphics.device.destroy();
		graphics.instance.destroy();
	}

private:
	vk::detail::DynamicLoader m_loader;

public:
	GraphicContext graphics;
};

constexpr uint64_t Base = 0x60000000ull;
constexpr uint64_t Page = 0x4000;

void CheckHostEvent(RenderContext& context) {
	auto& scheduler = context.GetCommandScheduler();
	context.MapMemory(Base, Page);
	Sync::TriggerEopEventAtEndOfPipe(scheduler.Current(), 123, 0);
	const auto tick = scheduler.CurrentTick();
	context.UnmapMemory(Base, Page);
	Require(scheduler.CurrentTick() == tick, "host-only event submitted GPU work during unmap");
	Require(!context.IsMapped(Base, Page), "unmap retained the mapping");
	scheduler.Finish();
	scheduler.WaitPriorityOperations(tick);
	std::puts("PASS: host-only event does not drain unrelated unmap");
}

void CheckQueuedRanges(RenderContext& context) {
	auto&            scheduler = context.GetCommandScheduler();
	std::atomic<int> completed = 0;
	const auto       tick      = scheduler.CurrentTick();
	scheduler.DeferPriorityOperation([&] { ++completed; }, GuestRange {Base, Page});
	scheduler.DeferPriorityOperation([&] { ++completed; }, GuestRange {Base + Page * 2, Page});
	Require(scheduler.HasPendingPriorityOperations({Base, Page}), "first queued range lost");
	Require(scheduler.HasPendingPriorityOperations({Base + Page * 2, Page}),
	        "later queued range lost");
	Require(scheduler.HasPendingPriorityOperations({Base - 1, 2}), "partial overlap missed");
	Require(scheduler.HasPendingPriorityOperations({Base + 1, 1}), "contained range missed");
	Require(scheduler.HasPendingPriorityOperations({Base - 1, Page + 2}),
	        "containing range missed");
	Require(!scheduler.HasPendingPriorityOperations({Base - Page, Page}),
	        "left adjacency overlaps");
	Require(!scheduler.HasPendingPriorityOperations({Base + Page, Page}),
	        "right adjacency overlaps");
	context.MapMemory(Base + Page, Page);
	context.UnmapMemory(Base + Page, Page);
	Require(scheduler.CurrentTick() == tick && completed == 0, "disjoint unmap drained callbacks");
	context.MapMemory(Base, Page);
	context.UnmapMemory(Base, Page);
	Require(scheduler.CurrentTick() == tick + 1 && completed == 2,
	        "overlapping unmap did not finish callbacks");
	Require(!scheduler.HasPendingPriorityOperations(), "completed operations retained");
	std::puts("PASS: queued ranges, overlap boundaries and completion");
}

void CheckUnknownRange(RenderContext& context) {
	auto&             scheduler = context.GetCommandScheduler();
	std::atomic<bool> completed = false;
	scheduler.DeferPriorityOperation([&] { completed = true; });
	const auto tick = scheduler.CurrentTick();
	context.MapMemory(Base, Page);
	context.UnmapMemory(Base, Page);
	Require(scheduler.CurrentTick() == tick + 1 && completed, "unknown range was not conservative");
	std::puts("PASS: unknown range remains conservative");
}

void CheckGuestWriteback(RenderContext& context) {
	using namespace Libs::LibKernel::Memory;
	int64_t offset = -1;
	Require(KernelAllocateDirectMemory(0, KernelGetDirectMemorySize(), Page, Page, 0, &offset) == 0,
	        "allocate guest backing");
	void* mapped = reinterpret_cast<void*>(Base);
	Require(KernelMapDirectMemory(&mapped, Page, 0x3, 0x10, offset, Page) == 0,
	        "map guest backing");
	context.MapMemory(Base, Page);
	static constexpr uint32_t expected  = 0x1234abcd;
	auto&                     scheduler = context.GetCommandScheduler();
	scheduler.DeferPriorityOperation([] { WriteBacking(Base, &expected, sizeof(expected)); },
	                                 GuestRange {Base, sizeof(expected)});
	context.UnmapMemory(Base, Page);
	uint32_t actual = 0;
	Require(TryReadBacking(Base, &actual, sizeof(actual)), "read guest backing");
	Require(actual == expected, "unmap returned before guest writeback");
	Require(KernelMunmap(Base, Page) == 0, "unmap guest backing");
	Require(KernelReleaseDirectMemory(offset, Page) == 0, "release guest backing");
	std::puts("PASS: guest writeback completes before unmap returns");
}

void CheckActiveRange(RenderContext& context) {
	auto&                 scheduler = context.GetCommandScheduler();
	std::binary_semaphore entered(0);
	std::binary_semaphore release(0);
	scheduler.DeferPriorityOperation(
	    [&] {
		    entered.release();
		    release.acquire();
	    },
	    GuestRange {Base, Page});
	const auto tick = scheduler.CurrentTick();
	scheduler.Flush();
	entered.acquire();
	Require(scheduler.HasPendingPriorityOperations({Base, Page}),
	        "executing callback lost its range");
	Require(!scheduler.HasPendingPriorityOperations({Base + Page, Page}),
	        "executing callback aliases adjacent range");
	context.MapMemory(Base + Page, Page);
	context.UnmapMemory(Base + Page, Page);
	Require(scheduler.CurrentTick() == tick + 1,
	        "disjoint unmap submitted while callback executing");
	release.release();
	scheduler.WaitPriorityOperations(tick);
	Require(!scheduler.HasPendingPriorityOperations({Base, Page}),
	        "finished callback retained its range");
	std::puts("PASS: active callback range retained until completion");
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	Common::InitializeThreads();
	static Common::Subsystems subsystems;
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	Config::Load(options);
	subsystems.Initialize<Log::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
	ShaderInit();
	VulkanContext  vulkan;
	RenderContext  context(vulkan.graphics);
	HW::Context    registers {};
	HW::UserConfig user_config {};
	HW::Shader     shaders {};
	context.GetCommandScheduler().Begin(registers, user_config, shaders);
	CheckHostEvent(context);
	CheckQueuedRanges(context);
	CheckUnknownRange(context);
	CheckActiveRange(context);
	CheckGuestWriteback(context);
	return EXIT_SUCCESS;
}
