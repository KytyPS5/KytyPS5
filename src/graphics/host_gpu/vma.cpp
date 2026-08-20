#include "graphics/host_gpu/vulkanCommon.h"

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

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vma.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace Libs::Graphics {

// Allocation priority for VK_EXT_memory_priority: 0.0 means "demote this
// first", 1.0 means "demote this last", and VMA uses 0.5 when nothing is
// specified. It is a hint about relative value, so raising one class is
// enough to order it against everything left at the default.
//
// Only images are raised here, deliberately. A demoted render target has to
// be paged back in before the next pass that samples it, whereas most buffer
// content can be re-uploaded from guest memory the emulator still holds. The
// generic CreateBuffer() path also serves vertex/index buffers that may be
// just as hot as an image, so lowering it wholesale would be guessing, and a
// wrong guess here costs performance silently. Finer classification (staging
// and tiler scratch really are cheap to lose) is a sensible follow-up once
// someone measures it.
//
// This complements TextureCache::RunGarbageCollector() rather than replacing
// it: the GC still frees things, this just gives the driver a cheaper option
// one step earlier.
namespace {
constexpr float MEMORY_PRIORITY_IMAGE = 0.8F;
} // namespace

namespace {

struct MemoryStats {
	std::atomic_uint64_t allocated[VK_MAX_MEMORY_TYPES] {};
	std::atomic_uint64_t count[VK_MAX_MEMORY_TYPES] {};
};

MemoryStats g_memory_stats;

void TrackAllocationImpl(const VulkanMemory& memory) {
	g_memory_stats.allocated[memory.type] += memory.requirements.size;
	g_memory_stats.count[memory.type]++;
}

void UntrackAllocationImpl(const VulkanMemory& memory) {
	EXIT_IF(g_memory_stats.allocated[memory.type] < memory.requirements.size);
	EXIT_IF(g_memory_stats.count[memory.type] == 0);
	g_memory_stats.allocated[memory.type] -= memory.requirements.size;
	g_memory_stats.count[memory.type]--;
}

} // namespace

void VulkanTrackAllocation(const VulkanMemory& memory) {
	TrackAllocationImpl(memory);
}

void VulkanUntrackAllocation(const VulkanMemory& memory) {
	UntrackAllocationImpl(memory);
}

bool GraphicContext::CreateAllocator() {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(instance == nullptr || physical_device == nullptr || device == nullptr ||
	        allocator != nullptr);

	VmaVulkanFunctions functions {};
	functions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
	functions.vkGetDeviceProcAddr   = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

	VmaAllocatorCreateInfo info {};
	info.instance         = instance;
	info.physicalDevice   = physical_device;
	info.device           = device;
	info.pVulkanFunctions = &functions;
	info.vulkanApiVersion = VULKAN_TARGET_API_VERSION;
	info.flags = memory_budget_ext_enabled ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0;
	// Without this bit VMA ignores VmaAllocationCreateInfo::priority entirely,
	// so the per-allocation priorities set below would be silently inert.
	if (memory_priority_ext_enabled) {
		info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;
	}

	const auto result = static_cast<vk::Result>(vmaCreateAllocator(&info, &allocator));
	if (result != vk::Result::eSuccess) {
		LOGF("vmaCreateAllocator failed: %s\n", VulkanToString(result).c_str());
		return false;
	}
	return true;
}

// ── Persistent pipeline cache ────────────────────────────────────────────
//
// Every createGraphicsPipelines/createComputePipelines call in the renderer
// used to pass a null VkPipelineCache, so the driver recompiled each pipeline
// from scratch on every launch. PipelineCache in renderer/pipeline is a
// software map of vk::Pipeline handles: it stops us asking the driver twice
// in one run, but it does not survive process exit and it is not what the
// driver consults when it does compile.
//
// The blob on disk is untrusted input. It may have been written by a
// different GPU, or by an older driver, or by a run that was killed
// mid-write. Vulkan puts a header on it precisely so this can be checked, so
// validate vendor/device id and the pipeline-cache UUID against the device we
// actually bound to before handing anything to the driver. A mismatch is the
// normal consequence of a driver update, not an error worth shouting about.

namespace {

constexpr uint32_t PIPELINE_CACHE_HEADER_BYTES = 32;
constexpr uint32_t PIPELINE_CACHE_VERSION_ONE  = 1;

uint32_t ReadLe32(const uint8_t* p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) |
	       (static_cast<uint32_t>(p[2]) << 16U) | (static_cast<uint32_t>(p[3]) << 24U);
}

// True when the blob's header describes the device we are running on.
bool PipelineCacheMatchesDevice(const std::vector<uint8_t>&         blob,
                                 const vk::PhysicalDeviceProperties& props) {
	if (blob.size() < PIPELINE_CACHE_HEADER_BYTES) {
		return false;
	}
	const auto* h           = blob.data();
	const auto  header_size = ReadLe32(h + 0);
	const auto  header_ver  = ReadLe32(h + 4);
	const auto  vendor_id   = ReadLe32(h + 8);
	const auto  device_id   = ReadLe32(h + 12);

	return header_size >= PIPELINE_CACHE_HEADER_BYTES && header_size <= blob.size() &&
	       header_ver == PIPELINE_CACHE_VERSION_ONE && vendor_id == props.vendorID &&
	       device_id == props.deviceID &&
	       std::memcmp(h + 16, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

std::vector<uint8_t> ReadPipelineCacheFile(const std::filesystem::path& path) {
	std::error_code ec;
	if (!std::filesystem::exists(path, ec) || ec) {
		return {}; // first run
	}
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return {};
	}
	std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)),
	                           std::istreambuf_iterator<char>());
	return blob;
}

} // namespace

void GraphicContext::CreatePipelineCache() {
	EXIT_IF(device == nullptr);

	const auto path = Config::GetPipelineCacheFile();

	std::vector<uint8_t> blob;
	if (!path.empty()) {
		blob = ReadPipelineCacheFile(path);
		if (!blob.empty() && !PipelineCacheMatchesDevice(blob, physical_device_properties)) {
			LOGF("Pipeline cache does not match this device/driver, starting cold\n");
			blob.clear();
		}
	}

	vk::PipelineCacheCreateInfo info {};
	info.sType           = vk::StructureType::ePipelineCacheCreateInfo;
	info.initialDataSize = blob.size();
	info.pInitialData    = blob.empty() ? nullptr : blob.data();

	auto result = device.createPipelineCache(&info, nullptr, &pipeline_cache);
	if (result != vk::Result::eSuccess && !blob.empty()) {
		// The header matched but the driver still refused it. Try cold rather
		// than run without a cache at all.
		LOGF("Pipeline cache rejected by the driver (%s), retrying empty\n",
		     VulkanToString(result).c_str());
		info.initialDataSize = 0;
		info.pInitialData    = nullptr;
		result               = device.createPipelineCache(&info, nullptr, &pipeline_cache);
	}
	if (result != vk::Result::eSuccess) {
		LOGF("vkCreatePipelineCache failed: %s\n", VulkanToString(result).c_str());
		pipeline_cache = nullptr;
		return;
	}

	LOGF("Pipeline cache: %zu bytes loaded from %s\n", blob.size(),
	     path.empty() ? "(disabled)" : path.string().c_str());
}

void GraphicContext::SavePipelineCache() {
	if (pipeline_cache == nullptr) {
		return;
	}

	const auto path = Config::GetPipelineCacheFile();
	if (!path.empty()) {
		size_t     size   = 0;
		const auto sized  = device.getPipelineCacheData(pipeline_cache, &size, nullptr);
		if (sized == vk::Result::eSuccess && size > 0) {
			std::vector<uint8_t> blob(size);
			if (device.getPipelineCacheData(pipeline_cache, &size, blob.data()) ==
			    vk::Result::eSuccess) {
				// temp + rename: an interrupted shutdown must not leave a torn
				// file for the next run to detect and discard.
				std::error_code ec;
				auto            tmp = path;
				tmp += ".tmp";
				{
					std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
					if (out) {
						out.write(reinterpret_cast<const char*>(blob.data()),
						          static_cast<std::streamsize>(blob.size()));
					}
					if (!out) {
						std::filesystem::remove(tmp, ec);
						tmp.clear();
					}
				}
				if (!tmp.empty()) {
					std::filesystem::rename(tmp, path, ec);
					if (ec) {
						std::filesystem::remove(tmp, ec);
					} else {
						LOGF("Pipeline cache: %zu bytes written to %s\n", blob.size(),
						     path.string().c_str());
					}
				}
			}
		}
	}

}

void GraphicContext::DestroyPipelineCache() {
	if (pipeline_cache == nullptr) {
		return;
	}

	SavePipelineCache();
	device.destroyPipelineCache(pipeline_cache, nullptr);
	pipeline_cache = nullptr;
}

void GraphicContext::DestroyAllocator() {
	if (allocator == nullptr) {
		return;
	}
	vmaDestroyAllocator(allocator);
	allocator = nullptr;
}

uint64_t VulkanNextMemoryUniqueId() {
	static std::atomic_uint64_t sequence = 0;
	return ++sequence;
}

void GraphicContext::LogMemoryBudget() const {
	if (allocator == nullptr || physical_device == nullptr) {
		return;
	}

	const auto& properties = GetPhysicalDeviceMemoryProperties();
	VmaBudget   budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	for (uint32_t i = 0; i < properties.memoryHeapCount; i++) {
		LOGF("VMA heap %u: usage=%" PRIu64 ", budget=%" PRIu64 ", allocation=%" PRIu64
		     ", blocks=%" PRIu64 "\n",
		     i, static_cast<uint64_t>(budgets[i].usage), static_cast<uint64_t>(budgets[i].budget),
		     static_cast<uint64_t>(budgets[i].statistics.allocationBytes),
		     static_cast<uint64_t>(budgets[i].statistics.blockBytes));
	}
}

uint64_t GraphicContext::GetDeviceMemoryUsage() const {
	if (!CanReportMemoryUsage() || allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t usage = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const bool device_local =
		    static_cast<bool>(physical_device_memory_properties.memoryHeaps[heap].flags &
		                      vk::MemoryHeapFlagBits::eDeviceLocal);
		if (!discrete || device_local) {
			usage += budgets[heap].usage;
		}
	}
	return usage;
}

uint64_t GraphicContext::GetTotalMemoryBudget() const {
	if (allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t budget = 0;
	uint64_t local  = 0;
	uint64_t usage  = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const auto& properties = physical_device_memory_properties.memoryHeaps[heap];
		const bool  device_local =
		    static_cast<bool>(properties.flags & vk::MemoryHeapFlagBits::eDeviceLocal);
		if (device_local) {
			local += properties.size;
		}
		if (!discrete || device_local) {
			budget += CanReportMemoryUsage() ? budgets[heap].budget : properties.size;
			usage += CanReportMemoryUsage() ? budgets[heap].usage : 0;
		}
	}
	if (discrete) {
		return budget - std::min<uint64_t>(budget / 8, 1024ull * 1024 * 1024);
	}
	constexpr uint64_t system_reserve = 8ull * 1024 * 1024 * 1024;
	const auto         available      = budget > usage ? budget - usage : uint64_t {0};
	return std::max(local, available > system_reserve ? available - system_reserve : uint64_t {0});
}

void GraphicContext::CreateBuffer(uint64_t size, VulkanBuffer& buffer) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || buffer.buffer != nullptr ||
	        buffer.memory.allocation != nullptr || size == 0);

	vk::BufferCreateInfo buffer_info {};
	buffer_info.sType       = vk::StructureType::eBufferCreateInfo;
	buffer_info.size        = size;
	buffer_info.usage       = buffer.usage;
	buffer_info.sharingMode = vk::SharingMode::eExclusive;

	VmaAllocationCreateInfo alloc_info {};
	alloc_info.requiredFlags =
	    static_cast<vk::MemoryPropertyFlags::MaskType>(buffer.memory.property);
	alloc_info.preferredFlags =
	    static_cast<vk::MemoryPropertyFlags::MaskType>(buffer.memory.preferred_property);

	vk::Buffer::CType native_buffer = VK_NULL_HANDLE;
	const auto        result        = static_cast<vk::Result>(vmaCreateBuffer(
	    allocator, static_cast<const vk::BufferCreateInfo::NativeType*>(buffer_info), &alloc_info,
	    &native_buffer, &buffer.memory.allocation, &buffer.memory.allocation_info));
	buffer.buffer                   = native_buffer;
	if (result != vk::Result::eSuccess) {
		LogMemoryBudget();
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	device.getBufferMemoryRequirements(buffer.buffer, &buffer.memory.requirements);
	buffer.memory.type      = buffer.memory.allocation_info.memoryType;
	buffer.memory.memory    = buffer.memory.allocation_info.deviceMemory;
	buffer.memory.offset    = buffer.memory.allocation_info.offset;
	buffer.memory.unique_id = VulkanNextMemoryUniqueId();
	buffer.buffer_size      = size;
	VulkanTrackAllocation(buffer.memory);
}

void GraphicContext::DeleteBuffer(VulkanBuffer& buffer) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || buffer.buffer == nullptr ||
	        buffer.memory.allocation == nullptr);

	VulkanUntrackAllocation(buffer.memory);
	vmaDestroyBuffer(allocator, buffer.buffer, buffer.memory.allocation);
	buffer.buffer                 = nullptr;
	buffer.memory.memory          = nullptr;
	buffer.memory.allocation      = nullptr;
	buffer.memory.allocation_info = {};
	buffer.memory.offset          = 0;
}

bool GraphicContext::CreateImage(const vk::ImageCreateInfo& image_info, VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image != nullptr || image.memory.allocation != nullptr);

	auto&                   memory = image.memory;
	VmaAllocationCreateInfo alloc_info {};
	alloc_info.requiredFlags = static_cast<vk::MemoryPropertyFlags::MaskType>(memory.property);
	alloc_info.preferredFlags =
	    static_cast<vk::MemoryPropertyFlags::MaskType>(memory.preferred_property);
	alloc_info.priority = MEMORY_PRIORITY_IMAGE;

	vk::Image::CType native_image = VK_NULL_HANDLE;
	const auto       result       = static_cast<vk::Result>(
	    vmaCreateImage(allocator, static_cast<const vk::ImageCreateInfo::NativeType*>(image_info),
	                   &alloc_info, &native_image, &memory.allocation, &memory.allocation_info));
	image.image = native_image;
	if (result != vk::Result::eSuccess) {
		LogMemoryBudget();
		return false;
	}

	device.getImageMemoryRequirements(image.image, &memory.requirements);
	memory.type      = memory.allocation_info.memoryType;
	memory.memory    = memory.allocation_info.deviceMemory;
	memory.offset    = memory.allocation_info.offset;
	memory.unique_id = VulkanNextMemoryUniqueId();
	VulkanTrackAllocation(memory);
	return true;
}

void GraphicContext::DeleteImage(VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image == nullptr || image.memory.allocation == nullptr);

	auto& memory = image.memory;
	VulkanUntrackAllocation(memory);
	vmaDestroyImage(allocator, image.image, memory.allocation);
	image.image            = nullptr;
	memory.memory          = nullptr;
	memory.allocation      = nullptr;
	memory.allocation_info = {};
	memory.offset          = 0;
}

void GraphicContext::MapMemory(VulkanMemory& memory, void*& data) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || memory.allocation == nullptr);
	EXIT_NOT_IMPLEMENTED(static_cast<vk::Result>(vmaMapMemory(allocator, memory.allocation,
	                                                          &data)) != vk::Result::eSuccess);
}

void GraphicContext::UnmapMemory(VulkanMemory& memory) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || memory.allocation == nullptr);
	vmaUnmapMemory(allocator, memory.allocation);
}

} // namespace Libs::Graphics
