#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_

#include "common/alignment.h"
#include "common/assert.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"

#include <algorithm>
#include <atomic>
#include <compare>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

[[nodiscard]] uint64_t NextImageWriteEpoch() noexcept;

class Buffer;
class CommandScheduler;
class TileManager;
struct ImageTestAccess;

using ImageId = Common::SlotId;

struct CachedImageView {
	ImageViewInfo info;
	vk::ImageView view = nullptr;
};

struct ImageUsage {
	bool storage       = false;
	bool render_target = false;
	bool video_out     = false;
};

struct ImageBinding {
	vk::ImageLayout  attachment_layout = vk::ImageLayout::eUndefined;
	vk::AccessFlags2 attachment_access;
	bool             is_bound      = false;
	bool             is_target     = false;
	bool             needs_rebind  = false;
	bool             force_general = false;
	bool             shader_write  = false;
};

class Image final {
public:
	// With `report`, a failed device allocation sets BackingFailed() instead of exiting.
	Image(GraphicContext& graphics, CommandScheduler& scheduler, const ImageInfo& info,
	      GraphicContext::ImageAllocationReport* report = nullptr);
	~Image();
	KYTY_CLASS_NO_COPY(Image);

	[[nodiscard]] vk::ImageView FindView(const ImageViewInfo& view_info);
	using Barriers = std::vector<vk::ImageMemoryBarrier2>;
	[[nodiscard]] Barriers GetBarriers(vk::ImageLayout                      destination_layout,
	                                   vk::AccessFlags2                     destination_access,
	                                   vk::PipelineStageFlags2              destination_stage,
	                                   std::optional<ImageSubresourceRange> range);
	void Transit(vk::ImageLayout destination_layout, vk::AccessFlags2 destination_access,
	             std::optional<ImageSubresourceRange> range, vk::CommandBuffer command_buffer);
	void MarkShaderWritesFlushed(std::optional<ImageSubresourceRange> range);
	void Upload(std::span<const vk::BufferImageCopy> copies, vk::Buffer buffer, uint64_t offset,
	            uint64_t size);
	void Download(std::span<const vk::BufferImageCopy> copies, vk::Buffer buffer, uint64_t offset,
	              uint64_t size);
	void CopyImage(Image& source);
	void CopyLayers(Image& source, uint32_t base_layer, uint32_t layer_count);
	void Resolve(Image& source, const ImageSubresourceRange& source_range,
	             const ImageSubresourceRange& destination_range);
	void CopyImageWithBuffer(Image& source, Buffer& buffer, TileManager& tiler);
	void CopyMip(Image& source, uint32_t mip, uint32_t layer);

	void MarkCpuDirty() {
		m_cpu_dirty       = true;
		m_maybe_cpu_dirty = false;
		HeapChanged();
	}

	[[nodiscard]] bool IsCpuDirty() const { return m_cpu_dirty || m_maybe_cpu_dirty; }
	[[nodiscard]] bool IsDefinitelyCpuDirty() const { return m_cpu_dirty; }
	[[nodiscard]] bool IsMaybeCpuDirty() const { return m_maybe_cpu_dirty; }
	void               MarkMaybeCpuDirty() {
		if (!IsCpuDirty()) {
			m_maybe_cpu_hash  = HashGuestEdges();
			m_maybe_cpu_dirty = true;
			HeapChanged();
		}
	}
	[[nodiscard]] bool ResolveMaybeCpuHash(uint64_t hash) {
		if (!m_maybe_cpu_dirty || m_cpu_dirty) {
			EXIT("image cannot resolve maybe-dirty hash\n");
		}
		m_maybe_cpu_dirty = false;
		m_cpu_dirty |= hash != m_maybe_cpu_hash;
		return m_cpu_dirty;
	}

	void RefreshComplete() {
		if (!IsCpuDirty()) {
			EXIT("clean image cannot complete a refresh\n");
		}
		m_cpu_dirty       = false;
		m_maybe_cpu_dirty = false;
	}

	[[nodiscard]] bool IsGpuModified() const noexcept { return m_gpu_modified; }
	void               MarkGpuModified() noexcept {
		m_gpu_modified = true;
		++m_gpu_write_serial;
		m_gpu_write_epoch = NextImageWriteEpoch();
		std::fill(layer_content.begin(), layer_content.end(), m_gpu_write_epoch);
	}
	void ClearGpuModified() noexcept { m_gpu_modified = false; }
	// Counts GPU writes, never reset - unlike m_gpu_modified, which only the free path clears.
	[[nodiscard]] uint64_t GpuWriteSerial() const noexcept { return m_gpu_write_serial; }
	[[nodiscard]] uint64_t GpuWriteEpoch() const noexcept { return m_gpu_write_epoch; }
	[[nodiscard]] uint64_t BufferWriteEpoch() const noexcept { return m_buffer_write_epoch; }
	// After a same-layout alias copy, the copied write's epoch until this image is written again.
	[[nodiscard]] uint64_t ContentEpoch() const noexcept {
		return alias_stamp_epoch != 0 && m_gpu_write_epoch == alias_stamp_epoch
		           ? alias_content_epoch
		           : m_gpu_write_epoch;
	}

	[[nodiscard]] bool IsBufferModified() const noexcept { return m_buffer_modified; }
	void               MarkBufferModified() noexcept { MarkBufferModified(NextImageWriteEpoch()); }
	// `epoch` is when the write landed, not when it was noticed.
	void MarkBufferModified(uint64_t epoch) noexcept {
		m_buffer_modified    = true;
		m_buffer_write_epoch = epoch;
		m_superseded.clear();
		HeapChanged();
	}
	// Guest bytes a later GPU buffer write replaced; never written back from this image.
	bool               Supersede(uint64_t address, uint64_t size);
	[[nodiscard]] bool IsSuperseded() const noexcept { return !m_superseded.empty(); }
	[[nodiscard]] std::span<const std::pair<uint64_t, uint64_t>> SupersededRanges() const noexcept {
		return m_superseded;
	}
	[[nodiscard]] bool SupersededAfterLastGpuWrite() const noexcept {
		return IsSuperseded() && m_superseded_epoch > m_gpu_write_epoch;
	}
	void ClearBufferModified() noexcept { m_buffer_modified = false; }

	[[nodiscard]] bool Overlaps(uint64_t address, uint64_t size,
	                            bool pages = false) const noexcept {
		return pages ? ImagePageRangesOverlap(info.data.address, info.data.size, address, size)
		             : ImageRangeOverlaps(info.data.address, info.data.size, address, size);
	}
	[[nodiscard]] bool SafeToDownload() const noexcept {
		return IsGpuModified() && !IsBufferModified() && !IsCpuDirty();
	}
	[[nodiscard]] bool     BackingFailed() const noexcept { return m_backing_failed; }
	[[nodiscard]] bool IsTracked() const noexcept { return track_addr != 0 && track_addr_end != 0; }
	[[nodiscard]] uint64_t AccountedSize() const noexcept {
		return backing.image == nullptr ? 0 : Common::AlignUp(info.data.size, 1024);
	}
	[[nodiscard]] uint64_t HashGuestEdges() const;
	[[nodiscard]] vk::ImageLayout ReadLayout() const noexcept {
		return info.IsDepth() ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
		                      : vk::ImageLayout::eShaderReadOnlyOptimal;
	}
	// Resident image heaps sample an image without rediscovering it until one changes its
	// contents, tracking, identity or layout.
	void HeapChanged() const noexcept {
		if (heap_refs.load(std::memory_order_relaxed) != 0u) {
			heap_epoch.fetch_add(1u, std::memory_order_relaxed);
		}
	}

	static inline std::atomic<uint64_t> heap_epoch {0};

	[[nodiscard]] static bool FormatHasStencil(vk::Format format) noexcept;
	// vkCmdCopyImage needs matching aspect masks, so only identical formats carry stencil.
	[[nodiscard]] static bool CopyCarriesStencil(vk::Format source,
	                                             vk::Format destination) noexcept;
	// Bit-identical under VK_KHR_maintenance8; 24-bit depth keeps the buffered copy's packing.
	[[nodiscard]] static bool DepthColorCopyCompatible(vk::Format source,
	                                                   vk::Format destination) noexcept;

	ImageInfo                    info;
	VulkanImage                  backing;
	std::vector<CachedImageView> views;
	ImageUsage                   usage;
	ImageBinding                 binding;
	bool                         registered = false;
	// Parked: registered and collectable, but no longer an answer to an overlapping lookup.
	bool             dormant        = false;
	mutable uint32_t query_epoch    = 0;
	uint64_t         track_addr     = 0;
	uint64_t         track_addr_end = 0;
	ImageId          depth_id {};
	// Stencil associations that refer to this depth image.
	std::vector<ImageId> stencil_ids;
	// The current stencil plane's mapping into the depth image, also retained by its association.
	ImageSubresourceRange stencil_subresources;
	uint64_t              tick_accessed_last = 0;
	// Counted in presented frames, not queue submissions.
	uint64_t              frame_accessed_last = 0;
	uint64_t              frame_touched_last  = 0;
	size_t                lru_id              = 0;
	uint64_t              alias_stamp_epoch   = 0;
	uint64_t              alias_content_epoch = 0;
	ImageId               depth_twin {};
	std::vector<uint64_t> layer_content;
	// Resident image heap elements that select this image.
	std::atomic<uint32_t> heap_refs {0};

private:
	friend struct ImageTestAccess;

	[[nodiscard]] static vk::ImageAspectFlags FullAspectMask(vk::Format format) noexcept;
	[[nodiscard]] static uint32_t             CopyRows(uint64_t row_size, uint32_t rows,
	                                                   uint64_t capacity) noexcept;
	[[nodiscard]] static std::pair<uint32_t, uint32_t>
	SanitizeCopyLayers(const Image& source, const Image& destination, uint32_t depth);

	GraphicContext&                            m_graphics;
	CommandScheduler&                          m_scheduler;
	uint64_t                                   m_maybe_cpu_hash     = 0;
	bool                                       m_cpu_dirty          = false;
	bool                                       m_maybe_cpu_dirty    = false;
	uint64_t                                   m_gpu_write_serial   = 0;
	uint64_t                                   m_gpu_write_epoch    = 0;
	uint64_t                                   m_buffer_write_epoch = 0;
	std::vector<std::pair<uint64_t, uint64_t>> m_superseded;
	uint64_t                                   m_superseded_epoch = 0;
	bool                                       m_gpu_modified     = false;
	bool                                       m_buffer_modified  = false;
	bool                                       m_backing_failed   = false;
};

namespace ImageOps {

void                                 Validate(const ImageInfo& info);
[[nodiscard]] Prospero::BufferFormat RenderTargetTransferFormat(uint32_t bytes_per_element);

} // namespace ImageOps

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_
