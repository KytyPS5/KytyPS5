#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_

#include "common/alignment.h"
#include "common/assert.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"

#include <atomic>
#include <compare>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

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
	Image(GraphicContext& graphics, CommandScheduler& scheduler, const ImageInfo& info);
	~Image();
	KYTY_CLASS_NO_COPY(Image);

	[[nodiscard]] vk::ImageView FindView(const ImageViewInfo& view_info);
	using Barriers = std::vector<vk::ImageMemoryBarrier2>;
	[[nodiscard]] Barriers GetBarriers(vk::ImageLayout                      destination_layout,
	                                   vk::AccessFlags2                     destination_access,
	                                   vk::PipelineStageFlags2              destination_stage,
	                                   std::optional<ImageSubresourceRange> range);
	void Transit(vk::ImageLayout destination_layout, vk::AccessFlags2 destination_access,
	             std::optional<ImageSubresourceRange> range, CommandRecorder command_buffer);
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
		StateChanged();
	}

	void RefreshComplete() {
		m_cpu_dirty       = false;
		m_maybe_cpu_dirty = false;
		StateChanged();
	}

	[[nodiscard]] bool IsGpuModified() const noexcept { return m_gpu_modified; }
	void               MarkGpuModified() noexcept {
		if (!m_gpu_modified) {
			m_gpu_modified = true;
			StateChanged();
		}
	}
	void ClearGpuModified() noexcept {
		if (m_gpu_modified) {
			m_gpu_modified = false;
			StateChanged();
		}
	}

	[[nodiscard]] bool IsBufferModified() const noexcept { return m_buffer_modified; }
	void               MarkBufferModified() noexcept {
		m_buffer_modified = true;
		HeapChanged();
	}
	void ClearBufferModified() noexcept {
		if (m_buffer_modified) {
			m_buffer_modified = false;
			StateChanged();
		}
	}

	[[nodiscard]] bool Overlaps(uint64_t address, uint64_t size) const noexcept {
		return info.data.address < address + size && address < info.data.End();
	}
	[[nodiscard]] bool SafeToDownload() const noexcept {
		return IsGpuModified() && !IsBufferModified() && !IsCpuDirty();
	}
	[[nodiscard]] bool IsTracked() const noexcept { return track_addr != 0 && track_addr_end != 0; }
	[[nodiscard]] uint64_t AccountedSize() const noexcept {
		return backing.image == nullptr ? 0 : Common::AlignUp(info.data.size, 1024);
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

	// Advances the image's version (see TextureCache::ImageVersion()): what a binding of one of
	// its views, as it was bound, depends on changed.
	void StateChanged() const noexcept {
		if (version != nullptr) {
			version->fetch_add(1u, std::memory_order_release);
		}
	}
	// Resident image heaps sample an image without rediscovering it until it changes its
	// contents, tracking, identity or layout, which queues a heap change.
	void HeapChanged() const {
		StateChanged();
		if (heap_refs.load(std::memory_order_relaxed) != 0u) {
			std::scoped_lock lock {heap_mutex};
			heap_changes.push_back({info.data, serial});
		}
	}

	static inline std::mutex              heap_mutex; // After TextureCache::m_lock.
	static inline std::vector<HeapChange> heap_changes;
	static inline std::atomic<uint64_t>   serials {0};

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
	// Counts the transitions to a write access: every GPU or host write of the image takes one.
	uint64_t         write_count        = 0;
	// A depth copy (see TextureCache::FindDepthCopy()): the color image it copies, and the
	// write_count of that image the copy holds. depth_copy_source is reset when the color image
	// is deleted.
	bool             is_depth_copy      = false;
	ImageId          depth_copy_source {};
	uint64_t         depth_copy_writes  = UINT64_MAX;
	// A stencil copy (see TextureCache::FindStencilCopy()): depth_copy_source names the stencil
	// association it copies, depth_copy_writes the write_count of its depth image it holds, and
	// stencil_copy_writes its own write_count when it last matched that stencil.
	bool             is_stencil_copy     = false;
	uint64_t         stencil_copy_writes = 0;
	// Stencil associations that refer to this depth image.
	std::vector<ImageId> stencil_ids;
	// The current stencil plane's mapping into the depth image, also retained by its association.
	ImageSubresourceRange stencil_subresources;
	uint64_t         tick_accessed_last = 0;
	size_t           lru_id             = 0;
	// Resident image heap elements that select this image.
	std::atomic<uint32_t> heap_refs {0};
	// The tick of the image's last LRU touch (as in the texture cache's LRU list, without
	// loading its entry).
	uint64_t         lru_tick           = 0;
	// Bindless table bindings that keep binding the image without looking it up again (see
	// TextureCache::PinImage()).
	uint32_t         table_pins         = 0;
	// The pages whose access is watched while the image is read back lazily (see
	// TextureCache::ArmLazyReadback()); both zero while it is not.
	uint64_t         readback_begin     = 0;
	uint64_t         readback_end       = 0;
	// The texture cache's version counter of the image's slot (see StateChanged()); null outside
	// a texture cache.
	std::atomic<uint32_t>* version = nullptr;

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
