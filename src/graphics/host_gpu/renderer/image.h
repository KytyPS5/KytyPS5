#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_

#include "common/assert.h"
#include "graphics/host_gpu/renderer/imageInfo.h"

#include <cstddef>
#include <span>

namespace Libs::Graphics {

struct Image final: ImageInfo {
	Image& operator=(const ImageInfo& value) {
		if (m_cpu_dirty) {
			EXIT("dirty sampled image cannot be reassigned\n");
		}
		static_cast<ImageInfo&>(*this) = value;
		return *this;
	}

	void InvalidateCpuWrite(uint64_t vaddr, uint64_t size) {
		if (ImagePageRangesOverlap(address, this->size, vaddr, size)) {
			m_cpu_dirty = true;
		}
	}

	[[nodiscard]] bool IsCpuDirty() const { return m_cpu_dirty; }

	void RefreshComplete() {
		if (!m_cpu_dirty) {
			EXIT("clean sampled image cannot complete a refresh\n");
		}
		m_cpu_dirty = false;
	}

private:
	bool m_cpu_dirty = false;
};

struct ImageRetirementRange {
	uint64_t address = 0;
	uint64_t size    = 0;
	bool     retire  = false;
};

struct ImageRetirementConflict {
	size_t retired  = SIZE_MAX;
	size_t retained = SIZE_MAX;

	[[nodiscard]] bool Exists() const { return retired != SIZE_MAX; }
};

[[nodiscard]] inline ImageRetirementConflict
FindImageRetirementConflict(std::span<const ImageRetirementRange> ranges) {
	for (size_t retired = 0; retired < ranges.size(); retired++) {
		if (!ranges[retired].retire) {
			continue;
		}
		for (size_t retained = 0; retained < ranges.size(); retained++) {
			if (ranges[retained].retire) {
				continue;
			}
			if (ImagePageRangesOverlap(ranges[retired].address, ranges[retired].size,
			                           ranges[retained].address, ranges[retained].size)) {
				return {retired, retained};
			}
		}
	}
	return {};
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_
