#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_

#include "common/alignment.h"
#include "common/assert.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/eqaaPlan.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"

#include <atomic>
#include <compare>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class Buffer;
class CommandScheduler;
class Image;
class TileManager;
struct ImageTestAccess;

using ImageId = Common::SlotId;

struct CachedImageView {
	ImageViewInfo info;
	vk::ImageView view = nullptr;
};

struct ImageUsage {
	bool render_target = false;
	bool video_out     = false;
};

// A host-only multisampled render proxy of one view of a single-sample image.
struct EqaaShadow {
	std::unique_ptr<Image>            image;
	HostSamplePattern                 pattern;
	const vk::SampleLocationsInfoEXT* locations     = nullptr;
	uint32_t                          mip           = 0;
	uint32_t                          base_layer    = 0;
	uint32_t                          layers        = 1;
	uint16_t                          anchor_select = 0;
	// Aspects whose newest content is only in the shadow's anchor samples.
	vk::ImageAspectFlags dirty;
	// Aspects in which every host sample holds the same value.
	vk::ImageAspectFlags samples_equal;
	// The owner's content stamp at which every clean aspect matched it.
	uint64_t synced    = UINT64_MAX;
	uint64_t last_tick = 0;
	uint64_t bytes     = 0;
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
	Image(GraphicContext& graphics, CommandScheduler& scheduler, const ImageInfo& info,
	      vk::ImageCreateFlags extra_flags = {});
	~Image();
	KYTY_CLASS_NO_COPY(Image);

	// A depth- or stencil-compatible view format selects that plane of a depth image.
	[[nodiscard]] ImageViewInfo NormalizeView(const ImageViewInfo& view_info) const;
	[[nodiscard]] vk::ImageView FindView(const ImageViewInfo& view_info);
	using Barriers = std::vector<vk::ImageMemoryBarrier2>;
	[[nodiscard]] Barriers GetBarriers(vk::ImageLayout                      destination_layout,
	                                   vk::AccessFlags2                     destination_access,
	                                   vk::PipelineStageFlags2              destination_stage,
	                                   std::optional<ImageSubresourceRange> range);
	void Transit(vk::ImageLayout destination_layout, vk::AccessFlags2 destination_access,
	             std::optional<ImageSubresourceRange> range, vk::CommandBuffer command_buffer);
	void Upload(std::span<const vk::BufferImageCopy> copies, vk::Buffer buffer, uint64_t offset,
	            uint64_t size);
	void Download(std::span<const vk::BufferImageCopy> copies, vk::Buffer buffer, uint64_t offset,
	              uint64_t size);
	void CopyImage(Image& source);
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
	void ResolveMaybeCpuHash(uint64_t hash) {
		m_maybe_cpu_dirty = false;
		m_cpu_dirty       = hash != m_maybe_cpu_hash;
	}

	void RefreshComplete() {
		m_cpu_dirty       = false;
		m_maybe_cpu_dirty = false;
	}

	[[nodiscard]] bool IsGpuModified() const noexcept { return m_gpu_modified; }
	void               MarkGpuModified() noexcept { m_gpu_modified = true; }
	void               ClearGpuModified() noexcept { m_gpu_modified = false; }

	[[nodiscard]] bool IsBufferModified() const noexcept { return m_buffer_modified; }
	void               MarkBufferModified() noexcept {
		m_buffer_modified = true;
		HeapChanged();
	}
	void               ClearBufferModified() noexcept { m_buffer_modified = false; }

	[[nodiscard]] bool Overlaps(uint64_t address, uint64_t size) const noexcept {
		return info.data.address < address + size && address < info.data.End();
	}
	[[nodiscard]] bool SafeToDownload() const noexcept {
		return IsGpuModified() && !IsBufferModified() && !IsCpuDirty();
	}
	[[nodiscard]] bool IsTracked() const noexcept { return track_addr != 0 && track_addr_end != 0; }
	[[nodiscard]] uint64_t AccountedSize() const noexcept {
		return (backing.image == nullptr ? 0 : Common::AlignUp(info.data.size, 1024)) +
		       (eqaa != nullptr ? eqaa->bytes : 0);
	}
	[[nodiscard]] uint64_t HashGuestEdges() const;
	[[nodiscard]] vk::ImageLayout ReadLayout() const noexcept {
		return info.IsDepth() ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
		                      : vk::ImageLayout::eShaderReadOnlyOptimal;
	}
	struct HeapChange {
		GuestRange data;
		uint64_t   serial = 0;
	};

	// Resident image heaps sample an image without rediscovering it until it changes its
	// contents, tracking, identity or layout, which queues a heap change.
	void HeapChanged() const {
		if (heap_refs.load(std::memory_order_relaxed) != 0u) {
			std::scoped_lock lock {heap_mutex};
			heap_changes.push_back({info.data, serial});
		}
	}

	static inline std::mutex              heap_mutex; // After TextureCache::m_lock.
	static inline std::vector<HeapChange> heap_changes;
	static inline std::atomic<uint64_t>   serials {0};

	// Whether an image of `info` with these extra flags can be created with `required` usage.
	[[nodiscard]] static bool Creatable(GraphicContext& graphics, const ImageInfo& info,
	                                    vk::ImageCreateFlags extra_flags,
	                                    vk::ImageUsageFlags  required);

	// Unique per image, unlike ImageId: freed slots are reused.
	const uint64_t   serial = serials.fetch_add(1u, std::memory_order_relaxed) + 1u;
	ImageInfo        info;
	VulkanImage      backing;
	std::vector<CachedImageView> views;
	ImageUsage       usage;
	ImageBinding     binding;
	bool             registered     = false;
	uint64_t         track_addr     = 0;
	uint64_t         track_addr_end = 0;
	ImageId          depth_id {};
	// Stencil associations that refer to this depth image.
	std::vector<ImageId> stencil_ids;
	// The current stencil plane's mapping into the depth image, also retained by its association.
	ImageSubresourceRange stencil_subresources;
	uint64_t         tick_accessed_last = 0;
	size_t           lru_id             = 0;
	// Resident image heap elements that select this image.
	std::atomic<uint32_t> heap_refs {0};
	// Advances whenever a barrier hands the backing to a writer.
	uint64_t                    content_stamp = 0;
	std::unique_ptr<EqaaShadow> eqaa;
	// Set while the shadow writes its anchors back; any other transition of a dirty owner exits.
	bool eqaa_settling = false;
	// Custom sample locations that layout transitions of this depth image must preserve.
	const vk::SampleLocationsInfoEXT* barrier_sample_locations = nullptr;

private:
	friend struct ImageTestAccess;

	[[nodiscard]] static vk::ImageAspectFlags FullAspectMask(vk::Format format) noexcept;
	[[nodiscard]] static uint32_t             CopyRows(uint64_t row_size, uint32_t rows,
	                                                   uint64_t capacity) noexcept;
	[[nodiscard]] static std::pair<uint32_t, uint32_t>
	SanitizeCopyLayers(const Image& source, const Image& destination, uint32_t depth);

	GraphicContext&   m_graphics;
	CommandScheduler& m_scheduler;
	uint64_t          m_maybe_cpu_hash  = 0;
	bool              m_cpu_dirty       = false;
	bool              m_maybe_cpu_dirty = false;
	bool              m_gpu_modified    = false;
	bool              m_buffer_modified = false;
};

namespace ImageOps {

void                                 Validate(const ImageInfo& info);
[[nodiscard]] Prospero::BufferFormat RenderTargetTransferFormat(uint32_t bytes_per_element);

} // namespace ImageOps

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_
