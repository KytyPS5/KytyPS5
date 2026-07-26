#include "graphics/host_gpu/guestMemoryWindow.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <cstring>

namespace Libs::Graphics {

GuestMemoryWindow::~GuestMemoryWindow() {
	if (m_ctx == nullptr) {
		return;
	}
	for (auto& buffer: m_chunks) {
		if (buffer.buffer != nullptr) {
			m_ctx->device.destroyBuffer(buffer.buffer, nullptr);
		}
	}
	for (auto memory: m_memory) {
		if (memory != nullptr) {
			m_ctx->device.freeMemory(memory, nullptr);
		}
	}
	m_chunks.clear();
	m_memory.clear();
}

bool GuestMemoryWindow::ImportChunk(GraphicContext& ctx, uint64_t offset, uint64_t size,
                                    uint32_t memory_type) {
	vk::ImportMemoryHostPointerInfoEXT import {};
	import.sType        = vk::StructureType::eImportMemoryHostPointerInfoEXT;
	import.handleType   = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT;
	import.pHostPointer = m_backing_base + offset;

	vk::MemoryAllocateInfo allocate {};
	allocate.sType           = vk::StructureType::eMemoryAllocateInfo;
	allocate.pNext           = &import;
	allocate.allocationSize  = size;
	allocate.memoryTypeIndex = memory_type;

	vk::DeviceMemory memory;
	if (ctx.device.allocateMemory(&allocate, nullptr, &memory) != vk::Result::eSuccess) {
		LOGF_COLOR(Log::Color::Red,
		           "\t guest-memory window: import failed at offset 0x%016" PRIx64 "\n", offset);
		return false;
	}

	vk::ExternalMemoryBufferCreateInfo external {};
	external.sType       = vk::StructureType::eExternalMemoryBufferCreateInfo;
	external.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT;

	vk::BufferCreateInfo create {};
	create.sType       = vk::StructureType::eBufferCreateInfo;
	create.pNext       = &external;
	create.size        = size;
	create.usage       = vk::BufferUsageFlagBits::eStorageBuffer;
	create.sharingMode = vk::SharingMode::eExclusive;

	VulkanBuffer buffer {};
	buffer.usage       = create.usage;
	buffer.buffer_size = size;
	if (ctx.device.createBuffer(&create, nullptr, &buffer.buffer) != vk::Result::eSuccess) {
		ctx.device.freeMemory(memory, nullptr);
		return false;
	}
	if (ctx.device.bindBufferMemory(buffer.buffer, memory, 0) != vk::Result::eSuccess) {
		ctx.device.destroyBuffer(buffer.buffer, nullptr);
		ctx.device.freeMemory(memory, nullptr);
		return false;
	}

	m_memory.push_back(memory);
	m_chunks.push_back(buffer);
	return true;
}

bool GuestMemoryWindow::EnsureImported(GraphicContext& ctx) {
	if (m_attempted) {
		return m_available;
	}
	m_attempted = true;
	m_ctx       = &ctx;

	void*    base = nullptr;
	uint64_t size = 0;
	if (!LibKernel::Memory::QueryDirectMemoryBacking(&base, &size) || base == nullptr ||
	    size == 0) {
		LOGF_COLOR(Log::Color::Yellow, "\t guest-memory window: backing unavailable\n");
		return false;
	}
	m_backing_base = static_cast<uint8_t*>(base);
	m_backing_size = size;

	vk::PhysicalDeviceExternalMemoryHostPropertiesEXT host_properties {};
	host_properties.sType = vk::StructureType::ePhysicalDeviceExternalMemoryHostPropertiesEXT;
	vk::PhysicalDeviceProperties2 properties2 {};
	properties2.sType = vk::StructureType::ePhysicalDeviceProperties2;
	properties2.pNext = &host_properties;
	ctx.physical_device.getProperties2(&properties2);
	const auto alignment = host_properties.minImportedHostPointerAlignment;
	if (alignment == 0 || (reinterpret_cast<uintptr_t>(m_backing_base) % alignment) != 0) {
		LOGF_COLOR(Log::Color::Yellow,
		           "\t guest-memory window: backing is not import-aligned (%" PRIu64 ")\n",
		           static_cast<uint64_t>(alignment));
		return false;
	}

	// Every chunk imports the same kind of host allocation, so the supported memory-type mask is
	// queried once from the base pointer.
	vk::MemoryHostPointerPropertiesEXT pointer_properties {};
	pointer_properties.sType = vk::StructureType::eMemoryHostPointerPropertiesEXT;
	if (ctx.device.getMemoryHostPointerPropertiesEXT(
	        vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, m_backing_base,
	        &pointer_properties) != vk::Result::eSuccess ||
	    pointer_properties.memoryTypeBits == 0) {
		LOGF_COLOR(Log::Color::Yellow, "\t guest-memory window: no importable memory type\n");
		return false;
	}
	const auto& memory_properties = ctx.GetPhysicalDeviceMemoryProperties();
	uint32_t    memory_type       = UINT32_MAX;
	for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
		if ((pointer_properties.memoryTypeBits & (1u << i)) == 0) {
			continue;
		}
		const auto flags = memory_properties.memoryTypes[i].propertyFlags;
		if ((flags & vk::MemoryPropertyFlagBits::eHostVisible) &&
		    (flags & vk::MemoryPropertyFlagBits::eHostCoherent)) {
			memory_type = i;
			break;
		}
	}
	if (memory_type == UINT32_MAX) {
		LOGF_COLOR(Log::Color::Yellow,
		           "\t guest-memory window: no host-coherent importable memory type\n");
		return false;
	}

	const auto chunk_count = (m_backing_size + ChunkSize - 1u) / ChunkSize;
	if (chunk_count > MaxChunks) {
		LOGF_COLOR(Log::Color::Yellow,
		           "\t guest-memory window: backing needs %" PRIu64 " chunks, limit is %" PRIu32
		           "\n",
		           chunk_count, MaxChunks);
		return false;
	}
	m_chunks.reserve(chunk_count);
	m_memory.reserve(chunk_count);
	for (uint64_t i = 0; i < chunk_count; i++) {
		const auto offset = i * ChunkSize;
		// The import size has to stay a multiple of the import alignment, so the tail chunk is
		// rounded up rather than truncated; the backing mapping covers whole pages regardless.
		auto chunk = std::min(ChunkSize, m_backing_size - offset);
		chunk      = (chunk + alignment - 1u) & ~(static_cast<uint64_t>(alignment) - 1u);
		if (offset + chunk > m_backing_size) {
			chunk = m_backing_size - offset;
		}
		if (!ImportChunk(ctx, offset, chunk, memory_type)) {
			for (auto& buffer: m_chunks) {
				if (buffer.buffer != nullptr) {
					ctx.device.destroyBuffer(buffer.buffer, nullptr);
				}
			}
			for (auto memory: m_memory) {
				ctx.device.freeMemory(memory, nullptr);
			}
			m_chunks.clear();
			m_memory.clear();
			return false;
		}
	}

	m_available = true;
	RefreshRanges();
	LOGF_COLOR(Log::Color::Green,
	           "\t guest-memory window: imported %.2f GiB as %" PRIu64 " chunks, %" PRIu64
	           " ranges\n",
	           static_cast<double>(m_backing_size) / (1024.0 * 1024.0 * 1024.0), chunk_count,
	           static_cast<uint64_t>(m_ranges.size()));
	return true;
}

void GuestMemoryWindow::RefreshRanges() {
	if (!m_available) {
		return;
	}
	std::vector<LibKernel::Memory::GuestMemoryRange> ranges;
	LibKernel::Memory::QueryDirectMemoryRanges(ranges);
	m_ranges.clear();
	m_ranges.reserve(ranges.size());
	for (const auto& range: ranges) {
		RangeEntry entry {};
		entry.vaddr_lo          = static_cast<uint32_t>(range.vaddr);
		entry.vaddr_hi          = static_cast<uint32_t>(range.vaddr >> 32u);
		entry.size_lo           = static_cast<uint32_t>(range.size);
		entry.size_hi           = static_cast<uint32_t>(range.size >> 32u);
		entry.backing_offset_lo = static_cast<uint32_t>(range.backing_offset);
		entry.backing_offset_hi = static_cast<uint32_t>(range.backing_offset >> 32u);
		m_ranges.push_back(entry);
	}
	std::sort(m_ranges.begin(), m_ranges.end(), [](const RangeEntry& a, const RangeEntry& b) {
		return (static_cast<uint64_t>(a.vaddr_hi) << 32u | a.vaddr_lo) <
		       (static_cast<uint64_t>(b.vaddr_hi) << 32u | b.vaddr_lo);
	});
	m_range_generation++;

	// The table is empty until the guest maps something, so the alias check has to wait for the
	// first populated refresh rather than running at device-creation time.
	if (!m_alias_checked && !m_ranges.empty()) {
		m_alias_checked = true;
		const bool ok   = SelfTest();
		LOGF_COLOR(ok ? Log::Color::Green : Log::Color::Red,
		           "\t guest-memory window alias self-test: %s (%" PRIu64 " ranges)\n",
		           ok ? "ok" : "FAILED", static_cast<uint64_t>(m_ranges.size()));
	}
}

bool GuestMemoryWindow::SelfTest() const {
	if (!m_available || m_ranges.empty()) {
		return false;
	}
	// Prove the imported view and the guest view alias the same bytes: write a marker through the
	// guest-facing path, read it back through the backing pointer the GPU was handed.
	for (const auto& entry: m_ranges) {
		const auto entry_size = static_cast<uint64_t>(entry.size_hi) << 32u | entry.size_lo;
		if (entry_size < sizeof(uint64_t)) {
			continue;
		}
		const auto vaddr = static_cast<uint64_t>(entry.vaddr_hi) << 32u | entry.vaddr_lo;
		uint64_t   original = 0;
		if (!LibKernel::Memory::TryReadBacking(vaddr, &original, sizeof(original))) {
			continue;
		}
		const uint64_t marker = 0x4b59545957494e44ull; // "KYTYWIND"
		if (!LibKernel::Memory::TryWriteBacking(vaddr, &marker, sizeof(marker))) {
			continue;
		}
		uint64_t observed = 0;
		const auto entry_offset =
		    static_cast<uint64_t>(entry.backing_offset_hi) << 32u | entry.backing_offset_lo;
		std::memcpy(&observed, m_backing_base + entry_offset, sizeof(observed));
		LibKernel::Memory::TryWriteBacking(vaddr, &original, sizeof(original));
		return observed == marker;
	}
	return false;
}

GuestMemoryWindow& GetGuestMemoryWindow() {
	static GuestMemoryWindow window;
	return window;
}

} // namespace Libs::Graphics
