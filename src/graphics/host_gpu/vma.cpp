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
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <fmt/format.h>
#include <mutex>
#include <string>
#include <vector>

namespace Libs::Graphics {

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
	info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	if (memory_budget_ext_enabled) {
		info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
	}

	const auto result = static_cast<vk::Result>(vmaCreateAllocator(&info, &allocator));
	if (result != vk::Result::eSuccess) {
		LOGF("vmaCreateAllocator failed: %s\n", vk::to_string(result).c_str());
		return false;
	}
	return true;
}

namespace {

// Checkpoint markers index this ring; a marker is the record's sequence number plus one.
struct CheckpointRecord {
	GpuCheckpoint kind        = GpuCheckpoint::Dispatch;
	uint64_t      shader_hash = 0;
	uint64_t      args_addr   = 0;
};
constexpr uint32_t                            CheckpointCount = 1u << 16u;
constexpr uint32_t                            ArgsStride      = 16;
std::array<CheckpointRecord, CheckpointCount> g_checkpoints;
std::atomic<uint64_t>                         g_next_checkpoint {0};
// Indirect arguments as the GPU read them, copied just before each indirect command.
std::once_flag  g_args_once;
VkBuffer        g_args_buffer     = VK_NULL_HANDLE;
VmaAllocation   g_args_allocation = nullptr;
const uint32_t* g_args_mapped     = nullptr;

void ReportArgs(uint64_t marker, const CheckpointRecord& record) {
	if (record.args_addr == 0 || g_args_mapped == nullptr) {
		return;
	}
	const auto* args = g_args_mapped + ((marker - 1u) % CheckpointCount) * (ArgsStride / 4u);
	std::printf("    args at 0x%016" PRIx64 ": %u %u %u\n", record.args_addr, args[0], args[1],
	            args[2]);
}

void ReportCheckpoints(vk::Queue queue) {
	const auto get_data = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetQueueCheckpointDataNV;
	if (get_data == nullptr || queue == nullptr) {
		return;
	}
	uint32_t count = 0;
	get_data(queue, &count, nullptr);
	std::vector<VkCheckpointDataNV> data(count);
	for (auto& entry: data) {
		entry.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
	}
	get_data(queue, &count, data.data());
	const auto newest = g_next_checkpoint.load(std::memory_order_acquire);
	for (uint32_t i = 0; i < count; ++i) {
		const auto marker = reinterpret_cast<uintptr_t>(data[i].pCheckpointMarker);
		const auto stage  = vk::to_string(static_cast<vk::PipelineStageFlagBits>(data[i].stage));
		if (marker == 0 || marker > newest || newest - marker >= CheckpointCount) {
			std::printf("  checkpoint %s: marker %zu (no record)\n", stage.c_str(),
			            static_cast<size_t>(marker));
			continue;
		}
		const auto& record = g_checkpoints[(marker - 1u) % CheckpointCount];
		const char* kind   = record.kind == GpuCheckpoint::Draw       ? "draw"
		                     : record.kind == GpuCheckpoint::Dispatch ? "dispatch"
		                                                              : "indirect dispatch";
		std::printf("  checkpoint %s: #%zu %s shader 0x%016" PRIx64 " (%zu commands before the "
		            "newest)\n",
		            stage.c_str(), static_cast<size_t>(marker), kind, record.shader_hash,
		            static_cast<size_t>(newest - marker));
		ReportArgs(marker, record);
	}
	// Earlier indirect commands show what their arguments normally look like.
	uint32_t shown = 0;
	for (uint64_t marker = newest; marker > 0 && newest - marker < CheckpointCount && shown < 8;
	     --marker) {
		const auto& record = g_checkpoints[(marker - 1u) % CheckpointCount];
		if (record.args_addr != 0) {
			std::printf("  recent indirect #%zu shader 0x%016" PRIx64 "\n",
			            static_cast<size_t>(marker), record.shader_hash);
			ReportArgs(marker, record);
			shown++;
		}
	}
	std::fflush(stdout);
}

void CreateArgsBuffer(VmaAllocator allocator) {
	VkBufferCreateInfo buffer_info {};
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size  = uint64_t {CheckpointCount} * ArgsStride;
	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VmaAllocationCreateInfo alloc_info {};
	alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
	alloc_info.flags =
	    VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo info {};
	if (vmaCreateBuffer(allocator, &buffer_info, &alloc_info, &g_args_buffer, &g_args_allocation,
	                    &info) == VK_SUCCESS) {
		g_args_mapped = static_cast<const uint32_t*>(info.pMappedData);
	}
}

} // namespace

void GraphicContext::SetCheckpoint(vk::CommandBuffer command, GpuCheckpoint kind,
                                   uint64_t shader_hash, vk::Buffer args,
                                   vk::DeviceSize args_offset, uint64_t args_addr) const {
	if (!checkpoints_enabled) {
		return;
	}
	const auto sequence = g_next_checkpoint.fetch_add(1, std::memory_order_acq_rel) + 1u;
	g_checkpoints[(sequence - 1u) % CheckpointCount] = {kind, shader_hash,
	                                                    args ? args_addr : uint64_t {0}};
	if (args) {
		std::call_once(g_args_once, CreateArgsBuffer, allocator);
	}
	if (args && g_args_buffer != VK_NULL_HANDLE) {
		vk::MemoryBarrier barrier {};
		barrier.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
		barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
		command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                        vk::PipelineStageFlagBits::eTransfer, {}, 1, &barrier, 0, nullptr,
		                        0, nullptr);
		const vk::BufferCopy copy {args_offset, ((sequence - 1u) % CheckpointCount) * ArgsStride,
		                           3 * sizeof(uint32_t)};
		command.copyBuffer(args, vk::Buffer {g_args_buffer}, 1, &copy);
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdSetCheckpointNV(
	    command, reinterpret_cast<const void*>(static_cast<uintptr_t>(sequence)));
}

// A lost device says little by itself; VK_EXT_device_fault reports the addresses the GPU faulted
// on, which identify the buffer or image it was accessing.
void GraphicContext::ReportDeviceFault() const {
	if (checkpoints_enabled) {
		ReportCheckpoints(queue);
	}
	const auto get_fault_info = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT;
	if (!device_fault_enabled || get_fault_info == nullptr || device == nullptr) {
		return;
	}
	VkDeviceFaultCountsEXT counts {};
	counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
	if (get_fault_info(device, &counts, nullptr) != VK_SUCCESS) {
		return;
	}
	std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
	std::vector<VkDeviceFaultVendorInfoEXT>  vendors(counts.vendorInfoCount);
	counts.vendorBinarySize = 0;
	VkDeviceFaultInfoEXT info {};
	info.sType         = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
	info.pAddressInfos = addresses.data();
	info.pVendorInfos  = vendors.data();
	const auto result  = get_fault_info(device, &counts, &info);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
		return;
	}
	const auto report = [](const std::string& line) {
		LOGF("%s", line.c_str());
		std::printf("%s", line.c_str());
	};
	report(fmt::format("Device fault: {}\n", info.description));
	for (uint32_t i = 0; i < counts.addressInfoCount; ++i) {
		const auto& address = addresses[i];
		report(fmt::format(
		    "  address 0x{:016x} precision 0x{:x}: {}\n", address.reportedAddress,
		    address.addressPrecision,
		    vk::to_string(static_cast<vk::DeviceFaultAddressTypeEXT>(address.addressType))));
	}
	for (uint32_t i = 0; i < counts.vendorInfoCount; ++i) {
		const auto& vendor = vendors[i];
		report(fmt::format("  vendor: {} code=0x{:x} data=0x{:x}\n", vendor.description,
		                   vendor.vendorFaultCode, vendor.vendorFaultData));
	}
	std::fflush(stdout);
}

void GraphicContext::DestroyAllocator() {
	if (allocator == nullptr) {
		return;
	}
	if (g_args_buffer != VK_NULL_HANDLE) {
		vmaDestroyBuffer(allocator, g_args_buffer, g_args_allocation);
		g_args_buffer = VK_NULL_HANDLE;
		g_args_mapped = nullptr;
	}
	vmaDestroyAllocator(allocator);
	allocator = nullptr;
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

bool GraphicContext::CreateImage(const vk::ImageCreateInfo& image_info, VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image != nullptr || image.allocation != nullptr);

	VmaAllocationCreateInfo alloc_info {};
	alloc_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	vk::Image::CType native_image = VK_NULL_HANDLE;
	const auto        result       = static_cast<vk::Result>(
	    vmaCreateImage(allocator, static_cast<const vk::ImageCreateInfo::NativeType*>(image_info),
	                   &alloc_info, &native_image, &image.allocation, nullptr));
	image.image = native_image;
	if (result != vk::Result::eSuccess) {
		LogMemoryBudget();
		return false;
	}

	image.format     = image_info.format;
	image.image_type = image_info.imageType;
	image.extent     = image_info.extent;
	image.layers     = image_info.arrayLayers;
	image.mip_levels = image_info.mipLevels;
	image.samples    = static_cast<uint32_t>(image_info.samples);
	image.usage      = image_info.usage;
	image.flags      = image_info.flags;
	image.state      = {.layout = image_info.initialLayout};
	image.subresource_states.clear();

	return true;
}

void GraphicContext::DeleteImage(VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image == nullptr || image.allocation == nullptr);

	vmaDestroyImage(allocator, image.image, image.allocation);
	image.image      = nullptr;
	image.allocation = nullptr;
}

} // namespace Libs::Graphics
