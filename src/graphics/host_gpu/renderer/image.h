#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_

#include "common/assert.h"
#include "graphics/host_gpu/renderer/imageInfo.h"

#include <algorithm>
#include <cstddef>
#include <span>

namespace Libs::Graphics {

struct DepthStencilVulkanImage;
struct GpuTextureVulkanImage;
struct GraphicContext;
struct RenderTextureVulkanImage;
struct VideoOutVulkanImage;
struct VulkanImage;

struct Image final: ImageInfo {
	Image& operator=(const ImageInfo& value) {
		if (IsCpuDirty()) {
			EXIT("dirty sampled image cannot be reassigned\n");
		}
		static_cast<ImageInfo&>(*this) = value;
		m_track_begin                  = address;
		m_track_end                    = address + size;
		m_maybe_cpu_hash_valid         = false;
		return *this;
	}

	void InvalidateCpuWrite(uint64_t vaddr, uint64_t size) {
		if (ImageRangeOverlaps(address, this->size, vaddr, size)) {
			m_cpu_dirty            = true;
			m_maybe_cpu_dirty      = false;
			m_maybe_cpu_hash_valid = false;
			m_track_begin          = m_track_end;
		} else if (ImagePageRangesOverlap(address, this->size, vaddr, size)) {
			constexpr uint64_t page_mask = 4096 - 1;
			if (vaddr + size <= address) {
				const auto next_page = (address + page_mask) & ~page_mask;
				m_track_begin        = std::min(m_track_end, std::max(m_track_begin, next_page));
			} else if (vaddr >= address + this->size) {
				const auto page = (address + this->size) & ~page_mask;
				m_track_end     = std::max(m_track_begin, std::min(m_track_end, page));
			}
			m_maybe_cpu_dirty = m_track_begin == m_track_end;
		}
	}

	[[nodiscard]] bool IsCpuDirty() const { return m_cpu_dirty || m_maybe_cpu_dirty; }
	[[nodiscard]] bool IsDefinitelyCpuDirty() const { return m_cpu_dirty; }
	[[nodiscard]] bool IsMaybeCpuDirty() const { return m_maybe_cpu_dirty; }
	[[nodiscard]] bool NeedsMaybeCpuHash() const {
		return m_maybe_cpu_dirty && !m_maybe_cpu_hash_valid;
	}
	[[nodiscard]] bool IsCpuTrackingComplete() const {
		return m_track_begin == address && m_track_end == address + size;
	}
	void SetMaybeCpuHash(uint64_t hash) {
		if (!NeedsMaybeCpuHash()) {
			EXIT("sampled image cannot initialize maybe-dirty hash\n");
		}
		m_maybe_cpu_hash       = hash;
		m_maybe_cpu_hash_valid = true;
	}
	[[nodiscard]] bool ResolveMaybeCpuHash(uint64_t hash) {
		if (!m_maybe_cpu_dirty || !m_maybe_cpu_hash_valid || m_cpu_dirty) {
			EXIT("sampled image cannot resolve maybe-dirty hash\n");
		}
		m_maybe_cpu_dirty      = false;
		m_maybe_cpu_hash_valid = false;
		m_cpu_dirty            = hash != m_maybe_cpu_hash;
		if (!m_cpu_dirty) {
			m_track_begin = address;
			m_track_end   = address + size;
		}
		return m_cpu_dirty;
	}

	void RefreshComplete() {
		if (!IsCpuDirty()) {
			EXIT("clean sampled image cannot complete a refresh\n");
		}
		m_cpu_dirty            = false;
		m_maybe_cpu_dirty      = false;
		m_maybe_cpu_hash_valid = false;
		m_track_begin          = address;
		m_track_end            = address + size;
	}

private:
	bool     m_cpu_dirty            = false;
	bool     m_maybe_cpu_dirty      = false;
	bool     m_maybe_cpu_hash_valid = false;
	uint64_t m_track_begin          = 0;
	uint64_t m_track_end            = 0;
	uint64_t m_maybe_cpu_hash       = 0;
};

namespace ImageOps {

[[nodiscard]] GpuTextureVulkanImage* CreateTexture(GraphicContext* ctx, const ImageInfo& info,
                                                   bool storage, vk::ComponentMapping* components);
void CreateTextureViews(GraphicContext* ctx, GpuTextureVulkanImage* image, const ImageInfo& info,
                        bool storage, vk::ComponentMapping components);

[[nodiscard]] RenderTextureVulkanImage* CreateRenderTarget(GraphicContext*         ctx,
                                                           const RenderTargetInfo& info);
[[nodiscard]] uint32_t                  RenderTargetTransferFormat(uint32_t bytes_per_element);
void UploadRenderTargetLayers(GraphicContext* ctx, RenderTextureVulkanImage* image,
                              const RenderTargetInfo& info, uint32_t base_layer,
                              uint32_t layer_count, bool refresh);
void UploadRenderTarget(GraphicContext* ctx, RenderTextureVulkanImage* image,
                        const RenderTargetInfo& info, bool refresh);

[[nodiscard]] DepthStencilVulkanImage* CreateDepthTarget(GraphicContext*        ctx,
                                                         const DepthTargetInfo& info);

void                               ValidateVideoOut(GraphicContext* ctx, const VideoOutInfo& info);
[[nodiscard]] VideoOutVulkanImage* CreateVideoOut(GraphicContext* ctx, const VideoOutInfo& info);
void UploadVideoOut(GraphicContext* ctx, VideoOutVulkanImage* image, const VideoOutInfo& info,
                    bool refresh);

[[nodiscard]] GpuTextureVulkanImage* CreateDummyTexture(GraphicContext* ctx, bool uint_format,
                                                        bool image_3d, bool storage);

void Destroy(GraphicContext* ctx, VulkanImage* image);

} // namespace ImageOps

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
			if (ImageRangeOverlaps(ranges[retired].address, ranges[retired].size,
			                       ranges[retained].address, ranges[retained].size)) {
				return {retired, retained};
			}
		}
	}
	return {};
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_
