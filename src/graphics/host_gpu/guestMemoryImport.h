#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_GUESTMEMORYIMPORT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_GUESTMEMORYIMPORT_H_

#include "common/common.h"
#include "graphics/host_gpu/guestMemoryWindows.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;

// Guest direct memory imported into the GPU, so that a buffer over it is the guest's memory
// itself: what the GPU writes is in guest memory when the work completes, and what the CPU
// writes is what later GPU work reads. The PS5 has one memory for both; on a host where the GPU
// reaches system memory (an integrated GPU especially), buffers then need neither uploads nor
// read-backs, nor the write protection that finds what to upload.
//
// Direct memory is one memfd (memoryAddressSpace.inc). On Linux, /dev/udmabuf turns ranges of
// it into dma-bufs, which Vulkan imports (VK_EXT_external_memory_dma_buf). A dma-buf has a size
// limit (udmabuf's size_limit_mb, 64 MiB by default), so the memory is imported as windows of
// that size, each starting half a window after the previous: any range of up to half a window
// lies in one of them. A window is made the first time a buffer needs it, and pins its pages.
//
// Without udmabuf, the extensions or the config switch, Enabled() is false and buffers are
// copies (bufferCache.h).
class GuestMemoryImport final {
public:
	explicit GuestMemoryImport(GraphicContext& graphics);
	~GuestMemoryImport();
	KYTY_CLASS_NO_COPY(GuestMemoryImport);

	[[nodiscard]] bool Enabled() const noexcept { return m_enabled; }

	struct Placement {
		vk::DeviceMemory memory = nullptr;
		uint64_t         offset = 0;
	};
	// Whether [vaddr, vaddr + size) is direct memory mapped in one piece that fits a window.
	[[nodiscard]] bool CanPlace(uint64_t vaddr, uint64_t size) const;
	// The memory that holds [vaddr, vaddr + size) of guest memory, when CanPlace. GPU thread.
	[[nodiscard]] std::optional<Placement> Place(uint64_t vaddr, uint64_t size);


private:
	[[nodiscard]] vk::DeviceMemory Window(uint64_t index);

	GraphicContext&               m_graphics;
	bool                          m_enabled     = false;
	int                           m_udmabuf     = -1;
	int                           m_file        = -1;
	uint64_t                      m_file_size   = 0;
	uint64_t                      m_window      = 0;
	uint32_t                      m_memory_type = UINT32_MAX;
	std::vector<vk::DeviceMemory> m_windows;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_GUESTMEMORYIMPORT_H_ */
