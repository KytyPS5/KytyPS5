#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_GUESTMEMORYWINDOW_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_GUESTMEMORYWINDOW_H_

#include "common/common.h"
#include "graphics/host_gpu/graphicContext.h"

#include <cstdint>
#include <vector>

namespace Libs::Graphics {

// Gives shaders a way to read guest memory at addresses the host cannot predict -- a descriptor
// whose contents are themselves fetched from a run-time pointer, an unbased FLAT access, or a
// waterfall loop's bindless index.
//
// Guest direct memory is one contiguous host mapping of a shared object that every guest virtual
// view aliases, so VK_EXT_external_memory_host can hand those exact pages to the GPU with no copy
// and no synchronisation: a guest write through any alias is immediately visible here. That
// contiguous backing exceeds maxStorageBufferRange, so it is exposed as an array of equally sized
// chunks and the shader picks one by dividing.
//
// Guest virtual addresses are *not* contiguous within the backing, so translation goes through a
// range table the shader searches: guest vaddr -> backing offset -> (chunk, offset in chunk).
class GuestMemoryWindow {
public:
	// Must divide evenly into the address maths the emitter generates and stay under
	// maxStorageBufferRange.
	static constexpr uint64_t ChunkSize = 1ull << 30u; // 1 GiB
	static constexpr uint32_t MaxChunks = 32;

	// One searchable entry, mirrored by the shader-side struct. Sizes and backing offsets span the
	// whole multi-gigabyte backing, so both need 64 bits; the shader reads them as dword pairs.
	struct RangeEntry {
		uint32_t vaddr_lo          = 0;
		uint32_t vaddr_hi          = 0;
		uint32_t size_lo           = 0;
		uint32_t size_hi           = 0;
		uint32_t backing_offset_lo = 0;
		uint32_t backing_offset_hi = 0;
		uint32_t reserved0         = 0;
		uint32_t reserved1         = 0;
	};

	GuestMemoryWindow() = default;
	~GuestMemoryWindow();
	KYTY_CLASS_NO_COPY(GuestMemoryWindow);

	// Imports the backing. Safe to call repeatedly; only the first call does work. Returns false
	// when the platform or driver cannot import guest pages, in which case callers must keep using
	// the copy-based paths.
	bool EnsureImported(GraphicContext& ctx);

	[[nodiscard]] bool IsAvailable() const { return m_available; }
	[[nodiscard]] uint32_t ChunkCount() const { return static_cast<uint32_t>(m_chunks.size()); }
	[[nodiscard]] const std::vector<VulkanBuffer>& Chunks() const { return m_chunks; }

	// Refreshes the guest range table from the kernel. Cheap when nothing changed.
	void RefreshRanges();
	[[nodiscard]] const std::vector<RangeEntry>& Ranges() const { return m_ranges; }

	// Reads back through the imported mapping. Used by the self-test to prove the GPU and the guest
	// really are looking at the same bytes.
	bool SelfTest() const;

private:
	bool ImportChunk(GraphicContext& ctx, uint64_t offset, uint64_t size, uint32_t memory_type);

	bool                      m_available    = false;
	bool                      m_attempted    = false;
	uint8_t*                  m_backing_base = nullptr;
	uint64_t                  m_backing_size = 0;
	GraphicContext*           m_ctx          = nullptr;
	std::vector<VulkanBuffer> m_chunks;
	std::vector<vk::DeviceMemory> m_memory;
	std::vector<RangeEntry>       m_ranges;
	uint64_t                      m_range_generation = 0;
	bool                          m_alias_checked    = false;
};

GuestMemoryWindow& GetGuestMemoryWindow();

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_GUESTMEMORYWINDOW_H_ */
