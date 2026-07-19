#include "graphics/host_gpu/renderer/textureCache.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/objects/label.h"
#include "graphics/host_gpu/objects/textureCommon.h"
#include "graphics/host_gpu/renderer/bufferCache.h"
#include "graphics/host_gpu/renderer/dummyTextureCache.h"
#include "graphics/host_gpu/renderer/image.h"
#include "graphics/host_gpu/renderer/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderTargetBarriers.h"
#include "graphics/host_gpu/renderer/resourceMutex.h"
#include "graphics/host_gpu/transfer.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

thread_local const void* g_texture_cache_lock_owner = nullptr;
thread_local const void* g_texture_fault_owner      = nullptr;

[[nodiscard]] uint64_t HashSampledImageEdges(const Image& image) {
	constexpr uint64_t page_mask  = TRACKER_PAGE_SIZE - 1;
	const uint64_t     begin      = image.address;
	const uint64_t     end        = image.address + image.size;
	const uint64_t     head_end   = std::min(end, (begin + page_mask) & ~page_mask);
	const uint64_t     tail_begin = std::max(begin, end & ~page_mask);
	const uint64_t     head_size  = head_end - begin;
	const uint64_t     tail_size  = tail_begin < head_end ? end - head_end : end - tail_begin;
	std::array<uint8_t, TRACKER_PAGE_SIZE * 2> bytes {};
	if (head_size + tail_size > bytes.size()) {
		EXIT("TextureCache: sampled-image edge hash range overflow\n");
	}
	if ((head_size != 0 && !LibKernel::Memory::TryReadBacking(begin, bytes.data(), head_size)) ||
	    (tail_size != 0 &&
	     !LibKernel::Memory::TryReadBacking(tail_begin < head_end ? head_end : tail_begin,
	                                        bytes.data() + head_size, tail_size))) {
		EXIT("TextureCache: failed to hash sampled-image edge backing, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     image.address, image.size);
	}
	return XXH3_64bits(bytes.data(), static_cast<size_t>(head_size + tail_size));
}

class FaultSafeTextureLock final {
public:
	FaultSafeTextureLock(const void* owner, TrackingSpinLock& mutex): m_mutex(mutex) {
		if (g_texture_cache_lock_owner != nullptr) {
			EXIT("TextureCache: recursive cache lock acquisition, current=%p\n",
			     g_texture_cache_lock_owner);
		}
		g_texture_cache_lock_owner = owner;
		m_mutex.lock();
	}
	~FaultSafeTextureLock() {
		m_mutex.unlock();
		g_texture_cache_lock_owner = nullptr;
	}

private:
	TrackingSpinLock& m_mutex;
};

enum class ImageRangeOverlap : uint8_t { None, PageOnly, Bytes };

ImageRangeOverlap ClassifyImageRangeOverlap(uint64_t left, uint64_t left_size, uint64_t right,
                                            uint64_t right_size) {
	if (ImageRangeOverlaps(left, left_size, right, right_size)) {
		return ImageRangeOverlap::Bytes;
	}
	return ImagePageRangesOverlap(left, left_size, right, right_size) ? ImageRangeOverlap::PageOnly
	                                                                  : ImageRangeOverlap::None;
}

} // namespace

struct TextureCache::CachedImage {
	enum class Kind {
		Texture,
		StorageTexture,
		RenderTarget,
		DepthTarget,
		VideoOut
	} kind = Kind::Texture;
	Image            info;
	RenderTargetInfo target;
	DepthTargetInfo  depth;
	VideoOutInfo     video_out;
	GraphicContext*  ctx                 = nullptr;
	VulkanImage*     image               = nullptr;
	bool             gpu_modified        = false;
	bool             buffer_modified     = false;
	bool             stencil_initialized = false;
	bool             registered          = false;

	~CachedImage() {
		if (ctx == nullptr || image == nullptr || registered) {
			EXIT("TextureCache: cached image destroyed with invalid resources, ctx=%p image=%p "
			     "kind=%u registered=%d\n",
			     static_cast<const void*>(ctx), static_cast<const void*>(image),
			     static_cast<uint32_t>(kind), registered);
		}
		ImageOps::Destroy(ctx, image);
		image = nullptr;
	}

	[[nodiscard]] uint32_t RangeCount() const {
		return kind == Kind::DepthTarget && depth.stencil_address != 0 ? 2u : 1u;
	}
	[[nodiscard]] BufferImageBinding BufferBinding() const {
		switch (kind) {
			case Kind::Texture: return BufferImageBinding::Texture;
			case Kind::RenderTarget: return BufferImageBinding::RenderTarget;
			case Kind::VideoOut: return BufferImageBinding::VideoOut;
			case Kind::StorageTexture: return BufferImageBinding::StorageTexture;
			case Kind::DepthTarget: return BufferImageBinding::DepthTarget;
		}
		return BufferImageBinding::Unsupported;
	}
	[[nodiscard]] uint64_t Address(uint32_t index = 0) const {
		if (index >= RangeCount()) {
			EXIT("TextureCache: image address range index out of bounds, index=%u count=%u\n",
			     index, RangeCount());
		}
		if (index == 1) {
			return depth.stencil_address;
		}
		switch (kind) {
			case Kind::Texture: return info.address;
			case Kind::StorageTexture: return info.address;
			case Kind::RenderTarget: return target.address;
			case Kind::DepthTarget: return depth.address;
			case Kind::VideoOut: return video_out.address;
		}
		EXIT("TextureCache: unsupported cached image kind %u for address\n",
		     static_cast<uint32_t>(kind));
	}
	[[nodiscard]] uint64_t Size(uint32_t index = 0) const {
		if (index >= RangeCount()) {
			EXIT("TextureCache: image size range index out of bounds, index=%u count=%u\n", index,
			     RangeCount());
		}
		if (index == 1) {
			return depth.stencil_size;
		}
		switch (kind) {
			case Kind::Texture: return info.size;
			case Kind::StorageTexture: return info.size;
			case Kind::RenderTarget: return target.size;
			case Kind::DepthTarget: return depth.size;
			case Kind::VideoOut: return video_out.size;
		}
		EXIT("TextureCache: unsupported cached image kind %u for size\n",
		     static_cast<uint32_t>(kind));
	}
	[[nodiscard]] bool OverlapsRange(uint64_t address, uint64_t size, bool page) const {
		for (uint32_t i = 0; i < RangeCount(); i++) {
			if (page ? ImagePageRangesOverlap(address, size, Address(i), Size(i))
			         : ImageRangeOverlaps(address, size, Address(i), Size(i))) {
				return true;
			}
		}
		return false;
	}
	// A page-expanded region lookup identifies candidates before exact byte classification. Kyty's
	// tracker retains GPU ownership per 4 KiB page, so an edge-page
	// candidate must remain coherent even when the triggering byte lies just outside the image.
	[[nodiscard]] bool IsGpuReadbackPageCandidate(uint64_t address, uint64_t size) const {
		return gpu_modified && OverlapsRange(address, size, true);
	}
	[[nodiscard]] bool HasExactRange(uint64_t address, uint64_t size) const {
		for (uint32_t i = 0; i < RangeCount(); i++) {
			if (address == Address(i) && size == Size(i)) {
				return true;
			}
		}
		return false;
	}
};

void TextureCache::RegisterImageLocked(CachedImage& image) {
	if (image.registered) {
		EXIT("TextureCache: registering an already registered image\n");
	}
	std::vector<ImageOwnerIndex::ByteRange> ranges;
	ranges.reserve(image.RangeCount());
	for (uint32_t range = 0; range < image.RangeCount(); range++) {
		ranges.push_back({image.Address(range), image.Size(range)});
	}
	if (!m_image_owner_index.Register(&image, ranges)) {
		EXIT("TextureCache: invalid or duplicate image registration\n");
	}
	image.registered = true;
}

void TextureCache::UnregisterImageLocked(CachedImage& image, bool release_tracking) {
	if (!image.registered) {
		EXIT("TextureCache: unregistering an unregistered image\n");
	}
	std::vector<ImageOwnerIndex::ByteRange> final_releases;
	if (!m_image_owner_index.Unregister(&image, final_releases)) {
		EXIT("TextureCache: image missing from owner index\n");
	}
	if (release_tracking) {
		for (const auto& release: final_releases) {
			m_memory_tracker.UntrackMemory(release.address, release.size);
		}
	}
	image.registered = false;
}

VulkanImage* TextureCache::PublishImage(CommandBuffer*               command,
                                        std::shared_ptr<CachedImage> image) {
	if (command == nullptr || image == nullptr || image->image == nullptr || image->registered) {
		EXIT("TextureCache: invalid image publication, command=%p record=%p image=%p "
		     "registered=%d\n",
		     static_cast<const void*>(command), static_cast<const void*>(image.get()),
		     image != nullptr ? static_cast<const void*>(image->image) : nullptr,
		     image != nullptr && image->registered);
	}
	auto result = image->image;
	command->RetainResourceUntilFence(image);
	m_images.push_back(std::move(image));
	RegisterImageLocked(*m_images.back());
	return result;
}

std::vector<TextureCache::CachedImage*>
TextureCache::FindImagesInRegionLocked(uint64_t vaddr, uint64_t size, bool page_overlap) {
	return page_overlap ? m_image_owner_index.QueryCandidates(vaddr, size)
	                    : m_image_owner_index.Query(vaddr, size);
}

struct TextureCache::ReadbackWorker {
	enum class State : uint32_t {
		Idle,
		Claimed,
		Requested,
		Ready,
		Installed,
		Completed,
		Stopping,
		Stopped
	};
	struct ReadbackRange {
		uint64_t address;
		uint64_t size;
	};
	static_assert(std::atomic<State>::is_always_lock_free);

	explicit ReadbackWorker(TextureCache& owner): cache(owner), thread([this] { Run(); }) {}

	~ReadbackWorker() {
		auto expected = State::Idle;
		if (!state.compare_exchange_strong(expected, State::Stopping, std::memory_order_acq_rel)) {
			EXIT("TextureCache: cannot stop readback worker from state %u\n",
			     static_cast<uint32_t>(expected));
		}
		state.notify_all();
		thread.join();
		if (state.load(std::memory_order_acquire) != State::Stopped) {
			EXIT("TextureCache: readback worker did not reach stopped state\n");
		}
	}

	void Request(PageFaultAccess fault_access, uint64_t fault_vaddr, uint64_t fault_size) noexcept {
		const bool command_thread            = GraphicsRunIsCommandProcessorThread();
		const bool submissions_prepaused_now = GraphicsRunSubmissionLockHeld() || command_thread;
		const bool unsafe_gpu_lock = GraphicsRunGpuLockHeld() && !submissions_prepaused_now;
		if ((fault_access != PageFaultAccess::Read && fault_access != PageFaultAccess::Write) ||
		    unsafe_gpu_lock || LabelInCallback() || g_texture_cache_lock_owner != nullptr ||
		    g_texture_fault_owner != &cache) {
			EXIT("TextureCache: unsafe image readback request, access=%u command_thread=%d "
			     "submission_lock=%d gpu_lock=%d label_callback=%d cache_lock=%p fault_owner=%p\n",
			     static_cast<uint32_t>(fault_access), command_thread,
			     GraphicsRunSubmissionLockHeld(), GraphicsRunGpuLockHeld(), LabelInCallback(),
			     g_texture_cache_lock_owner, g_texture_fault_owner);
		}
		State expected = State::Idle;
		while (!state.compare_exchange_weak(expected, State::Claimed, std::memory_order_acq_rel)) {
			if (expected == State::Stopping || expected == State::Stopped) {
				EXIT("TextureCache: image readback requested while worker is stopping, state=%u\n",
				     static_cast<uint32_t>(expected));
			}
			state.wait(expected, std::memory_order_acquire);
			expected = State::Idle;
		}
		access                = fault_access;
		vaddr                 = fault_vaddr;
		size                  = fault_size;
		submissions_prepaused = submissions_prepaused_now;
		state.store(State::Requested, std::memory_order_release);
		state.notify_all();
		while (true) {
			const auto current = state.load(std::memory_order_acquire);
			if (current == State::Ready) {
				return;
			}
			if (current != State::Requested) {
				EXIT("TextureCache: invalid state while waiting for image readback, state=%u\n",
				     static_cast<uint32_t>(current));
			}
			state.wait(current, std::memory_order_acquire);
		}
	}

	[[nodiscard]] bool Complete(PageFaultAccess fault_access, uint64_t fault_vaddr,
	                            uint64_t fault_size) noexcept {
		const auto current = state.load(std::memory_order_acquire);
		if (current == State::Idle || current == State::Stopping || current == State::Stopped) {
			return false;
		}
		if (current != State::Ready) {
			EXIT("TextureCache: active image readback has invalid completion state %u\n",
			     static_cast<uint32_t>(current));
		}
		if (access != fault_access || vaddr != fault_vaddr || size != fault_size) {
			EXIT("TextureCache: mismatched active image readback completion\n");
		}
		state.store(State::Installed, std::memory_order_release);
		state.notify_all();
		return true;
	}

	[[nodiscard]] bool IsReady(PageFaultAccess fault_access, uint64_t fault_vaddr,
	                           uint64_t fault_size) const noexcept {
		return state.load(std::memory_order_acquire) == State::Ready && access == fault_access &&
		       vaddr == fault_vaddr && size == fault_size;
	}

	void Release(PageFaultAccess fault_access, uint64_t fault_vaddr, uint64_t fault_size) noexcept {
		const auto current = state.load(std::memory_order_acquire);
		if (current == State::Idle || current == State::Stopping || current == State::Stopped) {
			return;
		}
		if (current != State::Installed) {
			EXIT("TextureCache: active image readback has invalid release state %u\n",
			     static_cast<uint32_t>(current));
		}
		if (access != fault_access || vaddr != fault_vaddr || size != fault_size) {
			EXIT("TextureCache: mismatched active image readback release\n");
		}
		state.store(State::Completed, std::memory_order_release);
		state.notify_all();
		while (true) {
			const auto current = state.load(std::memory_order_acquire);
			if (current == State::Idle) {
				break;
			}
			if (current != State::Completed) {
				EXIT("TextureCache: invalid state while releasing image readback, state=%u\n",
				     static_cast<uint32_t>(current));
			}
			state.wait(current, std::memory_order_acquire);
		}
	}

	[[nodiscard]] ReadbackRange DownloadDepthTarget(CachedImage& cached,
	                                                bool         require_fault_range = true) {
		const auto& info           = cached.depth;
		const bool  has_stencil    = info.stencil_address != 0 || info.stencil_size != 0;
		const bool  has_htile      = info.htile_address != 0 || info.htile_size != 0;
		const bool  fault_in_depth = vaddr >= info.address && vaddr < info.address + info.size &&
		                             size <= info.address + info.size - vaddr;
		const bool  d16 =
		    info.guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm) &&
		    info.format == vk::Format::eD16Unorm && info.bytes_per_element == 2;
		const bool d32 =
		    info.guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float) &&
		    info.format == vk::Format::eD32Sfloat && info.bytes_per_element == 4;
		TileSizeAlign expected_stencil {};
		TileSizeAlign expected_htile {};
		TileSizeAlign expected_depth {};
		const bool    prospero_layout =
		    (d16 || d32) &&
		    TileGetDepthSize(info.width, info.height, 0,
		                     Prospero::GpuEnumValue(d16 ? Prospero::DepthFormat::kZ16
		                                                : Prospero::DepthFormat::kZ32F),
		                     Prospero::GpuEnumValue(Prospero::StencilFormat::kInvalid), has_htile,
		                     &expected_stencil, &expected_htile, &expected_depth);
		const auto expected_pitch =
		    prospero_layout
		        ? TileGetTexturePitch(info.guest_format, info.width, 1,
		                              Prospero::GpuEnumValue(Prospero::TileMode::kDepth))
		        : 0u;
		const bool layered_sizes =
		    info.layers != 0 && info.size <= UINT32_MAX &&
		    expected_depth.size <= UINT64_MAX / info.layers &&
		    info.size == expected_depth.size * info.layers &&
		    (!has_htile || (expected_htile.size <= UINT64_MAX / info.layers &&
		                    info.htile_size == expected_htile.size * info.layers));
		if ((require_fault_range && !fault_in_depth) || has_stencil || info.address == 0 ||
		    info.size == 0 || info.width == 0 || info.height == 0 || info.pitch < info.width ||
		    info.tile_mode != Prospero::GpuEnumValue(Prospero::TileMode::kDepth) ||
		    !prospero_layout || info.pitch != expected_pitch || !layered_sizes ||
		    expected_depth.align != 65536u || cached.image->layers != info.layers ||
		    (has_htile && expected_htile.align != 32768u)) {
			EXIT("TextureCache: unsupported depth-image readback layout, fault=0x%016" PRIx64
			     "+0x%016" PRIx64 " depth=0x%016" PRIx64 "+0x%016" PRIx64 " stencil=0x%016" PRIx64
			     "+0x%016" PRIx64 " htile=0x%016" PRIx64 "+0x%016" PRIx64
			     " extent=%ux%u pitch=%u/%u layers=%u/%u tile=%u format=%d guest_format=%u bpe=%u "
			     "expected_depth=0x%016" PRIx64 " expected_htile=0x%016" PRIx64 "\n",
			     vaddr, size, info.address, info.size, info.stencil_address, info.stencil_size,
			     info.htile_address, info.htile_size, info.width, info.height, info.pitch,
			     expected_pitch, info.layers, cached.image->layers, info.tile_mode,
			     static_cast<int>(info.format), info.guest_format, info.bytes_per_element,
			     expected_depth.size, expected_htile.size);
		}
		const auto rows = static_cast<uint64_t>(info.height - 1);
		if (rows > (UINT64_MAX - info.width) / info.pitch) {
			EXIT("TextureCache: depth-image readback size overflow\n");
		}
		const auto linear_elements = rows * info.pitch + info.width;
		if (linear_elements > UINT64_MAX / info.bytes_per_element) {
			EXIT("TextureCache: depth-image readback size overflow\n");
		}
		const auto linear_size    = linear_elements * info.bytes_per_element;
		const auto slice_size     = info.size / info.layers;
		const bool meta_overlap   = cache.HasMetaOverlapLocked(info.address, info.size);
		const bool buffer_overlap = cache.m_buffer_cache.HasPageOverlap(info.address, info.size);
		if (linear_size > slice_size || cached.image->format != info.format ||
		    cached.image->extent.width != info.width ||
		    cached.image->extent.height != info.height || meta_overlap || buffer_overlap) {
			EXIT("TextureCache: depth-image readback storage is unsupported, "
			     "depth=0x%016" PRIx64 "+0x%016" PRIx64 " linear=0x%016" PRIx64
			     " format=%d/%d extent=%ux%u/%ux%u meta=%d buffer=%d\n",
			     info.address, info.size, linear_size, static_cast<int>(cached.image->format),
			     static_cast<int>(info.format), cached.image->extent.width,
			     cached.image->extent.height, info.width, info.height, meta_overlap,
			     buffer_overlap);
		}
		download.resize(info.size);
		std::fill(download.begin(), download.end(), 0);
		const auto regions =
		    Transfer::MakeLayeredImageBufferCopies(info.layers, slice_size, info.pitch, info.width,
		                                           info.height, vk::ImageAspectFlagBits::eDepth);
		Transfer::DownloadImage(cached.ctx, download.data(), info.size, regions, cached.image,
		                        cached.image->layout);
		guest.resize(info.size);
		cache.m_tiler.TileImage(guest.data(), download.data(), info);
		Libs::LibKernel::Memory::WriteBacking(info.address, guest.data(), info.size);
		return {info.address, info.size};
	}

	[[nodiscard]] ReadbackRange DownloadColorImage(CachedImage& cached) {
		const bool storage = cached.kind == CachedImage::Kind::StorageTexture;
		const bool target  = cached.kind == CachedImage::Kind::RenderTarget;
		const auto info =
		    storage ? MakeColorImageTransferInfo(cached.info, VulkanFormat(cached.info.format),
		                                         Prospero::NumBytesPerElement(cached.info.format))
		            : MakeColorImageTransferInfo(cached.target);
		const bool linear = info.tile_mode == Prospero::GpuEnumValue(Prospero::TileMode::kLinear);
		const bool tiled_target = target && IsTiledRenderTarget(cached.target);
		const bool tiled_storage =
		    storage && info.tile_mode == Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
		bool single_layer_storage = false;
		if (storage) {
			switch (static_cast<Prospero::ImageType>(cached.info.type)) {
				case Prospero::ImageType::kColor2D:
				case Prospero::ImageType::kColor2DArray:
					single_layer_storage = cached.info.depth == 1;
					break;
				default: break;
			}
		}
		const bool basic_storage =
		    !storage ||
		    (single_layer_storage && cached.info.base_level == 0 && cached.info.levels == 1 &&
		     cached.info.base_array == 0 && (linear || tiled_storage));
		const auto layers = target ? cached.target.layers : 1u;
		if ((!linear && !tiled_target && !tiled_storage) || !basic_storage || info.levels != 1 ||
		    info.size > UINT32_MAX) {
			EXIT("TextureCache: unsupported color-image readback layout, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " extent=%ux%u pitch=%u bpe=%u levels=%u tile=%u kind=%u\n",
			     info.address, info.size, info.width, info.height, info.pitch,
			     info.bytes_per_element, info.levels, info.tile_mode,
			     static_cast<uint32_t>(cached.kind));
		}
		const auto slice_size     = info.size / layers;
		const bool meta_overlap   = cache.HasMetaOverlapLocked(info.address, info.size);
		const bool buffer_overlap = cache.m_buffer_cache.HasPageOverlap(info.address, info.size);
		if (meta_overlap || buffer_overlap) {
			EXIT("TextureCache: color-image readback storage is unsupported, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " meta=%d buffer=%d kind=%u\n",
			     info.address, info.size, meta_overlap, buffer_overlap,
			     static_cast<uint32_t>(cached.kind));
		}
		download.resize(info.size);
		std::fill(download.begin(), download.end(), 0);
		const auto regions = Transfer::MakeLayeredImageBufferCopies(layers, slice_size, info.pitch,
		                                                            info.width, info.height);
		Transfer::DownloadImage(cached.ctx, download.data(), info.size, regions, cached.image,
		                        cached.image->layout);
		if (tiled_target || tiled_storage) {
			guest.resize(info.size);
			const RenderTargetInfo layout =
			    target ? cached.target : RenderTargetInfo {info.address,
			                                               info.size,
			                                               info.format,
			                                               info.width,
			                                               info.height,
			                                               info.pitch,
			                                               info.bytes_per_element,
			                                               info.tile_mode,
			                                               info.levels,
			                                               1};
			cache.m_tiler.TileImage(guest.data(), download.data(), layout);
			Libs::LibKernel::Memory::WriteBacking(info.address, guest.data(), info.size);
		} else {
			Libs::LibKernel::Memory::WriteBacking(info.address, download.data(), info.size);
		}
		return {info.address, info.size};
	}

	void Run() noexcept {
		while (true) {
			auto current = state.load(std::memory_order_acquire);
			while (current == State::Idle) {
				state.wait(current, std::memory_order_acquire);
				current = state.load(std::memory_order_acquire);
			}
			if (current == State::Stopping) {
				state.store(State::Stopped, std::memory_order_release);
				state.notify_all();
				return;
			}
			if (current != State::Requested) {
				EXIT("TextureCache: readback worker received unsupported state %u\n",
				     static_cast<uint32_t>(current));
			}

			std::optional<GraphicsRunSubmissionLock> submissions;
			if (!submissions_prepaused) {
				submissions.emplace();
			}
			FaultSafeTextureLock lock(&cache, cache.m_lock);
			CachedImage*         selected = cache.FindGpuReadbackPageCandidateLocked(vaddr, size);
			const bool           render_target =
			    selected != nullptr && selected->kind == CachedImage::Kind::RenderTarget;
			const bool storage_texture =
			    selected != nullptr && selected->kind == CachedImage::Kind::StorageTexture;
			const bool depth_target =
			    selected != nullptr && selected->kind == CachedImage::Kind::DepthTarget;
			if ((!render_target && !storage_texture && !depth_target) || !selected->gpu_modified ||
			    selected->buffer_modified || selected->ctx == nullptr ||
			    selected->image == nullptr) {
				EXIT("TextureCache: unsupported GPU image readback owner, addr=0x%016" PRIx64
				     " size=0x%016" PRIx64 " access=%u image=%p kind=%u gpu_modified=%d "
				     "buffer_modified=%d ctx=%p vulkan_image=%p\n",
				     vaddr, size, static_cast<uint32_t>(access), static_cast<const void*>(selected),
				     selected != nullptr ? static_cast<uint32_t>(selected->kind) : UINT32_MAX,
				     selected != nullptr && selected->gpu_modified,
				     selected != nullptr && selected->buffer_modified,
				     selected != nullptr ? static_cast<const void*>(selected->ctx) : nullptr,
				     selected != nullptr ? static_cast<const void*>(selected->image) : nullptr);
			}

			const auto transfer =
			    depth_target ? DownloadDepthTarget(*selected) : DownloadColorImage(*selected);

			if (!cache.m_memory_tracker.IsRegionGpuModified(vaddr, size)) {
				EXIT("TextureCache: readback fault page is not GPU-modified, addr=0x%016" PRIx64
				     " size=0x%016" PRIx64 "\n",
				     vaddr, size);
			}
			const auto fault_page = vaddr & ~(TRACKER_PAGE_SIZE - 1);
			const auto target_end = transfer.address + transfer.size;
			const auto page_end   = fault_page + TRACKER_PAGE_SIZE;
			if (fault_page > transfer.address) {
				cache.m_memory_tracker.ForEachDownloadRange<true>(
				    transfer.address, fault_page - transfer.address,
				    [](uint64_t, uint64_t) noexcept {});
			}
			if (page_end < target_end) {
				cache.m_memory_tracker.ForEachDownloadRange<true>(
				    page_end, target_end - page_end, [](uint64_t, uint64_t) noexcept {});
			}

			state.store(State::Ready, std::memory_order_release);
			state.notify_all();
			while ((current = state.load(std::memory_order_acquire)) != State::Completed) {
				if (current != State::Ready && current != State::Installed) {
					EXIT("TextureCache: invalid image readback completion state %u\n",
					     static_cast<uint32_t>(current));
				}
				state.wait(current, std::memory_order_acquire);
			}
			if (cache.m_memory_tracker.IsRegionGpuModified(transfer.address, transfer.size)) {
				EXIT("TextureCache: completed image readback retained GPU ownership, "
				     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
				     transfer.address, transfer.size);
			}
			selected->gpu_modified = false;
			submissions_prepaused  = false;
			state.store(State::Idle, std::memory_order_release);
			state.notify_all();
		}
	}

	TextureCache&        cache;
	std::atomic<State>   state {State::Idle};
	PageFaultAccess      access                = PageFaultAccess::Unknown;
	uint64_t             vaddr                 = 0;
	uint64_t             size                  = 0;
	bool                 submissions_prepaused = false;
	std::vector<uint8_t> download;
	std::vector<uint8_t> guest;
	std::thread          thread;
};

void TextureCache::RequireRetirementIsolation(const std::vector<CachedImage*>& retire,
                                              const char* operation, uint64_t address,
                                              uint64_t size) const {
	if (retire.empty()) {
		return;
	}
	std::vector<ImageRetirementRange> ranges;
	ranges.reserve(m_images.size() * 2);
	for (const auto& cached: m_images) {
		const bool retiring = std::find(retire.begin(), retire.end(), cached.get()) != retire.end();
		for (uint32_t range = 0; range < cached->RangeCount(); range++) {
			ranges.push_back({cached->Address(range), cached->Size(range), retiring});
		}
	}
	const auto conflict = FindImageRetirementConflict(ranges);
	if (conflict.Exists()) {
		const auto& retired  = ranges[conflict.retired];
		const auto& retained = ranges[conflict.retained];
		EXIT("TextureCache: %s retirement leaves a tracked page alias, request=0x%016" PRIx64
		     "+0x%016" PRIx64 " retired=0x%016" PRIx64 "+0x%016" PRIx64 " retained=0x%016" PRIx64
		     "+0x%016" PRIx64 "\n",
		     operation, address, size, retired.address, retired.size, retained.address,
		     retained.size);
	}
}

void TextureCache::RetireImages(const std::vector<CachedImage*>& retire,
                                const CachedImage*               native_image_source) {
	if (retire.empty()) {
		return;
	}
	// FindTexture and FindRenderTarget retain cache records on their recording commands. Erasing
	// the lookup owner therefore defers Vulkan destruction until the last referencing fence.
	size_t removed              = 0;
	bool   native_image_retired = false;
	for (auto it = m_images.begin(); it != m_images.end();) {
		if (std::find(retire.begin(), retire.end(), it->get()) == retire.end()) {
			++it;
			continue;
		}
		const bool sampled      = (*it)->kind == CachedImage::Kind::Texture;
		const bool storage      = (*it)->kind == CachedImage::Kind::StorageTexture;
		const bool target       = (*it)->kind == CachedImage::Kind::RenderTarget ||
		                          (*it)->kind == CachedImage::Kind::DepthTarget;
		const bool native_image = it->get() == native_image_source;
		if (native_image) {
			bool source_valid = (*it)->gpu_modified && !(*it)->buffer_modified;
			switch ((*it)->kind) {
				case CachedImage::Kind::StorageTexture:
					source_valid = source_valid && !(*it)->info.IsCpuDirty();
					break;
				case CachedImage::Kind::RenderTarget:
				case CachedImage::Kind::DepthTarget: break;
				case CachedImage::Kind::Texture:
				case CachedImage::Kind::VideoOut: source_valid = false; break;
			}
			for (uint32_t range = 0; source_valid && range < (*it)->RangeCount(); range++) {
				source_valid =
				    m_memory_tracker.IsRegionGpuModified((*it)->Address(range), (*it)->Size(range));
			}
			if (!source_valid) {
				EXIT("TextureCache: invalid native image retirement, kind=%u gpu_modified=%d "
				     "buffer_modified=%d cpu_dirty=%d\n",
				     static_cast<uint32_t>((*it)->kind), (*it)->gpu_modified,
				     (*it)->buffer_modified,
				     (*it)->kind == CachedImage::Kind::StorageTexture && (*it)->info.IsCpuDirty());
			}
			(*it)->gpu_modified  = false;
			native_image_retired = true;
		}
		if ((!sampled && !storage && !target && !native_image) || (*it)->gpu_modified ||
		    (storage && ((*it)->buffer_modified || (*it)->info.IsCpuDirty())) ||
		    (target && (*it)->buffer_modified)) {
			EXIT("TextureCache: invalid image retirement, kind=%u gpu_modified=%d "
			     "buffer_modified=%d\n",
			     static_cast<uint32_t>((*it)->kind), (*it)->gpu_modified, (*it)->buffer_modified);
		}
		for (uint32_t range = 0; range < (*it)->RangeCount(); range++) {
			if (target && !native_image &&
			    m_memory_tracker.IsRegionGpuModified((*it)->Address(range), (*it)->Size(range))) {
				EXIT("TextureCache: clean target retirement retained tracker GPU ownership, "
				     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
				     (*it)->Address(range), (*it)->Size(range));
			}
		}
		// A native transition keeps tracker ownership continuous until the replacement is
		// published. Normal retirement only releases 4 KiB pages with no remaining image owner.
		UnregisterImageLocked(**it, !native_image);
		it = m_images.erase(it);
		removed++;
	}
	if (removed != retire.size()) {
		EXIT("TextureCache: image retirement set mismatch, requested=%zu removed=%zu\n",
		     retire.size(), removed);
	}
	if (native_image_source != nullptr && !native_image_retired) {
		EXIT("TextureCache: native image retirement source was not retired\n");
	}
}

namespace {

bool Equal(const ImageInfo& left, const ImageInfo& right) {
	return left.address == right.address && left.size == right.size &&
	       left.format == right.format && left.width == right.width &&
	       left.height == right.height && left.pitch == right.pitch &&
	       left.base_level == right.base_level && left.levels == right.levels &&
	       left.view_levels == right.view_levels && left.tile == right.tile &&
	       left.swizzle == right.swizzle && left.depth == right.depth && left.type == right.type &&
	       left.base_array == right.base_array;
}

bool EqualStorageBacking(const ImageInfo& left, const ImageInfo& right) {
	return left.address == right.address && left.size == right.size &&
	       left.format == right.format && left.width == right.width &&
	       left.height == right.height && left.pitch == right.pitch &&
	       left.levels == right.levels && left.tile == right.tile && left.depth == right.depth &&
	       left.type == right.type && left.base_array == right.base_array;
}

bool Equal(const RenderTargetInfo& left, const RenderTargetInfo& right) {
	return left.address == right.address && left.size == right.size &&
	       left.format == right.format && left.width == right.width &&
	       left.height == right.height && left.pitch == right.pitch &&
	       left.bytes_per_element == right.bytes_per_element && left.tile_mode == right.tile_mode &&
	       left.levels == right.levels && left.layers == right.layers;
}

bool Equal(const DepthTargetInfo& left, const DepthTargetInfo& right) {
	return left.address == right.address && left.size == right.size &&
	       left.stencil_address == right.stencil_address &&
	       left.stencil_size == right.stencil_size && left.htile_address == right.htile_address &&
	       left.htile_size == right.htile_size && left.format == right.format &&
	       left.guest_format == right.guest_format && left.width == right.width &&
	       left.height == right.height && left.pitch == right.pitch &&
	       left.bytes_per_element == right.bytes_per_element && left.tile_mode == right.tile_mode &&
	       left.layers == right.layers &&
	       left.stencil_htile_compressed == right.stencil_htile_compressed;
}

[[nodiscard]] bool IsCoherentGuestImageSource(const BufferImageCopySource& source, uint64_t address,
                                              uint64_t size) {
	// The current PS5 Tiler consumes coherent guest backing directly. A native buffer is an
	// optional future GPU-detiler source or staging fallback.
	return source.cpu_current && source.address == address && source.size == size &&
	       (source.buffer != nullptr || source.offset == 0);
}

[[nodiscard]] BufferImageCopySource SelectSourceRange(const BufferImageCopySource& source,
                                                      uint64_t address, uint64_t size) {
	if (address < source.address || size > source.size ||
	    address - source.address > source.size - size ||
	    (source.buffer != nullptr && source.offset > UINT64_MAX - (address - source.address))) {
		EXIT("TextureCache: image source subrange is invalid\n");
	}
	const auto offset = source.buffer != nullptr ? source.offset + address - source.address : 0;
	return {source.buffer, offset, address, size, source.cpu_current};
}

[[nodiscard]] DepthTargetInfo SelectDepthLayers(const DepthTargetInfo& info, uint32_t base_layer,
                                                uint32_t layer_count) {
	if (info.layers == 0 || base_layer >= info.layers || layer_count == 0 ||
	    layer_count > info.layers - base_layer || info.size % info.layers != 0 ||
	    info.stencil_size % info.layers != 0 || info.htile_size % info.layers != 0) {
		EXIT("TextureCache: depth layer range is invalid\n");
	}
	auto result   = info;
	result.layers = layer_count;
	for (auto [address, size]: {std::pair {&result.address, &result.size},
	                            std::pair {&result.stencil_address, &result.stencil_size},
	                            std::pair {&result.htile_address, &result.htile_size}}) {
		const auto slice_size = *size / info.layers;
		*address += slice_size * base_layer;
		*size = slice_size * layer_count;
	}
	return result;
}

void AppendLayerCopies(std::vector<ImageImageCopy>& regions, VulkanImage* source,
                       vk::ImageAspectFlags aspect) {
	for (uint32_t layer = 0; layer < source->layers; layer++) {
		for (uint32_t level = 0; level < source->mip_levels; level++) {
			ImageImageCopy region {};
			region.src_image  = source;
			region.src_aspect = aspect;
			region.dst_aspect = aspect;
			region.src_level  = level;
			region.dst_level  = level;
			region.src_layer  = layer;
			region.dst_layer  = layer;
			region.width      = std::max(source->extent.width >> level, 1u);
			region.height     = std::max(source->extent.height >> level, 1u);
			regions.push_back(region);
		}
	}
}

} // namespace

TextureCache::TextureCache(PageManager& page_manager, BufferCache& buffer_cache,
                           ResourceMutex& resource_mutex)
    : m_memory_tracker(page_manager), m_metadata_tracker(page_manager, PageWatchMode::Write),
      m_buffer_cache(buffer_cache), m_resource_mutex(resource_mutex) {
	if (!Common::Thread::IsMainThread()) {
		EXIT("TextureCache: construction is restricted to the main thread\n");
	}
	m_dummy_textures = std::make_unique<DummyTextureCache>();
	m_readback       = std::make_unique<ReadbackWorker>(*this);
}

TextureCache::CachedImage* TextureCache::FindGpuReadbackPageCandidateLocked(uint64_t vaddr,
                                                                            uint64_t size) {
	CachedImage* selected = nullptr;
	for (auto* cached: FindImagesInRegionLocked(vaddr, size, true)) {
		if (!cached->IsGpuReadbackPageCandidate(vaddr, size)) {
			continue;
		}
		if (selected != nullptr) {
			EXIT("TextureCache: CPU fault has multiple GPU-modified image page candidates, "
			     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 " first=%p second=%p\n",
			     vaddr, size, static_cast<const void*>(selected), static_cast<const void*>(cached));
		}
		selected = cached;
	}
	return selected;
}

void TextureCache::MarkSampledAliasesCpuDirtyLocked(uint64_t vaddr, uint64_t size) {
	for (auto* cached: FindImagesInRegionLocked(vaddr, size, true)) {
		if (cached->kind != CachedImage::Kind::Texture) {
			continue;
		}
		if (cached->gpu_modified &&
		    ImageRangeOverlaps(cached->info.address, cached->info.size, vaddr, size)) {
			EXIT("TextureCache: CPU write overlaps a GPU-modified sampled texture, "
			     "write=0x%016" PRIx64 "+0x%016" PRIx64 " image=0x%016" PRIx64 "+0x%016" PRIx64
			     "\n",
			     vaddr, size, cached->info.address, cached->info.size);
		}
		cached->info.InvalidateCpuWrite(vaddr, size);
		if (cached->info.NeedsMaybeCpuHash()) {
			cached->info.SetMaybeCpuHash(HashSampledImageEdges(cached->info));
		}
	}
}

void TextureCache::RetireSampledTargetAliases(GraphicContext* ctx, const ImageInfo& requested) {
	std::vector<CachedImage*> retire;
	bool                      wait_idle = false;
	for (auto* cached: FindImagesInRegionLocked(requested.address, requested.size, true)) {
		if (cached->kind != CachedImage::Kind::RenderTarget) {
			continue;
		}
		switch (ClassifySampledRenderTargetOverlap(requested, cached->target,
		                                           cached->buffer_modified, cached->ctx == ctx)) {
			case RenderTargetOverlap::None: continue;
			case RenderTargetOverlap::RetireTarget:
				wait_idle |= cached->gpu_modified;
				retire.push_back(cached);
				break;
			case RenderTargetOverlap::RetireSampled:
			case RenderTargetOverlap::PreserveStorage:
			case RenderTargetOverlap::ExpandTarget:
			case RenderTargetOverlap::Unsupported:
				EXIT("TextureCache: unsupported sampled/render-target alias, sampled=0x%016" PRIx64
				     "+0x%016" PRIx64 " target=0x%016" PRIx64 "+0x%016" PRIx64
				     " same_context=%d gpu=%d buffer=%d\n",
				     requested.address, requested.size, cached->target.address, cached->target.size,
				     cached->ctx == ctx, cached->gpu_modified, cached->buffer_modified);
		}
	}
	for (auto* cached: FindImagesInRegionLocked(requested.address, requested.size, true)) {
		if (cached->kind != CachedImage::Kind::DepthTarget) {
			continue;
		}
		const auto overlap = ClassifyImageRangeOverlap(requested.address, requested.size,
		                                               cached->depth.address, cached->depth.size);
		if (overlap == ImageRangeOverlap::None || overlap == ImageRangeOverlap::PageOnly) {
			continue;
		}
		const auto delta = cached->depth.address >= requested.address
		                       ? cached->depth.address - requested.address
		                       : UINT64_MAX;
		const bool contained =
		    delta <= requested.size && cached->depth.size <= requested.size - delta;
		const bool sampled_expansion = IsSampledDepthExpansion(requested, cached->depth);
		const bool exact_range =
		    requested.address == cached->depth.address && requested.size == cached->depth.size;
		const bool supported = cached->ctx == ctx && !cached->buffer_modified &&
		                       cached->depth.stencil_address == 0 &&
		                       cached->depth.stencil_size == 0 && cached->depth.layers == 1 &&
		                       contained && (exact_range || sampled_expansion);
		if (!supported) {
			EXIT("TextureCache: unsupported sampled/depth-target alias, sampled=0x%016" PRIx64
			     "+0x%016" PRIx64 " depth=0x%016" PRIx64 "+0x%016" PRIx64
			     " overlap=%u contained=%d same_context=%d gpu=%d buffer=%d stencil=0x%016" PRIx64
			     "+0x%016" PRIx64 " layers=%u expansion=%d"
			     " sampled_info={fmt=%u extent=%ux%u pitch=%u levels=%u/%u tile=%u type=%u}"
			     " depth_info={fmt=%u host=%d extent=%ux%u pitch=%u tile=%u htile=0x%016" PRIx64
			     "+0x%016" PRIx64 "}\n",
			     requested.address, requested.size, cached->depth.address, cached->depth.size,
			     static_cast<uint32_t>(overlap), contained, cached->ctx == ctx,
			     cached->gpu_modified, cached->buffer_modified, cached->depth.stencil_address,
			     cached->depth.stencil_size, cached->depth.layers, sampled_expansion,
			     requested.format, requested.width, requested.height, requested.pitch,
			     requested.levels, requested.view_levels, requested.tile, requested.type,
			     cached->depth.guest_format, static_cast<int>(cached->depth.format),
			     cached->depth.width, cached->depth.height, cached->depth.pitch,
			     cached->depth.tile_mode, cached->depth.htile_address, cached->depth.htile_size);
		}
		wait_idle |= cached->gpu_modified;
		retire.push_back(cached);
	}
	if (retire.empty()) {
		return;
	}
	RequireRetirementIsolation(retire, "sampled target", requested.address, requested.size);
	if (wait_idle) {
		Transfer::WaitForGraphicsIdle(ctx);
	}
	for (auto* cached: retire) {
		if (!cached->gpu_modified) {
			continue;
		}
		const auto transfer = cached->kind == CachedImage::Kind::DepthTarget
		                          ? m_readback->DownloadDepthTarget(*cached, false)
		                          : m_readback->DownloadColorImage(*cached);
		m_memory_tracker.ForEachDownloadRange<true>(transfer.address, transfer.size,
		                                            [](uint64_t, uint64_t) noexcept {});
		cached->gpu_modified = false;
	}
	std::vector<uint64_t> retire_depth_metadata;
	for (auto* cached: retire) {
		if (cached->kind != CachedImage::Kind::DepthTarget || cached->depth.htile_address == 0) {
			continue;
		}
		const bool retained_owner =
		    std::any_of(m_images.begin(), m_images.end(), [&](const auto& other) {
			    return std::find(retire.begin(), retire.end(), other.get()) == retire.end() &&
			           other->kind == CachedImage::Kind::DepthTarget &&
			           other->depth.htile_address == cached->depth.htile_address &&
			           other->depth.htile_size == cached->depth.htile_size;
		    });
		if (!retained_owner &&
		    std::find(retire_depth_metadata.begin(), retire_depth_metadata.end(),
		              cached->depth.htile_address) == retire_depth_metadata.end()) {
			retire_depth_metadata.push_back(cached->depth.htile_address);
		}
	}
	for (const auto address: retire_depth_metadata) {
		auto metadata = m_surface_metas.find(address);
		auto owner    = std::find_if(retire.begin(), retire.end(), [&](const auto* cached) {
			return cached->kind == CachedImage::Kind::DepthTarget &&
			       cached->depth.htile_address == address;
		});
		if (metadata == m_surface_metas.end() || owner == retire.end() ||
		    metadata->second.size != (*owner)->depth.htile_size) {
			EXIT(
			    "TextureCache: retiring depth target has invalid HTile metadata, addr=0x%016" PRIx64
			    " size=0x%016" PRIx64 "\n",
			    address, owner != retire.end() ? (*owner)->depth.htile_size : 0);
		}
		if (metadata->second.gpu_modified) {
			m_metadata_tracker.ForEachDownloadRange<true>(metadata->first, metadata->second.size,
			                                              [](uint64_t, uint64_t) noexcept {});
		}
		m_metadata_tracker.UntrackMemory(metadata->first, metadata->second.size);
		m_surface_metas.erase(metadata);
	}
	RetireImages(retire);
}

void TextureCache::ResolveStorageImageOverlaps(GraphicContext* ctx, const ImageInfo& requested) {
	std::vector<CachedImage*> retire;
	for (auto* cached: FindImagesInRegionLocked(requested.address, requested.size, true)) {
		const bool tracker_gpu =
		    m_memory_tracker.IsRegionGpuModified(cached->Address(), cached->Size());
		switch (ClassifyStorageImageOverlap(
		    requested.address, requested.size, cached->Address(), cached->Size(),
		    cached->kind == CachedImage::Kind::Texture, cached->ctx == ctx, cached->gpu_modified,
		    cached->buffer_modified, tracker_gpu)) {
			case StorageImageOverlap::None: continue;
			case StorageImageOverlap::RetireSampled: retire.push_back(cached); continue;
			case StorageImageOverlap::PageNeighbor: continue;
			case StorageImageOverlap::Unsupported:
				EXIT("TextureCache: unsupported storage-image byte alias, requested=0x%016" PRIx64
				     "+0x%016" PRIx64 " existing=0x%016" PRIx64 "+0x%016" PRIx64
				     " kind=%u same_context=%d gpu=%d/%d buffer=%d\n",
				     requested.address, requested.size, cached->Address(), cached->Size(),
				     static_cast<uint32_t>(cached->kind), cached->ctx == ctx, cached->gpu_modified,
				     tracker_gpu, cached->buffer_modified);
		}
	}
	RequireRetirementIsolation(retire, "storage neighbor", requested.address, requested.size);
	for (auto* cached: retire) {
		if (!cached->gpu_modified) {
			continue;
		}
		const auto transfer = m_readback->DownloadColorImage(*cached);
		m_memory_tracker.ForEachDownloadRange<true>(transfer.address, transfer.size,
		                                            [](uint64_t, uint64_t) noexcept {});
		cached->gpu_modified = false;
	}
	RetireImages(retire);
}

void TextureCache::RetireStorageDepthAliasLocked(GraphicContext* ctx, const ImageInfo& requested) {
	CachedImage* selected = nullptr;
	for (auto* entry: FindImagesInRegionLocked(requested.address, requested.size, true)) {
		auto& cached = *entry;
		if (cached.kind != CachedImage::Kind::DepthTarget ||
		    !cached.OverlapsRange(requested.address, requested.size, true)) {
			continue;
		}
		const auto& depth = cached.depth;
		const bool  exact_d32_uint =
		    cached.ctx == ctx && cached.gpu_modified && !cached.buffer_modified &&
		    depth.address == requested.address && depth.size == requested.size &&
		    depth.stencil_address == 0 && depth.stencil_size == 0 && depth.htile_address == 0 &&
		    depth.htile_size == 0 && depth.layers == 1 && depth.format == vk::Format::eD32Sfloat &&
		    depth.guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float) &&
		    depth.bytes_per_element == 4 && depth.width == requested.width &&
		    depth.height == requested.height && depth.pitch == requested.pitch &&
		    requested.format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32UInt) &&
		    requested.tile == Prospero::GpuEnumValue(Prospero::TileMode::kDepth) &&
		    requested.type == Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) &&
		    requested.depth == 1 && requested.levels == 1;
		if (!exact_d32_uint || selected != nullptr) {
			EXIT("TextureCache: unsupported storage/depth-target alias, requested=0x%016" PRIx64
			     "+0x%016" PRIx64 " depth=0x%016" PRIx64 "+0x%016" PRIx64
			     " exact=%d ambiguous=%d\n",
			     requested.address, requested.size, depth.address, depth.size, exact_d32_uint,
			     selected != nullptr);
		}
		selected = &cached;
	}
	if (selected == nullptr) {
		return;
	}
	Transfer::WaitForGraphicsIdle(ctx);
	const auto transfer = m_readback->DownloadDepthTarget(*selected, false);
	m_memory_tracker.ForEachDownloadRange<true>(transfer.address, transfer.size,
	                                            [](uint64_t, uint64_t) noexcept {});
	selected->gpu_modified = false;
	RetireImages({selected});
}

TextureCache::~TextureCache() {
	m_readback.reset();
	if (!m_images.empty()) {
		Transfer::WaitForGraphicsIdle(m_images.front()->ctx);
	}
	for (const auto& image: m_images) {
		UnregisterImageLocked(*image, false);
	}
	m_images.clear();
	m_dummy_textures.reset();
}

VulkanImage* TextureCache::FindTexture(CommandBuffer* command, GraphicContext* ctx,
                                       const ImageInfo& info, bool metadata_read) {
	if (info.address == 0 || info.size == 0 || info.address >= TRACKER_ADDRESS_SIZE ||
	    info.size > TRACKER_ADDRESS_SIZE - info.address || info.width == 0 || info.height == 0 ||
	    info.depth == 0 || info.levels == 0 || info.levels >= 16 || info.view_levels == 0 ||
	    info.base_level + info.view_levels > info.levels) {
		EXIT("TextureCache: invalid sampled-image request, command=%p ctx=%p addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 " extent=%ux%ux%u levels=%u\n",
		     static_cast<const void*>(command), static_cast<const void*>(ctx), info.address,
		     info.size, info.width, info.height, info.depth, info.levels);
	}
	std::lock_guard transaction(m_resource_mutex);
	{
		FaultSafeTextureLock lock(this, m_lock);
		RetireSampledTargetAliases(ctx, info);
	}
	BufferImageCopySource source {nullptr, 0, info.address, info.size, true};
	if (m_buffer_cache.HasPageOverlap(info.address, info.size)) {
		// ObtainBufferForImage publishes dirty native-buffer bytes when necessary and otherwise
		// uses a CPU-current staging fallback through guest backing.
		source = m_buffer_cache.ObtainBufferForImage(info.address, info.size);
		if (!IsCoherentGuestImageSource(source, info.address, info.size)) {
			EXIT("TextureCache: sampled-image buffer source is inconsistent, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " source=%p source_addr=0x%016" PRIx64
			     " source_size=0x%016" PRIx64 " current=%d\n",
			     info.address, info.size, static_cast<const void*>(source.buffer), source.address,
			     source.size, source.cpu_current);
		}
	}
	FaultSafeTextureLock lock(this, m_lock);
	if (HasMetaOverlapLocked(info.address, info.size) != metadata_read) {
		EXIT("TextureCache: sampled-image metadata classification mismatch, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 " requested=%d\n",
		     info.address, info.size, metadata_read);
	}
	std::shared_ptr<CachedImage> storage_match;
	const auto                   requested_view_format = TextureGetFormat(info.format);
	for (auto& cached: m_images) {
		if (cached->kind != CachedImage::Kind::StorageTexture) {
			continue;
		}
		switch (ClassifyStorageSampledOverlap(info, cached->info, requested_view_format,
		                                      cached->image->format, cached->gpu_modified,
		                                      cached->info.IsCpuDirty(), cached->ctx == ctx)) {
			case StorageSampledOverlap::None: break;
			case StorageSampledOverlap::ExactImage:
				if (storage_match != nullptr) {
					EXIT("TextureCache: duplicate exact storage image for sampled binding, "
					     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
					     info.address, info.size);
				}
				storage_match = cached;
				break;
			case StorageSampledOverlap::Unsupported:
				EXIT("TextureCache: unsupported sampled/storage image alias, "
				     "requested=0x%016" PRIx64 "+0x%016" PRIx64 " storage=0x%016" PRIx64
				     "+0x%016" PRIx64 " gpu_modified=%d cpu_dirty=%d same_context=%d"
				     " requested_info={format=%u extent=%ux%ux%u pitch=%u base=%u levels=%u"
				     " view_levels=%u tile=%u swizzle=0x%03x type=%u base_array=%u}"
				     " storage_info={format=%u extent=%ux%ux%u pitch=%u base=%u levels=%u"
				     " view_levels=%u tile=%u swizzle=0x%03x type=%u base_array=%u}\n",
				     info.address, info.size, cached->info.address, cached->info.size,
				     cached->gpu_modified, cached->info.IsCpuDirty(), cached->ctx == ctx,
				     info.format, info.width, info.height, info.depth, info.pitch, info.base_level,
				     info.levels, info.view_levels, info.tile, info.swizzle, info.type,
				     info.base_array, cached->info.format, cached->info.width, cached->info.height,
				     cached->info.depth, cached->info.pitch, cached->info.base_level,
				     cached->info.levels, cached->info.view_levels, cached->info.tile,
				     cached->info.swizzle, cached->info.type, cached->info.base_array);
		}
	}
	if (storage_match != nullptr) {
		if (m_memory_tracker.IsRegionCpuModified(info.address, info.size) ||
		    !m_memory_tracker.IsRegionGpuModified(info.address, info.size) ||
		    storage_match->image->type != VulkanImageType::StorageTexture ||
		    storage_match->image->image_view[VulkanImage::VIEW_DEFAULT] == nullptr) {
			EXIT("TextureCache: sampled storage-image ownership or view is inconsistent, "
			     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     info.address, info.size);
		}
		command->RetainResourceUntilFence(storage_match);
		return storage_match->image;
	}
	if (m_memory_tracker.IsRegionCpuModified(info.address, info.size)) {
		MarkSampledAliasesCpuDirtyLocked(info.address, info.size);
	}
	std::shared_ptr<CachedImage> match;
	for (auto& cached: m_images) {
		if (cached->kind != CachedImage::Kind::Texture || !Equal(info, cached->info)) {
			continue;
		}
		if (match != nullptr || cached->gpu_modified || cached->ctx != ctx) {
			EXIT("TextureCache: invalid exact sampled-image cache match, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " duplicate=%d gpu_modified=%d same_context=%d\n",
			     info.address, info.size, match != nullptr, cached->gpu_modified,
			     cached->ctx == ctx);
		}
		match = cached;
	}
	if (match != nullptr) {
		if (match->info.IsMaybeCpuDirty() && !match->info.IsDefinitelyCpuDirty()) {
			const bool changed =
			    match->info.ResolveMaybeCpuHash(HashSampledImageEdges(match->info));
			if (!changed) {
				m_memory_tracker.ForEachUploadRange(
				    info.address, info.size, false, [](uint64_t, uint64_t) noexcept {},
				    []() noexcept {});
			}
		}
		const bool buffer_dirty = match->buffer_modified;
		if (buffer_dirty) {
			if (match->gpu_modified ||
			    !IsCoherentGuestImageSource(source, info.address, info.size)) {
				EXIT("TextureCache: sampled-image refresh has invalid buffer ownership, "
				     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
				     " source=%p current=%d gpu_modified=%d\n",
				     info.address, info.size, static_cast<const void*>(source.buffer),
				     source.cpu_current, match->gpu_modified);
			}
			m_memory_tracker.MarkRegionAsCpuModified(info.address, info.size);
		}
		if (match->info.IsCpuDirty() || buffer_dirty) {
			m_memory_tracker.ForEachUploadRange(
			    info.address, info.size, false, [](uint64_t, uint64_t) noexcept {},
			    [&]() noexcept {
				    m_tiler.DetileImage(match->ctx,
				                        static_cast<GpuTextureVulkanImage*>(match->image),
				                        match->info, source, true, false);
			    });
			if (match->info.IsCpuDirty()) {
				match->info.RefreshComplete();
			}
			match->buffer_modified = false;
		}
		command->RetainResourceUntilFence(match);
		return match->image;
	}
	for (const auto& cached: m_images) {
		if (cached->kind != CachedImage::Kind::Texture) {
			if (!cached->OverlapsRange(info.address, info.size, true)) {
				continue;
			}
			EXIT("TextureCache: sampled image overlaps GPU target, requested=0x%016" PRIx64
			     "+0x%016" PRIx64 " target=0x%016" PRIx64 "+0x%016" PRIx64
			     " target_kind=%u requested_info={format=%u extent=%ux%ux%u pitch=%u levels=%u"
			     " tile=%u type=%u}\n",
			     info.address, info.size, cached->Address(), cached->Size(),
			     static_cast<uint32_t>(cached->kind), info.format, info.width, info.height,
			     info.depth, info.pitch, info.levels, info.tile, info.type);
		}
		const auto overlap =
		    ClassifySampledOverlap(info, cached->info, cached->gpu_modified, cached->ctx == ctx);
		if (overlap == SampledOverlap::Unsupported) {
			EXIT("TextureCache: unsupported sampled-texture alias, requested=0x%016" PRIx64
			     "+0x%016" PRIx64 " existing=0x%016" PRIx64 "+0x%016" PRIx64
			     " gpu_modified=%d same_context=%d\n",
			     info.address, info.size, cached->info.address, cached->info.size,
			     cached->gpu_modified, cached->ctx == ctx);
		}
	}
	m_images.reserve(m_images.size() + 1);
	auto cached  = std::make_shared<CachedImage>();
	cached->kind = CachedImage::Kind::Texture;
	cached->info = info;
	cached->ctx  = ctx;
	vk::ComponentMapping components {};
	cached->image = ImageOps::CreateTexture(ctx, info, false, &components);
	m_memory_tracker.ForEachUploadRange(
	    info.address, info.size, false, [](uint64_t, uint64_t) noexcept {},
	    [&]() noexcept {
		    m_tiler.DetileImage(cached->ctx, static_cast<GpuTextureVulkanImage*>(cached->image),
		                        cached->info, source, false, false);
	    });
	ImageOps::CreateTextureViews(ctx, static_cast<GpuTextureVulkanImage*>(cached->image), info,
	                             false, components);
	return PublishImage(command, std::move(cached));
}

StorageTextureVulkanImage* TextureCache::FindStorageTexture(CommandBuffer*   command,
                                                            GraphicContext*  ctx,
                                                            const ImageInfo& info) {
	const bool supported_type =
	    info.type == Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) ||
	    info.type == Prospero::GpuEnumValue(Prospero::ImageType::kColor2DArray) ||
	    info.type == Prospero::GpuEnumValue(Prospero::ImageType::kColor3D);
	const bool supported_depth_tile =
	    info.tile == Prospero::GpuEnumValue(Prospero::TileMode::kDepth) &&
	    IsSupportedStorageDepthTile(info.format, info.type, info.width, info.height, info.depth);
	if (info.address == 0 || info.size == 0 || info.address >= TRACKER_ADDRESS_SIZE ||
	    info.size > TRACKER_ADDRESS_SIZE - info.address || info.width == 0 || info.height == 0 ||
	    info.depth == 0 || info.levels == 0 || info.levels > 16 || info.base_level >= info.levels ||
	    info.view_levels != 1 || info.base_array != 0 || !supported_type ||
	    (info.tile != Prospero::GpuEnumValue(Prospero::TileMode::kLinear) &&
	     info.tile != Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) &&
	     !supported_depth_tile) ||
	    !IsSupportedStorageSwizzle(info.format, info.swizzle)) {
		EXIT("TextureCache: unsupported storage-image request, command=%p ctx=%p "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     " extent=%ux%ux%u levels=%u base_level=%u base_array=%u type=%u tile=%u "
		     "swizzle=0x%03x\n",
		     static_cast<const void*>(command), static_cast<const void*>(ctx), info.address,
		     info.size, info.width, info.height, info.depth, info.levels, info.base_level,
		     info.base_array, info.type, info.tile, info.swizzle);
	}
	if ((info.type == Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) && info.depth != 1) ||
	    (info.type == Prospero::GpuEnumValue(Prospero::ImageType::kColor3D) && info.depth == 0) ||
	    (info.type == Prospero::GpuEnumValue(Prospero::ImageType::kColor2DArray) &&
	     info.base_array >= info.depth)) {
		EXIT("TextureCache: storage-image type and depth disagree, type=%u depth=%u\n", info.type,
		     info.depth);
	}

	std::lock_guard       transaction(m_resource_mutex);
	const bool            buffer_overlap = m_buffer_cache.HasPageOverlap(info.address, info.size);
	BufferImageCopySource source {nullptr, 0, info.address, info.size, true};
	if (buffer_overlap) {
		source = m_buffer_cache.ObtainBufferForImage(info.address, info.size);
		if (!IsCoherentGuestImageSource(source, info.address, info.size)) {
			EXIT("TextureCache: storage-image buffer source is inconsistent, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " source=%p source_addr=0x%016" PRIx64
			     " source_size=0x%016" PRIx64 " current=%d\n",
			     info.address, info.size, static_cast<const void*>(source.buffer), source.address,
			     source.size, source.cpu_current);
		}
	}
	FaultSafeTextureLock lock(this, m_lock);
	RequireNoMetaOverlapLocked(info.address, info.size);
	if (supported_depth_tile &&
	    info.format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32UInt)) {
		RetireStorageDepthAliasLocked(ctx, info);
	}

	std::shared_ptr<CachedImage> match;
	for (auto& cached: m_images) {
		if (cached->kind != CachedImage::Kind::StorageTexture ||
		    !EqualStorageBacking(info, cached->info)) {
			continue;
		}
		if (match != nullptr || cached->ctx != ctx || cached->info.IsCpuDirty()) {
			EXIT("TextureCache: invalid exact storage-image cache match, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " duplicate=%d same_context=%d cpu_dirty=%d\n",
			     info.address, info.size, match != nullptr, cached->ctx == ctx,
			     cached->info.IsCpuDirty());
		}
		match = cached;
	}
	if (match != nullptr) {
		const bool cpu_modified = m_memory_tracker.IsRegionCpuModified(info.address, info.size);
		const bool gpu_modified = m_memory_tracker.IsRegionGpuModified(info.address, info.size);
		const auto rebind       = ClassifyStorageBufferRebind(
		    buffer_overlap, match->gpu_modified, match->buffer_modified, gpu_modified, cpu_modified,
		    IsCoherentGuestImageSource(source, info.address, info.size));
		if (rebind == StorageBufferRebind::Unsupported) {
			EXIT("TextureCache: storage-image ownership is inconsistent, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64
			     " cached_gpu_modified=%d buffer_modified=%d tracker_gpu_modified=%d "
			     "cpu_modified=%d buffer_overlap=%d source_current=%d\n",
			     info.address, info.size, match->gpu_modified, match->buffer_modified, gpu_modified,
			     cpu_modified, buffer_overlap, source.cpu_current);
		}
		if (rebind == StorageBufferRebind::RefreshFromBacking) {
			// The formatted buffer may cover only one page of a packed mip chain. The
			// synchronization path published the complete tiled backing before that write,
			// and ObtainBufferForImage has now folded its dirty subrange back into it.
			m_memory_tracker.MarkRegionAsCpuModified(info.address, info.size);
			m_memory_tracker.ForEachUploadRange(
			    info.address, info.size, true, [](uint64_t, uint64_t) noexcept {},
			    [&]() noexcept {
				    m_tiler.DetileImage(match->ctx,
				                        static_cast<GpuTextureVulkanImage*>(match->image),
				                        match->info, source, true, true);
			    });
			match->buffer_modified = false;
			match->gpu_modified    = true;
		} else if (!gpu_modified) {
			if (cpu_modified) {
				m_memory_tracker.ForEachUploadRange(
				    info.address, info.size, true, [](uint64_t, uint64_t) noexcept {},
				    [&]() noexcept {
					    m_tiler.DetileImage(
					        match->ctx, static_cast<GpuTextureVulkanImage*>(match->image),
					        match->info, {nullptr, 0, match->info.address, match->info.size, true},
					        true, true);
				    });
			} else {
				// A readback leaves both copies current. Reclaim ownership without an unnecessary
				// upload through the clean UpdateImage storage-binding path.
				m_memory_tracker.MarkRegionAsGpuModified(info.address, info.size);
			}
			match->gpu_modified = true;
		}
		command->RetainResourceUntilFence(match);
		return static_cast<StorageTextureVulkanImage*>(match->image);
	}

	ResolveStorageImageOverlaps(ctx, info);
	m_images.reserve(m_images.size() + 1);
	auto cached  = std::make_shared<CachedImage>();
	cached->kind = CachedImage::Kind::StorageTexture;
	cached->info = info;
	cached->ctx  = ctx;
	vk::ComponentMapping components {};
	cached->image = ImageOps::CreateTexture(ctx, info, true, &components);
	m_memory_tracker.ForEachUploadRange(
	    info.address, info.size, true, [](uint64_t, uint64_t) noexcept {},
	    [&]() noexcept {
		    m_tiler.DetileImage(cached->ctx, static_cast<GpuTextureVulkanImage*>(cached->image),
		                        cached->info, source, false, true);
	    });
	cached->gpu_modified = true;
	ImageOps::CreateTextureViews(ctx, static_cast<GpuTextureVulkanImage*>(cached->image), info,
	                             true, components);
	return static_cast<StorageTextureVulkanImage*>(PublishImage(command, std::move(cached)));
}

RenderTextureVulkanImage* TextureCache::FindRenderTarget(CommandBuffer*          command,
                                                         GraphicContext*         ctx,
                                                         const RenderTargetInfo& info) {
	const bool standard64 = IsSupportedStandard64RenderTarget(info);
	if (info.address == 0 || info.size == 0 || info.address >= TRACKER_ADDRESS_SIZE ||
	    info.size > TRACKER_ADDRESS_SIZE - info.address || info.format == vk::Format::eUndefined ||
	    info.width == 0 || info.height == 0 || info.pitch < info.width ||
	    info.bytes_per_element == 0 || info.levels == 0 || info.levels > 16 || info.layers == 0 ||
	    info.size % info.layers != 0 ||
	    (info.tile_mode != Prospero::GpuEnumValue(Prospero::TileMode::kLinear) &&
	     info.tile_mode != Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) &&
	     !standard64)) {
		EXIT("TextureCache: invalid render-target request, ctx=%p addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 " extent=%ux%u pitch=%u bpe=%u tile=%u format=%d\n",
		     static_cast<const void*>(ctx), info.address, info.size, info.width, info.height,
		     info.pitch, info.bytes_per_element, info.tile_mode, static_cast<int>(info.format));
	}
	if (info.levels > 1) {
		TileSizeAlign layout {};
		if (info.tile_mode != Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) ||
		    !TileGetRenderTargetMipLayout(info.width, info.height, info.pitch,
		                                  info.bytes_per_element, info.levels, &layout, nullptr,
		                                  nullptr) ||
		    layout.size > UINT64_MAX / info.layers ||
		    static_cast<uint64_t>(layout.size) * info.layers != info.size) {
			EXIT("TextureCache: unsupported render-target mip layout, size=0x%016" PRIx64
			     " expected=0x%08x extent=%ux%u pitch=%u bpe=%u levels=%u tile=%u\n",
			     info.size, layout.size, info.width, info.height, info.pitch,
			     info.bytes_per_element, info.levels, info.tile_mode);
		}
	}
	if (standard64) {
		const auto format = ImageOps::RenderTargetTransferFormat(info.bytes_per_element);
		const auto expected_pitch =
		    TileGetTexturePitch(format, info.width, info.levels, info.tile_mode);
		TileSizeAlign layout {};
		TileGetTextureSize(format, info.width, info.height, expected_pitch, info.levels,
		                   info.tile_mode, &layout, nullptr, nullptr);
		if (expected_pitch != info.pitch || layout.align != 65536 || layout.size != info.size) {
			EXIT("TextureCache: invalid Standard64KB render-target layout,"
			     " size=0x%016" PRIx64 " expected=0x%08x align=0x%08x pitch=%u/%u\n",
			     info.size, layout.size, layout.align, info.pitch, expected_pitch);
		}
	}
	const auto rows = static_cast<uint64_t>(info.height - 1);
	if (rows > (UINT64_MAX - info.width) / info.pitch) {
		EXIT("TextureCache: render-target element count overflow, extent=%ux%u pitch=%u\n",
		     info.width, info.height, info.pitch);
	}
	const auto elements = rows * info.pitch + info.width;
	if (elements > UINT64_MAX / info.bytes_per_element ||
	    elements * info.bytes_per_element > UINT64_MAX / info.layers ||
	    info.size < elements * info.bytes_per_element * info.layers) {
		EXIT("TextureCache: invalid render-target storage, size=0x%016" PRIx64
		     " elements=0x%016" PRIx64 " bpe=%u\n",
		     info.size, elements, info.bytes_per_element);
	}
	std::lock_guard       transaction(m_resource_mutex);
	BufferImageCopySource target_source {nullptr, 0, info.address, info.size, true};
	const bool target_buffer_overlap = m_buffer_cache.HasPageOverlap(info.address, info.size);
	if (target_buffer_overlap) {
		// A render target may use a containing native buffer after dirty bytes are published or
		// coherent guest backing for a clean partial view. ObtainBufferForImage keeps GPU-dirty
		// partial ownership and inconsistent state as hard failures.
		target_source = m_buffer_cache.ObtainBufferForImage(info.address, info.size);
	}
	FaultSafeTextureLock lock(this, m_lock);
	RequireNoMetaOverlapLocked(info.address, info.size);
	std::shared_ptr<CachedImage> match;
	for (auto& cached: m_images) {
		if (cached->kind != CachedImage::Kind::RenderTarget || cached->ctx != ctx ||
		    (!Equal(info, cached->target) && !IsCompatibleRenderTargetView(cached->target, info) &&
		     !IsCompatibleRenderTargetBacking(cached->target, info))) {
			continue;
		}
		match = cached;
	}
	if (match != nullptr) {
		if (match->buffer_modified) {
			if (match->gpu_modified ||
			    !IsCoherentGuestImageSource(target_source, info.address, info.size)) {
				EXIT("TextureCache: render-target refresh has invalid buffer ownership, "
				     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
				     " source=%p current=%d gpu_modified=%d\n",
				     info.address, info.size, static_cast<const void*>(target_source.buffer),
				     target_source.cpu_current, match->gpu_modified);
			}
			m_memory_tracker.MarkRegionAsCpuModified(info.address, info.size);
		}
		if (!match->gpu_modified && m_memory_tracker.IsRegionCpuModified(info.address, info.size)) {
			m_memory_tracker.ForEachUploadRange(
			    info.address, info.size, false, [](uint64_t, uint64_t) noexcept {},
			    [&]() noexcept {
				    ImageOps::UploadRenderTarget(
				        ctx, static_cast<RenderTextureVulkanImage*>(match->image), info, true);
			    });
			match->buffer_modified = false;
		}
		command->RetainResourceUntilFence(match);
		return static_cast<RenderTextureVulkanImage*>(match->image);
	}
	std::vector<CachedImage*>    retire;
	std::vector<CachedImage*>    buffer_owned_depth_retire;
	std::shared_ptr<CachedImage> native_image_source;
	for (const auto& entry: m_images) {
		auto& cached = *entry;
		if (!cached.OverlapsRange(info.address, info.size, true)) {
			continue;
		}
		RenderTargetOverlap overlap = RenderTargetOverlap::Unsupported;
		switch (cached.kind) {
			case CachedImage::Kind::Texture:
				overlap = ClassifyRenderTargetOverlap(cached.info, cached.gpu_modified,
				                                      cached.ctx == ctx, info);
				break;
			case CachedImage::Kind::StorageTexture:
				overlap = ClassifyStorageRenderTargetOverlap(
				    cached.info, cached.image->format, cached.gpu_modified, cached.buffer_modified,
				    cached.info.IsCpuDirty() ||
				        m_memory_tracker.IsRegionCpuModified(cached.info.address, cached.info.size),
				    cached.ctx == ctx, info);
				break;
			case CachedImage::Kind::RenderTarget:
				overlap =
				    ClassifyRenderTargetOverlap(cached.target, cached.gpu_modified,
				                                cached.buffer_modified, cached.ctx == ctx, info);
				break;
			case CachedImage::Kind::DepthTarget:
				if (CanRetireBufferOwnedDepthForRenderTarget(
				        cached.depth, cached.gpu_modified, cached.buffer_modified,
				        cached.ctx == ctx, target_source.buffer != nullptr, info)) {
					overlap = RenderTargetOverlap::RetireTarget;
				}
				break;
			case CachedImage::Kind::VideoOut: break;
		}
		if (overlap == RenderTargetOverlap::None) {
			continue;
		}
		bool supported = false;
		switch (overlap) {
			case RenderTargetOverlap::RetireSampled:
				supported = cached.kind == CachedImage::Kind::Texture;
				break;
			case RenderTargetOverlap::PreserveStorage:
				supported = cached.kind == CachedImage::Kind::StorageTexture &&
				            native_image_source == nullptr;
				if (supported) {
					native_image_source = entry;
				}
				break;
			case RenderTargetOverlap::ExpandTarget:
				supported = cached.kind == CachedImage::Kind::RenderTarget &&
				            native_image_source == nullptr;
				if (supported) {
					native_image_source = entry;
				}
				break;
			case RenderTargetOverlap::RetireTarget:
				supported =
				    cached.kind == CachedImage::Kind::RenderTarget ||
				    (cached.kind == CachedImage::Kind::DepthTarget && target_buffer_overlap &&
				     IsCoherentGuestImageSource(target_source, info.address, info.size));
				if (supported && cached.kind == CachedImage::Kind::DepthTarget) {
					buffer_owned_depth_retire.push_back(&cached);
				}
				break;
			case RenderTargetOverlap::None:
			case RenderTargetOverlap::Unsupported: break;
		}
		if (!supported) {
			EXIT("TextureCache: unsupported render-target alias, requested=0x%016" PRIx64
			     "+0x%016" PRIx64 " existing_kind=%u existing=0x%016" PRIx64 "+0x%016" PRIx64
			     " gpu_modified=%d same_context=%d"
			     " sampled_format=%u extent=%ux%ux%u pitch=%u levels=%u base_level=%u"
			     " tile=%u type=%u base_array=%u\n",
			     info.address, info.size, static_cast<uint32_t>(cached.kind), cached.Address(),
			     cached.Size(), cached.gpu_modified, cached.ctx == ctx, cached.info.format,
			     cached.info.width, cached.info.height, cached.info.depth, cached.info.pitch,
			     cached.info.levels, cached.info.base_level, cached.info.tile, cached.info.type,
			     cached.info.base_array);
		}
		retire.push_back(&cached);
	}
	RequireRetirementIsolation(retire, "render target", info.address, info.size);
	for (auto* cached: buffer_owned_depth_retire) {
		// The exact formatted buffer owns the current bytes. Clear the stale-image marker only
		// after coherence validation so normal retirement can remove the obsolete Vulkan shape.
		cached->buffer_modified = false;
	}
	RetireImages(retire, native_image_source.get());
	auto cached                = std::make_shared<CachedImage>();
	cached->kind               = CachedImage::Kind::RenderTarget;
	cached->target             = info;
	cached->ctx                = ctx;
	cached->image              = ImageOps::CreateRenderTarget(ctx, info);
	const bool preserve_native = native_image_source != nullptr;
	if (preserve_native) {
		command->RetainResourceUntilFence(native_image_source);
		if (native_image_source->kind == CachedImage::Kind::RenderTarget) {
			const auto& old          = native_image_source->target;
			const auto  tail_address = info.address + old.size;
			const auto  tail_size    = info.size - old.size;
			if (old.layers >= info.layers || tail_size == 0 ||
			    !IsCoherentGuestImageSource(target_source, info.address, info.size)) {
				EXIT("TextureCache: invalid render-target expansion source, old_layers=%u "
				     "new_layers=%u old_size=0x%016" PRIx64 " new_size=0x%016" PRIx64 "\n",
				     old.layers, info.layers, old.size, info.size);
			}
			m_memory_tracker.ForEachUploadRange(
			    tail_address, tail_size, true, [](uint64_t, uint64_t) noexcept {},
			    [&]() noexcept {
				    ImageOps::UploadRenderTargetLayers(
				        ctx, static_cast<RenderTextureVulkanImage*>(cached->image), info,
				        old.layers, info.layers - old.layers, false);
			    });
		}
		std::vector<ImageImageCopy> regions;
		regions.reserve(static_cast<size_t>(native_image_source->image->layers) *
		                native_image_source->image->mip_levels);
		AppendLayerCopies(regions, native_image_source->image, vk::ImageAspectFlagBits::eColor);
		Transfer::CopyImage(command, regions, cached->image, RENDER_COLOR_IMAGE_LAYOUT);
	} else {
		m_memory_tracker.ForEachUploadRange(
		    info.address, info.size, false, [](uint64_t, uint64_t) noexcept {},
		    [&]() noexcept {
			    ImageOps::UploadRenderTarget(
			        ctx, static_cast<RenderTextureVulkanImage*>(cached->image), info, false);
		    });
	}
	cached->gpu_modified = preserve_native;
	return static_cast<RenderTextureVulkanImage*>(PublishImage(command, std::move(cached)));
}

DepthStencilVulkanImage* TextureCache::FindDepthTarget(CommandBuffer* command, GraphicContext* ctx,
                                                       const DepthTargetInfo& info) {
	const bool has_stencil = info.stencil_address != 0 || info.stencil_size != 0;
	const bool has_htile   = info.htile_address != 0 || info.htile_size != 0;
	if (info.address == 0 || info.size == 0 || info.address >= TRACKER_ADDRESS_SIZE ||
	    info.size > TRACKER_ADDRESS_SIZE - info.address || (info.address & 0xffffu) != 0 ||
	    info.width == 0 || info.height == 0 || info.pitch < info.width || info.layers == 0 ||
	    info.size % info.layers != 0 || info.size > UINT32_MAX || info.stencil_size > UINT32_MAX ||
	    info.tile_mode != Prospero::GpuEnumValue(Prospero::TileMode::kDepth) ||
	    (has_stencil &&
	     (info.stencil_address == 0 || info.stencil_size == 0 ||
	      info.stencil_size % info.layers != 0 || info.stencil_address >= TRACKER_ADDRESS_SIZE ||
	      info.stencil_size > TRACKER_ADDRESS_SIZE - info.stencil_address ||
	      (info.stencil_address & 0xffffu) != 0 ||
	      ImagePageRangesOverlap(info.address, info.size, info.stencil_address,
	                             info.stencil_size))) ||
	    ((info.htile_address == 0) != (info.htile_size == 0)) ||
	    (has_htile &&
	     (info.htile_size % info.layers != 0 || info.htile_address >= TRACKER_ADDRESS_SIZE ||
	      info.htile_size > TRACKER_ADDRESS_SIZE - info.htile_address ||
	      (info.htile_address & 0x7fffu) != 0 ||
	      ImagePageRangesOverlap(info.address, info.size, info.htile_address, info.htile_size) ||
	      (has_stencil && ImagePageRangesOverlap(info.stencil_address, info.stencil_size,
	                                             info.htile_address, info.htile_size)))) ||
	    (info.stencil_htile_compressed && (!has_stencil || !has_htile)) ||
	    !IsSupportedDepthTargetFormat(info)) {
		EXIT("TextureCache: unsupported depth target, command=%p ctx=%p depth=0x%016" PRIx64
		     "+0x%016" PRIx64 " stencil=0x%016" PRIx64 "+0x%016" PRIx64 " htile=0x%016" PRIx64
		     "+0x%016" PRIx64 " extent=%ux%u pitch=%u tile=%u format=%d guest_format=%u bpe=%u\n",
		     static_cast<const void*>(command), static_cast<const void*>(ctx), info.address,
		     info.size, info.stencil_address, info.stencil_size, info.htile_address,
		     info.htile_size, info.width, info.height, info.pitch, info.tile_mode,
		     static_cast<int>(info.format), info.guest_format, info.bytes_per_element);
	}
	const auto rows = static_cast<uint64_t>(info.height - 1);
	if (rows > (UINT64_MAX - info.width) / info.pitch) {
		EXIT("TextureCache: depth-target element count overflow, extent=%ux%u pitch=%u\n",
		     info.width, info.height, info.pitch);
	}
	const auto elements = rows * info.pitch + info.width;
	if (elements > UINT64_MAX / info.bytes_per_element ||
	    elements * info.bytes_per_element > UINT64_MAX / info.layers ||
	    info.size < elements * info.bytes_per_element * info.layers) {
		EXIT("TextureCache: depth storage is too small, size=0x%016" PRIx64
		     " elements=0x%016" PRIx64 " bpe=%u\n",
		     info.size, elements, info.bytes_per_element);
	}
	if (has_stencil &&
	    (elements > UINT64_MAX / info.layers || info.stencil_size < elements * info.layers)) {
		EXIT("TextureCache: stencil storage is too small, size=0x%016" PRIx64
		     " elements=0x%016" PRIx64 "\n",
		     info.stencil_size, elements);
	}
	if (has_stencil && !info.stencil_load_clear && !CanLoadRawStencilPlane(info)) {
		EXIT("TextureCache: HTile-compressed stencil load is unsupported, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 " htile=0x%016" PRIx64 "+0x%016" PRIx64 "\n",
		     info.stencil_address, info.stencil_size, info.htile_address, info.htile_size);
	}
	if (has_htile) {
		RegisterMeta(info.htile_address, info.htile_size, info.layers);
	}
	std::lock_guard transaction(m_resource_mutex);
	// BufferCache treats an untracked guest range as CPU-current. That is useful when
	// uploading from guest memory, but it is not evidence that a clean native image is
	// stale. Only a genuinely overlapping buffer participates in transition selection.
	const bool depth_buffer_overlap = m_buffer_cache.HasPageOverlap(info.address, info.size);
	const auto depth_source         = m_buffer_cache.ObtainBufferForImage(info.address, info.size);
	const auto stencil_source =
	    has_stencil ? m_buffer_cache.ObtainBufferForImage(info.stencil_address, info.stencil_size)
	                : BufferImageCopySource {};
	FaultSafeTextureLock lock(this, m_lock);
	RequireNoMetaOverlapLocked(info.address, info.size);
	if (has_stencil) {
		RequireNoMetaOverlapLocked(info.stencil_address, info.stencil_size);
	}
	std::shared_ptr<CachedImage> match;
	for (auto& cached: m_images) {
		if (cached->kind != CachedImage::Kind::DepthTarget || cached->ctx != ctx ||
		    (!Equal(info, cached->depth) && !IsCompatibleDepthTargetBacking(cached->depth, info))) {
			continue;
		}
		match = cached;
	}
	if (match != nullptr) {
		if (!CanLoadStencilAttachment(info, match->stencil_initialized)) {
			EXIT("TextureCache: stencil load requires initialized contents, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     info.stencil_address, info.stencil_size);
		}
		if (has_stencil && info.stencil_load_clear) {
			match->stencil_initialized = true;
		}
		if (match->buffer_modified) {
			if (match->gpu_modified || !depth_buffer_overlap ||
			    !IsCoherentGuestImageSource(depth_source, info.address, info.size)) {
				EXIT("TextureCache: depth-target refresh has invalid buffer ownership, "
				     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
				     " gpu_modified=%d overlap=%d source_current=%d\n",
				     info.address, info.size, match->gpu_modified, depth_buffer_overlap,
				     depth_source.cpu_current);
			}
			m_memory_tracker.MarkRegionAsCpuModified(info.address, info.size);
			m_memory_tracker.ForEachUploadRange(
			    info.address, info.size, true, [](uint64_t, uint64_t) noexcept {},
			    [&]() noexcept {
				    m_tiler.DetileImage(ctx, static_cast<DepthStencilVulkanImage*>(match->image),
				                        info, depth_source, true);
			    });
			match->buffer_modified = false;
			match->gpu_modified    = true;
		} else if (!match->gpu_modified && depth_source.cpu_dirty) {
			m_memory_tracker.ForEachUploadRange(
			    info.address, info.size, false, [](uint64_t, uint64_t) noexcept {},
			    [&]() noexcept {
				    m_tiler.DetileImage(ctx, static_cast<DepthStencilVulkanImage*>(match->image),
				                        info, depth_source, true);
			    });
		}
		if (!match->gpu_modified && has_stencil && stencil_source.cpu_dirty) {
			m_memory_tracker.ForEachUploadRange(
			    info.stencil_address, info.stencil_size, false, [](uint64_t, uint64_t) noexcept {},
			    [&]() noexcept {
				    if (!info.stencil_load_clear) {
					    m_tiler.DetileStencil(ctx,
					                          static_cast<DepthStencilVulkanImage*>(match->image),
					                          info, stencil_source, true);
					    match->stencil_initialized = true;
				    }
			    });
		}
		command->RetainResourceUntilFence(match);
		return static_cast<DepthStencilVulkanImage*>(match->image);
	}
	std::vector<CachedImage*>    retire;
	std::shared_ptr<CachedImage> sampled_depth_source;
	std::shared_ptr<CachedImage> native_depth_source;
	std::shared_ptr<CachedImage> discarded_depth_source;
	std::shared_ptr<CachedImage> retired_storage_source;
	for (const auto& entry: m_images) {
		auto&      cached = *entry;
		const bool overlaps =
		    cached.OverlapsRange(info.address, info.size, true) ||
		    (has_stencil && cached.OverlapsRange(info.stencil_address, info.stencil_size, true));
		if (!overlaps) {
			continue;
		}
		DepthOverlap overlap = DepthOverlap::Unsupported;
		switch (cached.kind) {
			case CachedImage::Kind::Texture:
				overlap = cached.ctx == ctx
				              ? ClassifyDepthOverlap(cached.info, cached.gpu_modified, info)
				              : DepthOverlap::Unsupported;
				break;
			case CachedImage::Kind::DepthTarget:
				overlap =
				    ClassifyDepthTargetOverlap(cached.depth, cached.gpu_modified,
				                               cached.buffer_modified, cached.ctx == ctx, info);
				break;
			case CachedImage::Kind::StorageTexture:
				overlap = cached.ctx == ctx ? ClassifyStorageDepthOverlap(cached.info, info)
				                            : DepthOverlap::Unsupported;
				break;
			case CachedImage::Kind::RenderTarget:
			case CachedImage::Kind::VideoOut: break;
		}
		bool supported = false;
		switch (overlap) {
			case DepthOverlap::RetireSampled:
				supported = cached.kind == CachedImage::Kind::Texture;
				if (supported && !info.depth_load_clear && sampled_depth_source == nullptr) {
					sampled_depth_source = entry;
				}
				break;
			case DepthOverlap::RetireStorage:
				supported = cached.kind == CachedImage::Kind::StorageTexture &&
				            cached.gpu_modified && !cached.buffer_modified &&
				            !cached.info.IsCpuDirty() && retired_storage_source == nullptr &&
				            native_depth_source == nullptr && discarded_depth_source == nullptr;
				if (supported) {
					retired_storage_source = entry;
				}
				break;
			case DepthOverlap::ExpandTarget:
				supported = cached.kind == CachedImage::Kind::DepthTarget &&
				            native_depth_source == nullptr && retired_storage_source == nullptr;
				if (supported) {
					native_depth_source = entry;
				}
				break;
			case DepthOverlap::DiscardTarget:
				supported = cached.kind == CachedImage::Kind::DepthTarget &&
				            discarded_depth_source == nullptr && native_depth_source == nullptr;
				supported = supported && retired_storage_source == nullptr;
				if (supported) {
					discarded_depth_source = entry;
				}
				break;
			case DepthOverlap::None:
			case DepthOverlap::Unsupported: break;
		}
		if (!supported) {
			EXIT("TextureCache: unsupported depth-target alias, depth=0x%016" PRIx64
			     "+0x%016" PRIx64 " existing_kind=%u existing=0x%016" PRIx64 "+0x%016" PRIx64 "\n",
			     info.address, info.size, static_cast<uint32_t>(cached.kind), cached.Address(),
			     cached.Size());
		}
		retire.push_back(&cached);
	}
	RequireRetirementIsolation(retire, "depth target", info.address, info.size);
	if (retired_storage_source != nullptr) {
		m_memory_tracker.UnmarkRegionAsGpuModified(retired_storage_source->info.address,
		                                           retired_storage_source->info.size);
		retired_storage_source->gpu_modified = false;
	}
	const auto* transition_source =
	    native_depth_source != nullptr
	        ? native_depth_source.get()
	        : (discarded_depth_source != nullptr ? discarded_depth_source.get() : nullptr);
	RetireImages(retire, transition_source);
	const bool coherent_guest_stencil =
	    has_stencil &&
	    IsCoherentGuestImageSource(stencil_source, info.stencil_address, info.stencil_size);
	// Native expansion must preserve old layers. Guest data initializes only a wholly new image.
	const bool stencil_contents_available = native_depth_source != nullptr
	                                            ? native_depth_source->stencil_initialized
	                                            : coherent_guest_stencil;
	if (!CanLoadStencilAttachment(info, stencil_contents_available)) {
		EXIT("TextureCache: new stencil target requires a clear before stencil access, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     info.stencil_address, info.stencil_size);
	}
	auto cached                 = std::make_shared<CachedImage>();
	cached->kind                = CachedImage::Kind::DepthTarget;
	cached->depth               = info;
	cached->ctx                 = ctx;
	cached->stencil_initialized = !has_stencil;
	cached->image               = ImageOps::CreateDepthTarget(ctx, info);
	if (discarded_depth_source != nullptr) {
		command->RetainResourceUntilFence(discarded_depth_source);
	}
	if (retired_storage_source != nullptr) {
		command->RetainResourceUntilFence(retired_storage_source);
	}
	if (native_depth_source != nullptr) {
		const auto& old = native_depth_source->depth;
		if (old.layers >= info.layers || native_depth_source->image->layers != old.layers) {
			EXIT("TextureCache: invalid depth expansion source, old_layers=%u new_layers=%u\n",
			     old.layers, info.layers);
		}
		command->RetainResourceUntilFence(native_depth_source);
		const auto tail       = SelectDepthLayers(info, old.layers, info.layers - old.layers);
		const auto depth_tail = SelectSourceRange(depth_source, tail.address, tail.size);
		if (!info.depth_load_clear &&
		    !IsCoherentGuestImageSource(depth_tail, tail.address, tail.size)) {
			EXIT("TextureCache: depth expansion tail is not CPU-current\n");
		}
		m_memory_tracker.ForEachUploadRange(
		    tail.address, tail.size, true, [](uint64_t, uint64_t) noexcept {},
		    [&]() noexcept {
			    if (!info.depth_load_clear) {
				    m_tiler.DetileImage(ctx, static_cast<DepthStencilVulkanImage*>(cached->image),
				                        tail, depth_tail, false, old.layers);
			    }
		    });
		if (has_stencil) {
			const auto stencil_tail =
			    SelectSourceRange(stencil_source, tail.stencil_address, tail.stencil_size);
			if (!info.stencil_load_clear &&
			    !IsCoherentGuestImageSource(stencil_tail, tail.stencil_address,
			                                tail.stencil_size)) {
				EXIT("TextureCache: stencil expansion tail is not CPU-current\n");
			}
			m_memory_tracker.ForEachUploadRange(
			    tail.stencil_address, tail.stencil_size, true, [](uint64_t, uint64_t) noexcept {},
			    [&]() noexcept {
				    if (!info.stencil_load_clear) {
					    m_tiler.DetileStencil(ctx,
					                          static_cast<DepthStencilVulkanImage*>(cached->image),
					                          tail, stencil_tail, false, old.layers);
				    }
			    });
			cached->stencil_initialized = true;
		}
		std::vector<ImageImageCopy> regions;
		regions.reserve(native_depth_source->image->layers * (has_stencil ? 2u : 1u));
		AppendLayerCopies(regions, native_depth_source->image, vk::ImageAspectFlagBits::eDepth);
		if (has_stencil) {
			AppendLayerCopies(regions, native_depth_source->image,
			                  vk::ImageAspectFlagBits::eStencil);
		}
		Transfer::CopyImage(command, regions, cached->image,
		                    vk::ImageLayout::eDepthStencilAttachmentOptimal);
		cached->gpu_modified = true;
	} else {
		if (sampled_depth_source != nullptr &&
		    (sampled_depth_source->image->type != VulkanImageType::Texture ||
		     sampled_depth_source->image->format != VulkanFormat(info.guest_format) ||
		     sampled_depth_source->image->extent.width != info.width ||
		     sampled_depth_source->image->extent.height != info.height)) {
			EXIT("TextureCache: sampled-depth native source is inconsistent\n");
		}
		const auto transition_source = SelectDepthTransitionSource(
		    info.depth_load_clear, sampled_depth_source != nullptr,
		    sampled_depth_source != nullptr && sampled_depth_source->info.IsCpuDirty(),
		    sampled_depth_source != nullptr && sampled_depth_source->buffer_modified,
		    depth_buffer_overlap, depth_source.cpu_dirty);
		m_memory_tracker.ForEachUploadRange(
		    info.address, info.size, false, [](uint64_t, uint64_t) noexcept {},
		    [&]() noexcept {
			    switch (transition_source) {
				    case DepthTransitionSource::None: break;
				    case DepthTransitionSource::Guest:
					    m_tiler.DetileImage(ctx,
					                        static_cast<DepthStencilVulkanImage*>(cached->image),
					                        info, depth_source, false);
					    break;
				    case DepthTransitionSource::Native:
					    command->RetainResourceUntilFence(sampled_depth_source);
					    Transfer::CopyImageViaBuffer(
					        command, ctx, sampled_depth_source->image,
					        vk::ImageAspectFlagBits::eColor, cached->image,
					        vk::ImageAspectFlagBits::eDepth, info.bytes_per_element,
					        vk::ImageLayout::eDepthStencilAttachmentOptimal);
					    break;
			    }
		    });
		if (has_stencil) {
			m_memory_tracker.ForEachUploadRange(
			    info.stencil_address, info.stencil_size, false, [](uint64_t, uint64_t) noexcept {},
			    [&]() noexcept {
				    if (!info.stencil_load_clear) {
					    m_tiler.DetileStencil(ctx,
					                          static_cast<DepthStencilVulkanImage*>(cached->image),
					                          info, stencil_source, false);
				    }
			    });
			cached->stencil_initialized = true;
		}
	}
	return static_cast<DepthStencilVulkanImage*>(PublishImage(command, std::move(cached)));
}

std::vector<VideoOutVulkanImage*>
TextureCache::RegisterVideoOutSurfaces(GraphicContext*                  ctx,
                                       const std::vector<VideoOutInfo>& infos) {
	if (infos.empty()) {
		EXIT("TextureCache: video-out registration requires surfaces\n");
	}
	for (const auto& info: infos) {
		ImageOps::ValidateVideoOut(ctx, info);
	}
	for (size_t i = 0; i < infos.size(); i++) {
		for (size_t j = i + 1; j < infos.size(); j++) {
			if (ImagePageRangesOverlap(infos[i].address, infos[i].size, infos[j].address,
			                           infos[j].size)) {
				EXIT("TextureCache: video-out surfaces share pages, first=%zu second=%zu\n", i, j);
			}
		}
	}
	std::lock_guard transaction(m_resource_mutex);
	for (const auto& info: infos) {
		if (m_buffer_cache.HasPageOverlap(info.address, info.size)) {
			EXIT("TextureCache: video-out surface aliases buffer pages, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     info.address, info.size);
		}
	}
	FaultSafeTextureLock lock(this, m_lock);
	for (const auto& info: infos) {
		RequireNoMetaOverlapLocked(info.address, info.size);
		for (const auto& cached: m_images) {
			if (cached->OverlapsRange(info.address, info.size, true)) {
				EXIT(
				    "TextureCache: video-out surface aliases cached image pages, addr=0x%016" PRIx64
				    " size=0x%016" PRIx64 " existing_kind=%u\n",
				    info.address, info.size, static_cast<uint32_t>(cached->kind));
			}
		}
	}
	m_images.reserve(m_images.size() + infos.size());
	std::vector<VideoOutVulkanImage*> result;
	result.reserve(infos.size());
	for (const auto& info: infos) {
		auto cached       = std::make_shared<CachedImage>();
		cached->kind      = CachedImage::Kind::VideoOut;
		cached->video_out = info;
		cached->ctx       = ctx;
		cached->image     = ImageOps::CreateVideoOut(ctx, info);
		if (info.compression == VideoOutCompression::Uncompressed) {
			m_memory_tracker.ForEachUploadRange(
			    info.address, info.size, false, [](uint64_t, uint64_t) noexcept {},
			    [&]() noexcept {
				    ImageOps::UploadVideoOut(ctx, static_cast<VideoOutVulkanImage*>(cached->image),
				                             info, false);
			    });
		} else {
			// A compressed guest surface cannot be decoded without its DCC metadata. Establish the
			// normal tracked range, but leave the shared native image to be initialized by a GPU
			// render/clear before any sampled or presentation read.
			m_memory_tracker.ForEachUploadRange(
			    info.address, info.size, false, [](uint64_t, uint64_t) noexcept {},
			    []() noexcept {});
		}
		result.push_back(static_cast<VideoOutVulkanImage*>(cached->image));
		m_images.push_back(std::move(cached));
		RegisterImageLocked(*m_images.back());
	}
	return result;
}

void TextureCache::RefreshVideoOut(VideoOutVulkanImage* image, bool render_target) {
	if (image == nullptr) {
		EXIT("TextureCache: invalid video-out refresh, image=%p\n",
		     static_cast<const void*>(image));
	}
	std::lock_guard      transaction(m_resource_mutex);
	FaultSafeTextureLock lock(this, m_lock);
	const auto it = std::find_if(m_images.begin(), m_images.end(),
	                             [image](const auto& cached) { return cached->image == image; });
	if (it == m_images.end() || (*it)->kind != CachedImage::Kind::VideoOut) {
		EXIT("TextureCache: video-out image is not registered, image=%p\n",
		     static_cast<const void*>(image));
	}
	auto& cached = **it;
	if (cached.gpu_modified) {
		return;
	}
	const auto& info         = cached.video_out;
	const bool  image_dirty  = m_memory_tracker.IsRegionCpuModified(info.address, info.size);
	const bool  buffer_dirty = cached.buffer_modified ||
	                           m_buffer_cache.IsRegionCpuModified(info.address, info.size) ||
	                           m_buffer_cache.IsRegionGpuModified(info.address, info.size);
	if (!image_dirty && !buffer_dirty) {
		if (info.compression == VideoOutCompression::Uncompressed ||
		    CanUseVideoOutNativeWithoutUpload(info.compression, render_target, false, false)) {
			return;
		}
		EXIT("TextureCache: compressed video-out read requires native GPU contents, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     info.address, info.size);
	}
	if (info.compression != VideoOutCompression::Uncompressed) {
		EXIT("TextureCache: compressed video-out guest refresh is unsupported, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     " image_dirty=%d buffer_dirty=%d render_target=%d\n",
		     info.address, info.size, image_dirty, buffer_dirty, render_target);
	}
	if (buffer_dirty) {
		const auto source = m_buffer_cache.ObtainBufferForImage(info.address, info.size);
		if (!IsCoherentGuestImageSource(source, info.address, info.size)) {
			EXIT("TextureCache: invalid video-out source, addr=0x%016" PRIx64 " size=0x%016" PRIx64
			     " buffer=%p current=%d source=0x%016" PRIx64 "+0x%016" PRIx64
			     " buffer_modified=%d\n",
			     info.address, info.size, static_cast<const void*>(source.buffer),
			     source.cpu_current, source.address, source.size, cached.buffer_modified);
		}
		cached.buffer_modified = false;
	}
	m_memory_tracker.ForEachUploadRange(
	    info.address, info.size, false, [](uint64_t, uint64_t) noexcept {},
	    [&]() noexcept { ImageOps::UploadVideoOut(cached.ctx, image, info, true); });
}

void TextureCache::UnregisterVideoOutSurfaces(const std::vector<VideoOutVulkanImage*>& images) {
	if (images.empty()) {
		EXIT("TextureCache: video-out unregistration requires surfaces\n");
	}
	std::lock_guard           transaction(m_resource_mutex);
	FaultSafeTextureLock      lock(this, m_lock);
	std::vector<CachedImage*> selected;
	selected.reserve(images.size());
	for (size_t i = 0; i < images.size(); i++) {
		auto* image = images[i];
		if (image == nullptr ||
		    std::find(images.begin(), images.begin() + i, image) != images.begin() + i) {
			EXIT("TextureCache: invalid or duplicate video-out image at index %zu, image=%p\n", i,
			     static_cast<const void*>(image));
		}
		auto it = std::find_if(m_images.begin(), m_images.end(),
		                       [image](const auto& cached) { return cached->image == image; });
		if (it == m_images.end() || (*it)->kind != CachedImage::Kind::VideoOut) {
			EXIT("TextureCache: video-out image is not registered, index=%zu image=%p\n", i,
			     static_cast<const void*>(image));
		}
		selected.push_back(it->get());
	}
	GraphicContext* ctx = selected.front()->ctx;
	for (auto* cached: selected) {
		if (cached->ctx != ctx) {
			EXIT("TextureCache: video-out surfaces span graphics contexts, expected=%p actual=%p\n",
			     static_cast<const void*>(ctx), static_cast<const void*>(cached->ctx));
		}
		if (cached->buffer_modified) {
			EXIT("TextureCache: cannot unregister a buffer-dirty video-out surface, "
			     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     cached->Address(), cached->Size());
		}
	}
	Transfer::WaitForGraphicsIdle(ctx);
	for (auto* cached: selected) {
		if (cached->gpu_modified) {
			m_memory_tracker.UnmarkRegionAsGpuModified(cached->Address(), cached->Size());
			cached->gpu_modified = false;
		}
		UnregisterImageLocked(*cached, true);
	}
	for (auto* image: images) {
		auto it = std::find_if(m_images.begin(), m_images.end(),
		                       [image](const auto& cached) { return cached->image == image; });
		m_images.erase(it);
	}
}

bool TextureCache::ClearImageFromBuffer(CommandBuffer* command, uint64_t vaddr, uint64_t size,
                                        uint32_t packed_clear) {
	if (command == nullptr || command->IsInvalid() || vaddr == 0 || size == 0 ||
	    vaddr >= TRACKER_ADDRESS_SIZE || size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("TextureCache: invalid compute image clear, command=%p addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     static_cast<const void*>(command), vaddr, size);
	}
	m_buffer_cache.ValidateGpuAccess(vaddr, size, false, true);
	std::lock_guard      transaction(m_resource_mutex);
	FaultSafeTextureLock lock(this, m_lock);
	enum class ClearAspect : uint8_t { None, Color, Depth, Stencil };
	auto classify = [vaddr, size](const CachedImage& cached) {
		const bool color = cached.kind == CachedImage::Kind::VideoOut ||
		                   cached.kind == CachedImage::Kind::RenderTarget;
		if (color && cached.Address() == vaddr && cached.Size() == size) {
			return ClearAspect::Color;
		}
		if (cached.kind != CachedImage::Kind::DepthTarget) {
			return ClearAspect::None;
		}
		if (CanNativeClearDepthFromBuffer(cached.depth, vaddr, size)) {
			return ClearAspect::Depth;
		}
		if (cached.RangeCount() == 2 && cached.Address(1) == vaddr && cached.Size(1) == size) {
			return ClearAspect::Stencil;
		}
		return ClearAspect::None;
	};

	std::shared_ptr<CachedImage> match;
	ClearAspect                  aspect       = ClearAspect::None;
	bool                         incompatible = false;
	uint32_t                     matches      = 0;
	for (const auto& cached: m_images) {
		if (!cached->OverlapsRange(vaddr, size, true)) {
			continue;
		}
		const auto candidate = classify(*cached);
		if (candidate == ClearAspect::None) {
			incompatible = true;
			continue;
		}
		if (aspect == ClearAspect::None) {
			aspect = candidate;
			match  = cached;
		} else if (candidate != aspect) {
			incompatible = true;
			continue;
		}
		matches++;
	}
	if (matches > 1) {
		EXIT("TextureCache: compute image clear has ambiguous exact aliases, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 " aspect=%u matches=%u\n",
		     vaddr, size, static_cast<uint32_t>(aspect), matches);
	}
	if (incompatible || match == nullptr) {
		return false;
	}

	float depth_clear = 0.0f;
	if (aspect == ClearAspect::Depth &&
	    !DecodePackedDepthClear(match->image->format, packed_clear, &depth_clear)) {
		return false;
	}
	uint8_t stencil_clear = 0;
	if (aspect == ClearAspect::Stencil && !DecodePackedStencilClear(packed_clear, &stencil_clear)) {
		return false;
	}

	RequireNoMetaOverlapLocked(vaddr, size);
	const bool buffer_overlap = m_buffer_cache.HasPageOverlap(vaddr, size);
	const bool buffer_cpu_modified =
	    buffer_overlap && m_buffer_cache.IsRegionCpuModified(vaddr, size);
	const bool buffer_gpu_modified =
	    buffer_overlap && m_buffer_cache.IsRegionGpuModified(vaddr, size);
	BufferImageCopySource source {nullptr, 0, vaddr, size, true};
	if (buffer_overlap && !buffer_cpu_modified && !buffer_gpu_modified) {
		source = m_buffer_cache.ObtainBufferForImage(vaddr, size);
	}
	const bool buffer_source_valid =
	    !buffer_overlap || IsCoherentGuestImageSource(source, vaddr, size);
	const bool image_cpu_modified = m_memory_tracker.IsRegionCpuModified(vaddr, size);
	const bool invalid_common = !buffer_source_valid || buffer_cpu_modified ||
	                            buffer_gpu_modified || match->buffer_modified || image_cpu_modified;
	if (aspect == ClearAspect::Color && invalid_common) {
		EXIT("TextureCache: compute image clear requires exclusive GPU image ownership, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     " buffer_overlap=%d source_valid=%d buffer_cpu_modified=%d"
		     " buffer_gpu_modified=%d buffer_modified=%d image_cpu_modified=%d\n",
		     vaddr, size, buffer_overlap, buffer_source_valid, buffer_cpu_modified,
		     buffer_gpu_modified, match->buffer_modified, image_cpu_modified);
	}
	if (aspect != ClearAspect::Color && (!match->gpu_modified || invalid_common)) {
		const bool  depth       = aspect == ClearAspect::Depth;
		const char* plane       = depth ? "depth" : "stencil";
		const char* source_name = depth ? "buffer" : "stencil";
		EXIT("TextureCache: compute %s clear requires a GPU-owned depth image and a clean "
		     "%s source, addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     " gpu_modified=%d buffer_overlap=%d source_valid=%d buffer_cpu_modified=%d"
		     " buffer_gpu_modified=%d buffer_modified=%d image_cpu_modified=%d\n",
		     plane, source_name, vaddr, size, match->gpu_modified, buffer_overlap,
		     buffer_source_valid, buffer_cpu_modified, buffer_gpu_modified, match->buffer_modified,
		     image_cpu_modified);
	}

	auto vk_buffer = command->Handle();
	if (aspect == ClearAspect::Color) {
		vk::ClearColorValue clear {};
		if (!DecodePackedColorClear(match->image->format, packed_clear, &clear)) {
			return false;
		}
		GraphicsRenderColorImageBarrier(vk_buffer, match->image,
		                                vk::ImageLayout::eTransferDstOptimal);
		const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0,
		                                       VK_REMAINING_MIP_LEVELS, 0, match->image->layers};
		vk_buffer.clearColorImage(match->image->image, match->image->layout, &clear, 1, &range);
		GraphicsRenderColorImageBarrier(vk_buffer, match->image, RENDER_COLOR_IMAGE_LAYOUT);
		if (!match->gpu_modified) {
			m_memory_tracker.MarkRegionAsGpuModified(vaddr, size);
			match->gpu_modified = true;
		}
		command->RetainResourceUntilFence(match);
		return true;
	}

	if (match->image->layout == vk::ImageLayout::eUndefined ||
	    match->image->layout == vk::ImageLayout::ePreinitialized) {
		EXIT("TextureCache: compute %s clear has invalid source layout %u\n",
		     aspect == ClearAspect::Depth ? "depth" : "stencil",
		     static_cast<uint32_t>(match->image->layout));
	}
	const auto old_layout = match->image->layout;
	GraphicsRenderDepthStencilImageBarrier(vk_buffer, match->image,
	                                       vk::ImageLayout::eTransferDstOptimal);
	const vk::ClearDepthStencilValue clear {depth_clear, stencil_clear};
	const auto                       clear_aspect = static_cast<vk::ImageAspectFlags>(
	    aspect == ClearAspect::Depth ? vk::ImageAspectFlagBits::eDepth
	                                 : vk::ImageAspectFlagBits::eStencil);
	const vk::ImageSubresourceRange range {clear_aspect, 0, VK_REMAINING_MIP_LEVELS, 0,
	                                       match->image->layers};
	vk_buffer.clearDepthStencilImage(match->image->image, match->image->layout, &clear, 1, &range);
	GraphicsRenderDepthStencilImageBarrier(vk_buffer, match->image, old_layout);
	if (aspect == ClearAspect::Stencil) {
		match->stencil_initialized = true;
	}
	command->RetainResourceUntilFence(match);
	return true;
}

void TextureCache::MarkGpuWritten(VulkanImage* image) {
	if (image == nullptr) {
		EXIT("TextureCache: invalid GPU-write notification, image=%p\n",
		     static_cast<const void*>(image));
	}
	std::lock_guard      transaction(m_resource_mutex);
	FaultSafeTextureLock lock(this, m_lock);
	for (auto& cached: m_images) {
		if (cached->image != image) {
			continue;
		}
		if (cached->kind != CachedImage::Kind::RenderTarget &&
		    cached->kind != CachedImage::Kind::DepthTarget &&
		    cached->kind != CachedImage::Kind::VideoOut) {
			EXIT("TextureCache: sampled texture cannot be marked GPU-written, image=%p kind=%u\n",
			     static_cast<const void*>(image), static_cast<uint32_t>(cached->kind));
		}
		for (uint32_t i = 0; i < cached->RangeCount(); i++) {
			if (m_buffer_cache.HasPageOverlap(cached->Address(i), cached->Size(i))) {
				const bool cpu_modified =
				    m_buffer_cache.IsRegionCpuModified(cached->Address(i), cached->Size(i));
				const bool gpu_modified =
				    m_buffer_cache.IsRegionGpuModified(cached->Address(i), cached->Size(i));
				if (cpu_modified || gpu_modified) {
					EXIT(
					    "TextureCache: GPU-written image aliases a dirty buffer, addr=0x%016" PRIx64
					    " size=0x%016" PRIx64 " cpu_modified=%d gpu_modified=%d\n",
					    cached->Address(i), cached->Size(i), cpu_modified, gpu_modified);
				}
			}
			if (m_memory_tracker.IsRegionCpuModified(cached->Address(i), cached->Size(i))) {
				EXIT("TextureCache: GPU-write begins while image range is CPU-modified, "
				     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
				     cached->Address(i), cached->Size(i));
			}
		}
		if (cached->kind == CachedImage::Kind::DepthTarget) {
			if ((cached->depth.htile_address == 0) != (cached->depth.htile_size == 0)) {
				EXIT("TextureCache: depth target has incomplete HTile range, addr=0x%016" PRIx64
				     " size=0x%016" PRIx64 "\n",
				     cached->depth.htile_address, cached->depth.htile_size);
			}
			if (cached->depth.htile_address != 0) {
				auto meta = m_surface_metas.find(cached->depth.htile_address);
				if (meta == m_surface_metas.end() ||
				    meta->second.size != cached->depth.htile_size) {
					EXIT("TextureCache: depth target HTile metadata is missing or mismatched, "
					     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
					     cached->depth.htile_address, cached->depth.htile_size);
				}
				m_buffer_cache.ValidateGpuAccess(cached->depth.htile_address,
				                                 cached->depth.htile_size, false, true);
				m_metadata_tracker.ForEachUploadRange(
				    cached->depth.htile_address, cached->depth.htile_size, true,
				    [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
				meta->second.gpu_modified = true;
			}
		}
		if (!cached->gpu_modified) {
			for (uint32_t i = 0; i < cached->RangeCount(); i++) {
				m_memory_tracker.MarkRegionAsGpuModified(cached->Address(i), cached->Size(i));
			}
			cached->gpu_modified = true;
		}
		return;
	}
	EXIT("TextureCache: GPU-written image is not registered, image=%p\n",
	     static_cast<const void*>(image));
}

void TextureCache::PrepareHostWrite(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("TextureCache: invalid host-write range, addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     "\n",
		     vaddr, size);
	}
	FaultSafeTextureLock lock(this, m_lock);
	const bool           metadata_overlap = HasMetaOverlapLocked(vaddr, size);
	bool                 found            = false;
	for (const auto& cached: m_images) {
		for (uint32_t range = 0; range < cached->RangeCount(); range++) {
			// CPU-current depth targets use the same tracked guest refresh path as sampled and
			// color images. GPU-owned targets and metadata aliases remain unsupported below.
			const bool host_refreshable = (cached->kind == CachedImage::Kind::Texture ||
			                               cached->kind == CachedImage::Kind::RenderTarget ||
			                               cached->kind == CachedImage::Kind::DepthTarget) &&
			                              !cached->buffer_modified;
			switch (ClassifyHostWriteOverlap(vaddr, size, cached->Address(range),
			                                 cached->Size(range), host_refreshable,
			                                 cached->gpu_modified, metadata_overlap)) {
				case HostWriteOverlap::None: break;
				case HostWriteOverlap::InvalidateImage: found = true; break;
				case HostWriteOverlap::Unsupported:
					EXIT("TextureCache: host write aliases unsupported image, "
					     "write=0x%016" PRIx64 "+0x%016" PRIx64 " image=0x%016" PRIx64
					     "+0x%016" PRIx64
					     " kind=%u gpu_modified=%d buffer_modified=%d metadata_overlap=%d\n",
					     vaddr, size, cached->Address(range), cached->Size(range),
					     static_cast<uint32_t>(cached->kind), cached->gpu_modified,
					     cached->buffer_modified, metadata_overlap);
			}
		}
	}
	if (!found) {
		EXIT("TextureCache: host write expected a refreshable image overlap, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	// Explicitly invalidate on a write fault. The resource transaction is already held here, so
	// writing first would recursively enter the cache.
	m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
	MarkSampledAliasesCpuDirtyLocked(vaddr, size);
}

void TextureCache::SynchronizeColorImageToBufferLocked(CachedImage& cached, uint64_t write_address,
                                                       uint64_t write_size) {
	const bool       render_target = cached.kind == CachedImage::Kind::RenderTarget;
	const bool       video_out     = cached.kind == CachedImage::Kind::VideoOut;
	const bool       storage       = cached.kind == CachedImage::Kind::StorageTexture;
	RenderTargetInfo target        = cached.target;
	if (storage) {
		const auto& info         = cached.info;
		target.address           = info.address;
		target.size              = info.size;
		target.format            = VulkanFormat(info.format);
		target.width             = info.width;
		target.height            = info.height;
		target.pitch             = info.pitch;
		target.bytes_per_element = Prospero::RenderTargetBytesPerElement(info.format);
		target.tile_mode         = info.tile;
		target.levels            = info.levels;
	}
	if (video_out) {
		const auto& info         = cached.video_out;
		target.address           = info.address;
		target.size              = info.size;
		target.format            = info.format;
		target.width             = info.width;
		target.height            = info.height;
		target.pitch             = info.pitch;
		target.bytes_per_element = info.bytes_per_element;
		target.tile_mode         = info.tile_mode;
	}
	const bool    linear = target.tile_mode == Prospero::GpuEnumValue(Prospero::TileMode::kLinear);
	const bool    tiled  = IsTiledRenderTarget(target);
	TileSizeAlign exact {};
	bool          single_slice = false;
	if (IsSupportedStandard64RenderTarget(target)) {
		exact        = {static_cast<uint32_t>(target.size), 65536};
		single_slice = true;
	} else {
		single_slice = TileGetRenderTargetSize(target.width, target.height, target.pitch,
		                                       target.bytes_per_element, &exact);
	}
	const bool layered_size =
	    single_slice && static_cast<uint64_t>(exact.size) * target.layers == target.size;
	if (storage && target.levels > 1) {
		single_slice = TileGetRenderTargetMipLayout(target.width, target.height, target.pitch,
		                                            target.bytes_per_element, target.levels, &exact,
		                                            nullptr, nullptr);
	}
	const bool exact_tiled = tiled && exact.align == 65536 &&
	                         (storage ? single_slice && exact.size == target.size : layered_size);
	const bool valid_kind =
	    render_target || storage ||
	    (video_out && cached.video_out.compression == VideoOutCompression::Uncompressed);
	if (!valid_kind || !cached.gpu_modified || cached.buffer_modified ||
	    (!storage && target.levels != 1) || target.size > UINT32_MAX || (!linear && !exact_tiled) ||
	    HasMetaOverlapLocked(target.address, target.size)) {
		EXIT("TextureCache: unsupported color-image buffer synchronization, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     " extent=%ux%u pitch=%u bpe=%u levels=%u layers=%u tile=%u"
		     " kind=%u compression=%u gpu_modified=%d buffer_modified=%d\n",
		     target.address, target.size, target.width, target.height, target.pitch,
		     target.bytes_per_element, target.levels, target.layers, target.tile_mode,
		     static_cast<uint32_t>(cached.kind),
		     video_out ? static_cast<uint32_t>(cached.video_out.compression) : 0,
		     cached.gpu_modified, cached.buffer_modified);
	}
	if (write_address < target.address || write_size == 0 ||
	    write_address - target.address > target.size ||
	    write_size > target.size - (write_address - target.address)) {
		EXIT("TextureCache: image synchronization write is outside backing, "
		     "write=0x%016" PRIx64 "+0x%016" PRIx64 " image=0x%016" PRIx64 "+0x%016" PRIx64 "\n",
		     write_address, write_size, target.address, target.size);
	}
	const auto slice_size = target.size / target.layers;
	if (cached.image->format != target.format || cached.image->extent.width != target.width ||
	    cached.image->extent.height != target.height ||
	    (tiled && !IsSupportedRenderTargetElementSize(target.bytes_per_element)) ||
	    HasMetaOverlapLocked(target.address, target.size)) {
		EXIT("TextureCache: color-image buffer synchronization storage mismatch, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 " linear=0x%016" PRIx64 "\n",
		     target.address, target.size);
	}

	// This is the CPU Tiler backend for the image-to-buffer synchronization seam.
	// The vectors and Vulkan staging allocation retain capacity; a future PS5 GPU tiler can replace
	// this block without changing alias classification or ownership transitions.
	Transfer::WaitForGraphicsIdle(cached.ctx);
	m_buffer_transition_linear.resize(target.size);
	std::fill(m_buffer_transition_linear.begin(), m_buffer_transition_linear.end(), 0);
	std::vector<ImageBufferCopy> regions;
	if (storage) {
		auto layout = TextureCalcUploadLayout(
		    cached.info.format, cached.info.width, cached.info.height, cached.info.levels,
		    cached.info.depth, cached.info.pitch, cached.info.tile, cached.info.size, true, false,
		    false, "StorageTextureReadback");
		auto uploads = TextureBuildUploadRegions(
		    layout, cached.image->format, cached.info.width, cached.info.height, cached.info.depth,
		    cached.info.levels, false, false, TextureUploadDestination::MipLevels,
		    TextureUploadSliceLayout::MipChainPerSlice);
		regions.reserve(uploads.size());
		for (const auto& upload: uploads) {
			regions.push_back({upload.offset, upload.pitch, upload.dst_level, upload.width,
			                   upload.height, upload.copy_height, upload.dst_layer, upload.dst_x,
			                   upload.dst_y, upload.dst_z, upload.aspect});
		}
	} else {
		regions = Transfer::MakeLayeredImageBufferCopies(target.layers, slice_size, target.pitch,
		                                                 target.width, target.height);
	}
	Transfer::DownloadImage(cached.ctx, m_buffer_transition_linear.data(), target.size, regions,
	                        cached.image, cached.image->layout);
	if (storage) {
		m_buffer_transition_guest.resize(target.size);
		m_tiler.TileImage(m_buffer_transition_guest.data(), m_buffer_transition_linear.data(),
		                  cached.info);
		Libs::LibKernel::Memory::WriteBacking(target.address, m_buffer_transition_guest.data(),
		                                      target.size);
	} else if (tiled) {
		m_buffer_transition_guest.resize(target.size);
		m_tiler.TileImage(m_buffer_transition_guest.data(), m_buffer_transition_linear.data(),
		                  target);
		Libs::LibKernel::Memory::WriteBacking(target.address, m_buffer_transition_guest.data(),
		                                      target.size);
	} else {
		Libs::LibKernel::Memory::WriteBacking(target.address, m_buffer_transition_linear.data(),
		                                      target.size);
	}
	m_memory_tracker.ForEachDownloadRange<true>(target.address, target.size,
	                                            [](uint64_t, uint64_t) noexcept {});
	// Only the impending buffer-write range needs publication into BufferCache ownership. The
	// complete image backing was reconstructed above so a later partial-buffer rebind can detile
	// the coherent mip chain without requiring one cached buffer to contain the whole image.
	m_buffer_cache.PublishImageBacking(write_address, write_size);
	cached.gpu_modified    = false;
	cached.buffer_modified = true;
}

void TextureCache::SynchronizeDepthImageToBufferLocked(CachedImage& cached, uint64_t write_address,
                                                       uint64_t write_size) {
	const auto&   info        = cached.depth;
	const bool    has_stencil = info.stencil_address != 0 || info.stencil_size != 0;
	const bool    has_htile   = info.htile_address != 0 || info.htile_size != 0;
	TileSizeAlign expected_stencil {};
	TileSizeAlign expected_htile {};
	TileSizeAlign expected_depth {};
	const bool    d16 =
	    info.guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm) &&
	    info.format == vk::Format::eD16Unorm && info.bytes_per_element == 2;
	const bool d32 =
	    info.guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float) &&
	    info.format == vk::Format::eD32Sfloat && info.bytes_per_element == 4;
	const bool layout =
	    (d16 || d32) &&
	    TileGetDepthSize(info.width, info.height, 0,
	                     Prospero::GpuEnumValue(d16 ? Prospero::DepthFormat::kZ16
	                                                : Prospero::DepthFormat::kZ32F),
	                     Prospero::GpuEnumValue(Prospero::StencilFormat::kInvalid), false,
	                     &expected_stencil, &expected_htile, &expected_depth);
	if (cached.kind != CachedImage::Kind::DepthTarget || !cached.gpu_modified ||
	    cached.buffer_modified || write_address != info.address || write_size != info.size ||
	    has_stencil || has_htile || info.layers != 1 || !layout || expected_depth.align != 65536 ||
	    expected_depth.size != info.size || cached.image->format != info.format ||
	    cached.image->extent.width != info.width || cached.image->extent.height != info.height ||
	    HasMetaOverlapLocked(info.address, info.size)) {
		EXIT("TextureCache: unsupported depth-image buffer synchronization, "
		     "write=0x%016" PRIx64 "+0x%016" PRIx64 " depth=0x%016" PRIx64 "+0x%016" PRIx64
		     " extent=%ux%u layers=%u format=%d guest=%u bpe=%u"
		     " stencil=%d htile=%d gpu_modified=%d buffer_modified=%d\n",
		     write_address, write_size, info.address, info.size, info.width, info.height,
		     info.layers, static_cast<int>(info.format), info.guest_format, info.bytes_per_element,
		     has_stencil, has_htile, cached.gpu_modified, cached.buffer_modified);
	}
	Transfer::WaitForGraphicsIdle(cached.ctx);
	m_buffer_transition_linear.resize(info.size);
	std::fill(m_buffer_transition_linear.begin(), m_buffer_transition_linear.end(), 0);
	const auto regions = Transfer::MakeLayeredImageBufferCopies(
	    1, info.size, info.pitch, info.width, info.height, vk::ImageAspectFlagBits::eDepth);
	Transfer::DownloadImage(cached.ctx, m_buffer_transition_linear.data(), info.size, regions,
	                        cached.image, cached.image->layout);
	m_buffer_transition_guest.resize(info.size);
	m_tiler.TileImage(m_buffer_transition_guest.data(), m_buffer_transition_linear.data(), info);
	Libs::LibKernel::Memory::WriteBacking(info.address, m_buffer_transition_guest.data(),
	                                      info.size);
	m_memory_tracker.ForEachDownloadRange<true>(info.address, info.size,
	                                            [](uint64_t, uint64_t) noexcept {});
	m_buffer_cache.PublishImageBacking(write_address, write_size);
	cached.gpu_modified    = false;
	cached.buffer_modified = true;
}

bool TextureCache::InvalidateMemoryFromGPU(uint64_t vaddr, uint64_t size,
                                           bool formatted_buffer_write) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("TextureCache: invalid GPU invalidation range, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	FaultSafeTextureLock lock(this, m_lock);
	for (auto it = m_surface_metas.begin(); it != m_surface_metas.end();) {
		switch (ClassifyImageRangeOverlap(vaddr, size, it->first, it->second.size)) {
			case ImageRangeOverlap::None:
			case ImageRangeOverlap::PageOnly: ++it; continue;
			case ImageRangeOverlap::Bytes: break;
		}
		LOGF("TextureCache: discarding overwritten virtual metadata, write=0x%016" PRIx64
		     "+0x%016" PRIx64 " metadata=0x%016" PRIx64 "+0x%016" PRIx64 "\n",
		     vaddr, size, it->first, it->second.size);
		if (it->second.gpu_modified) {
			m_metadata_tracker.ForEachDownloadRange<true>(it->first, it->second.size,
			                                              [](uint64_t, uint64_t) noexcept {});
		}
		m_metadata_tracker.UntrackMemory(it->first, it->second.size);
		it = m_surface_metas.erase(it);
	}
	auto             match  = m_images.end();
	BufferImageWrite action = BufferImageWrite::None;
	for (auto it = m_images.begin(); it != m_images.end(); ++it) {
		auto& cached = **it;
		if (!cached.OverlapsRange(vaddr, size, false)) {
			continue;
		}
		const auto next = ClassifyBufferImageWrite(vaddr, size, cached.Address(), cached.Size(),
		                                           cached.BufferBinding(), cached.gpu_modified,
		                                           formatted_buffer_write, cached.buffer_modified);
		if (match != m_images.end() || next == BufferImageWrite::None ||
		    next == BufferImageWrite::Unsupported) {
			EXIT("TextureCache: unsupported GPU invalidation alias, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " cached_kind=%u cached=0x%016" PRIx64 "+0x%016" PRIx64
			     " gpu_modified=%d buffer_modified=%d formatted=%d ambiguous=%d\n",
			     vaddr, size, static_cast<uint32_t>(cached.kind), cached.Address(), cached.Size(),
			     cached.gpu_modified, cached.buffer_modified, formatted_buffer_write,
			     match != m_images.end());
		}
		match  = it;
		action = next;
	}
	if (match == m_images.end()) {
		return false;
	}
	auto& cached = **match;
	switch (action) {
		case BufferImageWrite::InvalidateTexture:
		case BufferImageWrite::InvalidateVideoOut:
		case BufferImageWrite::InvalidateStorageTexture:
		case BufferImageWrite::InvalidateDepthTarget:
		case BufferImageWrite::InvalidateRenderTarget: cached.buffer_modified = true; return true;
		case BufferImageWrite::SynchronizeRenderTarget:
		case BufferImageWrite::SynchronizeStorageTexture:
			SynchronizeColorImageToBufferLocked(cached, vaddr, size);
			return true;
		case BufferImageWrite::SynchronizeDepthTarget:
			SynchronizeDepthImageToBufferLocked(cached, vaddr, size);
			return true;
		case BufferImageWrite::SynchronizeVideoOut:
			SynchronizeColorImageToBufferLocked(cached, vaddr, size);
			return true;
		case BufferImageWrite::None:
		case BufferImageWrite::Unsupported:
			EXIT("TextureCache: invalid GPU invalidation action %u\n",
			     static_cast<uint32_t>(action));
	}
	return false;
}

DepthStencilVulkanImage* TextureCache::FindDepthTargetByRange(CommandBuffer* command,
                                                              uint64_t vaddr, uint64_t size,
                                                              bool allow_containing_sampled) {
	if (command == nullptr || vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("TextureCache: invalid depth-target range query, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	FaultSafeTextureLock lock(this, m_lock);
	CachedImage*         found = nullptr;
	for (auto* cached: FindImagesInRegionLocked(vaddr, size, false)) {
		if (cached->kind != CachedImage::Kind::DepthTarget ||
		    !cached->OverlapsRange(vaddr, size, false)) {
			continue;
		}
		const bool containing_sampled =
		    allow_containing_sampled && vaddr == cached->depth.address && size > cached->depth.size;
		if ((!IsDepthTargetRangeCompatible(cached->depth, vaddr, size) && !containing_sampled) ||
		    found != nullptr) {
			EXIT("TextureCache: incompatible or ambiguous depth-target range, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " cached=0x%016" PRIx64 "+0x%016" PRIx64 " previous=%p\n",
			     vaddr, size, cached->depth.address, cached->depth.size,
			     static_cast<const void*>(found));
		}
		if (containing_sampled) {
			return nullptr;
		}
		const bool stencil_range =
		    vaddr == cached->depth.stencil_address && size == cached->depth.stencil_size;
		if (stencil_range && !cached->stencil_initialized) {
			EXIT("TextureCache: sampled stencil range is uninitialized, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     vaddr, size);
		}
		found = cached;
	}
	if (found == nullptr) {
		return nullptr;
	}
	const auto owner = std::find_if(m_images.begin(), m_images.end(),
	                                [found](const auto& image) { return image.get() == found; });
	if (owner == m_images.end()) {
		EXIT("TextureCache: page-table depth target has no cache owner\n");
	}
	command->RetainResourceUntilFence(*owner);
	return static_cast<DepthStencilVulkanImage*>(found->image);
}

RenderTextureVulkanImage* TextureCache::FindRenderTargetByRange(CommandBuffer* command,
                                                                uint64_t vaddr, uint64_t size) {
	if (command == nullptr || vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("TextureCache: invalid render-target range query, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	FaultSafeTextureLock lock(this, m_lock);
	CachedImage*         found = nullptr;
	for (auto* cached: FindImagesInRegionLocked(vaddr, size, false)) {
		if (cached->kind != CachedImage::Kind::RenderTarget ||
		    !ImageRangeOverlaps(vaddr, size, cached->Address(), cached->Size())) {
			continue;
		}
		const auto slice_size =
		    cached->target.layers != 0 ? cached->target.size / cached->target.layers : 0;
		const bool contained = vaddr == cached->target.address && slice_size != 0 &&
		                       cached->target.size % cached->target.layers == 0 &&
		                       size <= cached->target.size && size % slice_size == 0;
		if (!contained) {
			continue;
		}
		if (found != nullptr) {
			EXIT("TextureCache: incompatible or ambiguous render-target range, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " cached=0x%016" PRIx64 "+0x%016" PRIx64 " previous=%p\n",
			     vaddr, size, cached->Address(), cached->Size(), static_cast<const void*>(found));
		}
		found = cached;
	}
	if (found == nullptr) {
		return nullptr;
	}
	const auto owner = std::find_if(m_images.begin(), m_images.end(),
	                                [found](const auto& image) { return image.get() == found; });
	if (owner == m_images.end()) {
		EXIT("TextureCache: page-table render target has no cache owner\n");
	}
	command->RetainResourceUntilFence(*owner);
	return static_cast<RenderTextureVulkanImage*>(found->image);
}

TextureCache::RegionInfo TextureCache::QueryRegion(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("TextureCache: invalid region query, addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	FaultSafeTextureLock lock(this, m_lock);
	RegionInfo           result;
	const auto           candidates = FindImagesInRegionLocked(vaddr, size, true);
	result.image_pages              = !candidates.empty();
	for (const auto* cached: candidates) {
		const bool bytes = cached->OverlapsRange(vaddr, size, false);
		result.image_bytes |= bytes;
		result.gpu_image_bytes |= bytes && cached->gpu_modified;
		result.non_sampled_pages |= cached->kind != CachedImage::Kind::Texture;
	}
	for (const auto& [address, metadata]: m_surface_metas) {
		result.metadata_pages |= ImagePageRangesOverlap(vaddr, size, address, metadata.size);
		result.metadata_bytes |= ImageRangeOverlaps(vaddr, size, address, metadata.size);
	}
	result.gpu_metadata_bytes =
	    result.metadata_bytes && m_metadata_tracker.IsRegionGpuModified(vaddr, size);
	return result;
}

void TextureCache::RegisterMeta(uint64_t vaddr, uint64_t size, uint32_t layers) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr || (vaddr & 0x7fffu) != 0 || layers == 0 ||
	    layers > 32 || size % layers != 0) {
		EXIT("TextureCache: invalid metadata registration, addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     " layers=%u\n",
		     vaddr, size, layers);
	}
	std::lock_guard transaction(m_resource_mutex);
	if (m_buffer_cache.HasPageOverlap(vaddr, size)) {
		// Register virtual surface metadata independently of an earlier buffer view. Kyty's split
		// caches first publish any dirty buffer bytes; clean partial views use guest backing.
		const auto source = m_buffer_cache.ObtainBufferForImage(vaddr, size);
		if (!IsCoherentGuestImageSource(source, vaddr, size)) {
			EXIT("TextureCache: metadata buffer source is inconsistent, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " source=%p source_addr=0x%016" PRIx64
			     " source_size=0x%016" PRIx64 " current=%d\n",
			     vaddr, size, static_cast<const void*>(source.buffer), source.address, source.size,
			     source.cpu_current);
		}
	}
	FaultSafeTextureLock lock(this, m_lock);
	auto                 existing    = m_surface_metas.find(vaddr);
	uint64_t             range_vaddr = vaddr;
	uint64_t             range_size  = size;
	if (existing != m_surface_metas.end()) {
		const auto slice_size     = size / layers;
		const auto old_slice_size = existing->second.size / existing->second.layers;
		if (slice_size != old_slice_size ||
		    (size > existing->second.size) != (layers > existing->second.layers)) {
			EXIT("TextureCache: incompatible metadata backing growth\n");
		}
		if (layers <= existing->second.layers) {
			return;
		}
		range_vaddr += existing->second.size;
		range_size -= existing->second.size;
	}
	for (const auto& [address, meta]: m_surface_metas) {
		if (address != vaddr &&
		    ImagePageRangesOverlap(range_vaddr, range_size, address, meta.size)) {
			LOGF("TextureCache: registering overlapping metadata views, requested=0x%016" PRIx64
			     "+0x%016" PRIx64 " existing=0x%016" PRIx64 "+0x%016" PRIx64 "\n",
			     range_vaddr, range_size, address, meta.size);
		}
	}
	std::vector<CachedImage*> retire;
	for (const auto& cached: m_images) {
		if (!cached->OverlapsRange(range_vaddr, range_size, true)) {
			continue;
		}
		const auto overlap =
		    ClassifyMetaImageOverlap(cached->kind == CachedImage::Kind::Texture,
		                             cached->kind == CachedImage::Kind::RenderTarget,
		                             cached->gpu_modified, cached->buffer_modified);
		if (overlap == MetaImageOverlap::Unsupported) {
			EXIT("TextureCache: metadata aliases image pages, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " image_kind=%u image=0x%016" PRIx64 "+0x%016" PRIx64
			     " gpu_modified=%d buffer_modified=%d\n",
			     range_vaddr, range_size, static_cast<uint32_t>(cached->kind), cached->Address(),
			     cached->Size(), cached->gpu_modified, cached->buffer_modified);
		}
		if (overlap == MetaImageOverlap::RetireTarget) {
			retire.push_back(cached.get());
			continue;
		}
		LOGF("TextureCache: metadata aliases a CPU-current sampled image, metadata=0x%016" PRIx64
		     "+0x%016" PRIx64 " image=0x%016" PRIx64 "+0x%016" PRIx64 "\n",
		     range_vaddr, range_size, cached->Address(), cached->Size());
	}
	if (!retire.empty()) {
		RequireRetirementIsolation(retire, "metadata", range_vaddr, range_size);
		for (const auto* cached: retire) {
			LOGF("TextureCache: retiring a CPU-current render target for metadata reuse, "
			     "metadata=0x%016" PRIx64 "+0x%016" PRIx64 " target=0x%016" PRIx64 "+0x%016" PRIx64
			     "\n",
			     range_vaddr, range_size, cached->Address(), cached->Size());
		}
		RetireImages(retire);
	}
	if (existing == m_surface_metas.end()) {
		m_surface_metas.emplace(vaddr, MetaDataInfo {.size = size, .layers = layers});
	} else {
		existing->second.size   = size;
		existing->second.layers = layers;
	}
}

bool TextureCache::IsMeta(uint64_t vaddr) {
	FaultSafeTextureLock lock(this, m_lock);
	return m_surface_metas.contains(vaddr);
}

bool TextureCache::IsMetaRange(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		return false;
	}
	FaultSafeTextureLock lock(this, m_lock);
	const auto           it = m_surface_metas.find(vaddr);
	return it != m_surface_metas.end() && it->second.size == size;
}

bool TextureCache::HasMetaOverlapLocked(uint64_t vaddr, uint64_t size) const {
	for (const auto& [address, meta]: m_surface_metas) {
		if (ImagePageRangesOverlap(vaddr, size, address, meta.size)) {
			return true;
		}
	}
	return false;
}

void TextureCache::RequireNoMetaOverlapLocked(uint64_t vaddr, uint64_t size) const {
	if (HasMetaOverlapLocked(vaddr, size)) {
		EXIT("TextureCache: image range overlaps virtual metadata, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
}

bool TextureCache::IsMetaCleared(uint64_t vaddr, uint32_t slice) {
	FaultSafeTextureLock lock(this, m_lock);
	const auto           it = m_surface_metas.find(vaddr);
	if (it == m_surface_metas.end()) {
		return false;
	}
	if (slice >= it->second.layers) {
		EXIT("TextureCache: metadata clear slice out of range, addr=0x%016" PRIx64
		     " slice=%u layers=%u\n",
		     vaddr, slice, it->second.layers);
	}
	if ((it->second.clear_mask & (1u << slice)) == 0) {
		return false;
	}
	const auto slice_size = it->second.size / it->second.layers;
	const auto slice_addr = vaddr + slice_size * slice;
	if (!it->second.gpu_modified ||
	    !m_metadata_tracker.IsRegionGpuModified(slice_addr, slice_size) ||
	    m_metadata_tracker.IsRegionCpuModified(slice_addr, slice_size)) {
		EXIT("TextureCache: cleared metadata slice is not GPU-owned\n");
	}
	return true;
}

bool TextureCache::ClearMeta(uint64_t vaddr) {
	std::lock_guard      transaction(m_resource_mutex);
	FaultSafeTextureLock lock(this, m_lock);
	const auto           it = m_surface_metas.find(vaddr);
	if (it == m_surface_metas.end()) {
		return false;
	}
	m_metadata_tracker.ForEachUploadRange(
	    vaddr, it->second.size, true, [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
	it->second.gpu_modified = true;
	it->second.clear_mask   = it->second.layers == 32 ? UINT32_MAX : (1u << it->second.layers) - 1u;
	return true;
}

bool TextureCache::TouchMeta(uint64_t vaddr, uint32_t slice, bool is_clear) {
	FaultSafeTextureLock lock(this, m_lock);
	const auto           it = m_surface_metas.find(vaddr);
	if (it == m_surface_metas.end()) {
		return false;
	}
	if (slice >= it->second.layers) {
		EXIT("TextureCache: metadata update slice out of range\n");
	}
	const auto slice_size = it->second.size / it->second.layers;
	const auto slice_addr = vaddr + slice_size * slice;
	if (!it->second.gpu_modified ||
	    !m_metadata_tracker.IsRegionGpuModified(slice_addr, slice_size)) {
		EXIT("TextureCache: metadata update requires GPU ownership, addr=0x%016" PRIx64 "\n",
		     vaddr);
	}
	if (m_metadata_tracker.IsRegionCpuModified(slice_addr, slice_size)) {
		EXIT("TextureCache: metadata update races CPU modification, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     slice_addr, slice_size);
	}
	if (is_clear) {
		it->second.clear_mask |= 1u << slice;
	} else {
		it->second.clear_mask &= ~(1u << slice);
	}
	return true;
}

bool TextureCache::InvalidateMemory(PageFaultAccess access, uint64_t vaddr, uint64_t size,
                                    PageFaultPhase phase) noexcept {
	if (access != PageFaultAccess::Read && access != PageFaultAccess::Write) {
		EXIT("TextureCache: unsupported page-fault access %u, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 " phase=%u\n",
		     static_cast<uint32_t>(access), vaddr, size, static_cast<uint32_t>(phase));
	}
	if (phase == PageFaultPhase::Invalidate) {
		if (g_texture_fault_owner != nullptr) {
			EXIT("TextureCache: recursive page-fault invalidation, owner=%p\n",
			     g_texture_fault_owner);
		}
		m_fault_mutex.lock();
		g_texture_fault_owner = this;

		const bool metadata =
		    access == PageFaultAccess::Write &&
		    m_metadata_tracker.InvalidateVirtualGpuWrite(access, vaddr, size, phase);
		CpuFaultAction action         = CpuFaultAction::Untracked;
		bool           needs_readback = false;
		{
			FaultSafeTextureLock lock(this, m_lock);
			if (access == PageFaultAccess::Write) {
				MarkSampledAliasesCpuDirtyLocked(vaddr, size);
			}
			needs_readback = FindGpuReadbackPageCandidateLocked(vaddr, size) != nullptr;
			if (!needs_readback) {
				action = m_memory_tracker.BeginCpuFault(vaddr, size, access);
			}
		}

		if (needs_readback) {
			if (GraphicsRunIsCommandProcessorThread()) {
				GraphicsRunFinishCommandProcessors();
			}
			m_readback->Request(access, vaddr, size);
			action = m_memory_tracker.BeginCpuFault(vaddr, size, access);
			if (action != CpuFaultAction::Download) {
				EXIT("TextureCache: downloaded image did not retain the active fault page "
				     "tracking, addr=0x%016" PRIx64 " size=0x%016" PRIx64 " action=%u\n",
				     vaddr, size, static_cast<uint32_t>(action));
			}
		} else if (action == CpuFaultAction::Download) {
			EXIT("generic region invalidation cannot download GPU-dirty memory\n");
		}
		return metadata || action != CpuFaultAction::Untracked;
	}

	if (g_texture_fault_owner != this) {
		EXIT("TextureCache: page-fault phase has no matching invalidation, phase=%u owner=%p\n",
		     static_cast<uint32_t>(phase), g_texture_fault_owner);
	}
	if (phase == PageFaultPhase::Complete) {
		const bool metadata =
		    access == PageFaultAccess::Write &&
		    m_metadata_tracker.InvalidateVirtualGpuWrite(access, vaddr, size, phase);
		const bool downloaded = m_readback->IsReady(access, vaddr, size);
		const bool image      = m_memory_tracker.CompleteCpuFault(vaddr, size, access, downloaded);
		const bool readback   = m_readback->Complete(access, vaddr, size);
		if (readback && !image) {
			EXIT("TextureCache: image readback completed without a matching tracker fault\n");
		}
		return metadata || image || readback;
	}
	if (phase != PageFaultPhase::Release) {
		EXIT("TextureCache: unsupported page-fault phase %u\n", static_cast<uint32_t>(phase));
	}
	if (access == PageFaultAccess::Write) {
		(void)m_metadata_tracker.InvalidateVirtualGpuWrite(access, vaddr, size, phase);
	}
	m_readback->Release(access, vaddr, size);
	g_texture_fault_owner = nullptr;
	m_fault_mutex.unlock();
	return true;
}

void TextureCache::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("TextureCache: invalid unmap range, addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	std::lock_guard      transaction(m_resource_mutex);
	FaultSafeTextureLock lock(this, m_lock);
	const auto           end = vaddr + size;
	for (const auto& [address, meta]: m_surface_metas) {
		if (ImagePageRangesOverlap(vaddr, size, address, meta.size) &&
		    (vaddr > address || end < address + meta.size)) {
			EXIT("TextureCache: partial metadata unmap is unsupported, unmap=0x%016" PRIx64
			     "+0x%016" PRIx64 " metadata=0x%016" PRIx64 "+0x%016" PRIx64 "\n",
			     vaddr, size, address, meta.size);
		}
	}
	for (const auto& cached: m_images) {
		for (uint32_t i = 0; i < cached->RangeCount(); i++) {
			if (ImagePageRangesOverlap(vaddr, size, cached->Address(i), cached->Size(i)) &&
			    (vaddr > cached->Address(i) || end < cached->Address(i) + cached->Size(i))) {
				EXIT("TextureCache: partial image unmap is unsupported, unmap=0x%016" PRIx64
				     "+0x%016" PRIx64 " image=0x%016" PRIx64 "+0x%016" PRIx64 "\n",
				     vaddr, size, cached->Address(i), cached->Size(i));
			}
		}
		if (cached->OverlapsRange(vaddr, size, false) &&
		    cached->kind == CachedImage::Kind::VideoOut) {
			EXIT("TextureCache: registered video-out surface must be unregistered before unmap, "
			     "unmap=0x%016" PRIx64 "+0x%016" PRIx64 " image=0x%016" PRIx64 "+0x%016" PRIx64
			     " buffer_modified=%d\n",
			     vaddr, size, cached->Address(), cached->Size(), cached->buffer_modified);
		}
	}
	std::vector<uint64_t> retire_depth_metadata;
	for (const auto& cached: m_images) {
		if (cached->kind != CachedImage::Kind::DepthTarget ||
		    !cached->OverlapsRange(vaddr, size, false) || cached->depth.htile_address == 0) {
			continue;
		}
		const bool retained_owner =
		    std::any_of(m_images.begin(), m_images.end(), [&](const auto& other) {
			    return other.get() != cached.get() &&
			           other->kind == CachedImage::Kind::DepthTarget &&
			           !other->OverlapsRange(vaddr, size, false) &&
			           other->depth.htile_address == cached->depth.htile_address &&
			           other->depth.htile_size == cached->depth.htile_size;
		    });
		if (!retained_owner &&
		    std::find(retire_depth_metadata.begin(), retire_depth_metadata.end(),
		              cached->depth.htile_address) == retire_depth_metadata.end()) {
			retire_depth_metadata.push_back(cached->depth.htile_address);
		}
	}
	for (const auto address: retire_depth_metadata) {
		const auto meta  = m_surface_metas.find(address);
		const auto owner = std::find_if(m_images.begin(), m_images.end(), [&](const auto& cached) {
			return cached->kind == CachedImage::Kind::DepthTarget &&
			       cached->OverlapsRange(vaddr, size, false) &&
			       cached->depth.htile_address == address;
		});
		if (meta == m_surface_metas.end() || owner == m_images.end() ||
		    meta->second.size != (*owner)->depth.htile_size) {
			EXIT("TextureCache: retiring depth image has invalid HTile registration, "
			     "unmap=0x%016" PRIx64 "+0x%016" PRIx64 " metadata=0x%016" PRIx64
			     " registered=0x%016" PRIx64 " expected=0x%016" PRIx64 "\n",
			     vaddr, size, address, meta != m_surface_metas.end() ? meta->second.size : 0,
			     owner != m_images.end() ? (*owner)->depth.htile_size : 0);
		}
	}
	std::vector<ImageRetirementRange> metadata_ranges;
	metadata_ranges.reserve(m_surface_metas.size());
	for (const auto& [address, meta]: m_surface_metas) {
		const bool retiring = ImageRangeOverlaps(vaddr, size, address, meta.size) ||
		                      std::find(retire_depth_metadata.begin(), retire_depth_metadata.end(),
		                                address) != retire_depth_metadata.end();
		metadata_ranges.push_back({address, meta.size, retiring});
	}
	const auto metadata_conflict = FindImageRetirementConflict(metadata_ranges);
	if (metadata_conflict.Exists()) {
		const auto& retired  = metadata_ranges[metadata_conflict.retired];
		const auto& retained = metadata_ranges[metadata_conflict.retained];
		EXIT("TextureCache: depth-image retirement leaves a tracked metadata alias, "
		     "unmap=0x%016" PRIx64 "+0x%016" PRIx64 " retired=0x%016" PRIx64 "+0x%016" PRIx64
		     " retained=0x%016" PRIx64 "+0x%016" PRIx64 "\n",
		     vaddr, size, retired.address, retired.size, retained.address, retained.size);
	}
	GraphicContext* wait_ctx = nullptr;
	for (auto& cached: m_images) {
		if (cached->OverlapsRange(vaddr, size, false)) {
			if (wait_ctx != nullptr && wait_ctx != cached->ctx) {
				EXIT("TextureCache: unmap spans multiple graphics contexts, first=%p second=%p\n",
				     static_cast<const void*>(wait_ctx), static_cast<const void*>(cached->ctx));
			}
			wait_ctx = cached->ctx;
		}
	}
	if (wait_ctx != nullptr) {
		Transfer::WaitForGraphicsIdle(wait_ctx);
	}
	for (auto& cached: m_images) {
		if (!cached->OverlapsRange(vaddr, size, false) || !cached->gpu_modified) {
			continue;
		}
		for (uint32_t i = 0; i < cached->RangeCount(); i++) {
			m_memory_tracker.UnmarkRegionAsGpuModified(cached->Address(i), cached->Size(i));
		}
		cached->gpu_modified = false;
	}
	for (auto it = m_surface_metas.begin(); it != m_surface_metas.end();) {
		const bool allocation_unmapped =
		    ImageRangeOverlaps(vaddr, size, it->first, it->second.size);
		const bool depth_owner_retired =
		    std::find(retire_depth_metadata.begin(), retire_depth_metadata.end(), it->first) !=
		    retire_depth_metadata.end();
		if (!allocation_unmapped && !depth_owner_retired) {
			++it;
			continue;
		}
		if (depth_owner_retired) {
			LOGF("TextureCache: retiring HTile metadata with depth image, "
			     "unmap=0x%016" PRIx64 "+0x%016" PRIx64 " metadata=0x%016" PRIx64 "+0x%016" PRIx64
			     "\n",
			     vaddr, size, it->first, it->second.size);
		}
		if (it->second.gpu_modified) {
			m_metadata_tracker.ForEachDownloadRange<true>(it->first, it->second.size,
			                                              [](uint64_t, uint64_t) noexcept {});
		}
		m_metadata_tracker.UntrackMemory(it->first, it->second.size);
		it = m_surface_metas.erase(it);
	}
	m_memory_tracker.UntrackMemory(vaddr, size);
	for (auto it = m_images.begin(); it != m_images.end();) {
		if (!(**it).OverlapsRange(vaddr, size, false)) {
			++it;
			continue;
		}
		UnregisterImageLocked(**it, false);
		it = m_images.erase(it);
	}
}

VulkanImage* TextureCache::GetDummySampledTexture(bool uint_format, bool image_3d) {
	KYTY_PROFILER_BLOCK("TextureCache::GetDummySampledTexture");
	return m_dummy_textures->Get(g_render_ctx->GetGraphicCtx(), DummyTextureCache::Usage::Sampled,
	                             uint_format, image_3d);
}

VulkanImage* TextureCache::GetDummyStorageTexture(bool uint_format, bool image_3d) {
	KYTY_PROFILER_BLOCK("TextureCache::GetDummyStorageTexture");
	return m_dummy_textures->Get(g_render_ctx->GetGraphicCtx(), DummyTextureCache::Usage::Storage,
	                             uint_format, image_3d);
}

} // namespace Libs::Graphics
