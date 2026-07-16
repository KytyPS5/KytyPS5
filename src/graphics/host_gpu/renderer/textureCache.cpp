#include "graphics/host_gpu/renderer/textureCache.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/hostMemory.h"
#include "graphics/host_gpu/objects/label.h"
#include "graphics/host_gpu/objects/textureCommon.h"
#include "graphics/host_gpu/renderer/bufferCache.h"
#include "graphics/host_gpu/renderer/framebufferCache.h"
#include "graphics/host_gpu/renderer/image.h"
#include "graphics/host_gpu/renderer/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/resourceMutex.h"
#include "graphics/host_gpu/utils.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

namespace Libs::Graphics {

namespace {

thread_local const void* g_texture_cache_lock_owner = nullptr;
thread_local const void* g_texture_fault_owner      = nullptr;

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

bool Overlaps(uint64_t left, uint64_t left_size, uint64_t right, uint64_t right_size) {
	return left < right + right_size && right < left + left_size;
}

bool PageOverlaps(uint64_t left, uint64_t left_size, uint64_t right, uint64_t right_size) {
	return ImagePageRangesOverlap(left, left_size, right, right_size);
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
	GraphicContext*  ctx   = nullptr;
	VulkanImage*     image = nullptr;
	VulkanMemory     memory;
	bool             gpu_modified        = false;
	bool             buffer_modified     = false;
	bool             stencil_initialized = false;

	~CachedImage() {
		if (ctx == nullptr || image == nullptr) {
			EXIT("TextureCache: cached image destroyed with invalid resources, ctx=%p image=%p "
			     "kind=%u\n",
			     static_cast<const void*>(ctx), static_cast<const void*>(image),
			     static_cast<uint32_t>(kind));
		}
		switch (kind) {
			case Kind::Texture:
			case Kind::StorageTexture:
				TextureCache::DeleteGpuTexture(ctx, static_cast<GpuTextureVulkanImage*>(image),
				                               &memory);
				break;
			case Kind::RenderTarget:
				TextureCache::DeleteRenderTexture(
				    ctx, static_cast<RenderTextureVulkanImage*>(image), &memory);
				break;
			case Kind::DepthTarget:
				TextureCache::DeleteDepthStencil(ctx, static_cast<DepthStencilVulkanImage*>(image),
				                                 &memory);
				break;
			case Kind::VideoOut:
				TextureCache::DeleteVideoOut(ctx, static_cast<VideoOutVulkanImage*>(image),
				                             &memory);
				break;
		}
		image = nullptr;
	}

	[[nodiscard]] uint32_t RangeCount() const {
		return kind == Kind::DepthTarget && depth.stencil_address != 0 ? 2u : 1u;
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
			if (page ? PageOverlaps(address, size, Address(i), Size(i))
			         : Overlaps(address, size, Address(i), Size(i))) {
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
		const bool submissions_prepaused_now = GraphicsRunSubmissionLockHeld();
		const bool unsafe_gpu_lock = GraphicsRunGpuLockHeld() && !submissions_prepaused_now;
		if ((fault_access != PageFaultAccess::Read && fault_access != PageFaultAccess::Write) ||
		    GraphicsRunIsCommandProcessorThread() || unsafe_gpu_lock || LabelInCallback() ||
		    g_texture_cache_lock_owner != nullptr || g_texture_fault_owner != &cache) {
			EXIT("TextureCache: unsafe image readback request, access=%u command_thread=%d "
			     "submission_lock=%d gpu_lock=%d label_callback=%d cache_lock=%p fault_owner=%p\n",
			     static_cast<uint32_t>(fault_access), GraphicsRunIsCommandProcessorThread(),
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
		access = fault_access;
		vaddr  = fault_vaddr;
		size   = fault_size;
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

	[[nodiscard]] ReadbackRange DownloadDepthTarget(CachedImage& cached) {
		const auto&   info           = cached.depth;
		const bool    has_stencil    = info.stencil_address != 0 || info.stencil_size != 0;
		const bool    has_htile      = info.htile_address != 0 || info.htile_size != 0;
		const bool    fault_in_depth = vaddr >= info.address && vaddr < info.address + info.size &&
		                               size <= info.address + info.size - vaddr;
		const bool    d16 = info.guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm) &&
		                    info.format == VK_FORMAT_D16_UNORM && info.bytes_per_element == 2;
		const bool    d32 = info.guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float) &&
		                    info.format == VK_FORMAT_D32_SFLOAT && info.bytes_per_element == 4;
		TileSizeAlign expected_stencil {};
		TileSizeAlign expected_htile {};
		TileSizeAlign expected_depth {};
		const bool    prospero_layout =
		    (d16 || d32) &&
		    TileGetDepthSize(info.width, info.height, 0,
		                     Prospero::GpuEnumValue(d16 ? Prospero::DepthFormat::kZ16 : Prospero::DepthFormat::kZ32F),
		                     Prospero::GpuEnumValue(Prospero::StencilFormat::kInvalid), has_htile, &expected_stencil,
		                     &expected_htile, &expected_depth);
		const auto expected_pitch = prospero_layout
		                                ? TileGetTexturePitch(info.guest_format, info.width, 1,
		                                                      Prospero::GpuEnumValue(Prospero::TileMode::kDepth))
		                                : 0u;
		if (!fault_in_depth || has_stencil || info.address == 0 || info.size == 0 ||
		    info.width == 0 || info.height == 0 || info.pitch < info.width ||
		    info.tile_mode != Prospero::GpuEnumValue(Prospero::TileMode::kDepth) || !prospero_layout ||
		    info.pitch != expected_pitch || info.size != expected_depth.size ||
		    expected_depth.align != 65536u ||
		    (has_htile &&
		     (info.htile_size != expected_htile.size || expected_htile.align != 32768u))) {
			EXIT("TextureCache: unsupported depth-image readback layout, fault=0x%016" PRIx64
			     "+0x%016" PRIx64 " depth=0x%016" PRIx64 "+0x%016" PRIx64 " stencil=0x%016" PRIx64
			     "+0x%016" PRIx64 " htile=0x%016" PRIx64 "+0x%016" PRIx64
			     " extent=%ux%u pitch=%u/%u tile=%u format=%d guest_format=%u bpe=%u "
			     "expected_depth=0x%016" PRIx64 " expected_htile=0x%016" PRIx64 "\n",
			     vaddr, size, info.address, info.size, info.stencil_address, info.stencil_size,
			     info.htile_address, info.htile_size, info.width, info.height, info.pitch,
			     expected_pitch, info.tile_mode, static_cast<int>(info.format), info.guest_format,
			     info.bytes_per_element, expected_depth.size, expected_htile.size);
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
		const bool meta_overlap   = cache.HasMetaOverlapLocked(info.address, info.size);
		const bool buffer_overlap = cache.m_buffer_cache.HasPageOverlap(info.address, info.size);
		if (linear_size > info.size || cached.image->format != info.format ||
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
		UtilFillBuffer(cached.ctx, download.data(), linear_size, info.pitch, cached.image,
		               cached.image->layout, VK_IMAGE_ASPECT_DEPTH_BIT);
		guest.resize(info.size);
		cache.m_tiler.TileImage(guest.data(), download.data(), info);
		Libs::LibKernel::Memory::WriteBacking(info.address, guest.data(), info.size);
		return {info.address, info.size};
	}

	[[nodiscard]] ReadbackRange DownloadColorImage(CachedImage& cached) {
		const bool storage = cached.kind == CachedImage::Kind::StorageTexture;
		const bool target  = cached.kind == CachedImage::Kind::RenderTarget;
		const auto info =
		    storage ? MakeColorImageTransferInfo(cached.info, Prospero::SurfaceFormat(cached.info.format),
		                                         Prospero::NumBytesPerElement(cached.info.format))
		            : MakeColorImageTransferInfo(cached.target);
		const bool linear = info.tile_mode == Prospero::GpuEnumValue(Prospero::TileMode::kLinear);
		const bool tiled  = target && info.tile_mode == Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
		const bool basic_storage =
		    !storage || (cached.info.type == Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) &&
		                 cached.info.depth == 1 && cached.info.base_level == 0 &&
		                 cached.info.levels == 1 && cached.info.base_array == 0 && linear);
		const auto rows = static_cast<uint64_t>(info.height - 1);
		if ((!linear && !tiled) || !basic_storage || info.address == 0 || info.size == 0 ||
		    info.width == 0 || info.height == 0 || info.levels != 1 || info.pitch < info.width ||
		    info.bytes_per_element == 0 || rows > (UINT64_MAX - info.width) / info.pitch) {
			EXIT("TextureCache: unsupported color-image readback layout, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " extent=%ux%u pitch=%u bpe=%u levels=%u tile=%u kind=%u\n",
			     info.address, info.size, info.width, info.height, info.pitch,
			     info.bytes_per_element, info.levels, info.tile_mode,
			     static_cast<uint32_t>(cached.kind));
		}
		const auto linear_elements = rows * info.pitch + info.width;
		if (linear_elements > UINT64_MAX / info.bytes_per_element) {
			EXIT("TextureCache: color-image readback size overflow\n");
		}
		const auto linear_size    = linear_elements * info.bytes_per_element;
		const bool meta_overlap   = cache.HasMetaOverlapLocked(info.address, info.size);
		const bool buffer_overlap = cache.m_buffer_cache.HasPageOverlap(info.address, info.size);
		if (linear_size > info.size || cached.image->format != info.format ||
		    cached.image->extent.width != info.width ||
		    cached.image->extent.height != info.height ||
		    (tiled && info.bytes_per_element != 1 && info.bytes_per_element != 2 &&
		     info.bytes_per_element != 4 && info.bytes_per_element != 8) ||
		    meta_overlap || buffer_overlap) {
			EXIT("TextureCache: color-image readback storage is unsupported, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " linear=0x%016" PRIx64
			     " format=%d/%d extent=%ux%u/%ux%u bpe=%u tiled=%d meta=%d kind=%u\n",
			     info.address, info.size, linear_size, static_cast<int>(cached.image->format),
			     static_cast<int>(info.format), cached.image->extent.width,
			     cached.image->extent.height, info.width, info.height, info.bytes_per_element,
			     tiled, meta_overlap, static_cast<uint32_t>(cached.kind));
		}
		download.resize(info.size);
		std::fill(download.begin(), download.end(), 0);
		UtilFillBuffer(cached.ctx, download.data(), linear_size, info.pitch, cached.image,
		               cached.image->layout);
		if (tiled) {
			guest.resize(info.size);
			cache.m_tiler.TileImage(guest.data(), download.data(), cached.target);
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
			CachedImage* selected = cache.FindGpuReadbackPageCandidateLocked(vaddr, size);
			const bool   render_target =
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
			submissions_prepaused = false;
			state.store(State::Idle, std::memory_order_release);
			state.notify_all();
		}
	}

	TextureCache&        cache;
	std::atomic<State>   state {State::Idle};
	PageFaultAccess      access = PageFaultAccess::Unknown;
	uint64_t             vaddr  = 0;
	uint64_t             size   = 0;
	bool                 submissions_prepaused = false;
	std::vector<uint8_t> download;
	std::vector<uint8_t> guest;
	std::thread          thread;
};

void TextureCache::RetireImages(const std::vector<CachedImage*>& retire) {
	if (retire.empty()) {
		return;
	}
	// FindTexture and FindRenderTarget retain cache records on their recording commands. Erasing
	// the lookup owner therefore defers Vulkan destruction until the last referencing fence.
	size_t removed = 0;
	for (auto it = m_images.begin(); it != m_images.end();) {
		if (std::find(retire.begin(), retire.end(), it->get()) == retire.end()) {
			++it;
			continue;
		}
		const bool sampled = (*it)->kind == CachedImage::Kind::Texture;
		const bool target  = (*it)->kind == CachedImage::Kind::RenderTarget;
		if ((!sampled && !target) || (*it)->gpu_modified || (target && (*it)->buffer_modified)) {
			EXIT("TextureCache: invalid image retirement, kind=%u gpu_modified=%d "
			     "buffer_modified=%d\n",
			     static_cast<uint32_t>((*it)->kind), (*it)->gpu_modified, (*it)->buffer_modified);
		}
		for (uint32_t range = 0; range < (*it)->RangeCount(); range++) {
			if (target &&
			    m_memory_tracker.IsRegionGpuModified((*it)->Address(range), (*it)->Size(range))) {
				EXIT("TextureCache: clean target retirement retained tracker GPU ownership, "
				     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
				     (*it)->Address(range), (*it)->Size(range));
			}
			m_memory_tracker.UntrackMemory((*it)->Address(range), (*it)->Size(range));
		}
		it = m_images.erase(it);
		removed++;
	}
	if (removed != retire.size()) {
		EXIT("TextureCache: image retirement set mismatch, requested=%zu removed=%zu\n",
		     retire.size(), removed);
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

bool Equal(const RenderTargetInfo& left, const RenderTargetInfo& right) {
	return left.address == right.address && left.size == right.size &&
	       left.format == right.format && left.width == right.width &&
	       left.height == right.height && left.pitch == right.pitch &&
	       left.bytes_per_element == right.bytes_per_element && left.tile_mode == right.tile_mode &&
	       left.levels == right.levels;
}

bool Equal(const DepthTargetInfo& left, const DepthTargetInfo& right) {
	return left.address == right.address && left.size == right.size &&
	       left.stencil_address == right.stencil_address &&
	       left.stencil_size == right.stencil_size && left.htile_address == right.htile_address &&
	       left.htile_size == right.htile_size && left.format == right.format &&
	       left.guest_format == right.guest_format && left.width == right.width &&
	       left.height == right.height && left.pitch == right.pitch &&
	       left.bytes_per_element == right.bytes_per_element && left.tile_mode == right.tile_mode;
}

[[nodiscard]] bool IsCoherentGuestImageSource(const BufferImageCopySource& source,
                                              uint64_t address, uint64_t size) {
	// The current PS5 Tiler consumes coherent guest backing directly. A native buffer is an
	// optional future GPU-detiler source or staging fallback.
	return source.cpu_current && source.address == address && source.size == size &&
	       (source.buffer != nullptr || source.offset == 0);
}

VkImageAspectFlags DepthAspectMask(VkFormat format) {
	return VK_IMAGE_ASPECT_DEPTH_BIT |
	       (format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
	                format == VK_FORMAT_D32_SFLOAT_S8_UINT
	            ? VK_IMAGE_ASPECT_STENCIL_BIT
	            : 0u);
}

TextureImageCreateParams MakeImageParams(const ImageInfo& info, bool storage) {
	TextureImageCreateParams params {};
	params.fmt        = info.format;
	params.width      = info.width;
	params.height     = info.height;
	params.base_level = info.base_level;
	params.levels     = info.levels;
	params.depth      = info.depth;
	params.type       = info.type;
	// Storage image views use identity component mapping. The guest storage write mapping is
	// validated before this point and intentionally does not become a Vulkan view swizzle.
	params.swizzle               = storage ? DstSel(4, 5, 6, 7) : info.swizzle;
	params.format_usage          = TextureFormatUsage::Sampled | TextureFormatUsage::Storage;
	params.required_format_usage = storage
	                                   ? TextureFormatUsage::Sampled | TextureFormatUsage::Storage
	                                   : TextureFormatUsage::Sampled;
	params.view_usage      = storage ? TextureFormatUsage::Sampled | TextureFormatUsage::Storage
	                                 : TextureFormatUsage::Sampled;
	params.image_layout    = TextureUploadDestination::MipLevels;
	params.allow_cube_view = !storage;
	params.compatible_format_views =
	    storage && IsRgba8SrgbViewFormat(TextureGetFormat(info.format, params.format_usage));
	params.owner = storage ? "StorageTextureCache" : "TextureCache";
	return params;
}

bool FormatSupportsStorage(GraphicContext* ctx, VkFormat format) {
	VkFormatProperties properties {};
	vkGetPhysicalDeviceFormatProperties(ctx->physical_device, format, &properties);
	return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}

bool RenderTargetSupportsStorage(GraphicContext* ctx, VkFormat format, VkImageCreateFlags flags) {
	const auto compatible = BgraSrgbStorageViewFormat(format);
	const auto required_flags =
	    VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	const bool compatible_views = (flags & required_flags) == required_flags;
	return FormatSupportsStorage(ctx, format) ||
	       (compatible_views && compatible != VK_FORMAT_UNDEFINED &&
	        FormatSupportsStorage(ctx, compatible));
}

VkImageCreateFlags RenderTargetCreateFlags(VkFormat format) {
	return IsRgba8SrgbViewFormat(format)
	           ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT
	           : VkImageCreateFlags {0};
}

VkImageUsageFlags RenderTargetUsage(GraphicContext* ctx, VkFormat format,
                                    VkImageCreateFlags flags) {
	auto usage = static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
	             static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_SRC_BIT) |
	             static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_DST_BIT) |
	             static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_SAMPLED_BIT);
	if (RenderTargetSupportsStorage(ctx, format, flags)) {
		usage |= VK_IMAGE_USAGE_STORAGE_BIT;
	}
	VkImageFormatProperties properties {};
	if (vkGetPhysicalDeviceImageFormatProperties(ctx->physical_device, format, VK_IMAGE_TYPE_2D,
	                                             VK_IMAGE_TILING_OPTIMAL, usage, flags,
	                                             &properties) != VK_SUCCESS) {
		EXIT("TextureCache: render-target format does not support required usage, format=%d "
		     "usage=0x%x\n",
		     static_cast<int>(format), usage);
	}
	return usage;
}

void CreateRenderTargetView(GraphicContext* ctx, VulkanImage* image, int index,
                            VkComponentSwizzle r, VkComponentSwizzle g, VkComponentSwizzle b,
                            VkComponentSwizzle a, VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D,
                            VkFormat          view_format = VK_FORMAT_UNDEFINED,
                            VkImageUsageFlags view_usage = 0, uint32_t level_count = 0) {
	UtilCreateImageView(ctx, image, index, type, VK_IMAGE_ASPECT_COLOR_BIT, {r, g, b, a}, 0, 0, 1,
	                    level_count == 0 ? image->mip_levels : level_count, view_format,
	                    view_usage);
}

void CreateRenderAttachmentViews(GraphicContext* ctx, RenderTextureVulkanImage* image) {
	for (uint32_t level = 0; level < image->mip_levels; level++) {
		VkImageViewCreateInfo create {};
		create.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		create.image      = image->image;
		create.viewType   = VK_IMAGE_VIEW_TYPE_2D;
		create.format     = image->format;
		create.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
		                     VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
		create.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		create.subresourceRange.baseMipLevel   = level;
		create.subresourceRange.levelCount     = 1;
		create.subresourceRange.baseArrayLayer = 0;
		create.subresourceRange.layerCount     = 1;
		const auto result =
		    vkCreateImageView(ctx->device, &create, nullptr, &image->render_view[level]);
		if (result != VK_SUCCESS || image->render_view[level] == nullptr) {
			EXIT("TextureCache: failed to create render-target mip view, result=%d level=%u\n",
			     static_cast<int>(result), level);
		}
	}
}

void CreateRenderTargetViews(GraphicContext* ctx, RenderTextureVulkanImage* image) {
	CreateRenderTargetView(ctx, image, VulkanImage::VIEW_DEFAULT, VK_COMPONENT_SWIZZLE_IDENTITY,
	                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                       VK_COMPONENT_SWIZZLE_IDENTITY);
	CreateRenderTargetView(ctx, image, VulkanImage::VIEW_BGRA, VK_COMPONENT_SWIZZLE_B,
	                       VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R,
	                       VK_COMPONENT_SWIZZLE_IDENTITY);
	CreateRenderTargetView(ctx, image, VulkanImage::VIEW_R001, VK_COMPONENT_SWIZZLE_R,
	                       VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
	                       VK_COMPONENT_SWIZZLE_ONE);
	CreateRenderTargetView(ctx, image, VulkanImage::VIEW_RGB1, VK_COMPONENT_SWIZZLE_R,
	                       VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
	                       VK_COMPONENT_SWIZZLE_ONE);
	CreateRenderTargetView(ctx, image, VulkanImage::VIEW_R000, VK_COMPONENT_SWIZZLE_R,
	                       VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
	                       VK_COMPONENT_SWIZZLE_ZERO);
	CreateRenderTargetView(ctx, image, VulkanImage::VIEW_RG01, VK_COMPONENT_SWIZZLE_R,
	                       VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ZERO,
	                       VK_COMPONENT_SWIZZLE_ONE);
	CreateRenderTargetView(ctx, image, VulkanImage::VIEW_000R, VK_COMPONENT_SWIZZLE_ZERO,
	                       VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
	                       VK_COMPONENT_SWIZZLE_R);
	if (FormatSupportsStorage(ctx, image->format)) {
		CreateRenderTargetView(ctx, image, VulkanImage::VIEW_STORAGE, VK_COMPONENT_SWIZZLE_IDENTITY,
		                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
		                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_IMAGE_VIEW_TYPE_2D,
		                       VK_FORMAT_UNDEFINED, 0, 1);
	}
	const auto rgba_view_format = BgraToRgbaSampledViewFormat(image->format);
	if (rgba_view_format != VK_FORMAT_UNDEFINED) {
		CreateRenderTargetView(ctx, image, VulkanImage::VIEW_RGBA8_BGRA, VK_COMPONENT_SWIZZLE_B,
		                       VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R,
		                       VK_COMPONENT_SWIZZLE_A, VK_IMAGE_VIEW_TYPE_2D, rgba_view_format,
		                       VK_IMAGE_USAGE_SAMPLED_BIT);
	}
	CreateRenderAttachmentViews(ctx, image);
}

void CreateVideoOutViews(GraphicContext* ctx, VideoOutVulkanImage* image) {
	CreateRenderTargetView(ctx, image, VulkanImage::VIEW_DEFAULT, VK_COMPONENT_SWIZZLE_IDENTITY,
	                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                       VK_COMPONENT_SWIZZLE_IDENTITY);
	CreateRenderTargetView(ctx, image, VulkanImage::VIEW_BGRA, VK_COMPONENT_SWIZZLE_B,
	                       VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R,
	                       VK_COMPONENT_SWIZZLE_IDENTITY);
	CreateRenderTargetView(ctx, image, VulkanImage::VIEW_DEFAULT_ARRAY,
	                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                       VK_IMAGE_VIEW_TYPE_2D_ARRAY);
	CreateRenderTargetView(ctx, image, VulkanImage::VIEW_BGRA_ARRAY, VK_COMPONENT_SWIZZLE_B,
	                       VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R,
	                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
}

void UploadRenderTarget(GraphicContext* ctx, RenderTextureVulkanImage* image,
                        const RenderTargetInfo& info, bool refresh) {
	if (refresh) {
		VulkanDeviceWaitIdle(ctx);
	}
	image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (info.levels > 1) {
		uint32_t format = 0;
		switch (info.bytes_per_element) {
			case 1: format = Prospero::GpuEnumValue(Prospero::BufferFormat::k8UNorm); break;
			case 2: format = Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm); break;
			case 4: format = Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float); break;
			case 8: format = Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16_16_16Float); break;
			case 16: format = Prospero::GpuEnumValue(Prospero::BufferFormat::k32_32_32_32Float); break;
			default:
				EXIT("TextureCache: unsupported render-target mip element size: %u\n",
				     info.bytes_per_element);
		}
		auto layout = TextureCalcUploadLayout(format, info.width, info.height, info.levels, 1,
		                                      info.pitch, info.tile_mode, info.size, false, false,
		                                      false, "TextureCache render target");
		if (!layout.fmt_tiled_render_target || layout.pitch != info.pitch) {
			EXIT("TextureCache: unsupported render-target mip upload layout, pitch=%u/%u tile=%u\n",
			     info.pitch, layout.pitch, info.tile_mode);
		}
		auto regions = TextureBuildUploadRegions(
		    layout, info.format, info.width, info.height, 1, info.levels, false, false,
		    TextureUploadDestination::MipLevels, TextureUploadSliceLayout::MipChainPerSlice);
		TextureUploadGuestImage(ctx, image, reinterpret_cast<const void*>(info.address), info.size,
		                        regions, layout, format, info.width, info.height, 1, info.levels,
		                        TextureUploadSliceLayout::MipChainPerSlice,
		                        "TextureCache render target",
		                        static_cast<uint64_t>(VK_IMAGE_LAYOUT_GENERAL));
		return;
	}
	if (info.tile_mode == Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) &&
	    UtilBufferIsTiled(info.address, info.size)) {
		UtilScratchBuffer scratch(info.size);
		TileConvertTiledToLinearRenderTarget(
		    scratch.Data(), reinterpret_cast<const void*>(info.address), info.width, info.height,
		    info.pitch, info.bytes_per_element, info.size);
		UtilFillImage(ctx, image, scratch.Data(), info.size, info.pitch,
		              static_cast<uint64_t>(VK_IMAGE_LAYOUT_GENERAL));
	} else {
		UtilFillImage(ctx, image, reinterpret_cast<const void*>(info.address), info.size,
		              info.pitch, static_cast<uint64_t>(VK_IMAGE_LAYOUT_GENERAL));
	}
}

RenderTextureVulkanImage* CreateRenderTarget(GraphicContext* ctx, const RenderTargetInfo& info,
                                             VulkanMemory* memory) {
	auto* image          = new RenderTextureVulkanImage;
	image->extent.width  = info.width;
	image->extent.height = info.height;
	image->format        = info.format;
	image->mip_levels    = info.levels;
	image->layout        = VK_IMAGE_LAYOUT_UNDEFINED;
	UtilResetImageViews(image);
	VkImageCreateInfo create {};
	create.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.flags         = RenderTargetCreateFlags(info.format);
	create.imageType     = VK_IMAGE_TYPE_2D;
	create.extent        = {info.width, info.height, 1};
	create.mipLevels     = info.levels;
	create.arrayLayers   = 1;
	create.format        = info.format;
	create.tiling        = VK_IMAGE_TILING_OPTIMAL;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	create.usage         = RenderTargetUsage(ctx, info.format, create.flags);
	create.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	create.samples       = VK_SAMPLE_COUNT_1_BIT;
	memory->property     = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	if (!VulkanCreateImage(ctx, &create, image, memory)) {
		EXIT("TextureCache: failed to create render target, addr=0x%016" PRIx64
		     " extent=%ux%u format=%d\n",
		     info.address, info.width, info.height, static_cast<int>(info.format));
	}
	image->memory = *memory;
	CreateRenderTargetViews(ctx, image);
	return image;
}

void CreateDepthViews(GraphicContext* ctx, DepthStencilVulkanImage* image) {
	UtilCreateImageView(ctx, image, VulkanImage::VIEW_DEFAULT, VK_IMAGE_VIEW_TYPE_2D,
	                    DepthAspectMask(image->format),
	                    {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                     VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
	                    0, 0, 1, 1);
	UtilCreateImageView(ctx, image, VulkanImage::VIEW_DEPTH_TEXTURE, VK_IMAGE_VIEW_TYPE_2D,
	                    VK_IMAGE_ASPECT_DEPTH_BIT,
	                    {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
	                     VK_COMPONENT_SWIZZLE_R},
	                    0, 0, 1, 1);
	UtilCreateImageView(ctx, image, VulkanImage::VIEW_R000, VK_IMAGE_VIEW_TYPE_2D,
	                    VK_IMAGE_ASPECT_DEPTH_BIT,
	                    {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO,
	                     VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO},
	                    0, 0, 1, 1);
}

DepthStencilVulkanImage* CreateDepthTarget(GraphicContext* ctx, const DepthTargetInfo& info,
                                           VulkanMemory* memory) {
	VkImageCreateInfo create {};
	create.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.imageType     = VK_IMAGE_TYPE_2D;
	create.extent        = {info.width, info.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = 1;
	create.format        = info.format;
	create.tiling        = VK_IMAGE_TILING_OPTIMAL;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	create.usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
	                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.samples     = VK_SAMPLE_COUNT_1_BIT;
	VkImageFormatProperties properties {};
	if (vkGetPhysicalDeviceImageFormatProperties(ctx->physical_device, info.format,
	                                             VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
	                                             create.usage, 0, &properties) != VK_SUCCESS) {
		EXIT("TextureCache: depth format does not support required usage, format=%d usage=0x%x\n",
		     static_cast<int>(info.format), create.usage);
	}
	auto* image          = new DepthStencilVulkanImage;
	image->extent.width  = info.width;
	image->extent.height = info.height;
	image->guest_pitch   = info.pitch;
	image->format        = info.format;
	image->layout        = VK_IMAGE_LAYOUT_UNDEFINED;
	image->compressed    = false;
	UtilResetImageViews(image);
	memory->property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	if (!VulkanCreateImage(ctx, &create, image, memory)) {
		EXIT("TextureCache: failed to create depth target, addr=0x%016" PRIx64
		     " extent=%ux%u format=%d\n",
		     info.address, info.width, info.height, static_cast<int>(info.format));
	}
	image->memory = *memory;
	CreateDepthViews(ctx, image);
	return image;
}

void ValidateVideoOutInfo(GraphicContext* ctx, const VideoOutInfo& info) {
	const auto compression =
	    ClassifyVideoOutCompression(info.compression != VideoOutCompression::Uncompressed,
	                                info.metadata_address, info.dcc_control, 0);
	const bool metadata_invalid = compression == VideoOutCompression::Dcc256_64_64 &&
	                              (info.metadata_address >= TRACKER_ADDRESS_SIZE ||
	                               (info.metadata_address >= info.address &&
	                                info.metadata_address < info.address + info.size));
	if (ctx == nullptr || info.address == 0 || info.size == 0 ||
	    info.address >= TRACKER_ADDRESS_SIZE || info.size > TRACKER_ADDRESS_SIZE - info.address ||
	    (info.address & 0xffffu) != 0 || info.width == 0 || info.height == 0 ||
	    info.width > 16384 || info.height > 16384 || info.pitch < info.width ||
	    info.tile_mode != Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) || info.bytes_per_element != 4 ||
	    compression == VideoOutCompression::Unsupported || compression != info.compression ||
	    metadata_invalid || !IsSupportedVideoOutFormat(info.guest_format, info.format)) {
		EXIT("TextureCache: unsupported video-out surface, ctx=%p addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 " metadata=0x%016" PRIx64 " dcc=0x%08" PRIx32
		     " extent=%ux%u pitch=%u tile=%u guest_format=%u bpe=%u vk_format=%d\n",
		     static_cast<const void*>(ctx), info.address, info.size, info.metadata_address,
		     info.dcc_control, info.width, info.height, info.pitch, info.tile_mode,
		     info.guest_format, info.bytes_per_element, static_cast<int>(info.format));
	}
	TileSizeAlign exact {};
	TileGetTextureTotalSize(info.guest_format, info.width, info.height, 1, info.pitch, 1,
	                        info.tile_mode, false, &exact);
	if (exact.align != 65536 || exact.size != info.size ||
	    TileGetTexturePitch(info.guest_format, info.width, 1, info.tile_mode) != info.pitch) {
		EXIT("TextureCache: video-out tile layout mismatch, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 " expected_size=0x%016" PRIx64 " align=0x%016" PRIx64
		     " pitch=%u\n",
		     info.address, info.size, exact.size, exact.align, info.pitch);
	}
	(void)RenderTargetUsage(ctx, info.format, 0);
}

VideoOutVulkanImage* CreateVideoOut(GraphicContext* ctx, const VideoOutInfo& info,
                                    VulkanMemory* memory) {
	auto* image          = new VideoOutVulkanImage;
	image->extent.width  = info.width;
	image->extent.height = info.height;
	image->format        = info.format;
	image->layout        = VK_IMAGE_LAYOUT_UNDEFINED;
	UtilResetImageViews(image);
	VkImageCreateInfo create {};
	create.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.imageType     = VK_IMAGE_TYPE_2D;
	create.extent        = {info.width, info.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = 1;
	create.format        = info.format;
	create.tiling        = VK_IMAGE_TILING_OPTIMAL;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	create.usage         = RenderTargetUsage(ctx, info.format, 0);
	create.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	create.samples       = VK_SAMPLE_COUNT_1_BIT;
	memory->property     = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	if (!VulkanCreateImage(ctx, &create, image, memory)) {
		EXIT("TextureCache: failed to create video-out image, addr=0x%016" PRIx64
		     " extent=%ux%u format=%d\n",
		     info.address, info.width, info.height, static_cast<int>(info.format));
	}
	image->memory = *memory;
	CreateVideoOutViews(ctx, image);
	return image;
}

void UploadVideoOut(GraphicContext* ctx, VideoOutVulkanImage* image, const VideoOutInfo& info,
                    bool refresh) {
	if (info.compression != VideoOutCompression::Uncompressed) {
		EXIT("TextureCache: compressed video-out guest upload is unsupported, "
		     "addr=0x%016" PRIx64 " metadata=0x%016" PRIx64 " dcc=0x%08" PRIx32 "\n",
		     info.address, info.metadata_address, info.dcc_control);
	}
	if (refresh) {
		VulkanDeviceWaitIdle(ctx);
	}
	image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	UtilScratchBuffer scratch(info.size);
	TileConvertTiledToLinearRenderTarget(
	    scratch.Data(), reinterpret_cast<const void*>(info.address), info.width, info.height,
	    info.pitch, info.bytes_per_element, info.size);
	UtilFillImage(ctx, image, scratch.Data(), info.size, info.pitch,
	              static_cast<uint64_t>(VK_IMAGE_LAYOUT_GENERAL));
}

} // namespace

TextureCache::TextureCache(PageManager& page_manager, BufferCache& buffer_cache,
	                       ResourceMutex& resource_mutex)
    : m_memory_tracker(page_manager), m_metadata_tracker(page_manager, PageWatchMode::Write),
      m_buffer_cache(buffer_cache), m_resource_mutex(resource_mutex) {
	if (!Common::Thread::IsMainThread()) {
		EXIT("TextureCache: construction is restricted to the main thread\n");
	}
	m_readback = std::make_unique<ReadbackWorker>(*this);
}

VkImageView TextureCache::GetRenderTargetAttachmentView(GraphicContext*           ctx,
                                                        RenderTextureVulkanImage* image,
                                                        VkFormat format, uint32_t level) {
	if (ctx == nullptr || image == nullptr || image->image == nullptr ||
	    format == VK_FORMAT_UNDEFINED || level >= image->mip_levels || level >= 16) {
		EXIT("TextureCache: invalid render-target attachment view, image=%p format=%d"
		     " level=%u image_levels=%u\n",
		     static_cast<const void*>(image), static_cast<int>(format), level,
		     image != nullptr ? image->mip_levels : 0);
	}
	if (format == image->format) {
		if (image->render_view[level] == nullptr) {
			EXIT("TextureCache: base render-target attachment view is missing, level=%u\n", level);
		}
		return image->render_view[level];
	}
	if (!IsRgba8SrgbReinterpretation(image->format, format)) {
		EXIT("TextureCache: incompatible render-target attachment view, image_format=%d"
		     " view_format=%d level=%u\n",
		     static_cast<int>(image->format), static_cast<int>(format), level);
	}

	std::lock_guard lock(image->attachment_view_mutex);
	for (const auto& cached: image->attachment_views) {
		if (cached.format == format && cached.level == level) {
			return cached.view;
		}
	}

	VkImageViewUsageCreateInfo usage {};
	usage.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
	usage.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	VkImageViewCreateInfo create {};
	create.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create.pNext      = &usage;
	create.image      = image->image;
	create.viewType   = VK_IMAGE_VIEW_TYPE_2D;
	create.format     = format;
	create.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                     VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
	create.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	create.subresourceRange.baseMipLevel   = level;
	create.subresourceRange.levelCount     = 1;
	create.subresourceRange.baseArrayLayer = 0;
	create.subresourceRange.layerCount     = 1;
	VkImageView view                       = nullptr;
	const auto  result = vkCreateImageView(ctx->device, &create, nullptr, &view);
	if (result != VK_SUCCESS || view == nullptr) {
		EXIT("TextureCache: failed to create compatible render-target attachment view,"
		     " result=%d image_format=%d view_format=%d level=%u\n",
		     static_cast<int>(result), static_cast<int>(image->format), static_cast<int>(format),
		     level);
	}
	image->attachment_views.push_back({format, level, view});
	LOGF("TextureCache: created compatible render-target attachment view:"
	     " image_format=%d view_format=%d level=%u extent=%ux%u\n",
	     static_cast<int>(image->format), static_cast<int>(format), level, image->extent.width,
	     image->extent.height);
	return view;
}

VkImageView TextureCache::GetRenderTargetSampledView(GraphicContext*           ctx,
                                                     RenderTextureVulkanImage* image,
                                                     VkFormat view_format, int variant,
                                                     uint32_t base_level, uint32_t level_count) {
	if (ctx == nullptr || image == nullptr || image->image == nullptr || base_level >= 16 ||
	    view_format == VK_FORMAT_UNDEFINED || level_count == 0 ||
	    base_level + level_count > image->mip_levels) {
		EXIT("TextureCache: invalid render-target sampled view, image=%p variant=%d"
		     " view_format=%d base=%u count=%u image_levels=%u\n",
		     static_cast<const void*>(image), variant, static_cast<int>(view_format), base_level,
		     level_count, image != nullptr ? image->mip_levels : 0);
	}
	const bool compatible_variant_format =
	    view_format == image->format || (variant == VulkanImage::VIEW_RGBA8_BGRA &&
	                                     IsBgraToRgba8SampledView(image->format, view_format));
	if (!compatible_variant_format) {
		EXIT("TextureCache: incompatible render-target sampled view format, image_format=%d"
		     " view_format=%d variant=%d\n",
		     static_cast<int>(image->format), static_cast<int>(view_format), variant);
	}
	const auto default_view_format = variant == VulkanImage::VIEW_RGBA8_BGRA
	                                     ? BgraToRgbaSampledViewFormat(image->format)
	                                     : image->format;
	if (view_format == default_view_format && base_level == 0 && level_count == image->mip_levels) {
		if (variant < 0 || variant >= VulkanImage::VIEW_MAX ||
		    image->image_view[variant] == nullptr) {
			EXIT("TextureCache: full render-target sampled view is missing, variant=%d\n", variant);
		}
		return image->image_view[variant];
	}

	std::lock_guard lock(image->sampled_view_mutex);
	for (const auto& cached: image->sampled_views) {
		if (cached.format == view_format && cached.variant == variant &&
		    cached.base_level == base_level && cached.level_count == level_count) {
			return cached.view;
		}
	}

	VkComponentMapping components {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
	switch (variant) {
		case VulkanImage::VIEW_DEFAULT: break;
		case VulkanImage::VIEW_BGRA:
			components = {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R,
			              VK_COMPONENT_SWIZZLE_IDENTITY};
			break;
		case VulkanImage::VIEW_R001:
			components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO,
			              VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE};
			break;
		case VulkanImage::VIEW_RGB1:
			components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
			              VK_COMPONENT_SWIZZLE_ONE};
			break;
		case VulkanImage::VIEW_R000:
			components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO,
			              VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO};
			break;
		case VulkanImage::VIEW_RG01:
			components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ZERO,
			              VK_COMPONENT_SWIZZLE_ONE};
			break;
		case VulkanImage::VIEW_000R:
			components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
			              VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
			break;
		case VulkanImage::VIEW_RGBA8_BGRA:
			if (!IsBgraToRgba8SampledView(image->format, view_format)) {
				EXIT("TextureCache: incompatible mutable render-target sampled view\n");
			}
			components = {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R,
			              VK_COMPONENT_SWIZZLE_A};
			break;
		default:
			EXIT("TextureCache: unsupported render-target sampled view variant: %d\n", variant);
	}

	VkImageViewCreateInfo      create {};
	VkImageViewUsageCreateInfo usage {};
	usage.sType                            = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
	usage.usage                            = VK_IMAGE_USAGE_SAMPLED_BIT;
	create.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create.pNext                           = &usage;
	create.image                           = image->image;
	create.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
	create.format                          = view_format;
	create.components                      = components;
	create.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	create.subresourceRange.baseMipLevel   = base_level;
	create.subresourceRange.levelCount     = level_count;
	create.subresourceRange.baseArrayLayer = 0;
	create.subresourceRange.layerCount     = 1;
	VkImageView view                       = nullptr;
	const auto  result = vkCreateImageView(ctx->device, &create, nullptr, &view);
	if (result != VK_SUCCESS || view == nullptr) {
		EXIT("TextureCache: failed to create render-target sampled subresource view,"
		     " result=%d view_format=%d variant=%d base=%u count=%u\n",
		     static_cast<int>(result), static_cast<int>(view_format), variant, base_level,
		     level_count);
	}
	image->sampled_views.push_back({view_format, base_level, level_count, variant, view});
	return view;
}

VkImageView TextureCache::GetRenderTargetStorageView(GraphicContext*           ctx,
                                                     RenderTextureVulkanImage* image,
                                                     VkFormat view_format, uint32_t base_level,
                                                     uint32_t level_count) {
	if (ctx == nullptr || image == nullptr || image->image == nullptr ||
	    view_format == VK_FORMAT_UNDEFINED || level_count == 0 ||
	    base_level + level_count > image->mip_levels) {
		EXIT("TextureCache: invalid render-target storage view, image=%p view_format=%d"
		     " base=%u count=%u image_levels=%u\n",
		     static_cast<const void*>(image), static_cast<int>(view_format), base_level,
		     level_count, image != nullptr ? image->mip_levels : 0);
	}
	const bool exact      = view_format == image->format;
	const bool compatible = view_format == BgraSrgbStorageViewFormat(image->format);
	if (!exact && !compatible) {
		EXIT("TextureCache: incompatible render-target storage view, image_format=%d"
		     " view_format=%d base=%u count=%u\n",
		     static_cast<int>(image->format), static_cast<int>(view_format), base_level,
		     level_count);
	}
	if (exact) {
		if (image->image_view[VulkanImage::VIEW_STORAGE] == nullptr) {
			EXIT("TextureCache: base render-target storage view is missing\n");
		}
		if (base_level == 0 && level_count == 1) {
			return image->image_view[VulkanImage::VIEW_STORAGE];
		}
	}

	std::lock_guard lock(image->storage_view_mutex);
	for (const auto& cached: image->storage_views) {
		if (cached.format == view_format && cached.base_level == base_level &&
		    cached.level_count == level_count) {
			return cached.view;
		}
	}
	if (compatible && !FormatSupportsStorage(ctx, view_format)) {
		EXIT("TextureCache: compatible render-target storage format lacks storage support,"
		     " image_format=%d view_format=%d base=%u count=%u\n",
		     static_cast<int>(image->format), static_cast<int>(view_format), base_level,
		     level_count);
	}

	VkImageViewUsageCreateInfo usage {};
	usage.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
	usage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
	VkImageViewCreateInfo create {};
	create.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create.pNext      = &usage;
	create.image      = image->image;
	create.viewType   = VK_IMAGE_VIEW_TYPE_2D;
	create.format     = view_format;
	create.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                     VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
	create.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	create.subresourceRange.baseMipLevel   = base_level;
	create.subresourceRange.levelCount     = level_count;
	create.subresourceRange.baseArrayLayer = 0;
	create.subresourceRange.layerCount     = 1;
	VkImageView view                       = nullptr;
	const auto  result = vkCreateImageView(ctx->device, &create, nullptr, &view);
	if (result != VK_SUCCESS || view == nullptr) {
		EXIT("TextureCache: failed to create compatible render-target storage view,"
		     " result=%d image_format=%d view_format=%d base=%u count=%u\n",
		     static_cast<int>(result), static_cast<int>(image->format),
		     static_cast<int>(view_format), base_level, level_count);
	}
	image->storage_views.push_back({view_format, base_level, level_count, view});
	LOGF("TextureCache: created compatible render-target storage view: image_format=%d"
	     " view_format=%d base=%u count=%u extent=%ux%u\n",
	     static_cast<int>(image->format), static_cast<int>(view_format), base_level, level_count,
	     image->extent.width, image->extent.height);
	return view;
}

VkImageView TextureCache::GetStorageTextureSampledView(GraphicContext*            ctx,
                                                       StorageTextureVulkanImage* image,
                                                       const ImageInfo&           info) {
	const bool image_2d = info.type == Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) && info.depth == 1;
	const bool image_3d = info.type == Prospero::GpuEnumValue(Prospero::ImageType::kColor3D) && info.depth != 0;
	if (ctx == nullptr || image == nullptr || image->image == nullptr || (!image_2d && !image_3d) ||
	    info.base_array != 0 || info.levels != image->mip_levels ||
	    info.base_level >= info.levels || info.view_levels == 0 ||
	    info.base_level + info.view_levels > info.levels) {
		EXIT("TextureCache: invalid sampled view of storage texture, image=%p type=%u depth=%u"
		     " base=%u levels=%u view_levels=%u image_levels=%u base_array=%u\n",
		     static_cast<const void*>(image), info.type, info.depth, info.base_level, info.levels,
		     info.view_levels, image != nullptr ? image->mip_levels : 0, info.base_array);
	}
	const auto view_format = TextureGetFormat(info.format, TextureFormatUsage::Sampled);
	if (view_format != image->format && !IsRgba8SrgbReinterpretation(image->format, view_format)) {
		EXIT("TextureCache: incompatible sampled view of storage texture, image_format=%d"
		     " view_format=%d swizzle=0x%03x\n",
		     static_cast<int>(image->format), static_cast<int>(view_format), info.swizzle);
	}

	std::lock_guard lock(image->sampled_view_mutex);
	for (const auto& cached: image->sampled_views) {
		if (cached.format == view_format && cached.swizzle == info.swizzle &&
		    cached.type == info.type && cached.base_level == info.base_level &&
		    cached.level_count == info.view_levels) {
			return cached.view;
		}
	}

	VkImageViewUsageCreateInfo usage {};
	usage.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
	usage.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	VkImageViewCreateInfo create {};
	create.sType                         = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create.pNext                         = &usage;
	create.image                         = image->image;
	create.viewType                      = image_3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
	create.format                        = view_format;
	create.components                    = TextureGetComponentMapping(info.swizzle);
	create.subresourceRange.aspectMask   = VK_IMAGE_ASPECT_COLOR_BIT;
	create.subresourceRange.baseMipLevel = info.base_level;
	create.subresourceRange.levelCount   = info.view_levels;
	create.subresourceRange.baseArrayLayer = 0;
	create.subresourceRange.layerCount     = 1;
	VkImageView view                       = nullptr;
	const auto  result = vkCreateImageView(ctx->device, &create, nullptr, &view);
	if (result != VK_SUCCESS || view == nullptr) {
		EXIT("TextureCache: failed to create sampled view of storage texture, result=%d"
		     " image_format=%d view_format=%d swizzle=0x%03x type=%u base=%u count=%u\n",
		     static_cast<int>(result), static_cast<int>(image->format),
		     static_cast<int>(view_format), info.swizzle, info.type, info.base_level,
		     info.view_levels);
	}
	image->sampled_views.push_back(
	    {view_format, info.swizzle, info.type, info.base_level, info.view_levels, view});
	LOGF("TextureCache: created sampled view of storage texture: image_format=%d"
	     " view_format=%d swizzle=0x%03x type=%u base=%u count=%u\n",
	     static_cast<int>(image->format), static_cast<int>(view_format), info.swizzle, info.type,
	     info.base_level, info.view_levels);
	return view;
}

TextureCache::CachedImage* TextureCache::FindGpuReadbackPageCandidateLocked(uint64_t vaddr,
                                                                            uint64_t size) const {
	CachedImage* selected = nullptr;
	for (const auto& cached: m_images) {
		if (!cached->IsGpuReadbackPageCandidate(vaddr, size)) {
			continue;
		}
		if (selected != nullptr) {
			EXIT("TextureCache: CPU fault has multiple GPU-modified image page candidates, "
			     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 " first=%p second=%p\n",
			     vaddr, size, static_cast<const void*>(selected),
			     static_cast<const void*>(cached.get()));
		}
		selected = cached.get();
	}
	return selected;
}

void TextureCache::MarkSampledAliasesCpuDirtyLocked(uint64_t vaddr, uint64_t size) {
	for (auto& cached: m_images) {
		if (cached->kind != CachedImage::Kind::Texture) {
			continue;
		}
		if (cached->gpu_modified &&
		    ImagePageRangesOverlap(cached->info.address, cached->info.size, vaddr, size)) {
			EXIT("TextureCache: CPU write overlaps a GPU-modified sampled texture, "
			     "write=0x%016" PRIx64 "+0x%016" PRIx64 " image=0x%016" PRIx64 "+0x%016" PRIx64
			     "\n",
			     vaddr, size, cached->info.address, cached->info.size);
		}
		cached->info.InvalidateCpuWrite(vaddr, size);
	}
}

TextureCache::~TextureCache() {
	m_readback.reset();
	GraphicContext* ctx = nullptr;
	if (!m_images.empty()) {
		ctx = m_images.front()->ctx;
	} else {
		ctx = m_dummy_ctx;
	}
	if (ctx != nullptr) {
		VulkanDeviceWaitIdle(ctx);
	}
	m_images.clear();
	for (size_t i = 0; i < m_dummy_sampled_textures.size(); i++) {
		if (m_dummy_sampled_textures[i] != nullptr) {
			DeleteGpuTexture(ctx, static_cast<GpuTextureVulkanImage*>(m_dummy_sampled_textures[i]),
			                 m_dummy_sampled_memory[i]);
			delete m_dummy_sampled_memory[i];
		}
		if (m_dummy_storage_textures[i] != nullptr) {
			DeleteGpuTexture(ctx, static_cast<GpuTextureVulkanImage*>(m_dummy_storage_textures[i]),
			                 m_dummy_storage_memory[i]);
			delete m_dummy_storage_memory[i];
		}
	}
}

VulkanImage* TextureCache::FindTexture(CommandBuffer* command, GraphicContext* ctx,
                                       const ImageInfo& info, bool metadata_read) {
	if (command == nullptr || ctx == nullptr || info.address == 0 || info.size == 0 ||
	    info.address >= TRACKER_ADDRESS_SIZE || info.size > TRACKER_ADDRESS_SIZE - info.address ||
	    info.width == 0 || info.height == 0 || info.depth == 0 || info.levels == 0 ||
	    info.view_levels == 0 || info.base_level + info.view_levels > info.levels) {
		EXIT("TextureCache: invalid sampled-image request, command=%p ctx=%p addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 " extent=%ux%ux%u levels=%u\n",
		     static_cast<const void*>(command), static_cast<const void*>(ctx), info.address,
		     info.size, info.width, info.height, info.depth, info.levels);
	}
	std::lock_guard       transaction(m_resource_mutex);
	BufferImageCopySource source {nullptr, 0, info.address, info.size, true};
	if (m_buffer_cache.HasPageOverlap(info.address, info.size)) {
		// ObtainBufferForImage publishes dirty native-buffer bytes when necessary and otherwise
		// uses a CPU-current staging fallback through coherent guest backing.
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
	const auto requested_view_format = TextureGetFormat(info.format, TextureFormatUsage::Sampled);
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
			EXIT("TextureCache: sampled image shares pages with GPU target, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " target_kind=%u\n",
			     info.address, info.size, static_cast<uint32_t>(cached->kind));
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
	uint64_t readable = 0;
	if (!HostMemoryQueryReadable(info.address, info.size, &readable) || readable < info.size) {
		EXIT("TextureCache: new sampled image has unreadable guest backing, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 " readable=0x%016" PRIx64
		     " format=%u extent=%ux%ux%u pitch=%u levels=%u tile=%u type=%u\n",
		     info.address, info.size, readable, info.format, info.width, info.height, info.depth,
		     info.pitch, info.levels, info.tile, info.type);
	}

	m_images.reserve(m_images.size() + 1);
	auto cached   = std::make_shared<CachedImage>();
	cached->kind  = CachedImage::Kind::Texture;
	cached->info  = info;
	cached->ctx   = ctx;
	cached->image = new TextureVulkanImage;
	const auto components =
	    TextureCreateImage(ctx, cached->image, &cached->memory, MakeImageParams(info, false));
	m_memory_tracker.ForEachUploadRange(
	    info.address, info.size, false, [](uint64_t, uint64_t) noexcept {},
	    [&]() noexcept {
		    m_tiler.DetileImage(cached->ctx,
		                               static_cast<GpuTextureVulkanImage*>(cached->image),
		                               cached->info, source, false, false);
	    });
	TextureCreateImageViews(ctx, cached->image, components, info.type, info.base_array,
	                        info.base_level, info.view_levels, info.depth, true,
	                        TextureFormatUsage::Sampled);
	auto* image = cached->image;
	command->RetainResourceUntilFence(cached);
	m_images.push_back(std::move(cached));
	return image;
}

StorageTextureVulkanImage* TextureCache::FindStorageTexture(CommandBuffer*   command,
                                                            GraphicContext*  ctx,
                                                            const ImageInfo& info) {
	const bool supported_type = info.type == Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) ||
	                            info.type == Prospero::GpuEnumValue(Prospero::ImageType::kColor2DArray) ||
	                            info.type == Prospero::GpuEnumValue(Prospero::ImageType::kColor3D);
	if (command == nullptr || ctx == nullptr || info.address == 0 || info.size == 0 ||
	    info.address >= TRACKER_ADDRESS_SIZE || info.size > TRACKER_ADDRESS_SIZE - info.address ||
	    info.width == 0 || info.height == 0 || info.depth == 0 || info.levels != 1 ||
	    info.base_level != 0 || info.view_levels != 1 || info.base_array != 0 || !supported_type ||
	    (info.tile != Prospero::GpuEnumValue(Prospero::TileMode::kLinear) &&
	     info.tile != Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget)) ||
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
	    (info.type == Prospero::GpuEnumValue(Prospero::ImageType::kColor2DArray) && info.base_array >= info.depth)) {
		EXIT("TextureCache: storage-image type and depth disagree, type=%u depth=%u\n", info.type,
		     info.depth);
	}

	std::lock_guard transaction(m_resource_mutex);
	if (m_buffer_cache.HasPageOverlap(info.address, info.size)) {
		EXIT("TextureCache: storage image aliases buffer pages, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     info.address, info.size);
	}
	FaultSafeTextureLock lock(this, m_lock);
	RequireNoMetaOverlapLocked(info.address, info.size);

	std::shared_ptr<CachedImage> match;
	for (auto& cached: m_images) {
		if (cached->kind != CachedImage::Kind::StorageTexture || !Equal(info, cached->info)) {
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
		if (match->gpu_modified != gpu_modified || (gpu_modified && cpu_modified)) {
			EXIT("TextureCache: storage-image ownership is inconsistent, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64
			     " cached_gpu_modified=%d tracker_gpu_modified=%d cpu_modified=%d\n",
			     info.address, info.size, match->gpu_modified, gpu_modified, cpu_modified);
		}
		if (!gpu_modified) {
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

	for (const auto& cached: m_images) {
		if (cached->OverlapsRange(info.address, info.size, true)) {
			EXIT("TextureCache: unsupported storage-image alias, requested=0x%016" PRIx64
			     "+0x%016" PRIx64 " existing=0x%016" PRIx64 "+0x%016" PRIx64
			     " kind=%u gpu_modified=%d\n",
			     info.address, info.size, cached->Address(), cached->Size(),
			     static_cast<uint32_t>(cached->kind), cached->gpu_modified);
		}
	}
	uint64_t readable = 0;
	if (!HostMemoryQueryReadable(info.address, info.size, &readable) || readable < info.size) {
		EXIT("TextureCache: new storage image has unreadable guest backing, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 " readable=0x%016" PRIx64 "\n",
		     info.address, info.size, readable);
	}

	m_images.reserve(m_images.size() + 1);
	auto cached   = std::make_shared<CachedImage>();
	cached->kind  = CachedImage::Kind::StorageTexture;
	cached->info  = info;
	cached->ctx   = ctx;
	cached->image = new StorageTextureVulkanImage;
	const auto components =
	    TextureCreateImage(ctx, cached->image, &cached->memory, MakeImageParams(info, true));
	m_memory_tracker.ForEachUploadRange(
	    info.address, info.size, true, [](uint64_t, uint64_t) noexcept {},
	    [&]() noexcept {
		    m_tiler.DetileImage(
		        cached->ctx, static_cast<GpuTextureVulkanImage*>(cached->image), cached->info,
		        {nullptr, 0, cached->info.address, cached->info.size, true}, false, true);
	    });
	cached->gpu_modified = true;
	TextureCreateImageViews(ctx, cached->image, components, info.type, 0, 0, 1, info.depth, false,
	                        TextureFormatUsage::Sampled | TextureFormatUsage::Storage);
	auto* image = static_cast<StorageTextureVulkanImage*>(cached->image);
	command->RetainResourceUntilFence(cached);
	m_images.push_back(std::move(cached));
	return image;
}

RenderTextureVulkanImage* TextureCache::FindRenderTarget(CommandBuffer*          command,
                                                         GraphicContext*         ctx,
                                                         const RenderTargetInfo& info) {
	if (command == nullptr || ctx == nullptr || info.address == 0 || info.size == 0 ||
	    info.address >= TRACKER_ADDRESS_SIZE || info.size > TRACKER_ADDRESS_SIZE - info.address ||
	    info.format == VK_FORMAT_UNDEFINED || info.width == 0 || info.height == 0 ||
	    info.pitch < info.width || info.bytes_per_element == 0 || info.levels == 0 ||
	    info.levels > 16 ||
	    (info.tile_mode != Prospero::GpuEnumValue(Prospero::TileMode::kLinear) &&
	     info.tile_mode != Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget))) {
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
		    layout.size != info.size) {
			EXIT("TextureCache: unsupported render-target mip layout, size=0x%016" PRIx64
			     " expected=0x%08x extent=%ux%u pitch=%u bpe=%u levels=%u tile=%u\n",
			     info.size, layout.size, info.width, info.height, info.pitch,
			     info.bytes_per_element, info.levels, info.tile_mode);
		}
	}
	const auto rows = static_cast<uint64_t>(info.height - 1);
	if (rows > (UINT64_MAX - info.width) / info.pitch) {
		EXIT("TextureCache: render-target element count overflow, extent=%ux%u pitch=%u\n",
		     info.width, info.height, info.pitch);
	}
	const auto elements = rows * info.pitch + info.width;
	if (elements > UINT64_MAX / info.bytes_per_element ||
	    info.size < elements * info.bytes_per_element) {
		EXIT("TextureCache: invalid render-target storage, size=0x%016" PRIx64
		     " elements=0x%016" PRIx64 " bpe=%u\n",
		     info.size, elements, info.bytes_per_element);
	}
	std::lock_guard       transaction(m_resource_mutex);
	BufferImageCopySource target_source {nullptr, 0, info.address, info.size, true};
	if (m_buffer_cache.HasPageOverlap(info.address, info.size)) {
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
		    (!Equal(info, cached->target) && !IsCompatibleRenderTargetView(cached->target, info))) {
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
				    UploadRenderTarget(ctx, static_cast<RenderTextureVulkanImage*>(match->image),
				                       match->target, true);
			    });
			match->buffer_modified = false;
		}
		command->RetainResourceUntilFence(match);
		return static_cast<RenderTextureVulkanImage*>(match->image);
	}
	std::vector<CachedImage*> retire;
	for (const auto& entry: m_images) {
		auto& cached = *entry;
		if (!cached.OverlapsRange(info.address, info.size, true)) {
			continue;
		}
		const auto overlap =
		    cached.kind == CachedImage::Kind::Texture
		        ? ClassifyRenderTargetOverlap(cached.info, cached.gpu_modified, cached.ctx == ctx,
		                                      info)
		    : cached.kind == CachedImage::Kind::RenderTarget
		        ? ClassifyRenderTargetOverlap(cached.target, cached.gpu_modified,
		                                      cached.buffer_modified, cached.ctx == ctx, info)
		        : RenderTargetOverlap::Unsupported;
		const bool retire_sampled = cached.kind == CachedImage::Kind::Texture &&
		                            overlap == RenderTargetOverlap::RetireSampled;
		const bool retire_target  = cached.kind == CachedImage::Kind::RenderTarget &&
		                            overlap == RenderTargetOverlap::RetireTarget;
		if (!retire_sampled && !retire_target) {
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
	std::vector<ImageRetirementRange> ranges;
	ranges.reserve(m_images.size() * 2);
	for (const auto& entry: m_images) {
		const bool retiring = std::find(retire.begin(), retire.end(), entry.get()) != retire.end();
		for (uint32_t range = 0; range < entry->RangeCount(); range++) {
			ranges.push_back({entry->Address(range), entry->Size(range), retiring});
		}
	}
	const auto conflict = FindImageRetirementConflict(ranges);
	if (conflict.Exists()) {
		const auto& retired  = ranges[conflict.retired];
		const auto& retained = ranges[conflict.retained];
		EXIT("TextureCache: sampled retirement leaves a tracked page alias, target=0x%016" PRIx64
		     "+0x%016" PRIx64 " retired=0x%016" PRIx64 "+0x%016" PRIx64 " retained=0x%016" PRIx64
		     "+0x%016" PRIx64 "\n",
		     info.address, info.size, retired.address, retired.size, retained.address,
		     retained.size);
	}
	RetireImages(retire);
	auto cached    = std::make_shared<CachedImage>();
	cached->kind   = CachedImage::Kind::RenderTarget;
	cached->target = info;
	cached->ctx    = ctx;
	cached->image  = CreateRenderTarget(ctx, info, &cached->memory);
	m_memory_tracker.ForEachUploadRange(
	    info.address, info.size, false, [](uint64_t, uint64_t) noexcept {},
	    [&]() noexcept {
		    UploadRenderTarget(ctx, static_cast<RenderTextureVulkanImage*>(cached->image), info,
		                       false);
	    });
	auto* image = static_cast<RenderTextureVulkanImage*>(cached->image);
	command->RetainResourceUntilFence(cached);
	m_images.push_back(std::move(cached));
	return image;
}

DepthStencilVulkanImage* TextureCache::FindDepthTarget(CommandBuffer* command, GraphicContext* ctx,
                                                       const DepthTargetInfo& info) {
	const bool has_stencil = info.stencil_address != 0 || info.stencil_size != 0;
	const bool has_htile   = info.htile_address != 0 || info.htile_size != 0;
	if (command == nullptr || ctx == nullptr || info.address == 0 || info.size == 0 ||
	    info.address >= TRACKER_ADDRESS_SIZE || info.size > TRACKER_ADDRESS_SIZE - info.address ||
	    (info.address & 0xffffu) != 0 || info.width == 0 || info.height == 0 ||
	    info.pitch < info.width || info.tile_mode != Prospero::GpuEnumValue(Prospero::TileMode::kDepth) ||
	    (has_stencil &&
	     (info.stencil_address == 0 || info.stencil_size == 0 ||
	      info.stencil_address >= TRACKER_ADDRESS_SIZE ||
	      info.stencil_size > TRACKER_ADDRESS_SIZE - info.stencil_address ||
	      (info.stencil_address & 0xffffu) != 0 ||
	      PageOverlaps(info.address, info.size, info.stencil_address, info.stencil_size))) ||
	    ((info.htile_address == 0) != (info.htile_size == 0)) ||
	    (has_htile && (info.htile_address >= TRACKER_ADDRESS_SIZE ||
	                   info.htile_size > TRACKER_ADDRESS_SIZE - info.htile_address ||
	                   (info.htile_address & 0x7fffu) != 0 ||
	                   PageOverlaps(info.address, info.size, info.htile_address, info.htile_size) ||
	                   (has_stencil && PageOverlaps(info.stencil_address, info.stencil_size,
	                                                info.htile_address, info.htile_size)))) ||
	    !(info.guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm)
	          ? !has_stencil && info.format == VK_FORMAT_D16_UNORM && info.bytes_per_element == 2
	          : info.guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float) &&
	                info.bytes_per_element == 4 &&
	                info.format ==
	                    (has_stencil ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT))) {
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
	    info.size < elements * info.bytes_per_element) {
		EXIT("TextureCache: depth storage is too small, size=0x%016" PRIx64
		     " elements=0x%016" PRIx64 " bpe=%u\n",
		     info.size, elements, info.bytes_per_element);
	}
	if (has_stencil && info.stencil_size < elements) {
		EXIT("TextureCache: stencil storage is too small, size=0x%016" PRIx64
		     " elements=0x%016" PRIx64 "\n",
		     info.stencil_size, elements);
	}
	if (has_htile) {
		RegisterMeta(info.htile_address, info.htile_size);
	}
	std::lock_guard transaction(m_resource_mutex);
	const auto      depth_source = m_buffer_cache.ObtainBufferForImage(info.address, info.size);
	if (has_stencil) {
		(void)m_buffer_cache.ObtainBufferForImage(info.stencil_address, info.stencil_size);
	}
	FaultSafeTextureLock lock(this, m_lock);
	RequireNoMetaOverlapLocked(info.address, info.size);
	if (has_stencil) {
		RequireNoMetaOverlapLocked(info.stencil_address, info.stencil_size);
	}
	CachedImage* match = nullptr;
	for (auto& cached: m_images) {
		if (cached->kind != CachedImage::Kind::DepthTarget || !Equal(info, cached->depth)) {
			continue;
		}
		match = cached.get();
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
		if (!match->gpu_modified && m_memory_tracker.IsRegionCpuModified(info.address, info.size)) {
			if (has_stencil && match->stencil_initialized && !info.stencil_load_clear) {
				EXIT("TextureCache: depth refresh would discard initialized stencil, "
				     "addr=0x%016" PRIx64 "\n",
				     info.address);
			}
			m_memory_tracker.ForEachUploadRange(
			    info.address, info.size, false, [](uint64_t, uint64_t) noexcept {},
			    [&]() noexcept {
				    m_tiler.DetileImage(ctx,
				                               static_cast<DepthStencilVulkanImage*>(match->image),
				                               match->depth, depth_source, true);
			    });
		}
		if (!match->gpu_modified && has_stencil &&
		    m_memory_tracker.IsRegionCpuModified(info.stencil_address, info.stencil_size)) {
			if (!info.stencil_load_clear) {
				if (info.stencil_access) {
					EXIT("TextureCache: stencil refresh from guest memory is unsupported, "
					     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
					     info.stencil_address, info.stencil_size);
				}
			} else {
				m_memory_tracker.ForEachUploadRange(
				    info.stencil_address, info.stencil_size, false,
				    [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
			}
		}
		return static_cast<DepthStencilVulkanImage*>(match->image);
	}
	std::vector<CachedImage*> retire;
	for (const auto& entry: m_images) {
		auto&      cached = *entry;
		const bool overlaps =
		    cached.OverlapsRange(info.address, info.size, true) ||
		    (has_stencil && cached.OverlapsRange(info.stencil_address, info.stencil_size, true));
		if (!overlaps) {
			continue;
		}
		if (cached.kind != CachedImage::Kind::Texture || cached.ctx != ctx ||
		    ClassifyDepthOverlap(cached.info, cached.gpu_modified, info) !=
		        DepthOverlap::RetireSampled) {
			EXIT("TextureCache: unsupported depth-target alias, depth=0x%016" PRIx64
			     "+0x%016" PRIx64 " existing_kind=%u existing=0x%016" PRIx64 "+0x%016" PRIx64 "\n",
			     info.address, info.size, static_cast<uint32_t>(cached.kind), cached.Address(),
			     cached.Size());
		}
		retire.push_back(&cached);
	}
	std::vector<ImageRetirementRange> ranges;
	ranges.reserve(m_images.size() * 2);
	for (const auto& entry: m_images) {
		const bool retiring = std::find(retire.begin(), retire.end(), entry.get()) != retire.end();
		for (uint32_t range = 0; range < entry->RangeCount(); range++) {
			ranges.push_back({entry->Address(range), entry->Size(range), retiring});
		}
	}
	const auto conflict = FindImageRetirementConflict(ranges);
	if (conflict.Exists()) {
		const auto& retired  = ranges[conflict.retired];
		const auto& retained = ranges[conflict.retained];
		EXIT("TextureCache: sampled retirement leaves a tracked page alias, depth=0x%016" PRIx64
		     "+0x%016" PRIx64 " retired=0x%016" PRIx64 "+0x%016" PRIx64 " retained=0x%016" PRIx64
		     "+0x%016" PRIx64 "\n",
		     info.address, info.size, retired.address, retired.size, retained.address,
		     retained.size);
	}
	RetireImages(retire);
	if (!CanLoadStencilAttachment(info, false)) {
		EXIT("TextureCache: new stencil target requires a clear before stencil access, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     info.stencil_address, info.stencil_size);
	}
	auto cached                 = std::make_shared<CachedImage>();
	cached->kind                = CachedImage::Kind::DepthTarget;
	cached->depth               = info;
	cached->ctx                 = ctx;
	cached->stencil_initialized = !has_stencil || info.stencil_load_clear;
	cached->image               = CreateDepthTarget(ctx, info, &cached->memory);
	m_memory_tracker.ForEachUploadRange(
	    info.address, info.size, false, [](uint64_t, uint64_t) noexcept {},
	    [&]() noexcept {
		    if (!info.depth_load_clear) {
			    m_tiler.DetileImage(ctx,
			                               static_cast<DepthStencilVulkanImage*>(cached->image),
			                               info, depth_source, false);
		    }
	    });
	if (has_stencil) {
		m_memory_tracker.ForEachUploadRange(
		    info.stencil_address, info.stencil_size, false, [](uint64_t, uint64_t) noexcept {},
		    []() noexcept {});
	}
	auto* image = static_cast<DepthStencilVulkanImage*>(cached->image);
	m_images.push_back(std::move(cached));
	return image;
}

std::vector<VideoOutVulkanImage*>
TextureCache::RegisterVideoOutSurfaces(GraphicContext*                  ctx,
                                       const std::vector<VideoOutInfo>& infos) {
	if (infos.empty()) {
		EXIT("TextureCache: video-out registration requires surfaces\n");
	}
	for (const auto& info: infos) {
		ValidateVideoOutInfo(ctx, info);
	}
	for (size_t i = 0; i < infos.size(); i++) {
		for (size_t j = i + 1; j < infos.size(); j++) {
			if (PageOverlaps(infos[i].address, infos[i].size, infos[j].address, infos[j].size)) {
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
		cached->image     = CreateVideoOut(ctx, info, &cached->memory);
		if (info.compression == VideoOutCompression::Uncompressed) {
			m_memory_tracker.ForEachUploadRange(
			    info.address, info.size, false, [](uint64_t, uint64_t) noexcept {},
			    [&]() noexcept {
				    UploadVideoOut(ctx, static_cast<VideoOutVulkanImage*>(cached->image), info,
				                   false);
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
	    [&]() noexcept { UploadVideoOut(cached.ctx, image, info, true); });
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
	VulkanDeviceWaitIdle(ctx);
	for (auto* cached: selected) {
		if (cached->gpu_modified) {
			m_memory_tracker.UnmarkRegionAsGpuModified(cached->Address(), cached->Size());
			cached->gpu_modified = false;
		}
		m_memory_tracker.UntrackMemory(cached->Address(), cached->Size());
	}
	for (auto* image: images) {
		auto it = std::find_if(m_images.begin(), m_images.end(),
		                       [image](const auto& cached) { return cached->image == image; });
		m_images.erase(it);
	}
}

bool TextureCache::ClearColorImageFromBuffer(CommandBuffer* command, uint64_t vaddr, uint64_t size,
                                             uint32_t packed_clear) {
	if (command == nullptr || command->IsInvalid() || vaddr == 0 || size == 0 ||
	    vaddr >= TRACKER_ADDRESS_SIZE || size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("TextureCache: invalid compute image clear, command=%p addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     static_cast<const void*>(command), vaddr, size);
	}
	m_buffer_cache.ValidateGpuAccess(vaddr, size, false, true);
	std::lock_guard              transaction(m_resource_mutex);
	FaultSafeTextureLock         lock(this, m_lock);
	std::shared_ptr<CachedImage> match;
	for (const auto& cached: m_images) {
		if (!cached->OverlapsRange(vaddr, size, true)) {
			continue;
		}
		const bool color = cached->kind == CachedImage::Kind::VideoOut ||
		                   cached->kind == CachedImage::Kind::RenderTarget;
		const bool exact = cached->Address() == vaddr && cached->Size() == size;
		if (!color || !exact) {
			return false;
		}
		if (match != nullptr) {
			EXIT("TextureCache: compute image clear has ambiguous exact aliases, "
			     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 " first=%p second=%p\n",
			     vaddr, size, static_cast<const void*>(match->image),
			     static_cast<const void*>(cached->image));
		}
		match = cached;
	}
	if (match == nullptr) {
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
		// A prior buffer clear can leave one clean cached buffer after this range becomes a native
		// color target. Allow coexisting cache views only after the shared ownership
		// helper proves the stale source is coherent native-buffer or guest backing.
		source = m_buffer_cache.ObtainBufferForImage(vaddr, size);
	}
	const bool buffer_source_valid =
	    !buffer_overlap || IsCoherentGuestImageSource(source, vaddr, size);
	const bool image_cpu_modified = m_memory_tracker.IsRegionCpuModified(vaddr, size);
	if (!buffer_source_valid || buffer_cpu_modified || buffer_gpu_modified ||
	    match->buffer_modified || image_cpu_modified) {
		EXIT("TextureCache: compute image clear requires exclusive GPU image ownership, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     " buffer_overlap=%d source_valid=%d buffer_cpu_modified=%d"
		     " buffer_gpu_modified=%d buffer_modified=%d image_cpu_modified=%d\n",
		     vaddr, size, buffer_overlap, buffer_source_valid, buffer_cpu_modified,
		     buffer_gpu_modified, match->buffer_modified, image_cpu_modified);
	}
	VkClearColorValue clear {};
	if (!DecodePackedColorClear(match->image->format, packed_clear, &clear)) {
		return false;
	}
	auto* vk_buffer = command->GetPool()->buffers[command->GetIndex()];
	GraphicsRenderColorImageBarrier(vk_buffer, match->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	const VkImageSubresourceRange range {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
	                                     match->image->layers};
	vkCmdClearColorImage(vk_buffer, match->image->image, match->image->layout, &clear, 1, &range);
	GraphicsRenderColorImageBarrier(vk_buffer, match->image, RENDER_COLOR_IMAGE_LAYOUT);
	if (!match->gpu_modified) {
		m_memory_tracker.MarkRegionAsGpuModified(vaddr, size);
		match->gpu_modified = true;
	}
	command->RetainResourceUntilFence(match);
	return true;
}

bool TextureCache::ClearDepthImageFromBuffer(CommandBuffer* command, uint64_t vaddr, uint64_t size,
                                             uint32_t packed_clear) {
	if (command == nullptr || command->IsInvalid() || vaddr == 0 || size == 0 ||
	    vaddr >= TRACKER_ADDRESS_SIZE || size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("TextureCache: invalid compute depth clear, command=%p addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     static_cast<const void*>(command), vaddr, size);
	}
	m_buffer_cache.ValidateGpuAccess(vaddr, size, false, true);
	std::lock_guard              transaction(m_resource_mutex);
	FaultSafeTextureLock         lock(this, m_lock);
	std::shared_ptr<CachedImage> match;
	for (const auto& cached: m_images) {
		if (!cached->OverlapsRange(vaddr, size, true)) {
			continue;
		}
		const bool exact_depth = cached->kind == CachedImage::Kind::DepthTarget &&
		                         CanNativeClearDepthFromBuffer(cached->depth, vaddr, size);
		if (!exact_depth) {
			return false;
		}
		if (match != nullptr) {
			EXIT("TextureCache: compute depth clear has ambiguous exact aliases, "
			     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 " first=%p second=%p\n",
			     vaddr, size, static_cast<const void*>(match->image),
			     static_cast<const void*>(cached->image));
		}
		match = cached;
	}
	if (match == nullptr) {
		return false;
	}
	float depth_clear = 0.0f;
	if (!DecodePackedDepthClear(match->image->format, packed_clear, &depth_clear)) {
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
	if (!match->gpu_modified || !buffer_source_valid || buffer_cpu_modified ||
	    buffer_gpu_modified || match->buffer_modified || image_cpu_modified) {
		EXIT("TextureCache: compute depth clear requires a GPU-owned depth image and a clean "
		     "buffer source, addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     " gpu_modified=%d buffer_overlap=%d source_valid=%d buffer_cpu_modified=%d"
		     " buffer_gpu_modified=%d buffer_modified=%d image_cpu_modified=%d\n",
		     vaddr, size, match->gpu_modified, buffer_overlap, buffer_source_valid,
		     buffer_cpu_modified, buffer_gpu_modified, match->buffer_modified, image_cpu_modified);
	}
	if (match->image->layout == VK_IMAGE_LAYOUT_UNDEFINED ||
	    match->image->layout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
		EXIT("TextureCache: compute depth clear has invalid source layout %u\n",
		     static_cast<uint32_t>(match->image->layout));
	}
	auto*      vk_buffer  = command->GetPool()->buffers[command->GetIndex()];
	const auto old_layout = match->image->layout;
	GraphicsRenderDepthStencilImageBarrier(vk_buffer, match->image,
	                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	const VkClearDepthStencilValue clear {depth_clear, 0};
	const VkImageSubresourceRange  range {VK_IMAGE_ASPECT_DEPTH_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
	                                      match->image->layers};
	vkCmdClearDepthStencilImage(vk_buffer, match->image->image, match->image->layout, &clear, 1,
	                            &range);
	GraphicsRenderDepthStencilImageBarrier(vk_buffer, match->image, old_layout);
	command->RetainResourceUntilFence(match);
	return true;
}

bool TextureCache::ClearStencilImageFromBuffer(CommandBuffer* command, uint64_t vaddr,
                                               uint64_t size, uint32_t packed_clear) {
	if (command == nullptr || command->IsInvalid() || vaddr == 0 || size == 0 ||
	    vaddr >= TRACKER_ADDRESS_SIZE || size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("TextureCache: invalid compute stencil clear, command=%p addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     static_cast<const void*>(command), vaddr, size);
	}
	uint8_t stencil_clear = 0;
	if (!DecodePackedStencilClear(packed_clear, &stencil_clear)) {
		return false;
	}
	m_buffer_cache.ValidateGpuAccess(vaddr, size, false, true);
	std::lock_guard              transaction(m_resource_mutex);
	FaultSafeTextureLock         lock(this, m_lock);
	std::shared_ptr<CachedImage> match;
	for (const auto& cached: m_images) {
		if (!cached->OverlapsRange(vaddr, size, true)) {
			continue;
		}
		const bool exact_stencil = cached->kind == CachedImage::Kind::DepthTarget &&
		                           cached->RangeCount() == 2 && cached->Address(1) == vaddr &&
		                           cached->Size(1) == size;
		if (!exact_stencil) {
			return false;
		}
		if (match != nullptr) {
			EXIT("TextureCache: compute stencil clear has ambiguous exact aliases, "
			     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 " first=%p second=%p\n",
			     vaddr, size, static_cast<const void*>(match->image),
			     static_cast<const void*>(cached->image));
		}
		match = cached;
	}
	if (match == nullptr) {
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
	if (!match->gpu_modified || !buffer_source_valid || buffer_cpu_modified ||
	    buffer_gpu_modified || match->buffer_modified || image_cpu_modified) {
		EXIT("TextureCache: compute stencil clear requires a GPU-owned depth image and a clean "
		     "stencil source, addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     " gpu_modified=%d buffer_overlap=%d source_valid=%d buffer_cpu_modified=%d"
		     " buffer_gpu_modified=%d buffer_modified=%d image_cpu_modified=%d\n",
		     vaddr, size, match->gpu_modified, buffer_overlap, buffer_source_valid,
		     buffer_cpu_modified, buffer_gpu_modified, match->buffer_modified, image_cpu_modified);
	}
	if (match->image->layout == VK_IMAGE_LAYOUT_UNDEFINED ||
	    match->image->layout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
		EXIT("TextureCache: compute stencil clear has invalid source layout %u\n",
		     static_cast<uint32_t>(match->image->layout));
	}
	auto*      vk_buffer  = command->GetPool()->buffers[command->GetIndex()];
	const auto old_layout = match->image->layout;
	GraphicsRenderDepthStencilImageBarrier(vk_buffer, match->image,
	                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	const VkClearDepthStencilValue clear {0.0f, stencil_clear};
	const VkImageSubresourceRange range {VK_IMAGE_ASPECT_STENCIL_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
	                                     match->image->layers};
	vkCmdClearDepthStencilImage(vk_buffer, match->image->image, match->image->layout, &clear, 1,
	                            &range);
	GraphicsRenderDepthStencilImageBarrier(vk_buffer, match->image, old_layout);
	match->stencil_initialized = true;
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
				const bool cpu_modified = m_metadata_tracker.IsRegionCpuModified(
				    cached->depth.htile_address, cached->depth.htile_size);
				if (meta->second.gpu_modified && cpu_modified) {
					EXIT("TextureCache: depth write races CPU-modified HTile metadata, "
					     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
					     cached->depth.htile_address, cached->depth.htile_size);
				}
				if (!meta->second.gpu_modified) {
					m_metadata_tracker.ForEachUploadRange(
					    cached->depth.htile_address, cached->depth.htile_size, true,
					    [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
					meta->second.gpu_modified = true;
				}
				meta->second.clear_mask = 0;
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
			const bool host_refreshable = (cached->kind == CachedImage::Kind::Texture ||
			                               cached->kind == CachedImage::Kind::RenderTarget) &&
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

void TextureCache::SynchronizeRenderTargetToBufferLocked(CachedImage& cached) {
	const auto&   target = cached.target;
	const bool    linear = target.tile_mode == Prospero::GpuEnumValue(Prospero::TileMode::kLinear);
	const bool    tiled  = target.tile_mode == Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
	const auto    rows   = static_cast<uint64_t>(target.height - 1);
	TileSizeAlign exact {};
	const bool    exact_tiled = tiled &&
	                            TileGetRenderTargetSize(target.width, target.height, target.pitch,
	                                                    target.bytes_per_element, &exact) &&
	                            exact.align == 65536 && exact.size == target.size;
	if (cached.kind != CachedImage::Kind::RenderTarget || !cached.gpu_modified ||
	    cached.buffer_modified || cached.ctx == nullptr || cached.image == nullptr ||
	    (!linear && !exact_tiled) || target.address == 0 || target.size == 0 || target.width == 0 ||
	    target.height == 0 || target.pitch < target.width || target.bytes_per_element == 0 ||
	    target.levels != 1 || rows > (UINT64_MAX - target.width) / target.pitch) {
		EXIT("TextureCache: unsupported render-target buffer synchronization, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     " extent=%ux%u pitch=%u bpe=%u levels=%u tile=%u"
		     " gpu_modified=%d buffer_modified=%d\n",
		     target.address, target.size, target.width, target.height, target.pitch,
		     target.bytes_per_element, target.levels, target.tile_mode, cached.gpu_modified,
		     cached.buffer_modified);
	}
	const auto linear_elements = rows * target.pitch + target.width;
	if (linear_elements > UINT64_MAX / target.bytes_per_element) {
		EXIT("TextureCache: render-target buffer synchronization size overflow\n");
	}
	const auto linear_size = linear_elements * target.bytes_per_element;
	if (linear_size > target.size || cached.image->format != target.format ||
	    cached.image->extent.width != target.width ||
	    cached.image->extent.height != target.height ||
	    (tiled && target.bytes_per_element != 1 && target.bytes_per_element != 2 &&
	     target.bytes_per_element != 4 && target.bytes_per_element != 8) ||
	    HasMetaOverlapLocked(target.address, target.size)) {
		EXIT("TextureCache: render-target buffer synchronization storage mismatch, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 " linear=0x%016" PRIx64 "\n",
		     target.address, target.size, linear_size);
	}

	// This is the CPU Tiler backend for the image-to-buffer synchronization seam.
	// The vectors and Vulkan staging allocation retain capacity; a future PS5 GPU tiler can replace
	// this block without changing alias classification or ownership transitions.
	VulkanDeviceWaitIdle(cached.ctx);
	m_buffer_transition_linear.resize(target.size);
	std::fill(m_buffer_transition_linear.begin(), m_buffer_transition_linear.end(), 0);
	UtilFillBuffer(cached.ctx, m_buffer_transition_linear.data(), linear_size, target.pitch,
	               cached.image, cached.image->layout);
	if (tiled) {
		m_buffer_transition_guest.resize(target.size);
		m_tiler.TileImage(m_buffer_transition_guest.data(),
		                         m_buffer_transition_linear.data(), target);
		Libs::LibKernel::Memory::WriteBacking(target.address, m_buffer_transition_guest.data(),
		                                       target.size);
	} else {
		Libs::LibKernel::Memory::WriteBacking(target.address, m_buffer_transition_linear.data(),
		                                       target.size);
	}
	m_memory_tracker.ForEachDownloadRange<true>(target.address, target.size,
	                                            [](uint64_t, uint64_t) noexcept {});
	if (m_memory_tracker.IsRegionGpuModified(target.address, target.size)) {
		EXIT("TextureCache: render-target buffer synchronization retained GPU ownership\n");
	}
	m_buffer_cache.PublishImageBacking(target.address, target.size);
	cached.gpu_modified    = false;
	cached.buffer_modified = true;
}

void TextureCache::InvalidateMemoryFromGPU(uint64_t vaddr, uint64_t size,
                                           bool formatted_buffer_write) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("TextureCache: invalid GPU invalidation range, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	FaultSafeTextureLock lock(this, m_lock);
	auto                 match         = m_images.end();
	BufferImageAlias     action        = BufferImageAlias::None;
	bool                 sampled_write = false;
	for (auto it = m_images.begin(); it != m_images.end(); ++it) {
		auto& cached = **it;
		if (!cached.OverlapsRange(vaddr, size, true)) {
			continue;
		}
		const auto alias =
		    ClassifyBufferImageAlias(vaddr, size, cached.Address(), cached.Size(),
		                             cached.kind == CachedImage::Kind::VideoOut,
		                             cached.kind == CachedImage::Kind::RenderTarget,
		                             cached.gpu_modified, true, formatted_buffer_write);
		const bool sampled = cached.kind == CachedImage::Kind::Texture &&
		                     IsSampledBufferWriteTransition(vaddr, size, cached.Address(),
		                                                    cached.Size(), cached.gpu_modified);
		if (match != m_images.end() || (!sampled && alias != BufferImageAlias::VideoOutWrite &&
		                                alias != BufferImageAlias::RenderTargetWrite)) {
			EXIT("TextureCache: unsupported GPU invalidation alias, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " cached_kind=%u cached=0x%016" PRIx64 "+0x%016" PRIx64
			     " gpu_modified=%d buffer_modified=%d formatted=%d ambiguous=%d\n",
			     vaddr, size, static_cast<uint32_t>(cached.kind), cached.Address(), cached.Size(),
			     cached.gpu_modified, cached.buffer_modified, formatted_buffer_write,
			     match != m_images.end());
		}
		match         = it;
		action        = sampled ? BufferImageAlias::None : alias;
		sampled_write = sampled;
	}
	if (match == m_images.end()) {
		return;
	}
	if (HasMetaOverlapLocked(vaddr, size)) {
		EXIT("TextureCache: GPU invalidation overlaps metadata, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	if (sampled_write) {
		(**match).buffer_modified = true;
		return;
	}
	if (action == BufferImageAlias::VideoOutWrite) {
		auto& cached           = **match;
		cached.buffer_modified = true;
		return;
	}
	if (action == BufferImageAlias::RenderTargetWrite) {
		SynchronizeRenderTargetToBufferLocked(**match);
		return;
	}
	m_memory_tracker.UntrackMemory(vaddr, size);
	m_images.erase(match);
}

DepthStencilVulkanImage* TextureCache::FindDepthTargetByRange(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("TextureCache: invalid depth-target range query, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	FaultSafeTextureLock     lock(this, m_lock);
	DepthStencilVulkanImage* found = nullptr;
	for (auto& cached: m_images) {
		if (cached->kind != CachedImage::Kind::DepthTarget ||
		    !cached->OverlapsRange(vaddr, size, false)) {
			continue;
		}
		if (!IsDepthTargetRangeCompatible(cached->depth, vaddr, size) || found != nullptr) {
			EXIT("TextureCache: incompatible or ambiguous depth-target range, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " cached=0x%016" PRIx64 "+0x%016" PRIx64 " previous=%p\n",
			     vaddr, size, cached->depth.address, cached->depth.size,
			     static_cast<const void*>(found));
		}
		const bool stencil_range =
		    vaddr == cached->depth.stencil_address && size == cached->depth.stencil_size;
		if (stencil_range && !cached->stencil_initialized) {
			EXIT("TextureCache: sampled stencil range is uninitialized, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     vaddr, size);
		}
		found = static_cast<DepthStencilVulkanImage*>(cached->image);
	}
	return found;
}

RenderTextureVulkanImage* TextureCache::FindRenderTargetByRange(CommandBuffer* command,
                                                                uint64_t vaddr, uint64_t size) {
	if (command == nullptr || vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("TextureCache: invalid render-target range query, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	FaultSafeTextureLock         lock(this, m_lock);
	std::shared_ptr<CachedImage> found;
	for (auto& cached: m_images) {
		if (cached->kind != CachedImage::Kind::RenderTarget ||
		    !Overlaps(vaddr, size, cached->Address(), cached->Size())) {
			continue;
		}
		if (vaddr != cached->Address() || size != cached->Size() || found != nullptr) {
			EXIT("TextureCache: ambiguous or partial render-target range, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " cached=0x%016" PRIx64 "+0x%016" PRIx64 " previous=%p\n",
			     vaddr, size, cached->Address(), cached->Size(),
			     static_cast<const void*>(found.get()));
		}
		found = cached;
	}
	if (found == nullptr) {
		return nullptr;
	}
	command->RetainResourceUntilFence(found);
	return static_cast<RenderTextureVulkanImage*>(found->image);
}

bool TextureCache::HasGpuTargetPageOverlap(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("TextureCache: invalid GPU-target overlap query, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	FaultSafeTextureLock lock(this, m_lock);
	for (const auto& cached: m_images) {
		if (cached->kind != CachedImage::Kind::Texture &&
		    cached->OverlapsRange(vaddr, size, true)) {
			return true;
		}
	}
	return false;
}

bool TextureCache::HasPageOverlap(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("TextureCache: invalid image page-overlap query, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	FaultSafeTextureLock lock(this, m_lock);
	for (const auto& cached: m_images) {
		if (cached->OverlapsRange(vaddr, size, true)) {
			return true;
		}
	}
	return false;
}

bool TextureCache::HasRangeOverlap(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("TextureCache: invalid image-range-overlap query, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	FaultSafeTextureLock lock(this, m_lock);
	for (const auto& cached: m_images) {
		if (cached->OverlapsRange(vaddr, size, false)) {
			return true;
		}
	}
	return false;
}

bool TextureCache::HasGpuModifiedRangeOverlap(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("TextureCache: invalid GPU-modified image-overlap query, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	FaultSafeTextureLock lock(this, m_lock);
	for (const auto& cached: m_images) {
		if (cached->gpu_modified && cached->OverlapsRange(vaddr, size, false)) {
			return true;
		}
	}
	return false;
}

void TextureCache::RegisterMeta(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr || (vaddr & 0x7fffu) != 0) {
		EXIT("TextureCache: invalid metadata registration, addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     "\n",
		     vaddr, size);
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
	const auto           existing = m_surface_metas.find(vaddr);
	if (existing != m_surface_metas.end()) {
		if (existing->second.size != size) {
			EXIT("TextureCache: metadata address reused with a different size, addr=0x%016" PRIx64
			     " requested=0x%016" PRIx64 " existing=0x%016" PRIx64 "\n",
			     vaddr, size, existing->second.size);
		}
		return;
	}
	for (const auto& [address, meta]: m_surface_metas) {
		if (PageOverlaps(vaddr, size, address, meta.size)) {
			LOGF("TextureCache: registering overlapping metadata views, requested=0x%016" PRIx64
			     "+0x%016" PRIx64 " existing=0x%016" PRIx64 "+0x%016" PRIx64 "\n",
			     vaddr, size, address, meta.size);
		}
	}
	std::vector<CachedImage*> retire;
	for (const auto& cached: m_images) {
		if (!cached->OverlapsRange(vaddr, size, true)) {
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
			     vaddr, size, static_cast<uint32_t>(cached->kind), cached->Address(),
			     cached->Size(), cached->gpu_modified, cached->buffer_modified);
		}
		if (overlap == MetaImageOverlap::RetireTarget) {
			retire.push_back(cached.get());
			continue;
		}
		LOGF("TextureCache: metadata aliases a CPU-current sampled image, metadata=0x%016" PRIx64
		     "+0x%016" PRIx64 " image=0x%016" PRIx64 "+0x%016" PRIx64 "\n",
		     vaddr, size, cached->Address(), cached->Size());
	}
	if (!retire.empty()) {
		std::vector<ImageRetirementRange> ranges;
		ranges.reserve(m_images.size() * 2);
		for (const auto& cached: m_images) {
			const bool retiring =
			    std::find(retire.begin(), retire.end(), cached.get()) != retire.end();
			for (uint32_t range = 0; range < cached->RangeCount(); range++) {
				ranges.push_back({cached->Address(range), cached->Size(range), retiring});
			}
		}
		const auto conflict = FindImageRetirementConflict(ranges);
		if (conflict.Exists()) {
			const auto& retired  = ranges[conflict.retired];
			const auto& retained = ranges[conflict.retained];
			EXIT("TextureCache: metadata retirement leaves a tracked page alias, "
			     "metadata=0x%016" PRIx64 "+0x%016" PRIx64 " retired=0x%016" PRIx64 "+0x%016" PRIx64
			     " retained=0x%016" PRIx64 "+0x%016" PRIx64 "\n",
			     vaddr, size, retired.address, retired.size, retained.address, retained.size);
		}
		for (const auto* cached: retire) {
			LOGF("TextureCache: retiring a CPU-current render target for metadata reuse, "
			     "metadata=0x%016" PRIx64 "+0x%016" PRIx64 " target=0x%016" PRIx64 "+0x%016" PRIx64
			     "\n",
			     vaddr, size, cached->Address(), cached->Size());
		}
		RetireImages(retire);
	}
	m_surface_metas.emplace(vaddr, MetaDataInfo {size});
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

bool TextureCache::HasMetaOverlap(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("TextureCache: invalid metadata-overlap query, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	FaultSafeTextureLock lock(this, m_lock);
	for (const auto& [address, meta]: m_surface_metas) {
		if (PageOverlaps(vaddr, size, address, meta.size)) {
			return true;
		}
	}
	return false;
}

bool TextureCache::IsMetaGpuModified(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("TextureCache: invalid metadata ownership query, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	FaultSafeTextureLock lock(this, m_lock);
	for (const auto& [address, meta]: m_surface_metas) {
		if (PageOverlaps(vaddr, size, address, meta.size) &&
		    m_metadata_tracker.IsRegionGpuModified(vaddr, size)) {
			return true;
		}
	}
	return false;
}

bool TextureCache::HasMetaOverlapLocked(uint64_t vaddr, uint64_t size) const {
	for (const auto& [address, meta]: m_surface_metas) {
		if (PageOverlaps(vaddr, size, address, meta.size)) {
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
	if (slice >= 32) {
		EXIT("TextureCache: metadata clear slice out of range, addr=0x%016" PRIx64 " slice=%u\n",
		     vaddr, slice);
	}
	FaultSafeTextureLock lock(this, m_lock);
	const auto           it = m_surface_metas.find(vaddr);
	if (it != m_surface_metas.end() && it->second.gpu_modified &&
	    m_metadata_tracker.IsRegionCpuModified(vaddr, it->second.size)) {
		EXIT("TextureCache: GPU-owned metadata became CPU-modified, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, it->second.size);
	}
	return it != m_surface_metas.end() && (it->second.clear_mask & (1u << slice)) != 0;
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
	it->second.clear_mask   = UINT32_MAX;
	return true;
}

bool TextureCache::TouchMeta(uint64_t vaddr, uint32_t slice, bool is_clear) {
	if (slice >= 32) {
		EXIT("TextureCache: metadata update slice out of range, addr=0x%016" PRIx64 " slice=%u\n",
		     vaddr, slice);
	}
	FaultSafeTextureLock lock(this, m_lock);
	const auto           it = m_surface_metas.find(vaddr);
	if (it == m_surface_metas.end()) {
		return false;
	}
	if (!it->second.gpu_modified) {
		EXIT("TextureCache: metadata update requires GPU ownership, addr=0x%016" PRIx64 "\n",
		     vaddr);
	}
	if (m_metadata_tracker.IsRegionCpuModified(vaddr, it->second.size)) {
		EXIT("TextureCache: metadata update races CPU modification, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, it->second.size);
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
		if (PageOverlaps(vaddr, size, address, meta.size) &&
		    (vaddr > address || end < address + meta.size)) {
			EXIT("TextureCache: partial metadata unmap is unsupported, unmap=0x%016" PRIx64
			     "+0x%016" PRIx64 " metadata=0x%016" PRIx64 "+0x%016" PRIx64 "\n",
			     vaddr, size, address, meta.size);
		}
	}
	for (const auto& cached: m_images) {
		for (uint32_t i = 0; i < cached->RangeCount(); i++) {
			if (PageOverlaps(vaddr, size, cached->Address(i), cached->Size(i)) &&
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
		const bool retiring = Overlaps(vaddr, size, address, meta.size) ||
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
		VulkanDeviceWaitIdle(wait_ctx);
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
	for (auto& cached: m_images) {
		if (!cached->OverlapsRange(vaddr, size, false)) {
			continue;
		}
		for (uint32_t i = 0; i < cached->RangeCount(); i++) {
			m_memory_tracker.UntrackMemory(cached->Address(i), cached->Size(i));
		}
	}
	for (auto it = m_surface_metas.begin(); it != m_surface_metas.end();) {
		const bool allocation_unmapped = Overlaps(vaddr, size, it->first, it->second.size);
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
		it = m_images.erase(it);
	}
}

static constexpr int DummyTextureIndex(bool uint_format, bool image_3d) {
	return (image_3d ? 2 : 0) + (uint_format ? 1 : 0);
}

static constexpr uint32_t DummyTextureSwizzle() {
	return Prospero::GpuEnumValue(Prospero::CompSwizzle::kRed) | (Prospero::GpuEnumValue(Prospero::CompSwizzle::kGreen) << 3u) |
	       (Prospero::GpuEnumValue(Prospero::CompSwizzle::kBlue) << 6u) |
	       (Prospero::GpuEnumValue(Prospero::CompSwizzle::kAlpha) << 9u);
}

static TextureImageCreateParams MakeDummyTextureParams(bool uint_format, bool image_3d,
                                                       TextureFormatUsage usage,
                                                       const char*        owner) {
	TextureImageCreateParams params {};
	params.fmt = static_cast<uint32_t>(
	    Prospero::GpuEnumValue(uint_format ? Prospero::BufferFormat::k8_8_8_8UInt : Prospero::BufferFormat::k8_8_8_8UNorm));
	params.width        = 1;
	params.height       = 1;
	params.base_level   = 0;
	params.levels       = 1;
	params.depth        = 1;
	params.type         = Prospero::GpuEnumValue(image_3d ? Prospero::ImageType::kColor3D : Prospero::ImageType::kColor2D);
	params.swizzle      = DummyTextureSwizzle();
	params.format_usage = usage;
	params.required_format_usage    = usage;
	params.view_usage               = usage;
	params.image_layout             = TextureUploadDestination::MipLevels;
	params.allow_cube_view          = true;
	params.storage_swizzle_fallback = TextureHasFormatUsage(usage, TextureFormatUsage::Storage);
	params.owner                    = owner;
	return params;
}

static VulkanImage* CreateDummyTexture(GraphicContext* ctx, bool uint_format, bool image_3d,
                                       bool storage, VulkanMemory** memory) {
	if (memory == nullptr || *memory != nullptr) {
		EXIT("TextureCache: invalid dummy texture memory slot, slot=%p current=%p storage=%d\n",
		     static_cast<const void*>(memory),
		     memory == nullptr ? nullptr : static_cast<const void*>(*memory), storage);
	}
	auto* image  = (storage ? static_cast<VulkanImage*>(new StorageTextureVulkanImage)
	                        : new TextureVulkanImage);
	auto  usage  = (storage ? TextureFormatUsage::Storage : TextureFormatUsage::Sampled);
	auto  layout = (storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	auto  owner  = (storage ? "DummyStorageTexture" : "DummySampledTexture");

	*memory         = new VulkanMemory {};
	auto params     = MakeDummyTextureParams(uint_format, image_3d, usage, owner);
	auto components = TextureCreateImage(ctx, image, *memory, params);

	static constexpr uint32_t zero = 0;
	UtilFillImage(ctx, image, &zero, sizeof(zero), 1, static_cast<uint64_t>(layout));
	TextureCreateImageViews(ctx, image, components, params.type, 0, params.base_level,
	                        params.levels, params.depth, params.allow_cube_view, params.view_usage);
	return image;
}

void TextureCache::DeleteImageViews(GraphicContext* ctx, VulkanImage* image) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(image == nullptr);

	for (auto& view: image->image_view) {
		if (view != nullptr) {
			vkDestroyImageView(ctx->device, view, nullptr);
			view = nullptr;
		}
	}
}

void TextureCache::DeleteGpuTexture(GraphicContext* ctx, GpuTextureVulkanImage* image,
                                    VulkanMemory* mem) {
	KYTY_PROFILER_BLOCK("TextureCache::DeleteGpuTexture");

	EXIT_IF(ctx == nullptr);
	EXIT_IF(image == nullptr);

	if (image->type == VulkanImageType::StorageTexture) {
		auto* storage = static_cast<StorageTextureVulkanImage*>(image);
		for (auto& cached: storage->sampled_views) {
			if (cached.view != nullptr) {
				vkDestroyImageView(ctx->device, cached.view, nullptr);
				cached.view = nullptr;
			}
		}
		storage->sampled_views.clear();
	}
	DeleteImageViews(ctx, image);
	VulkanDeleteImage(ctx, image, mem);

	switch (image->type) {
		case VulkanImageType::Texture: delete static_cast<TextureVulkanImage*>(image); break;
		case VulkanImageType::StorageTexture:
			delete static_cast<StorageTextureVulkanImage*>(image);
			break;
		default: EXIT("unsupported gpu texture image type: %d\n", static_cast<int>(image->type));
	}
}

void TextureCache::DeleteRenderTexture(GraphicContext* ctx, RenderTextureVulkanImage* image,
                                       VulkanMemory* mem) {
	KYTY_PROFILER_BLOCK("TextureCache::DeleteRenderTexture");

	EXIT_IF(ctx == nullptr);
	EXIT_IF(image == nullptr);

	g_render_ctx->GetFramebufferCache()->FreeFramebufferByColor(image);
	for (auto& view: image->render_view) {
		if (view != nullptr) {
			vkDestroyImageView(ctx->device, view, nullptr);
			view = nullptr;
		}
	}
	for (auto& cached: image->attachment_views) {
		if (cached.view != nullptr) {
			vkDestroyImageView(ctx->device, cached.view, nullptr);
			cached.view = nullptr;
		}
	}
	image->attachment_views.clear();
	for (auto& cached: image->sampled_views) {
		if (cached.view != nullptr) {
			vkDestroyImageView(ctx->device, cached.view, nullptr);
			cached.view = nullptr;
		}
	}
	image->sampled_views.clear();
	for (auto& cached: image->storage_views) {
		if (cached.view != nullptr) {
			vkDestroyImageView(ctx->device, cached.view, nullptr);
			cached.view = nullptr;
		}
	}
	image->storage_views.clear();
	DeleteImageViews(ctx, image);
	VulkanDeleteImage(ctx, image, mem);
	delete image;
}

void TextureCache::DeleteDepthStencil(GraphicContext* ctx, DepthStencilVulkanImage* image,
                                      VulkanMemory* mem) {
	KYTY_PROFILER_BLOCK("TextureCache::DeleteDepthStencil");

	EXIT_IF(ctx == nullptr);
	EXIT_IF(image == nullptr);

	g_render_ctx->GetFramebufferCache()->FreeFramebufferByDepth(image);
	DeleteImageViews(ctx, image);
	VulkanDeleteImage(ctx, image, mem);
	delete image;
}

void TextureCache::DeleteVideoOut(GraphicContext* ctx, VideoOutVulkanImage* image,
                                  VulkanMemory* mem) {
	KYTY_PROFILER_BLOCK("TextureCache::DeleteVideoOut");

	EXIT_IF(ctx == nullptr);
	EXIT_IF(image == nullptr);

	g_render_ctx->GetFramebufferCache()->FreeFramebufferByColor(image);
	DeleteImageViews(ctx, image);
	VulkanDeleteImage(ctx, image, mem);
	delete image;
}

VulkanImage* TextureCache::GetDummySampledTexture(bool uint_format, bool image_3d) {
	KYTY_PROFILER_BLOCK("TextureCache::GetDummySampledTexture");

	Common::LockGuard lock(m_dummy_mutex);
	auto*             ctx = g_render_ctx->GetGraphicCtx();
	if (m_dummy_ctx != nullptr && m_dummy_ctx != ctx) {
		EXIT("TextureCache: sampled dummy texture context changed, previous=%p current=%p\n",
		     static_cast<const void*>(m_dummy_ctx), static_cast<const void*>(ctx));
	}
	m_dummy_ctx      = ctx;
	const auto index = DummyTextureIndex(uint_format, image_3d);
	auto*&     image = m_dummy_sampled_textures[index];
	if (image == nullptr) {
		image =
		    CreateDummyTexture(ctx, uint_format, image_3d, false, &m_dummy_sampled_memory[index]);
	}
	return image;
}

VulkanImage* TextureCache::GetDummyStorageTexture(bool uint_format, bool image_3d) {
	KYTY_PROFILER_BLOCK("TextureCache::GetDummyStorageTexture");

	Common::LockGuard lock(m_dummy_mutex);
	auto*             ctx = g_render_ctx->GetGraphicCtx();
	if (m_dummy_ctx != nullptr && m_dummy_ctx != ctx) {
		EXIT("TextureCache: storage dummy texture context changed, previous=%p current=%p\n",
		     static_cast<const void*>(m_dummy_ctx), static_cast<const void*>(ctx));
	}
	m_dummy_ctx      = ctx;
	const auto index = DummyTextureIndex(uint_format, image_3d);
	auto*&     image = m_dummy_storage_textures[index];
	if (image == nullptr) {
		image =
		    CreateDummyTexture(ctx, uint_format, image_3d, true, &m_dummy_storage_memory[index]);
	}
	return image;
}

} // namespace Libs::Graphics
