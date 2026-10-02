#include "graphics/host_gpu/hostMemoryImport.h"

#include "common/logging/log.h"
#include "common/timer.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <bit>
#include <cinttypes>
#include <cstdlib>

namespace Libs::Graphics {
namespace {

uint32_t QueryImportTypes(const GraphicContext& ctx, vk::ExternalMemoryHandleTypeFlagBits type,
                          const void* ptr) {
	auto [result, props] = ctx.device.getMemoryHostPointerPropertiesEXT(type, ptr);
	if (result != vk::Result::eSuccess) {
		LOGF("ZERO-COPY PROBE: getMemoryHostPointerProperties(%s) failed: %s\n",
		     vk::to_string(type).c_str(), vk::to_string(result).c_str());
		return 0;
	}
	const auto& mem = ctx.GetPhysicalDeviceMemoryProperties();
	LOGF("ZERO-COPY PROBE: %s memoryTypeBits=0x%08" PRIx32 "\n", vk::to_string(type).c_str(),
	     props.memoryTypeBits);
	for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
		if ((props.memoryTypeBits & (1u << i)) != 0) {
			const auto& t = mem.memoryTypes[i];
			LOGF("ZERO-COPY PROBE:   type %u heap %u (%" PRIu64 " MiB) flags=%s\n", i, t.heapIndex,
			     static_cast<uint64_t>(mem.memoryHeaps[t.heapIndex].size >> 20u),
			     vk::to_string(t.propertyFlags).c_str());
		}
	}
	return props.memoryTypeBits;
}

// Imports [ptr, ptr + size) as one allocation, binds a storage buffer with a device address to
// it, then releases both. Logs the timing because the import pins the pages.
bool TryImport(const GraphicContext& ctx, vk::ExternalMemoryHandleTypeFlagBits type, void* ptr,
               uint64_t size, uint32_t type_bits) {
	const auto type_index = static_cast<uint32_t>(std::countr_zero(type_bits));

	Common::Timer timer;
	timer.Start();

	vk::ImportMemoryHostPointerInfoEXT import_info {};
	import_info.handleType   = type;
	import_info.pHostPointer = ptr;

	vk::MemoryAllocateFlagsInfo flags_info {};
	flags_info.flags = vk::MemoryAllocateFlagBits::eDeviceAddress;
	flags_info.pNext = &import_info;

	vk::MemoryAllocateInfo alloc_info {};
	alloc_info.allocationSize  = size;
	alloc_info.memoryTypeIndex = type_index;
	alloc_info.pNext           = &flags_info;

	auto [alloc_result, memory] = ctx.device.allocateMemory(alloc_info);
	if (alloc_result != vk::Result::eSuccess) {
		LOGF("ZERO-COPY PROBE: import %" PRIu64 " MiB as type %u failed: %s\n", size >> 20u,
		     type_index, vk::to_string(alloc_result).c_str());
		return false;
	}
	const double import_ms = timer.GetTimeMs();

	vk::ExternalMemoryBufferCreateInfo external_info {};
	external_info.handleTypes = type;

	vk::BufferCreateInfo buffer_info {};
	buffer_info.size  = size;
	buffer_info.usage =
	    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;
	buffer_info.sharingMode = vk::SharingMode::eExclusive;
	buffer_info.pNext       = &external_info;

	bool ok                       = false;
	auto [buffer_result, buffer] = ctx.device.createBuffer(buffer_info);
	if (buffer_result != vk::Result::eSuccess) {
		LOGF("ZERO-COPY PROBE: createBuffer %" PRIu64 " MiB failed: %s\n", size >> 20u,
		     vk::to_string(buffer_result).c_str());
	} else {
		const auto bind_result = ctx.device.bindBufferMemory(buffer, memory, 0);
		if (bind_result != vk::Result::eSuccess) {
			LOGF("ZERO-COPY PROBE: bindBufferMemory %" PRIu64 " MiB failed: %s\n", size >> 20u,
			     vk::to_string(bind_result).c_str());
		} else {
			vk::BufferDeviceAddressInfo address_info {};
			address_info.buffer = buffer;
			const auto address  = ctx.device.getBufferAddress(address_info);
			LOGF("ZERO-COPY PROBE: imported %" PRIu64 " MiB as type %u in %.2f ms, "
			     "bda=0x%016" PRIx64 "\n",
			     size >> 20u, type_index, import_ms, static_cast<uint64_t>(address));
			ok = true;
		}
		ctx.device.destroyBuffer(buffer);
	}
	ctx.device.freeMemory(memory);
	return ok;
}

} // namespace

void ProbeHostMemoryImport(GraphicContext& ctx) {
	if (const char* value = std::getenv("KYTY_ZERO_COPY_PROBE"); value == nullptr || value[0] == '0') {
		return;
	}
	if (!ctx.external_memory_host_enabled) {
		LOGF("ZERO-COPY PROBE: VK_EXT_external_memory_host unavailable\n");
		return;
	}

	uint64_t       size = 0;
	const uint64_t base = LibKernel::Memory::GetGuestBackingView(&size);
	if (base == 0) {
		LOGF("ZERO-COPY PROBE: guest backing view unavailable\n");
		return;
	}
	const uint64_t alignment = ctx.min_imported_host_pointer_alignment;
	LOGF("ZERO-COPY PROBE: backing base=0x%016" PRIx64 " size=%" PRIu64
	     " MiB alignment=0x%" PRIx64 " aligned=%d maxMemoryAllocationCount=%u\n",
	     base, size >> 20u, alignment, alignment != 0 && (base % alignment) == 0 ? 1 : 0,
	     ctx.GetPhysicalDeviceProperties().limits.maxMemoryAllocationCount);

	auto* ptr = reinterpret_cast<void*>(base);
	for (const auto type: {vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
	                       vk::ExternalMemoryHandleTypeFlagBits::eHostMappedForeignMemoryEXT}) {
		const uint32_t type_bits = QueryImportTypes(ctx, type, ptr);
		if (type_bits == 0) {
			continue;
		}
		// Grow until the driver refuses: the largest single import decides the chunk size.
		for (const uint64_t chunk: {64ull << 20u, 256ull << 20u, 1ull << 30u, 4ull << 30u}) {
			if (chunk > size || !TryImport(ctx, type, ptr, chunk, type_bits)) {
				break;
			}
		}
	}
}

} // namespace Libs::Graphics
