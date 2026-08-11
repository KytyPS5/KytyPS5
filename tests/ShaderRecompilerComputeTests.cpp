#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "gpu_test_shaders/gpu_test_ms_depth_spv.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/hostMemory.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/gpuResourceManager.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/blitHelper.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/image/tiler.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderDraw.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/renderer/sync.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/window/windowInternal.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/decompiler/ShaderDecoder.h"
#include "graphics/shader/recompiler/emitter/SpirvBuilder.h"
#include "graphics/shader/recompiler/emitter/SpirvEmitter.h"
#include "graphics/shader/recompiler/ir/BindingLayout.h"
#include "graphics/shader/rectListShader.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"
#include "libs/agc.h"
#include "spirv-tools/libspirv.hpp"

#if __has_include("graphics/host_gpu/renderer/renderTargetBarriers.h")
#error "legacy render-target barrier API must remain deleted"
#endif

#if __has_include("graphics/host_gpu/transfer.h")
#error "legacy transfer facade must remain deleted"
#endif

#if __has_include("graphics/host_gpu/renderer/framebufferCache.h")
#error "classic framebuffer/render-pass cache must remain deleted"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <semaphore>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#endif

namespace Libs::Graphics {

template <typename Cache>
concept HasGetDownloadBuffer = requires(Cache& cache) { cache.GetDownloadBuffer(uint64_t {1}); };
static_assert(!HasGetDownloadBuffer<BufferCache>);

template <typename Cache>
concept HasSynchronizeImageToBuffer =
    requires(Cache& cache) { cache.SynchronizeImageToBuffer(uint64_t {1}, uint64_t {1}); };
template <typename Cache>
concept HasObtainBufferForImageCopy =
    requires(Cache& cache) { cache.ObtainBufferForImageCopy(uint64_t {1}, uint64_t {1}); };
template <typename Cache>
concept HasObtainBufferForImageWrite =
    requires(Cache& cache) { cache.ObtainBufferForImageWrite(uint64_t {1}, uint64_t {1}); };
template <typename Cache>
concept HasDiscardGpuDirtyBytes =
    requires(Cache& cache) { cache.DiscardGpuDirtyBytes(uint64_t {1}, uint64_t {1}); };
template <typename Source>
concept HasGpuOwnedImageSource = requires(Source& source) { source.gpu_owned; };
static_assert(!HasSynchronizeImageToBuffer<TextureCache>);
static_assert(!HasObtainBufferForImageCopy<BufferCache>);
static_assert(!HasObtainBufferForImageWrite<BufferCache>);
static_assert(!HasDiscardGpuDirtyBytes<BufferCache>);
static_assert(!HasGpuOwnedImageSource<ImageBufferSource>);

template <typename Backing>
concept HasLegacyImageLayout = requires(Backing& backing) { backing.layout; };
static_assert(!HasLegacyImageLayout<VulkanImage>);

template <typename Manager>
concept HasPolicyDetileImage = requires { &Manager::DetileImage; };
template <typename Manager>
concept HasPolicyTileImageBacking = requires { &Manager::TileImageBacking; };
static_assert(!HasPolicyDetileImage<TileManager>);
static_assert(!HasPolicyTileImageBacking<TileManager>);
static_assert(BlitHelper::ColorToMsDepthLayout == vk::ImageLayout::eDepthStencilAttachmentOptimal);

struct BufferCacheTestAccess {
	static void SetGarbageCollectionThresholds(BufferCache& cache, uint64_t trigger,
	                                           uint64_t critical) {
		cache.m_trigger_gc_memory  = trigger;
		cache.m_critical_gc_memory = critical;
	}

	static StreamBuffer& DownloadBuffer(BufferCache& cache) { return cache.m_download_buffer; }

	static bool SynchronizeBufferFromImage(BufferCache& cache, Buffer& buffer, uint64_t address,
	                                       uint64_t size) {
		return cache.SynchronizeBufferFromImage(buffer, address, size);
	}
};

struct StreamBufferTestAccess {
	static bool NormalizeReservation(bool coherent, uint64_t atom, uint64_t& size,
	                                 uint64_t& alignment) {
		return StreamBuffer::NormalizeReservation(coherent, atom, size, alignment);
	}
};

struct ImageTestAccess {
	static uint32_t CopyRows(uint64_t row_size, uint32_t rows, uint64_t capacity) {
		return Image::CopyRows(row_size, rows, capacity);
	}
};

struct TileManagerTestAccess {
	static uint32_t ConversionRows(uint64_t offset, uint64_t row_stride, uint64_t active,
	                               uint32_t remaining, uint64_t alignment, uint64_t max_range,
	                               uint32_t max_groups) {
		return TileManager::ConversionRows(offset, row_stride, active, remaining, alignment,
		                                   max_range, max_groups);
	}
};

struct TextureCacheTestAccess {
	static_assert(TextureCache::ImagePageTable::kPageBits == 20);
	static_assert(TextureCache::ImagePageTable::kAddressSpaceBits == 40);
	static_assert(TextureCache::ImagePageTable::kFirstLevelBits == 10);

	static std::unique_lock<TrackingSpinLock> Lock(TextureCache& cache) {
		return std::unique_lock(cache.m_lock);
	}

	static void ConfigureGarbageCollection(TextureCache& cache, std::span<const ImageId> oldest,
	                                       uint64_t tick, uint64_t pressure) {
		cache.m_trigger_gc_memory  = 0;
		cache.m_pressure_gc_memory = pressure;
		cache.m_critical_gc_memory = UINT64_MAX;
		cache.m_gc_tick            = tick;
		std::vector<ImageId> live;
		cache.m_lru_cache = {};
		for (auto& slot: cache.m_slots) {
			if (slot.image != nullptr && slot.image->registered) {
				slot.image->tick_accessed_last = cache.m_scheduler.CurrentTick();
				live.push_back(
				    {static_cast<uint32_t>(&slot - cache.m_slots.data()), slot.generation});
			}
		}
		for (const auto id: oldest) {
			const auto owner = cache.ResolveOwner(id);
			if (owner != nullptr && owner->registered) {
				owner->tick_accessed_last = 0;
				owner->lru_id             = cache.m_lru_cache.Insert(id, 0);
			}
		}
		for (const auto id: live) {
			if (std::ranges::find(oldest, id) == oldest.end()) {
				cache.ResolveImage(id).lru_id = cache.m_lru_cache.Insert(id, tick);
			}
		}
	}

	static bool Contains(const TextureCache& cache, ImageId id) {
		const auto owner = cache.ResolveOwner(id);
		return owner != nullptr && owner->registered;
	}

	static std::vector<ImageId> FindImages(TextureCache& cache, uint64_t address, uint64_t size,
	                                       bool page_overlap) {
		std::lock_guard      lock(cache.m_lock);
		const auto           found = cache.FindImagesInRegion(address, size, page_overlap);
		std::vector<ImageId> result;
		result.reserve(found.size());
		for (const auto id: found) {
			result.push_back(id);
		}
		return result;
	}

	static size_t PageOwnerCount(TextureCache& cache, uint64_t address) {
		std::lock_guard lock(cache.m_lock);
		const auto*     owners = cache.m_image_page_table.Find(
		    static_cast<size_t>(address >> TextureCache::ImagePageTable::kPageBits));
		return owners == nullptr ? 0 : owners->size();
	}

	static size_t OwnedPageCount(TextureCache& cache, uint64_t address, uint64_t size, ImageId id) {
		std::lock_guard                  lock(cache.m_lock);
		TextureCache::ImagePageTable::PageRange pages {};
		if (!TextureCache::ImagePageTable::TryGetPageRange(address, size, pages)) {
			return 0;
		}
		size_t count = 0;
		for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
			const auto* owners = cache.m_image_page_table.Find(page);
			count += owners != nullptr && owners->Contains(id) ? 1 : 0;
		}
		return count;
	}

	static void AddPageOwner(TextureCache& cache, uint64_t address, ImageId id) {
		std::lock_guard lock(cache.m_lock);
		cache.m_image_page_table[static_cast<size_t>(
		    address >> TextureCache::ImagePageTable::kPageBits)]
		    .push_back(id);
	}

	static bool RemovePageOwner(TextureCache& cache, uint64_t address, ImageId id) {
		std::lock_guard lock(cache.m_lock);
		auto*           owners = cache.m_image_page_table.Find(
		    static_cast<size_t>(address >> TextureCache::ImagePageTable::kPageBits));
		return owners != nullptr && owners->Erase(id);
	}

	static void SetQueryEpoch(TextureCache& cache, uint32_t epoch) {
		std::lock_guard lock(cache.m_lock);
		cache.m_image_query_epoch = epoch;
	}

	static uint32_t QueryEpoch(TextureCache& cache) {
		std::lock_guard lock(cache.m_lock);
		return cache.m_image_query_epoch;
	}

	static ImageId InsertImage(TextureCache& cache, const ImageInfo& info) {
		std::lock_guard lock(cache.m_lock);
		return cache.InsertImage(info);
	}

	static void DeleteImage(TextureCache& cache, ImageId id) {
		std::lock_guard lock(cache.m_lock);
		cache.DeleteImage(id);
	}

	static std::shared_ptr<Image> Owner(const TextureCache& cache, ImageId id) {
		return cache.ResolveOwner(id);
	}

	static bool PendingDownload(const TextureCache& cache, ImageId id) {
		return cache.m_download_images.contains(id);
	}

	static size_t NullImageCount(const TextureCache& cache) { return cache.m_null_images.size(); }

	static void TrackDownload(TextureCache& cache, ImageId id) { cache.TrackImageDownload(id); }

	static void AssociateStencil(TextureCache& cache, ImageId depth, GuestRange stencil) {
		cache.AssociateStencil(depth, stencil);
	}

	static void SetLinearReadback(TextureCache& cache, bool enabled) {
		cache.m_readback_linear_images = enabled;
	}

	static std::pair<uint8_t*, uint64_t> MapDownload(TextureCache& cache, uint64_t size,
	                                                 uint64_t alignment) {
		return cache.MapDownload(size, alignment);
	}

	static bool TryDownload(TextureCache& cache, ImageId id) { return cache.TryDownloadImage(id); }

	static TileManager& Tiler(TextureCache& cache) { return *cache.m_tiler; }
};

struct RenderExecutorTestAccess {
	static DescriptorCache::TextureBinding
	ResolveTexture(RenderExecutor& executor, const ShaderRecompiler::IR::ImageResource& resource,
	               const ShaderRecompiler::IR::DescriptorValue& value) {
		return executor.ResolveTexture(resource, value);
	}

	static auto PrepareGraphicsBindings(RenderExecutor& executor, CommandBuffer& buffer,
	                                    const ShaderStageRuntime& vertex,
	                                    const ShaderStageRuntime& pixel, bool pixel_active) {
		return executor.PrepareGraphicsBindings(buffer, vertex, pixel, pixel_active);
	}

	static void CommitBindings(RenderExecutor& executor, CommandBuffer& buffer,
	                           DescriptorCache::PreparedBindings& bindings) {
		executor.CommitBindings(buffer, vk::PipelineBindPoint::eGraphics, nullptr, bindings);
	}

	static void ResolveRenderDepthTarget(RenderExecutor& executor, uint64_t submit_id,
	                                     RenderCommandBuffer& buffer, RenderDepthInfo& depth) {
		executor.ResolveRenderDepthTarget(submit_id, buffer, depth);
	}

	static void ResolveRenderColorTarget(RenderExecutor& executor, uint64_t submit_id,
	                                     RenderCommandBuffer& buffer, RenderColorInfo& color,
	                                     uint32_t slot) {
		executor.ResolveRenderColorTarget(submit_id, buffer, color, 0, slot);
	}

	static void BindRenderTarget(RenderExecutor& executor, ImageId id) {
		executor.BindRenderTarget(id);
	}

	static RenderState AcquireRenderTargets(RenderExecutor& executor, CommandBuffer& buffer,
	                                        RenderColorInfo* colors, uint32_t color_count,
	                                        RenderDepthInfo& depth) {
		return executor.AcquireRenderTargets(buffer, colors, color_count, depth);
	}

	static void ResetBindings(RenderExecutor& executor) { executor.ResetBindings(); }

	static bool BoundImagesInOrder(const RenderExecutor&         executor,
	                               const std::shared_ptr<Image>& first,
	                               const std::shared_ptr<Image>& second) {
		return executor.m_bound_images.size() == 2 && executor.m_bound_images[0] == first &&
		       executor.m_bound_images[1] == second;
	}
};

struct DescriptorCacheTestAccess {
	static vk::DescriptorImageInfo MakeImageInfo(const DescriptorCache::TextureBinding& binding) {
		return DescriptorCache::MakeImageInfo(binding);
	}
};

namespace {

using u32          = uint32_t;
using BindingType  = TextureCache::BindingType;
using ImageDesc    = TextureCache::ImageDesc;
using ShaderOpcode = ShaderRecompiler::Decoder::Opcode;

constexpr u32 InlineU32(u32 value) {
	return 128u + value;
}

constexpr u32 Vgpr(u32 reg) {
	return 256u + reg;
}

constexpr u32 EncodeSMovB32(u32 dst, u32 src) {
	return 0x80000000u | (0x7du << 23u) | ((dst & 0x7fu) << 16u) | (0x03u << 8u) | (src & 0xffu);
}

constexpr u32 EncodeSop1(u32 opcode, u32 dst, u32 src) {
	return 0x80000000u | (0x7du << 23u) | ((dst & 0x7fu) << 16u) | ((opcode & 0xffu) << 8u) |
	       (src & 0xffu);
}

constexpr u32 EncodeSop2(u32 opcode, u32 dst, u32 src0, u32 src1) {
	return 0x80000000u | ((opcode & 0x7fu) << 23u) | ((dst & 0x7fu) << 16u) |
	       ((src1 & 0xffu) << 8u) | (src0 & 0xffu);
}

constexpr u32 EncodeSopc(u32 opcode, u32 src0, u32 src1) {
	return 0x80000000u | (0x7eu << 23u) | ((opcode & 0x7fu) << 16u) | ((src1 & 0xffu) << 8u) |
	       (src0 & 0xffu);
}

constexpr u32 EncodeSopp(u32 opcode, u32 simm = 0) {
	return 0x80000000u | (0x7fu << 23u) | ((opcode & 0x7fu) << 16u) | (simm & 0xffffu);
}

constexpr u32 EncodeSopk(u32 opcode, u32 dst, u32 imm) {
	return 0x80000000u | (((opcode + 0x60u) & 0x7fu) << 23u) | ((dst & 0x7fu) << 16u) |
	       (imm & 0xffffu);
}

constexpr u32 EncodeVop1(u32 opcode, u32 dst, u32 src0) {
	return (0x3fu << 25u) | ((dst & 0xffu) << 17u) | ((opcode & 0xffu) << 9u) | (src0 & 0x1ffu);
}

constexpr u32 EncodeVop1Sdwa(u32 src0, u32 dst_sel = 6, u32 dst_u = 0, u32 src0_sel = 6,
                             u32 src0_sext = 0, u32 src0_neg = 0, u32 src0_abs = 0, u32 s0 = 0,
                             u32 clamp = 0, u32 omod = 0) {
	return (src0 & 0xffu) | ((dst_sel & 0x7u) << 8u) | ((dst_u & 0x3u) << 11u) |
	       ((clamp & 0x1u) << 13u) | ((omod & 0x3u) << 14u) | ((src0_sel & 0x7u) << 16u) |
	       ((src0_sext & 0x1u) << 19u) | ((src0_neg & 0x1u) << 20u) | ((src0_abs & 0x1u) << 21u) |
	       ((s0 & 0x1u) << 23u);
}

constexpr u32 EncodeVop2(u32 opcode, u32 dst, u32 src0, u32 src1) {
	return ((opcode & 0x3fu) << 25u) | ((dst & 0xffu) << 17u) | ((src1 & 0xffu) << 9u) |
	       (src0 & 0x1ffu);
}

constexpr u32 EncodeVop2Sdwa(u32 src0, u32 dst_sel = 6, u32 dst_u = 0, u32 src0_sel = 6,
                             u32 src1_sel = 6, u32 src0_sext = 0, u32 src1_sext = 0,
                             u32 src0_neg = 0, u32 src0_abs = 0, u32 src1_neg = 0, u32 src1_abs = 0,
                             u32 s0 = 0, u32 s1 = 0, u32 clamp = 0, u32 omod = 0) {
	return (src0 & 0xffu) | ((dst_sel & 0x7u) << 8u) | ((dst_u & 0x3u) << 11u) |
	       ((clamp & 0x1u) << 13u) | ((omod & 0x3u) << 14u) | ((src0_sel & 0x7u) << 16u) |
	       ((src0_sext & 0x1u) << 19u) | ((src0_neg & 0x1u) << 20u) | ((src0_abs & 0x1u) << 21u) |
	       ((s0 & 0x1u) << 23u) | ((src1_sel & 0x7u) << 24u) | ((src1_sext & 0x1u) << 27u) |
	       ((src1_neg & 0x1u) << 28u) | ((src1_abs & 0x1u) << 29u) | ((s1 & 0x1u) << 31u);
}

constexpr u32 EncodeVop2Dpp(u32 src0, u32 dpp_ctrl = 0, u32 row_mask = 0xf, u32 bank_mask = 0xf,
                            u32 src0_neg = 0, u32 src0_abs = 0, u32 src1_neg = 0,
                            u32 src1_abs = 0) {
	return (src0 & 0xffu) | ((dpp_ctrl & 0x1ffu) << 8u) | ((src0_neg & 0x1u) << 20u) |
	       ((src0_abs & 0x1u) << 21u) | ((src1_neg & 0x1u) << 22u) | ((src1_abs & 0x1u) << 23u) |
	       ((bank_mask & 0xfu) << 24u) | ((row_mask & 0xfu) << 28u);
}

constexpr u32 EncodeVop3Word0(u32 opcode, u32 dst, u32 abs = 0, u32 op_sel = 0,
                              bool clamp = false) {
	return (0x35u << 26u) | ((opcode & 0x3ffu) << 16u) | ((abs & 0x7u) << 8u) |
	       ((op_sel & 0xfu) << 11u) | (clamp ? (1u << 15u) : 0u) | (dst & 0xffu);
}

constexpr u32 EncodeVop3BWord0(u32 opcode, u32 vdst, u32 sdst) {
	return (0x35u << 26u) | ((opcode & 0x3ffu) << 16u) | ((sdst & 0x7fu) << 8u) | (vdst & 0xffu);
}

constexpr u32 EncodeVop3Word1(u32 src0, u32 src1, u32 src2 = 0, u32 omod = 0, u32 neg = 0) {
	return (src0 & 0x1ffu) | ((src1 & 0x1ffu) << 9u) | ((src2 & 0x1ffu) << 18u) |
	       ((omod & 0x3u) << 27u) | ((neg & 0x7u) << 29u);
}

constexpr u32 EncodeVop3pWord0(u32 opcode, u32 dst, u32 op_sel_hi = 0, u32 op_sel = 0,
                               u32 neg_hi = 0, bool clamp = false) {
	return (0x33u << 26u) | ((opcode & 0x7fu) << 16u) | ((neg_hi & 0x7u) << 8u) |
	       ((op_sel & 0x7u) << 11u) | ((op_sel_hi & 0x4u) << 12u) | (clamp ? (1u << 15u) : 0u) |
	       (dst & 0xffu);
}

constexpr u32 EncodeVop3pWord1(u32 src0, u32 src1, u32 src2 = 0, u32 op_sel_hi = 0, u32 neg = 0) {
	return (src0 & 0x1ffu) | ((src1 & 0x1ffu) << 9u) | ((src2 & 0x1ffu) << 18u) |
	       ((op_sel_hi & 0x3u) << 27u) | ((neg & 0x7u) << 29u);
}

constexpr u32 EncodeVopc(u32 opcode, u32 src0, u32 src1) {
	return (0x3eu << 25u) | ((opcode & 0xffu) << 17u) | ((src1 & 0xffu) << 9u) | (src0 & 0x1ffu);
}

constexpr u32 EncodeVopcSdwa(u32 src0, u32 sdst = 0, u32 sd = 0, u32 src0_sel = 6, u32 src1_sel = 6,
                             u32 src0_sext = 0, u32 src1_sext = 0, u32 src0_neg = 0,
                             u32 src0_abs = 0, u32 src1_neg = 0, u32 src1_abs = 0, u32 s0 = 0,
                             u32 s1 = 0) {
	return (src0 & 0xffu) | ((sdst & 0x7fu) << 8u) | ((sd & 0x1u) << 15u) |
	       ((src0_sel & 0x7u) << 16u) | ((src0_sext & 0x1u) << 19u) | ((src0_neg & 0x1u) << 20u) |
	       ((src0_abs & 0x1u) << 21u) | ((s0 & 0x1u) << 23u) | ((src1_sel & 0x7u) << 24u) |
	       ((src1_sext & 0x1u) << 27u) | ((src1_neg & 0x1u) << 28u) | ((src1_abs & 0x1u) << 29u) |
	       ((s1 & 0x1u) << 31u);
}

constexpr u32 EncodeMubuf0(u32 opcode, u32 offset = 0, bool idxen = false, bool offen = true,
                           bool glc = false) {
	return (0x38u << 26u) | ((opcode & 0x7fu) << 18u) | (offen ? (1u << 12u) : 0u) |
	       (idxen ? (1u << 13u) : 0u) | (glc ? (1u << 14u) : 0u) | (offset & 0xfffu);
}

constexpr u32 EncodeMubuf1(u32 vdata, u32 srsrc, u32 vaddr, u32 soffset = 128) {
	return ((soffset & 0xffu) << 24u) | ((srsrc & 0x1fu) << 16u) | ((vdata & 0xffu) << 8u) |
	       (vaddr & 0xffu);
}

constexpr u32 EncodeMtbuf0(u32 opcode, u32 dfmt, u32 nfmt, u32 offset = 0, bool idxen = false,
                           bool offen = true) {
	return (0x3au << 26u) | (offset & 0xfffu) | (offen ? (1u << 12u) : 0u) |
	       (idxen ? (1u << 13u) : 0u) | ((opcode & 0x7u) << 16u) | ((dfmt & 0xfu) << 19u) |
	       ((nfmt & 0x7u) << 23u);
}

constexpr u32 EncodeMtbuf1(u32 opcode, u32 vdata, u32 srsrc, u32 vaddr, u32 soffset = 128) {
	return (((opcode >> 3u) & 1u) << 21u) | ((soffset & 0xffu) << 24u) | ((srsrc & 0x1fu) << 16u) |
	       ((vdata & 0xffu) << 8u) | (vaddr & 0xffu);
}

constexpr u32 EncodeSmem0(u32 opcode, u32 dst, u32 sbase = 0) {
	return (0x3du << 26u) | ((opcode & 0xffu) << 18u) | ((dst & 0x7fu) << 6u) | (sbase & 0x3fu);
}

constexpr u32 EncodeSmem1(u32 offset, u32 soffset = 0) {
	return (offset & 0x1fffffu) | ((soffset & 0x7fu) << 25u);
}

constexpr u32 EncodeMimg0(u32 opcode, u32 dmask, u32 nsa_dwords = 0, bool glc = false,
                          u32 dim = 1) {
	return (0x3cu << 26u) | ((opcode >> 7u) & 0x1u) | ((nsa_dwords & 0x3u) << 1u) |
	       ((dim & 0x7u) << 3u) | ((dmask & 0xfu) << 8u) | (glc ? (1u << 13u) : 0u) |
	       ((opcode & 0x7fu) << 18u);
}

constexpr u32 EncodeMimg1(u32 vdata, u32 vaddr, u32 srsrc = 0, u32 ssamp = 0, bool a16 = false) {
	return ((ssamp & 0x1fu) << 21u) | ((srsrc & 0x1fu) << 16u) | ((vdata & 0xffu) << 8u) |
	       (vaddr & 0xffu) | (a16 ? (1u << 30u) : 0u);
}

constexpr u32 EncodeVintrp(u32 opcode, u32 dst, u32 attr, u32 chan, u32 src) {
	return (0x32u << 26u) | ((dst & 0xffu) << 18u) | ((opcode & 0x3u) << 16u) |
	       ((attr & 0x3fu) << 10u) | ((chan & 0x3u) << 8u) | (src & 0xffu);
}

constexpr u32 EncodeExp0(u32 target, u32 en, bool done = true, bool compr = false,
                         bool vm = false) {
	return (0x3eu << 26u) | ((target & 0x3fu) << 4u) | (en & 0xfu) | (compr ? (1u << 10u) : 0u) |
	       (done ? (1u << 11u) : 0u) | (vm ? (1u << 12u) : 0u);
}

constexpr u32 EncodeExp1(u32 src0, u32 src1, u32 src2, u32 src3) {
	return (src0 & 0xffu) | ((src1 & 0xffu) << 8u) | ((src2 & 0xffu) << 16u) |
	       ((src3 & 0xffu) << 24u);
}

constexpr u32 EncodeFlat0(u32 opcode, u32 segment, u32 offset = 0) {
	return (0x37u << 26u) | ((opcode & 0x7fu) << 18u) | ((segment & 0x3u) << 14u) |
	       (offset & 0xfffu);
}

constexpr u32 EncodeFlat1(u32 vdst, u32 saddr, u32 data, u32 addr) {
	return ((vdst & 0xffu) << 24u) | ((saddr & 0x7fu) << 16u) | ((data & 0xffu) << 8u) |
	       (addr & 0xffu);
}

constexpr u32 EncodeDs0(u32 opcode, u32 offset = 0, bool gds = false) {
	return (0x36u << 26u) | ((opcode & 0xffu) << 18u) | (gds ? (1u << 17u) : 0u) |
	       (offset & 0xffffu);
}

constexpr u32 EncodeDs1Ex(u32 vdst, u32 data1, u32 data0, u32 addr) {
	return ((vdst & 0xffu) << 24u) | ((data1 & 0xffu) << 16u) | ((data0 & 0xffu) << 8u) |
	       (addr & 0xffu);
}

constexpr u32 EncodeDs1(u32 vdst, u32 data0, u32 addr) {
	return EncodeDs1Ex(vdst, 0, data0, addr);
}

void AppendSMovLiteral(std::vector<u32>* code, u32 dst, u32 literal) {
	code->push_back(EncodeSMovB32(dst, 255u));
	code->push_back(literal);
}

void AppendVMovLiteral(std::vector<u32>* code, u32 dst, u32 literal) {
	code->push_back(EncodeVop1(0x01, dst, 255u));
	code->push_back(literal);
}

void AppendVMovU32(std::vector<u32>* code, u32 dst, u32 value) {
	if (value <= 64u) {
		code->push_back(EncodeVop1(0x01, dst, InlineU32(value)));
		return;
	}
	AppendVMovLiteral(code, dst, value);
}

void AppendVop3(std::vector<u32>* code, u32 opcode, u32 dst, u32 src0, u32 src1, u32 src2 = 0,
                u32 abs = 0, u32 op_sel = 0, bool clamp = false, u32 omod = 0, u32 neg = 0) {
	code->push_back(EncodeVop3Word0(opcode, dst, abs, op_sel, clamp));
	code->push_back(EncodeVop3Word1(src0, src1, src2, omod, neg));
}

void AppendVop3B(std::vector<u32>* code, u32 opcode, u32 vdst, u32 sdst, u32 src0, u32 src1,
                 u32 src2 = 0) {
	code->push_back(EncodeVop3BWord0(opcode, vdst, sdst));
	code->push_back(EncodeVop3Word1(src0, src1, src2));
}

void AppendVop3p(std::vector<u32>* code, u32 opcode, u32 dst, u32 src0, u32 src1, u32 src2 = 0,
                 u32 op_sel_hi = 0, u32 op_sel = 0, u32 neg_hi = 0, u32 neg = 0) {
	code->push_back(EncodeVop3pWord0(opcode, dst, op_sel_hi, op_sel, neg_hi));
	code->push_back(EncodeVop3pWord1(src0, src1, src2, op_sel_hi, neg));
}

void AppendBufferLoadDword(std::vector<u32>* code, u32 dst_vgpr, u32 address_vgpr) {
	code->push_back(EncodeMubuf0(0x0cu));
	code->push_back(EncodeMubuf1(dst_vgpr, 0, address_vgpr));
}

void AppendBufferLoadOpcode(std::vector<u32>* code, u32 opcode, u32 dst_vgpr, u32 address_vgpr) {
	code->push_back(EncodeMubuf0(opcode));
	code->push_back(EncodeMubuf1(dst_vgpr, 0, address_vgpr));
}

void AppendBufferStoreDword(std::vector<u32>* code, u32 value_vgpr, u32 address_vgpr) {
	code->push_back(EncodeMubuf0(0x1cu));
	code->push_back(EncodeMubuf1(value_vgpr, 12, address_vgpr));
}

void AppendBufferStoreOpcode(std::vector<u32>* code, u32 opcode, u32 value_vgpr, u32 address_vgpr,
                             bool glc = false) {
	code->push_back(EncodeMubuf0(opcode, 0, false, true, glc));
	code->push_back(EncodeMubuf1(value_vgpr, 12, address_vgpr));
}

void AppendTBufferLoadOpcode(std::vector<u32>* code, u32 opcode, u32 dst_vgpr, u32 address_vgpr) {
	code->push_back(EncodeMtbuf0(opcode, 14, 7));
	code->push_back(EncodeMtbuf1(opcode, dst_vgpr, 0, address_vgpr));
}

constexpr u32 BufferFormat(Prospero::BufferFormat format) {
	return static_cast<uint32_t>(format);
}

void AppendTBufferLoadFormatOpcode(std::vector<u32>* code, u32 opcode, u32 dst_vgpr,
                                   u32 address_vgpr, Prospero::BufferFormat format) {
	const auto value = BufferFormat(format);
	code->push_back(EncodeMtbuf0(opcode, value & 0xfu, (value >> 4u) & 0x7u));
	code->push_back(EncodeMtbuf1(opcode, dst_vgpr, 0, address_vgpr));
}

void AppendTBufferStoreOpcode(std::vector<u32>* code, u32 opcode, u32 value_vgpr,
                              u32 address_vgpr) {
	code->push_back(EncodeMtbuf0(opcode, 14, 7));
	code->push_back(EncodeMtbuf1(opcode, value_vgpr, 0, address_vgpr));
}

void AppendSmemLoadOpcode(std::vector<u32>* code, u32 opcode, u32 dst_sgpr, u32 byte_offset) {
	code->push_back(EncodeSmem0(opcode, dst_sgpr));
	code->push_back(EncodeSmem1(byte_offset));
}

void AppendStoreVgpr(std::vector<u32>* code, u32 value_vgpr, u32 dword_index) {
	AppendVMovU32(code, 31, dword_index * 4u);
	AppendBufferStoreDword(code, value_vgpr, 31);
}

void AppendStoreVgprAtLaneDwordOffset(std::vector<u32>* code, u32 value_vgpr, u32 lane_vgpr,
                                      u32 dword_offset) {
	if (dword_offset == 0u) {
		code->push_back(EncodeVop2(0x1a, 31, InlineU32(2), lane_vgpr));
	} else {
		AppendVMovU32(code, 31, dword_offset);
		code->push_back(EncodeVop2(0x25, 31, Vgpr(lane_vgpr), 31));
		code->push_back(EncodeVop2(0x1a, 31, InlineU32(2), 31));
	}
	AppendBufferStoreDword(code, value_vgpr, 31);
}

void AppendStoreSgpr(std::vector<u32>* code, u32 value_sgpr, u32 dword_index) {
	code->push_back(EncodeVop1(0x01, 30, value_sgpr));
	AppendStoreVgpr(code, 30, dword_index);
}

void AppendStoreSgprAtLaneDwordOffset(std::vector<u32>* code, u32 value_sgpr, u32 lane_vgpr,
                                      u32 dword_offset) {
	code->push_back(EncodeVop1(0x01, 30, value_sgpr));
	AppendStoreVgprAtLaneDwordOffset(code, 30, lane_vgpr, dword_offset);
}

void AppendStoreSgprPair(std::vector<u32>* code, u32 value_sgpr, u32 first_dword_index) {
	AppendStoreSgpr(code, value_sgpr, first_dword_index);
	AppendStoreSgpr(code, value_sgpr + 1u, first_dword_index + 1u);
}

void AppendEnd(std::vector<u32>* code) {
	code->push_back(0xbf810000u);
}

std::string Hex(u32 value) {
	char buffer[32] = {};
	std::snprintf(buffer, sizeof(buffer), "0x%08" PRIx32, value);
	return buffer;
}

std::string VulkanResultName(vk::Result result) {
	return VulkanToString(result);
}

[[noreturn]] void Fail(const char* shader_name, const char* stage, const std::string& message) {
	std::fprintf(stderr, "ShaderRecompilerComputeTests: %s failed at %s: %s\n", shader_name, stage,
	             message.c_str());
	std::abort();
}

void Require(const char* shader_name, const char* stage, bool value, const std::string& message);

void EnsureConfigInitialized() {
	static bool config_initialized = false;
	if (!config_initialized) {
		static Common::Subsystems subsystems;
		Common::InitializeThreads();
		subsystems.Initialize<Config::Lifecycle>();
		Config::ConfigOptions options;
		options.printf_direction = Config::OutputDirection::Silent;
		Config::Load(options);
		subsystems.Initialize<Log::Lifecycle>();
		subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
		config_initialized = true;
	}
}

void RequireVk(const char* shader_name, const char* stage, vk::Result result, const char* action) {
	if (result != vk::Result::eSuccess) {
		Fail(shader_name, stage, std::string(action) + " returned " + VulkanResultName(result));
	}
}

void Require(const char* shader_name, const char* stage, bool value, const std::string& message) {
	if (!value) {
		Fail(shader_name, stage, message);
	}
}

void CheckLeastRecentlyUsedCacheOrdering() {
	Common::LeastRecentlyUsedCache<uint32_t, uint64_t> cache;
	const auto                                         first  = cache.Insert(1, 1);
	const auto                                         middle = cache.Insert(2, 2);
	const auto                                         last   = cache.Insert(3, 3);
	(void)first;
	(void)last;
	cache.Touch(middle, 4);
	std::vector<uint32_t> visited;
	cache.ForEachItemBelow(4, [&](uint32_t value) {
		visited.push_back(value);
		return visited.size() > 3;
	});
	Require("LeastRecentlyUsedCache", "non-tail touch traversal",
	        visited == std::vector<uint32_t> {1, 3, 2},
	        "touching a non-tail item left a cycle or changed LRU order");
	std::printf("[host]    %-32s ok\n", "LeastRecentlyUsedCache");
}

struct TestCase {
	const char*                   name = "";
	std::vector<u32>              code;
	std::vector<u32>              initial;
	std::vector<u32>              expected;
	std::vector<ShaderOpcode>     opcodes;
	u32                           image_width  = 4;
	u32                           image_height = 4;
	std::vector<u32>              sampled_image_rgba;
	std::vector<std::vector<u32>> sampled_image_rgba_mips;
	vk::Format                    sampled_image_format           = vk::Format::eR32G32B32A32Sfloat;
	u32                           sampled_image_dwords_per_pixel = 4;
	vk::ImageType                 sampled_image_type             = vk::ImageType::e2D;
	vk::ImageViewType             sampled_image_view_type        = vk::ImageViewType::e2D;
	u32                           sampled_image_layers           = 1;
	u32                           sampled_image_view_base_layer  = 0;
	u32                           sampled_image_view_layers      = 0;
	std::vector<u32>              storage_image_rgba;
	std::vector<u32>              expected_storage_image_rgba;
	vk::Format                    storage_image_format           = vk::Format::eR32G32B32A32Sfloat;
	u32                           storage_image_dwords_per_pixel = 4;
	std::vector<u32>              storage_image_r32ui;
	std::vector<u32>              expected_storage_image_r32ui;
	std::vector<std::string>      required_spirv;
	std::vector<std::string>      forbidden_spirv;
	ShaderComputeInputInfo        compute_info {};
	bool                          has_compute_info = false;
	u32                           dispatch_x       = 1;
	u32                           dispatch_y       = 1;
	u32                           dispatch_z       = 1;
	std::array<u32, 64>           user_data {};
	bool                          has_user_data               = false;
	u32                           image_descriptor_swizzle    = DstSel(4, 5, 6, 7);
	bool                          compile_only                = false;
	size_t                        storage_buffer_range_dwords = 0;
	std::vector<u32>              storage_buffer_offsets;
	bool                          force_shader_data_storage = false;
	std::optional<uint64_t>       flat_memory_base;
	std::vector<u32>              gds_initial;
	std::vector<u32>              expected_gds;
};

struct SkippedCase {
	const char* name   = "";
	const char* reason = "";
};

struct GraphicsCase {
	const char*               name = "";
	std::vector<u32>          fragment_code;
	std::vector<u32>          expected_pixel;
	std::vector<ShaderOpcode> opcodes;
	std::array<u32, 64>       user_data {};
	bool                      has_user_data = false;
	std::vector<u32>          push_constants;
	std::vector<u32>          pixel_interpolator_settings;
	bool                      pixel_no_perspective = false;
	std::vector<u32>          vertices;
};

struct CompiledShader {
	std::vector<u32>              spirv;
	ShaderRecompiler::IR::Program program;
	std::vector<u32>              flattened_srt;
	std::vector<u32>              packed_user_data;
};

std::array<u32, 64> MakeNativeUserData(const std::array<u32, 64>* source) {
	std::array<u32, 64> data {};
	data[50] = 1u << 20u;
	if (source != nullptr) {
		data = *source;
	}
	return data;
}

bool ReadTestMemory(void* userdata, uint64_t address, u32* value) {
	const auto* data = static_cast<const std::vector<u32>*>(userdata);
	if (data == nullptr || value == nullptr || address % 4u != 0 || address / 4u >= data->size()) {
		return false;
	}
	*value = (*data)[address / 4u];
	return true;
}

void ValidateSpirv(const char* shader_name, const std::vector<u32>& spirv) {
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
	std::string          messages;
	tools.SetMessageConsumer([&messages](spv_message_level_t, const char*,
	                                     const spv_position_t& position, const char* message) {
		char buffer[1024] = {};
		std::snprintf(buffer, sizeof(buffer), "%zu:%zu: %s\n", position.line, position.column,
		              message);
		messages += buffer;
	});

	if (!tools.Validate(spirv)) {
		Fail(shader_name, "SPIR-V validation", messages);
	}
}

size_t CountText(const std::string& text, const std::string& needle) {
	size_t count = 0;
	for (size_t offset = 0; (offset = text.find(needle, offset)) != std::string::npos;
	     offset += needle.size()) {
		count++;
	}
	return count;
}

void CheckRectListShaders() {
	constexpr const char* name = "RectListShaders";

	auto program = std::make_shared<ShaderRecompiler::IR::Program>();
	program->info.inputs.push_back(
	    {ShaderRecompiler::IR::StageInputKind::Parameter, 0, 4, "in_param_0"});
	program->info.inputs.push_back(
	    {ShaderRecompiler::IR::StageInputKind::Parameter, 1, 4, "in_param_1"});

	ShaderVertexInputInfo vertex {};
	vertex.param_export_mask = 1u;
	ShaderPixelInputInfo pixel {};
	pixel.input_num                = 2;
	pixel.interpolator_settings[0] = 0x400u;
	pixel.interpolator_settings[1] = 0;
	pixel.stage.program            = program;
	HW::PixelShaderInfo ps_regs {};
	const auto          perspective_id = ShaderGetIdPS(ps_regs, pixel, false);
	pixel.ps_no_perspective            = true;
	const auto no_perspective_id       = ShaderGetIdPS(ps_regs, pixel, false);
	pixel.ps_no_perspective            = false;
	Require(name, "pipeline identity", perspective_id != no_perspective_id,
	        "pixel interpolation mode must participate in the shader and pipeline key");

	const std::array<uint32_t, 2> active_inputs = {0, 1};
	Require(name, "duplicate mapping",
	        ShaderPixelParameterLocation(pixel, active_inputs, 0) == 0 &&
	            ShaderPixelParameterLocation(pixel, active_inputs, 1) == 1,
	        "duplicate pixel mappings must receive distinct effective output locations");

	const auto shaders = BuildRectListShaders(vertex, &pixel);
	Require(name, "SPIR-V version",
	        shaders.control.size() > 1 && shaders.control[1] == 0x00010500u &&
	            shaders.evaluation.size() > 1 && shaders.evaluation[1] == 0x00010500u,
	        "shadPS4-compatible vector selection requires SPIR-V 1.5");
	ValidateSpirv(name, shaders.control);
	ValidateSpirv(name, shaders.evaluation);

	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
	std::string          control_text;
	std::string          evaluation_text;
	Require(name, "control disassembly", tools.Disassemble(shaders.control, &control_text),
	        "failed to disassemble rectangle-list tessellation control shader");
	Require(name, "evaluation disassembly",
	        tools.Disassemble(shaders.evaluation, &evaluation_text),
	        "failed to disassemble rectangle-list tessellation evaluation shader");
	Require(name, "control execution mode",
	        control_text.find("TessellationControl") != std::string::npos &&
	            control_text.find("OutputVertices 4") != std::string::npos,
	        "rectangle-list control shader must produce four control points");
	Require(name, "evaluation execution modes",
	        evaluation_text.find("TessellationEvaluation") != std::string::npos &&
	            evaluation_text.find("Quads") != std::string::npos &&
	            evaluation_text.find("SpacingEqual") != std::string::npos &&
	            evaluation_text.find("VertexOrderCw") != std::string::npos,
	        "rectangle-list evaluation shader has the wrong patch modes");
	Require(name, "no geometry stage",
	        control_text.find("Geometry") == std::string::npos &&
	            evaluation_text.find("Geometry") == std::string::npos,
	        "rectangle-list expansion must not use geometry shaders");
	Require(name, "flat broadcast",
	        CountText(control_text, "OpVectorTimesScalar") == 6 &&
	            CountText(control_text, "OpSelect %v4float") == 2,
	        "flat parameters must use guest vertex zero instead of reconstructed values");
	Require(name, "remapped interface",
	        CountText(control_text, " Location 0") == 2 &&
	            CountText(control_text, " Location 1") == 1 &&
	            CountText(evaluation_text, " Location 0") == 2 &&
	            CountText(evaluation_text, " Location 1") == 2,
	        "duplicate pixel mappings must share one vertex input and keep distinct patch outputs");

	const auto position_only = BuildRectListShaders(vertex, nullptr);
	ValidateSpirv(name, position_only.control);
	ValidateSpirv(name, position_only.evaluation);
}

void CheckSpirvText(const TestCase& test, const std::vector<u32>& spirv) {
	if (test.required_spirv.empty() && test.forbidden_spirv.empty()) {
		return;
	}

	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
	std::string          text;
	if (!tools.Disassemble(spirv, &text)) {
		Fail(test.name, "SPIR-V disassembly", "failed to disassemble emitted SPIR-V");
	}
	for (const auto& required: test.required_spirv) {
		if (text.find(required) == std::string::npos) {
			Fail(test.name, "SPIR-V disassembly",
			     std::string("missing required text: ") + required);
		}
	}
	for (const auto& forbidden: test.forbidden_spirv) {
		if (text.find(forbidden) != std::string::npos) {
			Fail(test.name, "SPIR-V disassembly",
			     std::string("found forbidden text: ") + forbidden);
		}
	}
}

CompiledShader CompileCase(const TestCase& test) {
	auto       user_data  = MakeNativeUserData(test.has_user_data ? &test.user_data : nullptr);
	const auto uses_image = std::any_of(test.opcodes.begin(), test.opcodes.end(), [](auto op) {
		return op >= ShaderOpcode::ImageGetResinfo && op <= ShaderOpcode::ImageGather4H;
	});
	if (uses_image && ((user_data[3] >> 28u) & 0xfu) == 0) {
		user_data[3] = static_cast<uint32_t>(Prospero::ImageType::kColor2D) << 28u;
	}
	if (uses_image) {
		user_data[3] = (user_data[3] & ~0xfffu) | test.image_descriptor_swizzle;
	}
	if (!test.has_user_data) {
		user_data[2] = static_cast<u32>(test.initial.size() * sizeof(u32));
	}
	ShaderRecompiler::CompileOptions options;
	options.stage              = ShaderType::Compute;
	options.dump_ir            = true;
	options.compute_input_info = test.has_compute_info ? &test.compute_info : nullptr;
	options.user_data          = user_data.data();
	options.read_memory        = ReadTestMemory;
	options.read_memory_data   = const_cast<std::vector<u32>*>(&test.initial);
	options.flat_memory_base   = test.flat_memory_base;
	if (test.has_compute_info) {
		options.wave_size = test.compute_info.wave_size;
	}

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	if (!ShaderRecompiler::TryRecompile(test.code, options, result, &error)) {
		Fail(test.name, "decode/IR", error.c_str());
	}
	if (test.force_shader_data_storage) {
		result.program.bindings                = {};
		result.program.binding_layout_complete = false;
		if (!ShaderRecompiler::IR::AllocateBindings(result.program, {.max_push_dwords = 0},
		                                            &error)) {
			Fail(test.name, "binding layout", error.c_str());
		}
		const auto* shader_data = ShaderRecompiler::IR::FindBinding(
		    result.program.bindings, ShaderRecompiler::IR::DescriptorBindingKind::UserData);
		Require(test.name, "binding layout",
		        result.program.bindings.user_data_registers.empty() &&
		            result.program.bindings.push_constant_size == 0 && shader_data != nullptr,
		        "offset-only shader data did not use its storage fallback");
		std::vector<u32> storage_spirv;
		if (!ShaderRecompiler::Spirv::EmitProgram(result.program, result.resources, nullptr,
		                                          nullptr, options.compute_input_info,
		                                          storage_spirv, &error)) {
			Fail(test.name, "SPIR-V emit", error.c_str());
		}
		result.spirv = std::move(storage_spirv);
	}
	Require(test.name, "SPIR-V emit", !result.spirv.empty(), "recompiler returned empty SPIR-V");
	ValidateSpirv(test.name, result.spirv);
	CheckSpirvText(test, result.spirv);
	std::vector<u32> packed_user_data;
	for (const auto reg: result.program.bindings.user_data_registers) {
		packed_user_data.push_back(result.resources.user_data[reg - result.program.user_data_base]);
	}
	packed_user_data.resize(result.program.bindings.ShaderDataDwords());
	for (u32 i = 0; i < result.program.bindings.buffer_offset_count; i++) {
		const auto offset =
		    i < test.storage_buffer_offsets.size() ? test.storage_buffer_offsets[i] : 0u;
		Require(test.name, "shader data", offset % sizeof(u32) == 0 && offset < 256,
		        "storage buffer offset is not representable");
		packed_user_data[result.program.bindings.buffer_offset_dword + i / 4u] |=
		    offset << ((i % 4u) * 8u);
	}
	return {std::move(result.spirv), std::move(result.program),
	        std::move(result.resources.flattened_srt), std::move(packed_user_data)};
}

std::array<u32, 64> MakeStructuredStorageBufferData(u32 stride_bytes, u32 num_records,
                                                    bool add_tid = false, u32 format = 0) {
	std::array<u32, 64> data {};
	data[1] = (stride_bytes & 0x3fffu) << 16u;
	data[2] = num_records;
	data[3] = 1u << 24u;
	if (add_tid) {
		data[3] |= 1u << 23u;
	}
	data[3] |= (format & 0x7fu) << 12u;
	return data;
}

std::array<u32, 64> MakeStorageTextureData(Prospero::BufferFormat format) {
	std::array<u32, 64> data {};
	data[0] = 0x1000u;
	data[1] = (static_cast<uint32_t>(format) & 0x1ffu) << 20u;
	data[3] = static_cast<uint32_t>(Prospero::ImageType::kColor2D) << 28u;
	return data;
}

CompiledShader CompileFragmentCase(const GraphicsCase& test) {
	const auto user_data = MakeNativeUserData(test.has_user_data ? &test.user_data : nullptr);
	ShaderPixelInputInfo pixel_info {};
	pixel_info.input_num         = test.pixel_interpolator_settings.empty()
	                                   ? 1u
	                                   : static_cast<u32>(test.pixel_interpolator_settings.size());
	pixel_info.ps_no_perspective = test.pixel_no_perspective;
	for (u32 i = 0; i < std::size(pixel_info.interpolator_settings); i++) {
		pixel_info.interpolator_settings[i] = i;
	}
	for (u32 i = 0; i < test.pixel_interpolator_settings.size() &&
	                i < std::size(pixel_info.interpolator_settings);
	     i++) {
		pixel_info.interpolator_settings[i] = test.pixel_interpolator_settings[i];
	}

	ShaderRecompiler::CompileOptions options;
	options.stage            = ShaderType::Pixel;
	options.dump_ir          = false;
	options.pixel_input_info = &pixel_info;
	options.user_data        = user_data.data();

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	if (!ShaderRecompiler::TryRecompile(test.fragment_code, options, result, &error)) {
		Fail(test.name, "decode/IR", error.c_str());
	}
	Require(test.name, "SPIR-V emit", !result.spirv.empty(), "recompiler returned empty SPIR-V");
	ValidateSpirv(test.name, result.spirv);
	std::vector<u32> packed_user_data;
	for (const auto reg: result.program.bindings.user_data_registers) {
		packed_user_data.push_back(result.resources.user_data[reg - result.program.user_data_base]);
	}
	packed_user_data.resize(result.program.bindings.ShaderDataDwords());
	return {std::move(result.spirv), std::move(result.program),
	        std::move(result.resources.flattened_srt), std::move(packed_user_data)};
}

std::array<u32, 64> MakeSampledTextureData(Prospero::BufferFormat format) {
	return MakeStorageTextureData(format);
}

namespace TestSpv {

enum : u32 {
	ExecutionModelVertex   = 0,
	AddressingModelLogical = 0,
	MemoryModelGLSL450     = 1,
	CapabilityShader       = 1,
	StorageClassInput      = 1,
	StorageClassOutput     = 3,
	FunctionControlNone    = 0,
	DecorationBlock        = 2,
	DecorationBuiltIn      = 11,
	DecorationLocation     = 30,
	BuiltInPosition        = 0,
	OpTypeVoid             = 19,
	OpTypeInt              = 21,
	OpTypeFloat            = 22,
	OpTypeVector           = 23,
	OpTypeStruct           = 30,
	OpTypePointer          = 32,
	OpTypeFunction         = 33,
	OpConstant             = 43,
	OpFunction             = 54,
	OpFunctionEnd          = 56,
	OpVariable             = 59,
	OpLoad                 = 61,
	OpStore                = 62,
	OpAccessChain          = 65,
	OpDecorate             = 71,
	OpMemberDecorate       = 72,
	OpCompositeConstruct   = 80,
	OpCompositeExtract     = 81,
	OpLabel                = 248,
	OpReturn               = 253,
};

std::vector<u32> MakePassthroughVertexSpirv() {
	using ShaderRecompiler::Spirv::Builder;

	Builder    b;
	const auto void_type             = b.AllocateId();
	const auto uint_type             = b.AllocateId();
	const auto float_type            = b.AllocateId();
	const auto vec2_type             = b.AllocateId();
	const auto vec4_type             = b.AllocateId();
	const auto per_vertex_type       = b.AllocateId();
	const auto ptr_input_vec2        = b.AllocateId();
	const auto ptr_input_vec4        = b.AllocateId();
	const auto ptr_output_vec4       = b.AllocateId();
	const auto ptr_output_per_vertex = b.AllocateId();
	const auto func_type             = b.AllocateId();
	const auto const_u32_0           = b.AllocateId();
	const auto const_f32_0           = b.AllocateId();
	const auto const_f32_1           = b.AllocateId();
	const auto in_pos                = b.AllocateId();
	const auto in_color              = b.AllocateId();
	const auto out_color             = b.AllocateId();
	const auto per_vertex            = b.AllocateId();
	const auto main                  = b.AllocateId();
	const auto label                 = b.AllocateId();
	const auto pos2                  = b.AllocateId();
	const auto color4                = b.AllocateId();
	const auto pos_x                 = b.AllocateId();
	const auto pos_y                 = b.AllocateId();
	const auto position              = b.AllocateId();
	const auto position_ptr          = b.AllocateId();

	b.AddCapability({CapabilityShader});
	b.AddMemoryModel({AddressingModelLogical, MemoryModelGLSL450});
	b.AddEntryPoint(ExecutionModelVertex, main, "main", {in_pos, in_color, per_vertex, out_color});
	b.AddAnnotation({OpDecorate, in_pos, DecorationLocation, 0});
	b.AddAnnotation({OpDecorate, in_color, DecorationLocation, 1});
	b.AddAnnotation({OpDecorate, out_color, DecorationLocation, 0});
	b.AddAnnotation({OpDecorate, per_vertex_type, DecorationBlock});
	b.AddAnnotation({OpMemberDecorate, per_vertex_type, 0, DecorationBuiltIn, BuiltInPosition});

	b.AddType({OpTypeVoid, void_type});
	b.AddType({OpTypeInt, uint_type, 32, 0});
	b.AddType({OpTypeFloat, float_type, 32});
	b.AddType({OpTypeVector, vec2_type, float_type, 2});
	b.AddType({OpTypeVector, vec4_type, float_type, 4});
	b.AddType({OpTypeStruct, per_vertex_type, vec4_type});
	b.AddType({OpTypePointer, ptr_input_vec2, StorageClassInput, vec2_type});
	b.AddType({OpTypePointer, ptr_input_vec4, StorageClassInput, vec4_type});
	b.AddType({OpTypePointer, ptr_output_vec4, StorageClassOutput, vec4_type});
	b.AddType({OpTypePointer, ptr_output_per_vertex, StorageClassOutput, per_vertex_type});
	b.AddType({OpTypeFunction, func_type, void_type});
	b.AddType({OpConstant, uint_type, const_u32_0, 0});
	b.AddType({OpConstant, float_type, const_f32_0, 0x00000000u});
	b.AddType({OpConstant, float_type, const_f32_1, 0x3f800000u});
	b.AddType({OpVariable, ptr_input_vec2, in_pos, StorageClassInput});
	b.AddType({OpVariable, ptr_input_vec4, in_color, StorageClassInput});
	b.AddType({OpVariable, ptr_output_vec4, out_color, StorageClassOutput});
	b.AddType({OpVariable, ptr_output_per_vertex, per_vertex, StorageClassOutput});

	b.AddFunction({OpFunction, void_type, main, FunctionControlNone, func_type});
	b.AddFunction({OpLabel, label});
	b.AddFunction({OpLoad, vec2_type, pos2, in_pos});
	b.AddFunction({OpLoad, vec4_type, color4, in_color});
	b.AddFunction({OpCompositeExtract, float_type, pos_x, pos2, 0});
	b.AddFunction({OpCompositeExtract, float_type, pos_y, pos2, 1});
	b.AddFunction(
	    {OpCompositeConstruct, vec4_type, position, pos_x, pos_y, const_f32_0, const_f32_1});
	b.AddFunction({OpAccessChain, ptr_output_vec4, position_ptr, per_vertex, const_u32_0});
	b.AddFunction({OpStore, position_ptr, position});
	b.AddFunction({OpStore, out_color, color4});
	b.AddFunction({OpReturn});
	b.AddFunction({OpFunctionEnd});
	return b.Build();
}

} // namespace TestSpv

class VulkanHarness {
public:
	VulkanHarness() { Init(); }
	~VulkanHarness() { Destroy(); }

	VulkanHarness(const VulkanHarness&)            = delete;
	VulkanHarness& operator=(const VulkanHarness&) = delete;

	struct Buffer {
		vk::Buffer       buffer   = nullptr;
		vk::DeviceMemory memory   = nullptr;
		vk::DeviceSize   size     = 0;
		bool             coherent = false;
	};

	struct Image {
		vk::Image        image            = nullptr;
		vk::DeviceMemory memory           = nullptr;
		vk::ImageView    view             = nullptr;
		vk::Format       format           = vk::Format::eUndefined;
		vk::ImageLayout  layout           = vk::ImageLayout::eUndefined;
		u32              width            = 0;
		u32              height           = 0;
		u32              layers           = 1;
		u32              mip_levels       = 1;
		u32              dwords_per_pixel = 0;
	};

	[[nodiscard]] vk::Device      Device() const { return m_device; }
	[[nodiscard]] GraphicContext& RuntimeContext() {
		EnsureRuntimeContext();
		return m_runtime_context;
	}
	[[nodiscard]] RenderContext& RuntimeRenderer() {
		EnsureRuntimeContext();
		return Renderer();
	}

	void CheckCommandPoolGrowth() {
		EnsureRuntimeContext();
		std::vector<std::unique_ptr<CommandBuffer>> commands;
		for (uint32_t i = 0; i < 12; i++) {
			commands.push_back(std::make_unique<CommandBuffer>(Renderer().GetCommandScheduler()));
		}
		for (auto& command: commands) {
			Require("CommandPoolGrowth", "allocation", !command->IsInvalid(),
			        "unified command pool failed to grow");
			command->Begin();
			command->End();
			command->Execute();
		}
		for (auto& command: commands) {
			command->WaitForFence();
		}
		std::printf("[host]    %-32s ok\n", "CommandPoolGrowth");
	}

	void CheckSchedulerTimeline() {
		EnsureRuntimeContext();
		CommandScheduler scheduler(Renderer(), m_runtime_context);
		HW::Context      registers {};
		HW::UserConfig   user_config {};
		HW::Shader       shaders {};
		scheduler.Begin(registers, user_config, shaders);

		const auto first_tick = scheduler.CurrentTick();
		uint32_t   completed  = 0;
		scheduler.DeferOperation(
		    [owned = std::make_unique<uint32_t>(1), &completed] { completed = *owned; });
		scheduler.Flush();
		scheduler.Wait(first_tick);
		Require("SchedulerTimeline", "first tick",
		        scheduler.CurrentTick() == first_tick + 1 && scheduler.IsFree(first_tick) &&
		            completed == 1,
		        "timeline wait did not release its deferred operation");

		const auto        second_tick = scheduler.CurrentTick();
		std::atomic<bool> priority_in_callback {false};
		scheduler.DeferPriorityOperation(
		    [owned = std::make_unique<uint32_t>(2), &completed, &priority_in_callback] {
			    priority_in_callback = CommandScheduler::InDeferredOperation();
			    completed            = *owned;
		    });
		scheduler.Finish();
		scheduler.DrainPriorityOperations();
		Require("SchedulerTimeline", "finish",
		        scheduler.CurrentTick() == second_tick + 1 && scheduler.IsFree(second_tick) &&
		            completed == 2 && priority_in_callback.load() &&
		            !CommandScheduler::InDeferredOperation(),
		        "scheduler completion drain lost its tick or callback context");

		const auto implicit_flush_tick = scheduler.CurrentTick();
		scheduler.DeferOperation(
		    [owned = std::make_unique<uint32_t>(3), &completed] { completed = *owned; });
		scheduler.Wait(implicit_flush_tick);
		Require("SchedulerTimeline", "implicit flush",
		        scheduler.CurrentTick() == implicit_flush_tick + 1 &&
		            scheduler.IsFree(implicit_flush_tick) && completed == 3,
		        "waiting for the current tick did not flush it");

		vk::SemaphoreTypeCreateInfo timeline_type {};
		timeline_type.sType         = vk::StructureType::eSemaphoreTypeCreateInfo;
		timeline_type.semaphoreType = vk::SemaphoreType::eTimeline;
		vk::SemaphoreCreateInfo timeline_create {};
		timeline_create.sType           = vk::StructureType::eSemaphoreCreateInfo;
		timeline_create.pNext           = &timeline_type;
		vk::Semaphore external_timeline = nullptr;
		Require("SchedulerTimeline", "external timeline create",
		        m_runtime_context.device.createSemaphore(
		            &timeline_create, nullptr, &external_timeline) == vk::Result::eSuccess &&
		            external_timeline != nullptr,
		        "failed to create the external timeline semaphore");

		constexpr uint64_t external_wait_value = 9;
		const auto         external_wait_tick  = scheduler.CurrentTick();
		SubmitInfo         external_wait;
		external_wait.AddWait(external_timeline, external_wait_value);
		constexpr size_t blocked_submission_count = 6;
		std::array<vk::CommandBuffer, blocked_submission_count> blocked_handles {};
		for (size_t i = 0; i < blocked_submission_count; ++i) {
			blocked_handles[i] = scheduler.Current().Handle();
			if (i == 0) {
				scheduler.Flush(external_wait);
			} else {
				scheduler.Flush();
			}
		}
		bool distinct_handles = true;
		for (size_t i = 0; i < blocked_handles.size(); ++i) {
			for (size_t j = i + 1; j < blocked_handles.size(); ++j) {
				distinct_handles &= blocked_handles[i] != blocked_handles[j];
			}
		}
		const auto last_blocked_tick = scheduler.CurrentTick() - 1;
		Require("SchedulerTimeline", "external timeline overflow",
		        distinct_handles && last_blocked_tick ==
		                                external_wait_tick + blocked_submission_count - 1 &&
		            !scheduler.IsFree(last_blocked_tick),
		        "blocked submissions did not grow the command-buffer pool");
		vk::SemaphoreSignalInfo signal_info {};
		signal_info.sType     = vk::StructureType::eSemaphoreSignalInfo;
		signal_info.semaphore = external_timeline;
		signal_info.value     = external_wait_value;
		Require("SchedulerTimeline", "external timeline signal",
		        m_runtime_context.device.signalSemaphore(&signal_info) == vk::Result::eSuccess,
		        "failed to signal the external timeline semaphore");
		scheduler.Wait(last_blocked_tick);
		m_runtime_context.device.destroySemaphore(external_timeline, nullptr);
		scheduler.Flush();
		scheduler.Flush();
		Require("SchedulerTimeline", "timeline pool reuse",
		        std::ranges::find(blocked_handles, scheduler.Current().Handle()) !=
		            blocked_handles.end(),
		        "completed command buffers were not reused after timeline progress");
		scheduler.Shutdown();

		CommandScheduler draining(Renderer(), m_runtime_context);
		HW::Context      drain_registers {};
		HW::UserConfig   drain_user_config {};
		HW::Shader       drain_shaders {};
		draining.Begin(drain_registers, drain_user_config, drain_shaders);
		uint32_t reentrant = 0;
		draining.DeferOperation([&draining, &reentrant] {
			reentrant++;
			draining.DeferOperation([&reentrant] { reentrant++; });
		});

		std::binary_semaphore priority_entered {0};
		std::binary_semaphore release_priority {0};
		draining.DeferPriorityOperation([&draining, &priority_entered, &release_priority] {
			priority_entered.release();
			draining.Shutdown();
			release_priority.acquire();
		});
		std::jthread shutdown_thread([&draining] { draining.Shutdown(); });
		priority_entered.acquire();
		std::jthread second_shutdown_thread([&draining] { draining.Shutdown(); });

		std::atomic<bool> concurrent_completed {false};
		std::jthread      concurrent_defer([&draining, &concurrent_completed] {
			draining.DeferOperation([&concurrent_completed] { concurrent_completed = true; });
		});
		release_priority.release();
		shutdown_thread.join();
		second_shutdown_thread.join();
		concurrent_defer.join();
		Require("SchedulerTimeline", "shutdown drain",
		        reentrant == 2 && concurrent_completed.load(),
		        "shutdown lost reentrant or concurrent deferred work");
		std::printf("[host]    %-32s ok\n", "SchedulerTimeline");
	}

	void CheckGpuMappedRangeLifecycle() {
		EnsureRuntimeContext();
		auto&            context = Renderer();
		CommandScheduler scheduler(context, m_runtime_context);
		HW::Context      registers {};
		HW::UserConfig   user_config {};
		HW::Shader       shaders {};
		scheduler.Begin(registers, user_config, shaders);
		context.InitializeGpu(nullptr);
		auto&              gpu = context.GetGpu();
		GpuResourceManager resources(m_runtime_context, scheduler);
		resources.SetGpu(&gpu);

		constexpr uint64_t base = 0x0000000200000000ull;
		constexpr uint64_t page = 0x4000;
		resources.MapMemory(base, page * 4);
		resources.MapMemory(base + page * 2, page * 4);
		Require("GpuMappedRangeLifecycle", "union",
		        resources.IsMapped(base, page * 6) && !resources.IsMapped(base, page * 7),
		        "overlapping maps did not form one interval union");

		resources.UnmapMemory(base + page * 2, page * 2);
		Require("GpuMappedRangeLifecycle", "subtract",
		        resources.IsMapped(base, page * 2) &&
		            resources.IsMapped(base + page * 4, page * 2) &&
		            !resources.IsMapped(base, page * 6),
		        "partial unmap did not punch the expected interval hole");

		resources.UnmapMemory(base + page * 2, page * 2);
		Require("GpuMappedRangeLifecycle", "idempotent unmap",
		        resources.IsMapped(base, page * 2) && resources.IsMapped(base + page * 4, page * 2),
		        "unmapping an absent interval changed neighboring mappings");

		resources.UnmapMemory(base, page * 6);
		Require("GpuMappedRangeLifecycle", "clear", !resources.IsMapped(base, page * 6),
		        "full unmap did not clear the interval union");

		constexpr uint64_t old_prt = base + page * 8;
		constexpr uint64_t new_prt = base + page * 16;
		resources.MapMemory(old_prt, page * 4);
		resources.UnmapMemory(old_prt, page * 4);
		resources.MapMemory(new_prt, page * 6);
		Require("GpuMappedRangeLifecycle", "PRT replacement",
		        !resources.IsMapped(old_prt, page * 4) && resources.IsMapped(new_prt, page * 6),
		        "old-unmap/new-map did not replace full PRT coverage");

		resources.SetGpu(nullptr);
		scheduler.Finish();
		context.ShutdownGpu();
		std::printf("[host]    %-32s ok\n", "GpuMappedRangeLifecycle");
	}

	void CheckStreamBufferRing() {
		EnsureRuntimeContext();
		CommandScheduler scheduler(Renderer(), m_runtime_context);
		HW::Context      registers {};
		HW::UserConfig   user_config {};
		HW::Shader       shaders {};
		scheduler.Begin(registers, user_config, shaders);
		GpuResourceManager resources(m_runtime_context, scheduler);
		auto&              cache = resources.GetBufferCache();
		constexpr std::array<std::pair<MemoryUsage, uint64_t>, 4> utilities {{
		    {MemoryUsage::Upload, 512ull << 20},
		    {MemoryUsage::Stream, 64ull << 20},
		    {MemoryUsage::Download, 32ull << 20},
		    {MemoryUsage::DeviceLocal, 128ull << 20},
		}};
		std::array<vk::Buffer, utilities.size()>                  handles {};
		for (size_t i = 0; i < utilities.size(); i++) {
			auto& utility = cache.GetUtilityBuffer(utilities[i].first);
			handles[i]    = utility.Handle();
			Require("StreamBufferRing", "utility ownership",
			        utility.Usage() == utilities[i].first &&
			            utility.Size() == utilities[i].second && utility.Handle() != nullptr,
			        "BufferCache utility buffer does not match the fixed layout");
		}
		for (size_t i = 0; i < handles.size(); i++) {
			for (size_t j = i + 1; j < handles.size(); j++) {
				Require("StreamBufferRing", "utility identity", handles[i] != handles[j],
				        "utility usages alias the same native buffer");
			}
		}
		auto*      fixed_download = &cache.GetUtilityBuffer(MemoryUsage::Download);
		const auto fixed_handle   = fixed_download->Handle();
		const auto [oversized_download, oversized_offset] =
		    fixed_download->Map(fixed_download->Size() + 4, 64, false);
		const auto [download_probe, download_probe_offset] = fixed_download->Map(64, 64, false);
		Require("StreamBufferRing", "fixed download utility",
		        oversized_download == nullptr && oversized_offset == 0 &&
		            download_probe != nullptr && download_probe_offset == 0 &&
		            &cache.GetUtilityBuffer(MemoryUsage::Download) == fixed_download &&
		            fixed_download->Size() == (32ull << 20) &&
		            fixed_download->Handle() == fixed_handle,
		        "oversized download replaced or corrupted the fixed shared ring");
		fixed_download->Commit();

		std::vector<uint8_t> full_stream(64ull << 20, 0x5a);
		const auto           utility_tick = scheduler.CurrentTick();
		auto full_binding = cache.UploadTransient(full_stream.data(), full_stream.size(), 16);
		const std::array<uint8_t, 16> overflow_data {0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
		                                             0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
		                                             0xa5, 0xa5, 0xa5, 0xa5};
		auto                          overflow_binding =
		    cache.UploadTransient(overflow_data.data(), overflow_data.size(), 16);
		auto overflow_owner =
		    std::static_pointer_cast<Libs::Graphics::Buffer>(overflow_binding.owner);
		Require("StreamBufferRing", "current tick overflow",
		        full_binding.owner == nullptr &&
		            full_binding.buffer == cache.GetUtilityBuffer(MemoryUsage::Stream).Handle() &&
		            overflow_owner != nullptr &&
		            overflow_binding.buffer == overflow_owner->Handle() &&
		            overflow_binding.buffer != full_binding.buffer &&
		            overflow_owner->Mapped().size() == overflow_data.size() &&
		            std::memcmp(overflow_owner->Mapped().data(), overflow_data.data(),
		                        overflow_data.size()) == 0 &&
		            scheduler.CurrentTick() == utility_tick,
		        "current-tick stream overflow submitted partial state or lost bytes");

		const auto atom = m_runtime_context.physical_device_properties.limits.nonCoherentAtomSize;
		uint64_t   policy_size        = 17;
		uint64_t   policy_alignment   = 12;
		uint64_t   coherent_size      = policy_size;
		uint64_t   coherent_alignment = policy_alignment;
		Require("StreamBufferRing", "non-coherent reservation policy",
		        StreamBufferTestAccess::NormalizeReservation(false, 256, policy_size,
		                                                     policy_alignment) &&
		            policy_size == 256 && policy_alignment == 768 &&
		            StreamBufferTestAccess::NormalizeReservation(true, 256, coherent_size,
		                                                         coherent_alignment) &&
		            coherent_size == 17 && coherent_alignment == 12,
		        "non-coherent reservations are not isolated at the atom/texel LCM");
		const auto   small_ring_size = std::max<uint64_t>(64, atom * 2);
		StreamBuffer stream(m_runtime_context, scheduler, MemoryUsage::Upload, small_ring_size);
		const auto   first_size =
		    stream.IsCoherent() ? small_ring_size - 16 : small_ring_size - atom + 1;
		constexpr uint64_t blocked_size  = 24;
		const auto [first, first_offset] = stream.Map(first_size, 16, false);
		Require("StreamBufferRing", "first allocation", first != nullptr && first_offset == 0,
		        "initial stream allocation failed");
		std::memset(first, 0x5a, static_cast<size_t>(first_size));
		stream.Commit();

		const auto [oversize, oversize_offset] = stream.Map(small_ring_size + 1, 1, false);
		Require("StreamBufferRing", "oversize", oversize == nullptr && oversize_offset == 0,
		        "oversize allocation was accepted");
		const auto [blocked, blocked_offset]             = stream.Map(blocked_size, 1, false);
		const auto [blocked_again, blocked_again_offset] = stream.Map(blocked_size, 1, false);
		Require("StreamBufferRing", "transactional failure",
		        blocked == nullptr && blocked_offset == 0 && blocked_again == nullptr &&
		            blocked_again_offset == 0,
		        "failed wrap mutated stream state");

		scheduler.Flush();
		const auto [wrapped, wrapped_offset] = stream.Map(blocked_size, 1);
		Require("StreamBufferRing", "watched wrap", wrapped != nullptr && wrapped_offset == 0,
		        "completed watch did not release the wrapped allocation");
		stream.Commit();

		const auto download_alignment = atom / std::gcd<uint64_t>(atom, 12) * 12;
		const auto download_ring_size =
		    std::max<uint64_t>(64, download_alignment + std::max<uint64_t>(atom, 16));
		StreamBuffer download(m_runtime_context, scheduler, MemoryUsage::Download,
		                      download_ring_size);
		const auto [download_data, download_offset] = download.Map(16, 8);
		Require("StreamBufferRing", "download allocation",
		        download_data != nullptr && download_offset == 0,
		        "download stream is not host visible");
		download.Commit();
		scheduler.Flush();
		const auto [second_download, second_download_offset] = download.Map(16, 12);
		Require("StreamBufferRing", "download atom isolation",
		        second_download != nullptr &&
		            (download.IsCoherent() ||
		             (download_offset % atom == 0 && second_download_offset % atom == 0 &&
		              second_download_offset >= download_offset + atom)) &&
		            second_download_offset % 12 == 0,
		        "download reservations share a non-coherent atom or lost caller "
		        "alignment");
		download.Commit();

		scheduler.Finish();
		download.Invalidate(download_offset, 16);
		std::printf("[host]    %-32s ok\n", "StreamBufferRing");
	}

	void CheckGpuCommandLane() {
		EnsureRuntimeContext();
		auto& context = Renderer();
		context.InitializeGpu(nullptr);
		auto& gpu = context.GetGpu();

		const auto      caller_thread = std::this_thread::get_id();
		std::thread::id gpu_thread;
		std::thread::id nested_thread;
		uint32_t        order = 0;
		gpu.SendCommand([&order] { order = 1; });
		gpu.SendCommandSync([&] {
			gpu_thread = std::this_thread::get_id();
			Require("GpuCommandLane", "FIFO", order == 1,
			        "synchronous command overtook an older host command");
			Require("GpuCommandLane", "GPU context", Gpu::IsGpuThread(),
			        "host command did not run on the GPU thread");
			gpu.SendCommandSync([&nested_thread] { nested_thread = std::this_thread::get_id(); });
			order = 2;
		});
		Require("GpuCommandLane", "dispatch thread",
		        order == 2 && gpu_thread != caller_thread && nested_thread == gpu_thread,
		        "host command did not run on the GPU thread or nested sync deadlocked");

		{
			const auto make_release_mem = [](uint32_t data_sel, uint32_t interrupt_selector,
			                                 void* destination, uint64_t value,
			                                 uint32_t action = 0x28u,
			                                 uint32_t gcr_cntl = 1u << 9u) {
				std::array<uint32_t, 8> packet {};
				const auto              address     = reinterpret_cast<uint64_t>(destination);
				const auto              event_index = (action >= 0x2fu ? 6u : 5u);
				packet[0] = KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_RELEASE_MEM);
				packet[1] = action | (event_index << 8u) | (gcr_cntl << 12u);
				packet[2] = (interrupt_selector << 24u) | (data_sel << 29u);
				packet[3] = static_cast<uint32_t>(address);
				packet[4] = static_cast<uint32_t>(address >> 32u);
				packet[5] = static_cast<uint32_t>(value);
				packet[6] = static_cast<uint32_t>(value >> 32u);
				packet[7] = 0x123u;
				return packet;
			};

			vk::SemaphoreTypeCreateInfo timeline_type {};
			timeline_type.sType         = vk::StructureType::eSemaphoreTypeCreateInfo;
			timeline_type.semaphoreType = vk::SemaphoreType::eTimeline;
			vk::SemaphoreCreateInfo timeline_create {};
			timeline_create.sType           = vk::StructureType::eSemaphoreCreateInfo;
			timeline_create.pNext           = &timeline_type;
			vk::Semaphore blocked_timeline = nullptr;
			Require("GpuCommandLane", "blocked timeline create",
			        m_runtime_context.device.createSemaphore(
			            &timeline_create, nullptr, &blocked_timeline) == vk::Result::eSuccess &&
			            blocked_timeline != nullptr,
			        "failed to create the blocked GPU timeline");

			constexpr uint64_t release_tick = 1;
			auto&              gpu_scheduler = context.GetCommandScheduler();
			auto               processor     = std::make_unique<CommandProcessor>(context);
			gpu.SendCommandSync([&] {
				processor->BufferInit();
				SubmitInfo blocked_submit;
				blocked_submit.AddWait(blocked_timeline, release_tick);
				gpu_scheduler.Flush(blocked_submit);
			});

			std::binary_semaphore parser_complete {0};
			alignas(uint64_t) uint64_t interrupt_only_gds_label = UINT64_MAX;
			alignas(uint64_t) uint64_t cb_db_release_label       = UINT64_MAX;
			bool                       parser_kept_nonblocking_boundaries = false;
			std::jthread               no_gpu_wait([&] {
				gpu.SendCommandSync([&] {
					processor->BufferInit();
					const auto tick_before = gpu_scheduler.CurrentTick();
					processor->TriggerEvent(0x16u, 0u);
					auto         packet = make_release_mem(0, 0, nullptr, 0);
					Pm4Execution execution;
					const auto   result = processor->Process(execution, packet.data(), packet.size());
					processor->IncrementDe();
					const bool cache_and_counter_did_not_submit =
					    result == Pm4ProcessResult::Complete &&
					    gpu_scheduler.CurrentTick() == tick_before;

					const auto cb_db_tick = gpu_scheduler.CurrentTick();
					auto cb_db_release = make_release_mem(2, 0, &cb_db_release_label, 0, 0x14u, 0);
					Pm4Execution cb_db_execution;
					const auto   cb_db_result = processor->Process(
					    cb_db_execution, cb_db_release.data(), cb_db_release.size());
					const bool cb_db_release_did_not_submit =
					    cb_db_result == Pm4ProcessResult::Complete && cb_db_release_label == 0 &&
					    gpu_scheduler.CurrentTick() == cb_db_tick;

					auto gds_interrupt_only =
					    make_release_mem(5, 1, &interrupt_only_gds_label, 1ull << 16u);
					Pm4Execution gds_interrupt_execution;
					const auto   interrupt_tick = gpu_scheduler.CurrentTick();
					const auto   interrupt_result = processor->Process(
					    gds_interrupt_execution, gds_interrupt_only.data(), gds_interrupt_only.size());
					parser_kept_nonblocking_boundaries =
					    cache_and_counter_did_not_submit && cb_db_release_did_not_submit &&
					    interrupt_result == Pm4ProcessResult::Complete &&
					    gpu_scheduler.CurrentTick() == interrupt_tick + 1 &&
					    interrupt_only_gds_label == UINT64_MAX;
				});
				parser_complete.release();
			});
			const bool parser_did_not_wait_gpu =
			    parser_complete.try_acquire_for(std::chrono::seconds(2));

			vk::SemaphoreSignalInfo signal_info {};
			signal_info.sType     = vk::StructureType::eSemaphoreSignalInfo;
			signal_info.semaphore = blocked_timeline;
			signal_info.value     = release_tick;
			Require("GpuCommandLane", "blocked timeline signal",
			        m_runtime_context.device.signalSemaphore(&signal_info) == vk::Result::eSuccess,
			        "failed to release the blocked GPU timeline");
			no_gpu_wait.join();
			gpu.SendCommandSync([&] { gpu_scheduler.Finish(); });
			m_runtime_context.device.destroySemaphore(blocked_timeline, nullptr);
			Require("GpuCommandLane", "nonblocking GPU packets",
			        parser_did_not_wait_gpu && parser_kept_nonblocking_boundaries,
			        "a cache event, GL2 writeback, DE counter, or interrupt boundary used the wrong "
			        "submission behavior");

			alignas(uint64_t) uint64_t release_label = 0;
			alignas(uint64_t) uint64_t gds_label     = UINT64_MAX;
			bool                       release_mem_submission_counts = false;
			gpu.SendCommandSync([&] {
				processor->BufferInit();

				auto         immediate = make_release_mem(1, 0, &release_label, 0x11223344u);
				Pm4Execution immediate_execution;
				const auto   immediate_tick = gpu_scheduler.CurrentTick();
				const auto   immediate_result =
				    processor->Process(immediate_execution, immediate.data(), immediate.size());
				const bool immediate_split_once =
				    gpu_scheduler.CurrentTick() == immediate_tick + 1;

				auto         gds = make_release_mem(5, 0, &gds_label, 1ull << 16u);
				Pm4Execution gds_execution;
				const auto   gds_tick   = gpu_scheduler.CurrentTick();
				const auto   gds_result = processor->Process(gds_execution, gds.data(), gds.size());
				const bool   gds_waited_once = gpu_scheduler.CurrentTick() == gds_tick + 1;

				auto         interrupt_only = make_release_mem(0, 4, nullptr, 0);
				Pm4Execution interrupt_execution;
				const auto   interrupt_tick = gpu_scheduler.CurrentTick();
				const auto interrupt_result = processor->Process(
				    interrupt_execution, interrupt_only.data(), interrupt_only.size());
				const bool interrupt_split_once =
				    gpu_scheduler.CurrentTick() == interrupt_tick + 1;

				auto         gds_interrupt = make_release_mem(5, 2, &gds_label, 1ull << 16u);
				Pm4Execution gds_interrupt_execution;
				const auto   gds_interrupt_tick = gpu_scheduler.CurrentTick();
				const auto   gds_interrupt_result = processor->Process(
				    gds_interrupt_execution, gds_interrupt.data(), gds_interrupt.size());
				const bool gds_interrupt_waited_once =
				    gpu_scheduler.CurrentTick() == gds_interrupt_tick + 1;

				release_mem_submission_counts =
				    immediate_result == Pm4ProcessResult::Complete && immediate_split_once &&
				    gds_result == Pm4ProcessResult::Complete && gds_waited_once &&
				    interrupt_result == Pm4ProcessResult::Complete && interrupt_split_once &&
				    gds_interrupt_result == Pm4ProcessResult::Complete &&
				    gds_interrupt_waited_once;
			});
			gpu.SendCommandSync([&] { gpu_scheduler.Finish(); });
			Require("GpuCommandLane", "RELEASE_MEM submission counts",
			        release_mem_submission_counts &&
			            static_cast<uint32_t>(release_label) == 0x11223344u &&
			            static_cast<uint32_t>(gds_label) == 0,
			        "RELEASE_MEM lost its required split/readback or retained a redundant GPU wait");
		}

		alignas(uint32_t) uint32_t packet_marker_a = 0;
		alignas(uint32_t) uint32_t packet_marker_b = 0;
		const auto write_packet = [](uint32_t* packet, uint32_t* destination, uint32_t value) {
			const auto destination_address = reinterpret_cast<uint64_t>(destination);
			packet[0] = KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0);
			packet[1] = 0;
			packet[2] = static_cast<uint32_t>(destination_address);
			packet[3] = static_cast<uint32_t>(destination_address >> 32u);
			packet[4] = value;
		};
		std::array<uint32_t, 1024> polling_loop {};
		for (size_t i = 0; i < polling_loop.size() - 4; i += 2) {
			polling_loop[i]     = KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO);
			polling_loop[i + 1] = 0;
		}
		const auto loop_address = reinterpret_cast<uint64_t>(polling_loop.data());
		polling_loop[1020] = KYTY_PM4(4, Pm4::IT_INDIRECT_BUFFER, 0);
		polling_loop[1021] = static_cast<uint32_t>(loop_address);
		polling_loop[1022] = static_cast<uint32_t>(loop_address >> 32u);
		polling_loop[1023] = 0x0f200000u | static_cast<uint32_t>(polling_loop.size());

		std::array<uint32_t, 14> polling_commands {};
		write_packet(polling_commands.data(), &packet_marker_a, 11);
		polling_commands[5] = KYTY_PM4(4, Pm4::IT_INDIRECT_BUFFER, 0);
		polling_commands[6] = static_cast<uint32_t>(loop_address);
		polling_commands[7] = static_cast<uint32_t>(loop_address >> 32u);
		polling_commands[8] = 0x0f200000u | static_cast<uint32_t>(polling_loop.size());
		write_packet(polling_commands.data() + 9, &packet_marker_b, 22);
		std::binary_semaphore stream_gate_entered {0};
		std::binary_semaphore stream_gate_release {0};
		gpu.SendCommand([&] {
			stream_gate_entered.release();
			stream_gate_release.acquire();
		});
		gpu.Submit(polling_commands.data(), polling_commands.size(), nullptr, 0);
		stream_gate_entered.acquire();
		stream_gate_release.release();
		uint32_t packet_marker_a_at_callback = UINT32_MAX;
		uint32_t packet_marker_b_at_callback = UINT32_MAX;
		for (uint32_t attempt = 0; attempt < 8 && packet_marker_a_at_callback != 11; attempt++) {
			gpu.SendCommandSync([&] {
				if (packet_marker_a == 11 && packet_marker_b == 0) {
					packet_marker_a_at_callback = packet_marker_a;
					packet_marker_b_at_callback = packet_marker_b;
					polling_loop[1020] = KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO);
					polling_loop[1021] = 0;
					polling_loop[1022] = KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO);
					polling_loop[1023] = 0;
				}
			});
			std::this_thread::yield();
		}
		gpu.Done();
		Require("GpuCommandLane", "packet-boundary command polling",
		        packet_marker_a_at_callback == 11 && packet_marker_b_at_callback == 0 &&
		            packet_marker_a == 11 && packet_marker_b == 22,
		        "host command waited for an entire non-suspended PM4 stream");

		uint32_t   label   = 0;
		uint32_t   prefix  = 0;
		uint32_t   suffix  = 0;
		const auto address = [](const void* value) { return reinterpret_cast<uint64_t>(value); };
		std::array<uint32_t, 17> commands {};
		commands[0]  = KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0);
		commands[1]  = 0;
		commands[2]  = static_cast<uint32_t>(address(&prefix));
		commands[3]  = static_cast<uint32_t>(address(&prefix) >> 32u);
		commands[4]  = 11;
		commands[5]  = KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_WAIT_MEM_32);
		commands[6]  = static_cast<uint32_t>(address(&label));
		commands[7]  = static_cast<uint32_t>(address(&label) >> 32u);
		commands[8]  = UINT32_MAX;
		commands[9]  = 1;
		commands[10] = 0x10u | 3u;
		commands[12] = KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0);
		commands[13] = 0;
		commands[14] = static_cast<uint32_t>(address(&suffix));
		commands[15] = static_cast<uint32_t>(address(&suffix) >> 32u);
		commands[16] = 22;
		gpu.Submit(commands.data(), static_cast<uint32_t>(commands.size()), nullptr, 0);

		std::binary_semaphore ordered_started {0};
		std::atomic<bool>     ordered_finished {false};
		uint32_t              ordered_suffix = 0;
		std::jthread          ordered([&] {
			ordered_started.release();
			gpu.Done();
			ordered_suffix   = suffix;
			ordered_finished = true;
		});
		ordered_started.acquire();
		gpu.SendCommandSync([&] {
			Require("GpuCommandLane", "submit done barrier", !ordered_finished.load(),
			        "submit done returned before a queued submission");
			label = 1;
		});
		ordered.join();
		Require("GpuCommandLane", "ordered completion",
		        prefix == 11 && suffix == 22 && ordered_suffix == 22 && ordered_finished.load(),
		        "submit done did not drain prior PM4 work");

		auto&              resources         = context.GetGpuResources();
		constexpr uint64_t empty_unmap_base = 0x0000000200400000ull;
		constexpr uint64_t empty_unmap_size = 0x4000;
		resources.MapMemory(empty_unmap_base, empty_unmap_size);
		label  = 0;
		prefix = 0;
		suffix = 0;
		gpu.Submit(commands.data(), static_cast<uint32_t>(commands.size()), nullptr, 0);

		std::binary_semaphore unmap_complete {0};
		std::jthread unmap_thread([&] {
			resources.UnmapMemory(empty_unmap_base, empty_unmap_size);
			unmap_complete.release();
		});
		const bool unmap_returned = unmap_complete.try_acquire_for(std::chrono::seconds(2));
		gpu.SendCommandSync([&] { label = 1; });
		if (!unmap_returned) {
			unmap_complete.acquire();
		}
		unmap_thread.join();
		gpu.Done();
		Require("GpuCommandLane", "unmap queue progress",
		        unmap_returned && !resources.IsMapped(empty_unmap_base, empty_unmap_size) &&
		            prefix == 11 && suffix == 22,
		        "an unrelated unmap waited for a blocked PM4 submission");

		auto&                 scheduler = context.GetCommandScheduler();
		std::atomic<bool>      normal_completed {false};
		gpu.SendCommandSync(
		    [&] { scheduler.DeferOperation([&] { normal_completed = true; }); });
		resources.MapMemory(empty_unmap_base, empty_unmap_size);
		resources.UnmapMemory(empty_unmap_base, empty_unmap_size);
		Require("GpuCommandLane", "unmap native completion",
		        normal_completed.load() &&
		            !resources.IsMapped(empty_unmap_base, empty_unmap_size),
		        "unmap returned before an earlier native guest-memory callback");

		std::binary_semaphore priority_entered {0};
		std::binary_semaphore release_priority {0};
		gpu.SendCommandSync([&] {
			scheduler.DeferPriorityOperation([&] {
				priority_entered.release();
				release_priority.acquire();
			});
			scheduler.Flush();
		});
		priority_entered.acquire();
		resources.MapMemory(empty_unmap_base, empty_unmap_size);

		std::binary_semaphore priority_unmap_entered {0};
		std::binary_semaphore priority_unmap_complete {0};
		std::jthread priority_unmap_thread([&] {
			gpu.SendCommandSync([&] {
				priority_unmap_entered.release();
				resources.UnmapMemory(empty_unmap_base, empty_unmap_size);
			});
			priority_unmap_complete.release();
		});
		priority_unmap_entered.acquire();
		const bool unmap_overtook_priority =
		    priority_unmap_complete.try_acquire_for(std::chrono::seconds(1));
		release_priority.release();
		if (!unmap_overtook_priority) {
			priority_unmap_complete.acquire();
		}
		priority_unmap_thread.join();
		Require("GpuCommandLane", "unmap priority ordering",
		        !unmap_overtook_priority &&
		            !resources.IsMapped(empty_unmap_base, empty_unmap_size),
		        "unmap returned before an earlier guest-memory callback");

		constexpr uintptr_t fault_base          = 0x0000000200500000ull;
		constexpr uint64_t  fault_size          = 0x10000;
		int64_t             fault_direct_offset = -1;
		Require("GpuCommandLane", "processor fault direct allocation",
		        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
		            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), fault_size, fault_size,
		            0, &fault_direct_offset) == 0,
		        "processor-fault direct-memory allocation failed");
		void* fault_memory = reinterpret_cast<void*>(fault_base);
		Require("GpuCommandLane", "processor fault direct mapping",
		        Libs::LibKernel::Memory::KernelMapDirectMemory(
		            &fault_memory, fault_size, 0x3, 0x10, fault_direct_offset, fault_size) == 0,
		        "processor-fault fixed direct-memory mapping failed");
		Require("GpuCommandLane", "processor fault allocation",
		        fault_memory == reinterpret_cast<void*>(fault_base),
		        "fixed processor-fault allocation failed");
		resources.MapMemory(fault_base, fault_size);

		constexpr uint64_t                immediate_dst         = fault_base + 0x1000;
		constexpr uint64_t                immediate_memory_dst  = fault_base + 0x2000;
		constexpr uint64_t                memory_src            = fault_base + 0x4000;
		constexpr uint64_t                memory_copy_dst       = fault_base + 0x6000;
		constexpr uint64_t                memory_dst            = fault_base + 0x8000;
		constexpr uint64_t                l2_copy_dst           = fault_base + 0xa000;
		constexpr uint64_t                clean_cached_fill     = fault_base + 0x3000;
		constexpr uint64_t                clean_cached_copy     = fault_base + 0x3008;
		constexpr uint64_t                clean_cached_source   = fault_base + 0x3010;
		constexpr uint64_t                clean_cached_readback = fault_base + 0xb000;
		constexpr std::array<uint32_t, 2> source_words {0x11223344u, 0x55667788u};
		constexpr std::array<uint32_t, 2> clean_copy_words {0x13579bdfu, 0x2468ace0u};
		std::memcpy(reinterpret_cast<void*>(memory_src), source_words.data(), sizeof(source_words));
		std::memcpy(reinterpret_cast<void*>(clean_cached_source), clean_copy_words.data(),
		            sizeof(clean_copy_words));

		std::array<uint32_t, 49> dma_commands {};
		size_t                   dma_cursor = 0;
		const auto append_dma = [&](uint8_t dst_sel, uint64_t dst, uint8_t src_sel, uint64_t src,
		                            uint32_t bytes) {
			auto* packet = dma_commands.data() + dma_cursor;
			dma_cursor += 7;
			packet[0] = KYTY_PM4(7, Pm4::IT_DMA_DATA, 0);
			packet[1] =
			    (static_cast<uint32_t>(dst_sel) << 20u) | (static_cast<uint32_t>(src_sel) << 29u);
			packet[2] = static_cast<uint32_t>(src);
			packet[3] = static_cast<uint32_t>(src >> 32u);
			packet[4] = static_cast<uint32_t>(dst);
			packet[5] = static_cast<uint32_t>(dst >> 32u);
			packet[6] = bytes;
		};
		constexpr uint64_t gds_immediate_offset = 0;
		constexpr uint64_t gds_memory_offset    = 0x10;
		constexpr uint32_t immediate_value      = 0xa1b2c3d4u;
		append_dma(1, gds_immediate_offset, 2, immediate_value, sizeof(source_words));
		append_dma(0, immediate_dst, 1, gds_immediate_offset, sizeof(source_words));
		append_dma(1, gds_memory_offset, 0, memory_src, sizeof(source_words));
		append_dma(0, memory_dst, 1, gds_memory_offset, sizeof(source_words));
		append_dma(0, immediate_memory_dst, 2, immediate_value, sizeof(source_words));
		append_dma(0, memory_copy_dst, 0, memory_src, sizeof(source_words));
		append_dma(3, l2_copy_dst, 3, memory_src, sizeof(source_words));
		Require("GpuCommandLane", "DMA_DATA packet assembly", dma_cursor == dma_commands.size(),
		        "DMA_DATA GDS packet stream has the wrong size");
		gpu.Submit(dma_commands.data(), static_cast<uint32_t>(dma_commands.size()), nullptr, 0);
		gpu.Done();
		constexpr uint32_t clean_fill_value = 0xdecafbad;
		gpu.SendCommandSync([&] {
			auto& buffer_cache = resources.GetBufferCache();
			Require("GpuCommandLane", "clean cached setup",
			        buffer_cache.IsRegionRegistered(clean_cached_fill, sizeof(source_words)) &&
			            buffer_cache.HasGpuDirtyBytes(immediate_dst, sizeof(source_words)),
			        "DMA did not establish a cached page with a dirty sibling");

			buffer_cache.FillBuffer(clean_cached_fill, sizeof(source_words), clean_fill_value);
			buffer_cache.CopyBuffer(clean_cached_copy, clean_cached_source,
			                        sizeof(clean_copy_words));
			Require(
			    "GpuCommandLane", "clean cached DMA ownership",
			    !buffer_cache.HasGpuDirtyBytes(clean_cached_fill, sizeof(source_words)) &&
			        !buffer_cache.HasGpuDirtyBytes(clean_cached_copy, sizeof(clean_copy_words)) &&
			        buffer_cache.HasGpuDirtyBytes(immediate_dst, sizeof(source_words)),
			    "clean cached DMA acquired ownership or released a byte-disjoint "
			    "dirty sibling");

			constexpr uint64_t gds_clean_mirror = 0x20;
			buffer_cache.CopyBuffer(gds_clean_mirror, clean_cached_fill, 2 * sizeof(source_words),
			                        true, false);
			buffer_cache.CopyBuffer(clean_cached_readback, gds_clean_mirror,
			                        2 * sizeof(source_words), false, true);
		});
		Require("GpuCommandLane", "DMA_DATA immediate GDS readback",
		        resources.HandleFault(PageFaultAccess::Read, immediate_dst),
		        "immediate-to-GDS-to-memory copy did not publish GPU bytes");
		Require("GpuCommandLane", "DMA_DATA memory GDS readback",
		        resources.HandleFault(PageFaultAccess::Read, memory_dst),
		        "memory-to-GDS-to-memory copy did not publish GPU bytes");
		Require("GpuCommandLane", "DMA_DATA immediate memory readback",
		        resources.HandleFault(PageFaultAccess::Read, immediate_memory_dst),
		        "immediate-to-memory copy did not publish GPU bytes");
		Require("GpuCommandLane", "DMA_DATA memory copy readback",
		        resources.HandleFault(PageFaultAccess::Read, memory_copy_dst),
		        "memory-to-memory copy did not publish GPU bytes");
		Require("GpuCommandLane", "DMA_DATA L2 readback",
		        resources.HandleFault(PageFaultAccess::Read, l2_copy_dst),
		        "MemoryUsingL2 copy did not publish GPU bytes");
		Require("GpuCommandLane", "clean cached mirror readback",
		        resources.HandleFault(PageFaultAccess::Read, clean_cached_readback),
		        "clean host DMA was not reflected in the cached buffer");
		std::array<uint32_t, 2> immediate_words {};
		std::array<uint32_t, 2> copied_words {};
		std::array<uint32_t, 2> immediate_memory_words {};
		std::array<uint32_t, 2> memory_copy_words {};
		std::array<uint32_t, 2> l2_copy_words {};
		std::array<uint32_t, 4> clean_cached_words {};
		std::memcpy(immediate_words.data(), reinterpret_cast<const void*>(immediate_dst),
		            sizeof(immediate_words));
		std::memcpy(copied_words.data(), reinterpret_cast<const void*>(memory_dst),
		            sizeof(copied_words));
		std::memcpy(immediate_memory_words.data(),
		            reinterpret_cast<const void*>(immediate_memory_dst),
		            sizeof(immediate_memory_words));
		std::memcpy(memory_copy_words.data(), reinterpret_cast<const void*>(memory_copy_dst),
		            sizeof(memory_copy_words));
		std::memcpy(l2_copy_words.data(), reinterpret_cast<const void*>(l2_copy_dst),
		            sizeof(l2_copy_words));
		std::memcpy(clean_cached_words.data(), reinterpret_cast<const void*>(clean_cached_readback),
		            sizeof(clean_cached_words));
		Require("GpuCommandLane", "DMA_DATA immediate GDS contents",
		        immediate_words == std::array<uint32_t, 2> {immediate_value, immediate_value},
		        "immediate-to-GDS-to-memory bytes do not match");
		Require("GpuCommandLane", "DMA_DATA memory GDS contents", copied_words == source_words,
		        "memory-to-GDS-to-memory bytes do not match");
		Require("GpuCommandLane", "DMA_DATA immediate memory contents",
		        immediate_memory_words ==
		            std::array<uint32_t, 2> {immediate_value, immediate_value},
		        "immediate-to-memory bytes do not match");
		Require("GpuCommandLane", "DMA_DATA memory copy contents",
		        memory_copy_words == source_words, "memory-to-memory bytes do not match");
		Require("GpuCommandLane", "DMA_DATA L2 contents", l2_copy_words == source_words,
		        "MemoryUsingL2 bytes do not match");
		Require("GpuCommandLane", "clean cached DMA contents",
		        clean_cached_words == std::array<uint32_t, 4> {clean_fill_value, clean_fill_value,
		                                                       clean_copy_words[0],
		                                                       clean_copy_words[1]},
		        "host-memory fill/copy did not update the clean cached mirror");

		gpu.SendCommandSync([&] {
			Require("GpuCommandLane", "GPU memory invalidation",
			        resources.InvalidateMemory(fault_base, sizeof(uint32_t)),
			        "GPU memory invalidation did not find its mapped range");
		});

		auto& buffer_cache = resources.GetBufferCache();
		Require("GpuCommandLane", "clean tracked fault setup",
		        buffer_cache.IsRegionRegistered(immediate_dst, sizeof(uint32_t)) &&
		            !buffer_cache.HasGpuDirtyBytes(immediate_dst, sizeof(uint32_t)),
		        "clean write-fault test address is not backed by a clean cached Buffer");
		constexpr uint64_t dirty_fault_address = fault_base + 0xd000;
		constexpr uint32_t dirty_fault_value   = 0x5a17c0deu;
		constexpr uint32_t dirty_fault_stale   = 0x0ddba11u;
		std::memcpy(reinterpret_cast<void*>(dirty_fault_address), &dirty_fault_stale,
		            sizeof(dirty_fault_stale));
		gpu.SendCommandSync([&] {
			auto dirty = buffer_cache.ObtainBuffer(scheduler.Current(), dirty_fault_address,
			                                       sizeof(dirty_fault_value), true, false);
			Require("GpuCommandLane", "dirty tracked fault setup", dirty.owner != nullptr,
			        "dirty write-fault test could not create a cached Buffer");
			scheduler.Current().RetainResourceUntilFence(dirty.owner);
			buffer_cache.FillBuffer(dirty_fault_address, sizeof(dirty_fault_value),
			                        dirty_fault_value);
		});
		Require("GpuCommandLane", "dirty tracked fault ownership",
		        buffer_cache.HasGpuDirtyBytes(dirty_fault_address, sizeof(dirty_fault_value)),
		        "dirty write-fault test address did not acquire GPU ownership");

		std::binary_semaphore block_entered {0};
		std::binary_semaphore block_release {0};
		std::binary_semaphore clean_fault_complete {0};
		bool                  clean_fault_handled = false;
		gpu.SendCommand([&] {
			block_entered.release();
			block_release.acquire();
		});
		block_entered.acquire();
		std::jthread clean_fault_thread([&] {
			clean_fault_handled = resources.HandleFault(PageFaultAccess::Write, immediate_dst);
			clean_fault_complete.release();
		});
		const bool clean_fault_was_direct =
		    clean_fault_complete.try_acquire_for(std::chrono::seconds(1));
		if (!clean_fault_was_direct) {
			block_release.release();
			clean_fault_complete.acquire();
		}
		clean_fault_thread.join();
		Require("GpuCommandLane", "clean fault direct path",
		        clean_fault_was_direct && clean_fault_handled,
		        "a clean CPU write fault waited for the blocked GPU command lane");

		std::binary_semaphore dirty_fault_complete {0};
		bool                  dirty_fault_handled = false;
		std::jthread dirty_fault_thread([&] {
			dirty_fault_handled =
			    resources.HandleFault(PageFaultAccess::Write, dirty_fault_address);
			dirty_fault_complete.release();
		});
		const bool dirty_fault_bypassed_lane =
		    dirty_fault_complete.try_acquire_for(std::chrono::milliseconds(200));
		block_release.release();
		if (!dirty_fault_bypassed_lane) {
			dirty_fault_complete.acquire();
		}
		dirty_fault_thread.join();
		uint32_t dirty_fault_backing = 0;
		std::memcpy(&dirty_fault_backing, reinterpret_cast<const void*>(dirty_fault_address),
		            sizeof(dirty_fault_backing));
		Require("GpuCommandLane", "dirty fault synchronization",
		        !dirty_fault_bypassed_lane && dirty_fault_handled &&
		            dirty_fault_backing == dirty_fault_value &&
		            !buffer_cache.HasGpuDirtyBytes(dirty_fault_address, sizeof(dirty_fault_value)),
		        "a GPU-dirty write fault bypassed synchronization or lost the published bytes");

		constexpr uint64_t empty_copy_fault_address = fault_base + 0xe000;
		constexpr uint32_t empty_copy_fault_value   = 0x6b28d1efu;
		gpu.SendCommandSync([&] {
			auto dirty = buffer_cache.ObtainBuffer(scheduler.Current(), empty_copy_fault_address,
			                                       sizeof(empty_copy_fault_value), true, false);
			Require("GpuCommandLane", "empty-copy write setup", dirty.owner != nullptr,
			        "empty-copy write test could not create a cached Buffer");
			scheduler.Current().RetainResourceUntilFence(dirty.owner);
			buffer_cache.FillBuffer(empty_copy_fault_address, sizeof(empty_copy_fault_value),
			                        empty_copy_fault_value);
			buffer_cache.ReadMemory(empty_copy_fault_address, sizeof(empty_copy_fault_value));
			const bool gpu_owned = buffer_cache.IsRegionGpuModified(
			    empty_copy_fault_address, sizeof(empty_copy_fault_value));
			const bool cpu_owned = buffer_cache.IsRegionCpuModified(
			    empty_copy_fault_address, sizeof(empty_copy_fault_value));
			Require("GpuCommandLane", "empty-copy read ownership",
			        !gpu_owned && !cpu_owned,
			        "readback did not leave clean ownership for the queued write invalidation");
			buffer_cache.ReadMemory(empty_copy_fault_address, sizeof(empty_copy_fault_value), true);
		});
		uint32_t empty_copy_fault_backing = 0;
		std::memcpy(&empty_copy_fault_backing, reinterpret_cast<const void*>(empty_copy_fault_address),
		            sizeof(empty_copy_fault_backing));
		const bool empty_copy_cpu_owned = buffer_cache.IsRegionCpuModified(
		    empty_copy_fault_address, sizeof(empty_copy_fault_value));
		const bool empty_copy_gpu_owned = buffer_cache.IsRegionGpuModified(
		    empty_copy_fault_address, sizeof(empty_copy_fault_value));
		const bool empty_copy_dirty =
		    buffer_cache.HasGpuDirtyBytes(empty_copy_fault_address, sizeof(empty_copy_fault_value));
		Require("GpuCommandLane", "empty-copy write ownership",
		        empty_copy_fault_backing == empty_copy_fault_value &&
		            empty_copy_cpu_owned && !empty_copy_gpu_owned && !empty_copy_dirty,
		        "write invalidation without a remaining copy did not establish CPU ownership");

		resources.UnmapMemory(fault_base, fault_size);
		Require("GpuCommandLane", "processor fault unmap",
		        Libs::LibKernel::Memory::KernelMunmap(fault_base, fault_size) == 0,
		        "processor-fault direct-memory mapping release failed");
		Require("GpuCommandLane", "processor fault free",
		        Libs::LibKernel::Memory::KernelReleaseDirectMemory(fault_direct_offset,
		                                                           fault_size) == 0,
		        "processor-fault direct-memory allocation release failed");

		std::binary_semaphore command_entered {0};
		std::binary_semaphore release_command {0};
		std::atomic<bool>     shutdown_complete {false};
		std::atomic<bool>     submission_lane_entered {false};
		gpu.SendCommand([&] {
			command_entered.release();
			release_command.acquire();
			gpu.Done();
			submission_lane_entered = true;
		});
		command_entered.acquire();
		std::jthread shutdown_thread([&] {
			gpu.Shutdown();
			shutdown_complete = true;
		});
		while (!gpu.IsStopping()) {
			std::this_thread::yield();
		}
		Require("GpuCommandLane", "owned shutdown", !shutdown_complete.load(),
		        "GPU owner returned before an in-flight command completed");
		release_command.release();
		shutdown_thread.join();
		Require("GpuCommandLane", "owned shutdown completion",
		        shutdown_complete.load() && submission_lane_entered.load(),
		        "GPU owner did not drain a command that entered the submission lane");
		context.ShutdownGpu();
		std::printf("[host]    %-32s ok\n", "GpuCommandLane");
	}

	void CheckUnifiedImageViewCache() {
		EnsureRuntimeContext();
		constexpr const char* name = "UnifiedImageViewCache";
		CommandScheduler      scheduler(Renderer(), m_runtime_context);

		ImageInfo color_info {};
		color_info.pixel_format    = vk::Format::eR8G8B8A8Unorm;
		color_info.guest_format    = Prospero::BufferFormat::k8_8_8_8UNorm;
		color_info.type            = Prospero::ImageType::kColor2D;
		color_info.extent          = {8, 8, 1};
		color_info.resources       = {2, 2};
		color_info.pitch           = 8;
		color_info.bytes_per_block = 4;
		color_info.samples         = 1;
		color_info.tile_mode       = Prospero::TileMode::kLinear;
		color_info.mip_layout[0]   = {0, 512, 8, 8};
		color_info.mip_layout[1]   = {512, 128, 4, 4};

		Libs::Graphics::Image      color(m_runtime_context, scheduler, color_info);
		const vk::ComponentMapping identity {};
		const vk::ComponentMapping bgra {vk::ComponentSwizzle::eB, vk::ComponentSwizzle::eG,
		                                 vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eA};

		ImageViewInfo sampled {};
		sampled.format      = color_info.pixel_format;
		sampled.type        = vk::ImageViewType::e2D;
		sampled.aspect      = vk::ImageAspectFlagBits::eColor;
		sampled.base_level  = 0;
		sampled.level_count = 1;
		sampled.base_layer  = 0;
		sampled.layer_count = 1;
		sampled.mapping     = identity;
		sampled.usage       = vk::ImageUsageFlagBits::eSampled;

		const auto first              = color.FindView(sampled);
		const auto first_again        = color.FindView(sampled);
		auto       swizzled_info      = sampled;
		swizzled_info.mapping         = bgra;
		const auto swizzled           = color.FindView(swizzled_info);
		auto       mip_info           = sampled;
		mip_info.base_level           = 1;
		const auto mip                = color.FindView(mip_info);
		auto       layer_info         = sampled;
		layer_info.base_layer         = 1;
		const auto layer              = color.FindView(layer_info);
		auto       array_info         = sampled;
		array_info.type               = vk::ImageViewType::e2DArray;
		array_info.layer_count        = 2;
		const auto array              = color.FindView(array_info);
		auto       reinterpreted_info = sampled;
		reinterpreted_info.format     = vk::Format::eR8G8B8A8Uint;
		const auto reinterpreted      = color.FindView(reinterpreted_info);
		auto       storage_info       = sampled;
		storage_info.usage            = vk::ImageUsageFlagBits::eStorage;
		const auto storage            = color.FindView(storage_info);
		auto       attachment_info    = sampled;
		attachment_info.usage         = vk::ImageUsageFlagBits::eColorAttachment;
		const auto attachment         = color.FindView(attachment_info);

		Require(name, "color views",
		        first != nullptr && first_again == first && swizzled != nullptr &&
		            swizzled != first && mip != nullptr && mip != first && layer != nullptr &&
		            layer != first && array != nullptr && array != first &&
		            reinterpreted != nullptr && reinterpreted != first && storage != nullptr &&
		            storage != first && attachment == first && color.views.views.size() == 7,
		        "dynamic view identity omitted mapping, format, mip, layer, type, "
		        "or storage usage");

		ImageInfo depth_info {};
		depth_info.pixel_format    = vk::Format::eD32Sfloat;
		depth_info.guest_format    = Prospero::BufferFormat::k32Float;
		depth_info.type            = Prospero::ImageType::kColor2D;
		depth_info.extent          = {8, 8, 1};
		depth_info.resources       = {1, 2};
		depth_info.pitch           = 8;
		depth_info.bytes_per_block = 4;
		depth_info.samples         = 1;
		depth_info.tile_mode       = Prospero::TileMode::kDepth;
		depth_info.mip_layout[0]   = {0, 512, 8, 8};

		Libs::Graphics::Image depth(m_runtime_context, scheduler, depth_info);
		ImageViewInfo         depth_sampled {};
		depth_sampled.format             = depth_info.pixel_format;
		depth_sampled.type               = vk::ImageViewType::e2D;
		depth_sampled.aspect             = vk::ImageAspectFlagBits::eDepth;
		depth_sampled.base_level         = 0;
		depth_sampled.level_count        = 1;
		depth_sampled.base_layer         = 0;
		depth_sampled.layer_count        = 1;
		depth_sampled.mapping            = {vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eR,
		                                    vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eR};
		depth_sampled.usage              = vk::ImageUsageFlagBits::eSampled;
		const auto depth_first           = depth.FindView(depth_sampled);
		auto       depth_alias_info      = depth_sampled;
		depth_alias_info.format          = vk::Format::eR32Uint;
		const auto depth_alias           = depth.FindView(depth_alias_info);
		const auto depth_again           = depth.FindView(depth_sampled);
		auto       depth_array_info      = depth_sampled;
		depth_array_info.type            = vk::ImageViewType::e2DArray;
		depth_array_info.layer_count     = 2;
		const auto depth_array           = depth.FindView(depth_array_info);
		auto       depth_attachment_info = depth_array_info;
		depth_attachment_info.mapping    = {};
		depth_attachment_info.usage      = vk::ImageUsageFlagBits::eDepthStencilAttachment;
		const auto depth_attachment      = depth.FindView(depth_attachment_info);
		Require(name, "depth views",
		        depth_first != nullptr && depth_again == depth_first &&
		            depth_alias == depth_first && depth_array != nullptr &&
		            depth_array != depth_first && depth_attachment != nullptr &&
		            depth_attachment != depth_array && depth.views.views.size() == 3,
		        "unified depth view cache lost sampled/attachment identity");
		Require(
		    name, "role-free backing",
		    color.backing.image_type == vk::ImageType::e2D && color.backing.layers == 2 &&
		        depth.backing.image_type == vk::ImageType::e2D &&
		        ImageViewOps::FormatsCompatible(vk::Format::eR8G8B8A8Unorm,
		                                        vk::Format::eR8G8B8A8Uint) &&
		        !ImageViewOps::FormatsCompatible(vk::Format::eD32Sfloat, vk::Format::eR32Sfloat),
		    "role-free image backing or host format compatibility diverged");

		auto line_info          = color_info;
		line_info.type          = Prospero::ImageType::kColor1D;
		line_info.extent        = {8, 1, 1};
		line_info.resources     = {2, 2};
		line_info.mip_layout[0] = {0, 64, 8, 1};
		line_info.mip_layout[1] = {64, 16, 4, 1};
		Libs::Graphics::Image line(m_runtime_context, scheduler, line_info);
		auto                  line_view_info = sampled;
		line_view_info.type                  = vk::ImageViewType::e1D;
		const auto line_view                 = line.FindView(line_view_info);
		auto       line_array_info           = line_view_info;
		line_array_info.type                 = vk::ImageViewType::e1DArray;
		line_array_info.layer_count          = 2;
		const auto line_array                = line.FindView(line_array_info);
		Require(name, "1D views",
		        line_view != nullptr && line_array != nullptr && line_array != line_view &&
		            line.backing.image_type == vk::ImageType::e1D && line.backing.layers == 2,
		        "1D and 1D-array views did not use a first-class 1D backing");

		auto volume_info      = color_info;
		volume_info.type      = Prospero::ImageType::kColor3D;
		volume_info.extent    = {8, 8, 4};
		volume_info.resources = {2, 1};
		Libs::Graphics::Image volume(m_runtime_context, scheduler, volume_info);
		auto                  slice_info = sampled;
		slice_info.type                  = vk::ImageViewType::e2D;
		slice_info.base_level            = 1;
		slice_info.base_layer            = 1;
		const auto slice                 = volume.FindView(slice_info);
		auto       slice_array_info      = slice_info;
		slice_array_info.type            = vk::ImageViewType::e2DArray;
		slice_array_info.base_layer      = 0;
		slice_array_info.layer_count     = 2;
		const auto slice_array           = volume.FindView(slice_array_info);
		Require(name, "3D slice views",
		        slice != nullptr && slice_array != nullptr && slice_array != slice &&
		            static_cast<bool>(volume.backing.flags &
		                              vk::ImageCreateFlagBits::e2DArrayCompatible),
		        "2D slice views of a compatible 3D backing were rejected");
		std::printf("[host]    %-32s ok\n", name);
	}

	void CheckBufferCacheDirtyGarbageCollection() {
		constexpr const char* name                          = "BufferCacheDirtyGarbageCollection";
		constexpr uintptr_t   base                          = 0x0000000200700000ull;
		constexpr uint64_t    allocation_size               = 0x2400000;
		constexpr uint64_t    allocation_alignment          = 0x10000;
		constexpr uint64_t    first_offset                  = 0x100;
		constexpr uint64_t    second_offset                 = 0x200;
		constexpr uint64_t    clean_offset                  = 0x300;
		constexpr uint64_t    unmap_offset                  = 0x4100;
		constexpr uint64_t    partial_unmap_offset          = 0x8100;
		constexpr uint64_t    partial_unmap_survivor_offset = 0xc100;
		constexpr uint64_t    ring_fault_first_offset       = 0x2100;
		constexpr uint64_t    ring_fault_second_offset      = 0x2110;
		constexpr uint32_t    first_value                   = 0x10203040u;
		constexpr uint32_t    second_value                  = 0x50607080u;
		constexpr uint32_t    first_stale                   = 0x0badf00du;
		constexpr uint32_t    second_stale                  = 0xdeadbeefu;
		constexpr uint32_t    clean_value                   = 0xaabbccddu;
		constexpr uint32_t    unmap_value                   = 0x91a2b3c4u;
		constexpr uint32_t    partial_unmap_value           = 0x62738495u;
		constexpr uint32_t    partial_unmap_survivor_value  = 0xa6b7c8d9u;
		constexpr uint32_t    ring_fault_first_value        = 0x0a1b2c3du;
		constexpr uint32_t    ring_fault_second_value       = 0x4e5f6071u;

		EnsureRuntimeContext();
		auto&            context = Renderer();
		CommandScheduler scheduler(context, m_runtime_context);
		HW::Context      registers {};
		HW::UserConfig   user_config {};
		HW::Shader       shaders {};
		scheduler.Begin(registers, user_config, shaders);
		context.InitializeGpu(nullptr);
		auto& gpu = context.GetGpu();

		int64_t direct_offset = -1;
		Require(name, "direct allocation",
		        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
		            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), allocation_size,
		            allocation_alignment, 0, &direct_offset) == 0,
		        "dirty-GC direct-memory allocation failed");
		void* mapped = reinterpret_cast<void*>(base);
		Require(name, "direct mapping",
		        Libs::LibKernel::Memory::KernelMapDirectMemory(&mapped, allocation_size, 0x3, 0x10,
		                                                       direct_offset,
		                                                       allocation_alignment) == 0 &&
		            mapped == reinterpret_cast<void*>(base),
		        "dirty-GC fixed direct-memory mapping failed");
		auto* memory = static_cast<uint8_t*>(mapped);
		std::memcpy(memory + first_offset, &first_stale, sizeof(first_stale));
		std::memcpy(memory + second_offset, &second_stale, sizeof(second_stale));
		std::memcpy(memory + clean_offset, &clean_value, sizeof(clean_value));

		{
			GpuResourceManager resources(m_runtime_context, scheduler);
			resources.SetGpu(&gpu);
			auto& cache = resources.GetBufferCache();
			resources.MapMemory(base, allocation_size);

			const auto MarkGpuWrite = [&](uint64_t address, uint64_t size) {
				auto allocation =
				    cache.ObtainBuffer(scheduler.Current(), address, size, true, false);
				Require(name, "dirty allocation", allocation.owner != nullptr,
				        "dirty-GC buffer allocation failed");
				scheduler.Current().RetainResourceUntilFence(allocation.owner);
			};

			constexpr uint64_t index_offset = 0x180000;
			constexpr uint64_t index_page   = BufferCache::CACHING_PAGE_SIZE;
			constexpr uint64_t index_span   = 3 * index_page;
			const uint64_t     index_begin  = base + index_offset;
			Require(name, "registered-range empty lookup",
			        !cache.IsRegionRegistered(index_begin, index_span),
			        "empty Buffer cache reported a registered range");
			auto index_left = cache.ObtainBuffer(scheduler.Current(), index_begin + 0x100,
			                                     sizeof(uint32_t), false, false);
			auto index_right = cache.ObtainBuffer(scheduler.Current(),
			                                      index_begin + 2 * index_page + 0x100,
			                                      sizeof(uint32_t), false, false);
			Require(name, "registered-range owners",
			        index_left.owner != nullptr && index_right.owner != nullptr,
			        "failed to create disjoint Buffer index owners");
			scheduler.Current().RetainResourceUntilFence(index_left.owner);
			scheduler.Current().RetainResourceUntilFence(index_right.owner);
			Require(
			    name, "registered-range boundaries",
			    cache.IsRegionRegistered(index_begin, 1) &&
			        cache.IsRegionRegistered(index_begin + index_page - 1, 1) &&
			        !cache.IsRegionRegistered(index_begin - 1, 1) &&
			        !cache.IsRegionRegistered(index_begin + index_page, index_page) &&
			        cache.IsRegionRegistered(index_begin + index_page, index_page + 1) &&
			        !cache.IsRegionRegistered(index_begin + index_span, 1) &&
			        cache.IsRegionRegistered(index_begin - 1, index_span + 2),
			    "indexed lookup mishandled a half-open boundary, gap, or broad overlap");
			auto index_bridge = cache.ObtainBuffer(scheduler.Current(), index_begin + index_page - 1,
			                                       index_page + 2, false, false);
			Require(name, "registered-range merge",
			        index_bridge.owner != nullptr &&
			            cache.IsRegionRegistered(index_begin + index_page, index_page),
			        "bridging acquisition did not publish its merged Buffer range");
			scheduler.Current().RetainResourceUntilFence(index_bridge.owner);
			cache.UnmapMemory(index_begin, index_span);
			Require(name, "registered-range removal",
			        !cache.IsRegionRegistered(index_begin, index_span),
			        "unmapped Buffer owner remained in the registered-range index");

			MarkGpuWrite(base + first_offset, sizeof(first_value));
			MarkGpuWrite(base + second_offset, sizeof(second_value));
			cache.FillBuffer(base + first_offset, sizeof(first_value), first_value);
			cache.FillBuffer(base + second_offset, sizeof(second_value), second_value);
			auto&      download              = BufferCacheTestAccess::DownloadBuffer(cache);
			auto*      fixed_download        = &download;
			const auto fixed_download_handle = download.Handle();
			auto [ring_tail, ring_tail_offset] =
			    download.Map(download.Size() - sizeof(uint32_t), 4);
			Require(name, "fault-ring tail reservation",
			        ring_tail != nullptr && ring_tail_offset == 0,
			        "failed to position the reusable download ring near wrap");
			download.Commit();
			MarkGpuWrite(base + ring_fault_first_offset, sizeof(ring_fault_first_value));
			MarkGpuWrite(base + ring_fault_second_offset, sizeof(ring_fault_second_value));
			cache.FillBuffer(base + ring_fault_first_offset, sizeof(ring_fault_first_value),
			                 ring_fault_first_value);
			cache.FillBuffer(base + ring_fault_second_offset, sizeof(ring_fault_second_value),
			                 ring_fault_second_value);
			Require(name, "fault-ring wrapped batch",
			        resources.HandleFault(PageFaultAccess::Read, base + ring_fault_first_offset),
			        "fault readback could not wrap a live download-ring tick");
			uint64_t expected_packing_offset    = 128;
			uint64_t expected_packing_alignment = 1;
			if (!download.IsCoherent()) {
				Require(name, "fault-ring atom policy",
				        StreamBufferTestAccess::NormalizeReservation(
				            false,
				            m_runtime_context.physical_device_properties.limits.nonCoherentAtomSize,
				            expected_packing_offset, expected_packing_alignment),
				        "failed to calculate non-coherent fault-ring stride");
			}
			auto [packing_probe, packing_offset] = download.Map(1, 1, false);
			Require(name, "adjacent download reservation stride",
			        packing_probe != nullptr && packing_offset == expected_packing_offset,
			        "adjacent fault downloads did not reserve an atom-safe stride");
			download.Commit();
			uint32_t ring_fault_first_backing  = 0;
			uint32_t ring_fault_second_backing = 0;
			std::memcpy(&ring_fault_first_backing, memory + ring_fault_first_offset,
			            sizeof(ring_fault_first_backing));
			std::memcpy(&ring_fault_second_backing, memory + ring_fault_second_offset,
			            sizeof(ring_fault_second_backing));
			Require(name, "fault-ring wrapped contents",
			        ring_fault_first_backing == ring_fault_first_value &&
			            ring_fault_second_backing == ring_fault_second_value &&
			            &BufferCacheTestAccess::DownloadBuffer(cache) == fixed_download &&
			            download.Handle() == fixed_download_handle &&
			            download.Size() == (32ull << 20),
			        "wrapped fault batch published incorrect disjoint ranges");

			for (uint32_t tick = 0; tick < 160; tick++) {
				cache.RunGarbageCollector();
			}
			Require(name, "age before pressure", cache.IsRegionRegistered(base, allocation_size),
			        "buffer was reclaimed without memory pressure");
			BufferCacheTestAccess::SetGarbageCollectionThresholds(
			    cache, 0, std::numeric_limits<uint64_t>::max());
			const auto gc_submission_tick = scheduler.CurrentTick();
			cache.RunGarbageCollector();
			Require(name, "dirty retirement", !cache.IsRegionRegistered(base, allocation_size),
			        "aged GPU-dirty buffer survived pressured "
			        "collection");

			uint32_t first_before_completion  = 0;
			uint32_t second_before_completion = 0;
			Libs::LibKernel::Memory::TryReadBacking(base + first_offset, &first_before_completion,
			                                        sizeof(first_before_completion));
			Libs::LibKernel::Memory::TryReadBacking(base + second_offset, &second_before_completion,
			                                        sizeof(second_before_completion));
			Require(name, "deferred dirty retirement",
			        first_before_completion == first_stale &&
			            second_before_completion == second_stale &&
			            cache.IsRegionGpuModified(base + first_offset, sizeof(first_value)) &&
			            cache.IsRegionGpuModified(base + second_offset, sizeof(second_value)) &&
			            cache.HasGpuDirtyBytes(base + first_offset, sizeof(first_value)) &&
			            cache.HasGpuDirtyBytes(base + second_offset, sizeof(second_value)) &&
			            scheduler.CurrentTick() == gc_submission_tick,
			        "buffer GC published bytes or cleared dirty "
			        "ownership before GPU completion");
			scheduler.FinishCurrent();

			uint32_t first_backing  = 0;
			uint32_t second_backing = 0;
			uint32_t clean_backing  = 0;
			std::memcpy(&first_backing, memory + first_offset, sizeof(first_backing));
			std::memcpy(&second_backing, memory + second_offset, sizeof(second_backing));
			std::memcpy(&clean_backing, memory + clean_offset, sizeof(clean_backing));
			Require(name, "downloaded contents",
			        first_backing == first_value && second_backing == second_value &&
			            clean_backing == clean_value &&
			            !cache.IsRegionGpuModified(base + first_offset, sizeof(first_value)) &&
			            !cache.IsRegionGpuModified(base + second_offset, sizeof(second_value)) &&
			            !cache.HasGpuDirtyBytes(base + first_offset, sizeof(first_value)) &&
			            !cache.HasGpuDirtyBytes(base + second_offset, sizeof(second_value)),
			        "dirty GC did not publish exact ranges before "
			        "clearing broad page ownership");

			auto unmap_allocation = cache.ObtainBuffer(scheduler.Current(), base + unmap_offset,
			                                           sizeof(unmap_value), true, false);
			Require(name, "direct-unmap allocation", unmap_allocation.owner != nullptr,
			        "direct-unmap buffer allocation failed");
			scheduler.Current().RetainResourceUntilFence(unmap_allocation.owner);
			cache.FillBuffer(base + unmap_offset, sizeof(unmap_value), unmap_value);
			cache.UnmapMemory(base + 0x4000, 0x4000);
			uint32_t unmap_backing = 0;
			std::memcpy(&unmap_backing, memory + unmap_offset, sizeof(unmap_backing));
			Require(name, "direct-unmap contents",
			        unmap_backing == unmap_value &&
			            !cache.IsRegionRegistered(base + 0x4000, 0x4000),
			        "direct dirty unmap cleared tracking before "
			        "publishing bytes");

			auto partial_unmap_allocation =
			    cache.ObtainBuffer(scheduler.Current(), base + 0x8000, 0x8000, true, false);
			Require(name, "partial-unmap allocation", partial_unmap_allocation.owner != nullptr,
			        "cross-page cached buffer allocation failed");
			scheduler.Current().RetainResourceUntilFence(partial_unmap_allocation.owner);
			cache.FillBuffer(base + partial_unmap_offset, sizeof(partial_unmap_value),
			                 partial_unmap_value);
			cache.FillBuffer(base + partial_unmap_survivor_offset,
			                 sizeof(partial_unmap_survivor_value), partial_unmap_survivor_value);
			cache.UnmapMemory(base + 0x8000, 0x4000);
			auto survivor =
			    cache.ObtainBuffer(scheduler.Current(), base + partial_unmap_survivor_offset,
			                       sizeof(partial_unmap_survivor_value), false, true);
			Require(name, "partial-unmap survivor", survivor.buffer != nullptr,
			        "still-mapped cached-buffer remainder could not be recreated");
			if (survivor.owner != nullptr) {
				scheduler.Current().RetainResourceUntilFence(survivor.owner);
			}
			auto partial_unmap_readback =
			    CreateHostBuffer(name, sizeof(partial_unmap_survivor_value),
			                     vk::BufferUsageFlagBits::eTransferDst, {0});
			const vk::BufferCopy survivor_copy {survivor.offset, 0,
			                                    sizeof(partial_unmap_survivor_value)};
			scheduler.Current().Handle().copyBuffer(survivor.buffer, partial_unmap_readback.buffer,
			                                        1, &survivor_copy);
			vk::BufferMemoryBarrier survivor_barrier {};
			survivor_barrier.sType               = vk::StructureType::eBufferMemoryBarrier;
			survivor_barrier.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
			survivor_barrier.dstAccessMask       = vk::AccessFlagBits::eHostRead;
			survivor_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			survivor_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			survivor_barrier.buffer              = partial_unmap_readback.buffer;
			survivor_barrier.size                = partial_unmap_readback.size;
			scheduler.Current().Handle().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
			                                             vk::PipelineStageFlagBits::eHost, {}, 0,
			                                             nullptr, 1, &survivor_barrier, 0, nullptr);
			scheduler.Finish();
			Require(name, "partial-unmap survivor contents",
			        ReadBuffer(name, partial_unmap_readback, 1) ==
			            std::vector<u32> {partial_unmap_survivor_value},
			        "partial unmap recreated its still-mapped remainder from "
			        "uninitialized native bytes");
			DestroyBuffer(&partial_unmap_readback);

			constexpr uint64_t large_offset = 0x10000;
			constexpr uint64_t large_size   = 31ull * 1024 * 1024;
			constexpr uint32_t large_value  = 0x5aa55aa5u;
			constexpr uint32_t large_stale  = 0x12345678u;
			std::memcpy(memory + large_offset, &large_stale, sizeof(large_stale));
			std::memcpy(memory + large_offset + large_size - sizeof(large_stale), &large_stale,
			            sizeof(large_stale));
			auto large_allocation = cache.ObtainBuffer(scheduler.Current(), base + large_offset,
			                                           large_size, true, false);
			Require(name, "near-capacity dirty allocation", large_allocation.owner != nullptr,
			        "failed to allocate the near-capacity dirty native buffer");
			scheduler.Current().RetainResourceUntilFence(large_allocation.owner);
			cache.FillBuffer(base + large_offset, large_size, large_value);
			for (uint32_t tick = 0; tick <= 160; tick++) {
				cache.RunGarbageCollector();
			}
			Require(name, "near-capacity deferred retirement",
			        !cache.IsRegionRegistered(base + large_offset, large_size),
			        "near-capacity dirty Buffer survived pressured collection");
			uint32_t large_before_completion = 0;
			Libs::LibKernel::Memory::TryReadBacking(base + large_offset, &large_before_completion,
			                                        sizeof(large_before_completion));
			Require(name, "near-capacity backing remained deferred",
			        large_before_completion == large_stale,
			        "near-capacity Buffer GC published before its scheduler tick");
			scheduler.FinishCurrent();
			const auto large_image_source =
			    cache.ObtainBufferForImage(base + large_offset, sizeof(large_value));
			Require(name, "fixed download after Buffer retirement",
			        large_image_source.buffer != nullptr &&
			            &BufferCacheTestAccess::DownloadBuffer(cache) == fixed_download &&
			            fixed_download->Handle() == fixed_download_handle &&
			            fixed_download->Size() == (32ull << 20),
			        "image acquisition replaced the shared Buffer download stream");
			uint32_t large_first = 0;
			uint32_t large_last  = 0;
			Libs::LibKernel::Memory::TryReadBacking(base + large_offset, &large_first,
			                                        sizeof(large_first));
			Libs::LibKernel::Memory::TryReadBacking(base + large_offset + large_size -
			                                            sizeof(large_last),
			                                        &large_last, sizeof(large_last));
			Require(name, "near-capacity Buffer publication contents",
			        large_first == large_value && large_last == large_value,
			        "near-capacity Buffer GC did not publish its complete transfer");

			constexpr uint64_t grouped_first_offset  = 0x10000;
			constexpr uint64_t grouped_second_offset = 0x1200000;
			constexpr uint64_t grouped_owner_size    = 17ull * 1024 * 1024;
			constexpr uint32_t grouped_first_value   = 0x1122aabbu;
			constexpr uint32_t grouped_second_value  = 0x3344ccddu;
			constexpr uint32_t grouped_stale         = 0;
			static_assert(grouped_owner_size < (32ull << 20) &&
			              grouped_owner_size * 2 > (32ull << 20));
			Libs::LibKernel::Memory::WriteBacking(base + grouped_first_offset, &grouped_stale,
			                                      sizeof(grouped_stale));
			Libs::LibKernel::Memory::WriteBacking(base + grouped_second_offset, &grouped_stale,
			                                      sizeof(grouped_stale));
			auto grouped_first = cache.ObtainBuffer(
			    scheduler.Current(), base + grouped_first_offset, grouped_owner_size, true, false);
			auto grouped_second = cache.ObtainBuffer(
			    scheduler.Current(), base + grouped_second_offset, grouped_owner_size, true, false);
			Require(name, "per-owner GC allocation",
			        grouped_first.owner != nullptr && grouped_second.owner != nullptr &&
			            grouped_first.owner != grouped_second.owner,
			        "disjoint GC candidates merged into one Buffer owner");
			scheduler.Current().RetainResourceUntilFence(grouped_first.owner);
			scheduler.Current().RetainResourceUntilFence(grouped_second.owner);
			cache.FillBuffer(base + grouped_first_offset, grouped_owner_size, grouped_first_value);
			cache.FillBuffer(base + grouped_second_offset, grouped_owner_size,
			                 grouped_second_value);
			for (uint32_t tick = 0; tick <= 160; tick++) {
				cache.RunGarbageCollector();
			}
			Require(name, "per-owner fixed-ring retirement",
			        !cache.IsRegionRegistered(base + grouped_first_offset, grouped_owner_size) &&
			            !cache.IsRegionRegistered(base + grouped_second_offset, grouped_owner_size) &&
			            &BufferCacheTestAccess::DownloadBuffer(cache) == fixed_download &&
			            fixed_download->Handle() == fixed_download_handle,
			        "GC aggregated disjoint retirees or replaced the download ring");
			scheduler.FinishCurrent();
			uint32_t grouped_first_backing  = 0;
			uint32_t grouped_second_backing = 0;
			Libs::LibKernel::Memory::TryReadBacking(
			    base + grouped_first_offset, &grouped_first_backing, sizeof(grouped_first_backing));
			Libs::LibKernel::Memory::TryReadBacking(base + grouped_second_offset,
			                                        &grouped_second_backing,
			                                        sizeof(grouped_second_backing));
			Require(name, "per-owner fixed-ring contents",
			        grouped_first_backing == grouped_first_value &&
			            grouped_second_backing == grouped_second_value,
			        "per-owner GC transfers lost data while wrapping the fixed ring");

			constexpr uint64_t disjoint_owner_offset     = 0x2140000;
			constexpr uint64_t disjoint_owner_size       = 0x8000;
			constexpr uint64_t disjoint_dirty_offset     = disjoint_owner_offset + 0x100;
			constexpr uint64_t disjoint_new_owner_offset = disjoint_owner_offset + 0x5000;
			constexpr uint32_t disjoint_value            = 0x89abcdefu;
			constexpr uint32_t disjoint_stale            = 0x76543210u;
			std::memcpy(memory + disjoint_dirty_offset, &disjoint_stale, sizeof(disjoint_stale));
			auto wide_owner = cache.ObtainBuffer(scheduler.Current(), base + disjoint_owner_offset,
			                                     disjoint_owner_size, false, true);
			Require(name, "disjoint retirement owner", wide_owner.owner != nullptr,
			        "failed to create the clean cross-page retirement owner");
			scheduler.Current().RetainResourceUntilFence(wide_owner.owner);
			auto dirty_alias = cache.ObtainBuffer(scheduler.Current(), base + disjoint_dirty_offset,
			                                      sizeof(disjoint_value), true, false);
			if (dirty_alias.owner != nullptr) {
				scheduler.Current().RetainResourceUntilFence(dirty_alias.owner);
			}
			cache.FillBuffer(base + disjoint_dirty_offset, sizeof(disjoint_value), disjoint_value);
			for (uint32_t tick = 0; tick <= 160; tick++) {
				cache.RunGarbageCollector();
			}
			Require(name, "disjoint deferred retirement",
			        !cache.IsRegionRegistered(base + disjoint_owner_offset, disjoint_owner_size),
			        "cross-page owner survived pressured collection");
			uint32_t disjoint_before_unmap = 0;
			Libs::LibKernel::Memory::TryReadBacking(base + disjoint_dirty_offset,
			                                        &disjoint_before_unmap,
			                                        sizeof(disjoint_before_unmap));
			Require(name, "disjoint retirement remained deferred",
			        disjoint_before_unmap == disjoint_stale,
			        "whole-owner Buffer publication completed before synchronization");
			scheduler.FinishCurrent();
			cache.UnmapMemory(base + disjoint_owner_offset + 0x4000, 0x4000);
			uint32_t disjoint_after_unmap = 0;
			Libs::LibKernel::Memory::TryReadBacking(
			    base + disjoint_dirty_offset, &disjoint_after_unmap, sizeof(disjoint_after_unmap));
			Require(
			    name, "disjoint synchronized unmap",
			    disjoint_after_unmap == disjoint_value &&
			        !cache.HasGpuDirtyBytes(base + disjoint_dirty_offset, sizeof(disjoint_value)),
			    "disjoint unmap lost the completed whole-owner publication");
			auto disjoint_new_owner =
			    cache.ObtainBuffer(scheduler.Current(), base + disjoint_new_owner_offset,
			                       sizeof(disjoint_value), true, false);
			Require(name, "disjoint post-publication reacquire",
			        disjoint_new_owner.owner != nullptr,
			        "disjoint acquisition failed after whole-owner publication");
			scheduler.Current().RetainResourceUntilFence(disjoint_new_owner.owner);
			uint32_t disjoint_backing = 0;
			Libs::LibKernel::Memory::TryReadBacking(base + disjoint_dirty_offset, &disjoint_backing,
			                                        sizeof(disjoint_backing));
			Require(name, "disjoint retirement contents", disjoint_backing == disjoint_value,
			        "old retirement callback lost dirty bytes or retained ownership "
			        "after a disjoint reacquire");
			cache.ReadMemory(base + disjoint_new_owner_offset, sizeof(disjoint_value));

			constexpr uint64_t reacquire_owner_offset    = 0x2200000;
			constexpr uint64_t reacquire_owner_size      = 0x8000;
			constexpr uint64_t reacquire_dirty_offset    = reacquire_owner_offset + 0x100;
			constexpr uint64_t reacquire_disjoint_offset = reacquire_owner_offset + 0x5000;
			constexpr uint32_t reacquire_value           = 0xc001d00du;
			constexpr uint32_t reacquire_stale           = 0x0badf00du;
			std::memcpy(memory + reacquire_dirty_offset, &reacquire_stale, sizeof(reacquire_stale));
			auto reacquire_owner =
			    cache.ObtainBuffer(scheduler.Current(), base + reacquire_owner_offset,
			                       reacquire_owner_size, false, true);
			Require(name, "reacquire retirement owner", reacquire_owner.owner != nullptr,
			        "failed to create a fresh cross-page retirement owner");
			scheduler.Current().RetainResourceUntilFence(reacquire_owner.owner);
			auto reacquire_dirty =
			    cache.ObtainBuffer(scheduler.Current(), base + reacquire_dirty_offset,
			                       sizeof(reacquire_value), true, false);
			if (reacquire_dirty.owner != nullptr) {
				scheduler.Current().RetainResourceUntilFence(reacquire_dirty.owner);
			}
			cache.FillBuffer(base + reacquire_dirty_offset, sizeof(reacquire_value),
			                 reacquire_value);
			for (uint32_t tick = 0; tick <= 160; tick++) {
				cache.RunGarbageCollector();
			}
			Require(name, "reacquire deferred retirement",
			        !cache.IsRegionRegistered(base + reacquire_owner_offset, reacquire_owner_size),
			        "fresh cross-page owner survived pressured collection");
			uint32_t reacquire_before = 0;
			Libs::LibKernel::Memory::TryReadBacking(base + reacquire_dirty_offset,
			                                        &reacquire_before, sizeof(reacquire_before));
			Require(name, "reacquire publication remained deferred",
			        reacquire_before == reacquire_stale,
			        "fresh whole-owner publication completed before reacquisition");
			scheduler.FinishCurrent();
			auto reacquired =
			    cache.ObtainBuffer(scheduler.Current(), base + reacquire_disjoint_offset,
			                       sizeof(reacquire_value), true, false);
			Require(name, "independent disjoint post-publication reacquire",
			        reacquired.owner != nullptr,
			        "clean disjoint-half acquisition failed after publication");
			scheduler.Current().RetainResourceUntilFence(reacquired.owner);
			uint32_t reacquire_after = 0;
			Libs::LibKernel::Memory::TryReadBacking(base + reacquire_dirty_offset, &reacquire_after,
			                                        sizeof(reacquire_after));
			Require(
			    name, "independent disjoint reacquire contents",
			    reacquire_after == reacquire_value &&
			        !cache.HasGpuDirtyBytes(base + reacquire_dirty_offset, sizeof(reacquire_value)),
			    "disjoint-half acquisition failed to publish the retired "
			    "owner's dirty prefix");
			cache.ReadMemory(base + reacquire_disjoint_offset, sizeof(reacquire_value));

			resources.SetGpu(nullptr);
			resources.UnmapMemory(base, allocation_size);
			scheduler.Finish();
		}
		context.ShutdownGpu();
		Require(name, "unmap direct backing",
		        Libs::LibKernel::Memory::KernelMunmap(base, allocation_size) == 0,
		        "dirty-GC direct-memory mapping release failed");
		Require(
		    name, "release direct backing",
		    Libs::LibKernel::Memory::KernelReleaseDirectMemory(direct_offset, allocation_size) == 0,
		    "dirty-GC direct-memory allocation release failed");
		std::printf("[host]    %-32s ok\n", name);
	}

	void CheckUnifiedTextureCacheFlow() {
		constexpr const char* name                 = "UnifiedTextureCacheFlow";
		constexpr uintptr_t   base                 = 0x0000000200600000ull;
		constexpr uint64_t    allocation_size      = 0x2800000;
		constexpr uint64_t    allocation_alignment = 0x200000;
		EnsureRuntimeContext();
		auto&            context = Renderer();
		CommandScheduler scheduler(context, m_runtime_context);
		HW::Context      registers {};
		HW::UserConfig   user_config {};
		HW::Shader       shaders {};
		scheduler.Begin(registers, user_config, shaders);
		context.InitializeGpu(nullptr);
		auto&   gpu = context.GetGpu();
		int64_t direct_offset = -1;
		Require(name, "direct allocation",
		        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
		            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), allocation_size,
		            allocation_alignment, 0, &direct_offset) == 0,
		        "cache test direct-memory allocation failed");
		void* mapped = reinterpret_cast<void*>(base);
		Require(name, "direct mapping",
		        Libs::LibKernel::Memory::KernelMapDirectMemory(&mapped, allocation_size, 0x3, 0x10,
		                                                       direct_offset,
		                                                       allocation_alignment) == 0 &&
		            mapped == reinterpret_cast<void*>(base),
		        "fixed cache direct-memory mapping failed");
		auto*          memory  = static_cast<uint8_t*>(mapped);
		const uint32_t initial = 0x44332211u;
		std::memcpy(memory, &initial, sizeof(initial));

		{
			GpuResourceManager resources(m_runtime_context, scheduler);
			resources.SetGpu(&gpu);
			auto& texture_cache = resources.GetTextureCache();
			const auto [narrow_download, narrow_download_offset] =
			    TextureCacheTestAccess::MapDownload(texture_cache, 4, 4);
			const auto [wide_download, wide_download_offset] =
			    TextureCacheTestAccess::MapDownload(texture_cache, 1, 16);
			Require(name, "image download alignment",
			        narrow_download != nullptr && narrow_download_offset % 4 == 0 &&
			            wide_download != nullptr && wide_download_offset % 16 == 0,
			        "wide/block image readback was not aligned to its texel block");
			resources.MapMemory(base, allocation_size);

			ImageDesc sampled {};
			sampled.type              = BindingType::Texture;
			sampled.info.data         = {base, sizeof(initial)};
			sampled.info.pixel_format = vk::Format::eR8G8B8A8Srgb;
			sampled.info.guest_format =
			    Prospero::BufferFormat::k8_8_8_8Srgb;
			sampled.info.type            = Prospero::ImageType::kColor2D;
			sampled.info.extent          = {1, 1, 1};
			sampled.info.resources       = {1, 1};
			sampled.info.pitch           = 1;
			sampled.info.bytes_per_block = 4;
			sampled.info.samples         = 1;
			sampled.info.tile_mode       = Prospero::TileMode::kLinear;
			sampled.info.mip_layout[0]   = {0, 4, 1, 1};
			sampled.view_info.format     = sampled.info.pixel_format;
			sampled.view_info.type       = vk::ImageViewType::e2D;
			sampled.view_info.aspect     = vk::ImageAspectFlagBits::eColor;
			sampled.view_info.usage      = vk::ImageUsageFlagBits::eSampled;

			auto&      command                = scheduler.Current();
			auto       first_desc             = sampled;
			const auto first                  = texture_cache.FindImage(first_desc);
			auto       repeated_desc          = sampled;
			const auto repeated               = texture_cache.FindImage(repeated_desc);
			auto       compatible_desc        = sampled;
			compatible_desc.info.pixel_format = vk::Format::eR8G8B8A8Uint;
			compatible_desc.info.guest_format =
			    Prospero::BufferFormat::k8_8_8_8UInt;
			compatible_desc.view_info.format = compatible_desc.info.pixel_format;
			const auto compatible            = texture_cache.FindImage(compatible_desc);
			Require(name, "normalized FindImage",
			        first && repeated == first && compatible == first &&
			            texture_cache.GetImage(first).info.pixel_format ==
			                vk::Format::eR8G8B8A8Srgb,
			        "registered compatible backing did not reuse one ImageId");

			const auto IsOnlyImage = [](const std::vector<ImageId>& ids, ImageId expected) {
				return ids.size() == 1 && ids.front() == expected;
			};
			const auto exact_byte_miss =
			    TextureCacheTestAccess::FindImages(texture_cache, base + 8, 1, false);
			const auto touched_page_hit =
			    TextureCacheTestAccess::FindImages(texture_cache, base + 8, 1, true);
			const auto next_page_miss =
			    TextureCacheTestAccess::FindImages(texture_cache, base + 0x1000, 1, true);
			Require(name, "exact and 4-KiB page filtering",
			        exact_byte_miss.empty() && IsOnlyImage(touched_page_hit, first) &&
			            next_page_miss.empty(),
			        "coarse candidates did not preserve exact-byte and touched-page semantics");

			const auto MakeOwnershipInfo = [](uint64_t address, uint64_t size) {
				ImageInfo info {};
				info.data      = {address, size};
				info.extent    = {1, 1, 1};
				info.resources = {1, 1};
				info.samples   = 1;
				return info;
			};
			const auto spanning_info = MakeOwnershipInfo(base + 0x1ff000, 0x2000);
			const auto spanning = TextureCacheTestAccess::InsertImage(texture_cache, spanning_info);
			const auto spanning_results = TextureCacheTestAccess::FindImages(
			    texture_cache, spanning_info.data.address, spanning_info.data.size, false);
			Require(name, "one-MiB cross-page deduplication",
			        spanning && IsOnlyImage(spanning_results, spanning) &&
			            TextureCacheTestAccess::PageOwnerCount(texture_cache,
			                                                       spanning_info.data.address) == 1 &&
			            TextureCacheTestAccess::PageOwnerCount(
			                texture_cache, spanning_info.data.End() - 1) == 1,
			        "an image spanning two coarse pages was missing or returned more than once");
			TextureCacheTestAccess::DeleteImage(texture_cache, spanning);
			Require(name, "cross-page owner cleanup",
			        TextureCacheTestAccess::PageOwnerCount(texture_cache,
			                                               spanning_info.data.address) == 0 &&
			            TextureCacheTestAccess::PageOwnerCount(
			                texture_cache, spanning_info.data.End() - 1) == 0 &&
			            TextureCacheTestAccess::FindImages(texture_cache,
			                                               spanning_info.data.address,
			                                               spanning_info.data.size, false)
			                .empty(),
			        "cross-page unregister left a stale coarse-page membership");

			const auto shared_info = MakeOwnershipInfo(base + 0x500100, 0x100);
			const auto shared_first =
			    TextureCacheTestAccess::InsertImage(texture_cache, shared_info);
			const auto shared_second =
			    TextureCacheTestAccess::InsertImage(texture_cache, shared_info);
			const auto shared_results = TextureCacheTestAccess::FindImages(
			    texture_cache, shared_info.data.address, shared_info.data.size, false);
			Require(name, "shared coarse-page registration",
			        shared_results.size() == 2 &&
			            std::find(shared_results.begin(), shared_results.end(), shared_first) !=
			                shared_results.end() &&
			            std::find(shared_results.begin(), shared_results.end(), shared_second) !=
			                shared_results.end(),
			        "two registered owners were not retained in one coarse page");
			TextureCacheTestAccess::DeleteImage(texture_cache, shared_first);
			const auto shared_survivor = TextureCacheTestAccess::FindImages(
			    texture_cache, shared_info.data.address, shared_info.data.size, false);
			Require(name, "shared coarse-page unregister",
			        IsOnlyImage(shared_survivor, shared_second) &&
			            TextureCacheTestAccess::PageOwnerCount(texture_cache,
			                                                       shared_info.data.address) == 1,
			        "unregistering one owner removed its coarse-page neighbor");
			TextureCacheTestAccess::DeleteImage(texture_cache, shared_second);
			Require(name, "final shared coarse-page unregister",
			        TextureCacheTestAccess::PageOwnerCount(texture_cache,
			                                               shared_info.data.address) == 0,
			        "the final shared owner remained registered");

			constexpr uint64_t large_owner_size = 64ull * 1024 * 1024;
			const auto large_info = MakeOwnershipInfo(base + 0x800000, large_owner_size);
			const auto large_owner =
			    TextureCacheTestAccess::InsertImage(texture_cache, large_info);
			Require(name, "production one-MiB registration granularity",
			        TextureCacheTestAccess::OwnedPageCount(
			            texture_cache, large_info.data.address, large_info.data.size, large_owner) == 64,
			        "64 MiB image registration did not create exactly 64 coarse memberships");
			TextureCacheTestAccess::DeleteImage(texture_cache, large_owner);
			Require(name, "production one-MiB unregister granularity",
			        TextureCacheTestAccess::OwnedPageCount(
			            texture_cache, large_info.data.address, large_info.data.size, large_owner) == 0,
			        "large image unregister left coarse memberships behind");

			const ImageId stale {first.index, first.generation + 1};
			TextureCacheTestAccess::AddPageOwner(texture_cache, base, stale);
			const auto stale_filtered =
			    TextureCacheTestAccess::FindImages(texture_cache, base, sizeof(initial), false);
			Require(name, "stale page owner filtering",
			        IsOnlyImage(stale_filtered, first) &&
			            TextureCacheTestAccess::RemovePageOwner(texture_cache, base, stale),
			        "a stale generation escaped the direct page-owner lookup");

			TextureCacheTestAccess::SetQueryEpoch(texture_cache, UINT32_MAX);
			const auto wrap_results =
			    TextureCacheTestAccess::FindImages(texture_cache, base, sizeof(initial), false);
			Require(name, "page-owner query epoch wrap",
			        IsOnlyImage(wrap_results, first) &&
			            TextureCacheTestAccess::QueryEpoch(texture_cache) == 1,
			        "query deduplication failed while wrapping its epoch");

			auto&              image               = texture_cache.GetImage(first);
			constexpr uint32_t final_sampled_value = 0x88776655u;
			Require(name, "sampled write between discovery and acquisition",
			        resources.HandleFault(PageFaultAccess::Write, sampled.info.data.address),
			        "sampled image did not accept a CPU update after discovery");
			std::memcpy(memory, &final_sampled_value, sizeof(final_sampled_value));
			Require(name, "sampled final-acquisition precondition", image.IsCpuDirty(),
			        "the between-phase CPU write did not dirty the discovered image");
			const auto first_view       = texture_cache.FindTexture(first, first_desc);
			const auto repeated_view    = texture_cache.FindTexture(repeated, repeated_desc);
			auto       mapped_view_info = sampled.view_info;
			mapped_view_info.mapping.r  = vk::ComponentSwizzle::eB;
			mapped_view_info.mapping.b  = vk::ComponentSwizzle::eR;
			const auto mapped_view      = image.FindView(mapped_view_info);
			Require(name, "dynamic views",
			        first_view != nullptr && repeated_view == first_view &&
			            mapped_view != nullptr && mapped_view != first_view &&
			            image.views.views.size() == 2 && !image.usage.texture &&
			            !image.IsGpuModified() && !image.IsCpuDirty() &&
			            !TextureCacheTestAccess::PendingDownload(texture_cache, first),
			        "TextureCache::FindTexture did not preserve sampled view "
			        "identity, final refresh, and cache-only ownership");

			ImageInfo chain           = sampled.info;
			chain.data                = {0x10000, 0x8000};
			chain.extent              = {8, 8, 1};
			chain.resources           = {2, 4};
			chain.pitch               = 8;
			chain.mip_layout[0]       = {0, 0x4000, 8, 8};
			chain.mip_layout[1]       = {0x4000, 0x4000, 4, 4};
			ImageInfo subresource     = chain;
			subresource.data          = {0x16000, 0x1000};
			subresource.extent        = {4, 4, 1};
			subresource.resources     = {1, 1};
			subresource.pitch         = 4;
			subresource.mip_layout[0] = {0, 0x1000, 4, 4};
			const auto mip            = subresource.MipOf(chain);
			Require(name, "overlap resolution",
			        mip == 1 && subresource.SliceOf(chain, mip) == 2 &&
			            ImageRangeOverlaps(chain.data, subresource.data) &&
			            ImagePageRangesOverlap(chain.data, subresource.data),
			        "normalized mip/slice overlap did not resolve");

			auto       exact_desc = compatible_desc;
			const auto exact      = texture_cache.FindImage(exact_desc, true);
			Require(
			    name, "exact-format coexistence",
			    exact && exact != first && TextureCacheTestAccess::Contains(texture_cache, first) &&
			        texture_cache.GetImage(first).info.pixel_format == sampled.info.pixel_format &&
			        texture_cache.GetImage(exact).info.pixel_format ==
			            compatible_desc.info.pixel_format,
			    "exact-format lookup replaced or synchronized its compatible "
			    "cache record");
			auto       compatible_after_exact_desc = compatible_desc;
			const auto compatible_after_exact =
			    texture_cache.FindImage(compatible_after_exact_desc);
			Require(name, "last compatible format winner", compatible_after_exact == exact,
			        "non-exact lookup did not retain the last compatible "
			        "registration");

			auto null_desc              = sampled;
			null_desc.info.data         = {};
			auto       null_repeat_desc = null_desc;
			const auto null_image       = texture_cache.FindImage(null_desc);
			const auto null_repeat      = texture_cache.FindImage(null_repeat_desc);
			Require(name, "null image",
			        null_image && null_repeat == null_image && null_image != exact,
			        "typed null-image lookup was not stable");

			auto MakeLinearDesc = [&](uint64_t address, uint64_t size, vk::Format format,
			                          Prospero::BufferFormat guest_format, Prospero::ImageType type,
			                          vk::Extent3D extent, uint32_t layers,
			                          uint32_t bytes_per_block, uint32_t samples) {
				ImageDesc desc {};
				desc.type                 = BindingType::Texture;
				desc.info.data            = {address, size};
				desc.info.pixel_format    = format;
				desc.info.guest_format    = guest_format;
				desc.info.type            = type;
				desc.info.extent          = extent;
				desc.info.resources       = {1, layers};
				desc.info.pitch           = extent.width;
				desc.info.bytes_per_block = bytes_per_block;
				desc.info.samples         = samples;
				desc.info.tile_mode       = Prospero::TileMode::kLinear;
				desc.info.mip_layout[0]   = {0, size, extent.width, extent.height};
				desc.view_info.format     = format;
				if (type == Prospero::ImageType::kColor3D) {
					desc.view_info.type = vk::ImageViewType::e3D;
				} else if (layers > 1) {
					desc.view_info.type = vk::ImageViewType::e2DArray;
				} else {
					desc.view_info.type = vk::ImageViewType::e2D;
				}
				desc.view_info.aspect      = vk::ImageAspectFlagBits::eColor;
				desc.view_info.layer_count = layers;
				desc.view_info.usage       = vk::ImageUsageFlagBits::eSampled;
				return desc;
			};
			const auto HostReadBarrier = [&](vk::Buffer buffer, uint64_t size,
			                                 vk::PipelineStageFlags source_stage,
			                                 vk::AccessFlags        source_access) {
				vk::BufferMemoryBarrier barrier {};
				barrier.sType               = vk::StructureType::eBufferMemoryBarrier;
				barrier.srcAccessMask       = source_access;
				barrier.dstAccessMask       = vk::AccessFlagBits::eHostRead;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.buffer              = buffer;
				barrier.offset              = 0;
				barrier.size                = size;
				scheduler.Current().Handle().pipelineBarrier(source_stage,
				                                             vk::PipelineStageFlagBits::eHost, {},
				                                             0, nullptr, 1, &barrier, 0, nullptr);
			};
			const auto TransferReadBarrier = [&](vk::Buffer buffer, uint64_t size) {
				vk::BufferMemoryBarrier barrier {};
				barrier.sType               = vk::StructureType::eBufferMemoryBarrier;
				barrier.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
				barrier.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.buffer              = buffer;
				barrier.offset              = 0;
				barrier.size                = size;
				scheduler.Current().Handle().pipelineBarrier(
				    vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {},
				    0, nullptr, 1, &barrier, 0, nullptr);
			};

			// A formatted Buffer read must use the private
			// shadPS4-shaped image-copy path. Use a request larger
			// than the stream shortcut and poison guest backing
			// after upload so stale CPU staging cannot accidentally
			// satisfy the content check.
			constexpr uint64_t mip_prefix_offset = 0x2740000;
			constexpr auto     mip_format = Prospero::BufferFormat::k32UInt;
			constexpr auto     mip_tile   = Prospero::TileMode::kLinear;
			constexpr uint32_t mip_width  = 4097;
			constexpr uint32_t mip_height = 1;
			constexpr uint32_t mip_levels = 1;
			const uint32_t     mip_pitch =
			    TileGetTexturePitch(mip_format, mip_width, mip_tile);
			TileSizeAlign                  mip_total {};
			std::array<TileSizeOffset, 16> mip_sizes {};
			std::array<TilePaddedSize, 16> mip_padded {};
			TileGetTextureSize(mip_format, mip_width, mip_height, mip_levels, mip_tile,
			                   &mip_total, mip_sizes.data(), mip_padded.data());
			const uint64_t mip_prefix_size = mip_sizes[0].offset + mip_sizes[0].size;
			const uint64_t mip_guest_size  = mip_total.size + 256;
			Require(name, "formatted mip fixture",
			        mip_sizes[0].offset == 0 && mip_prefix_size > BufferCache::CACHING_PAGE_SIZE &&
			            mip_prefix_size < mip_guest_size && mip_guest_size % sizeof(uint32_t) == 0,
			        "single-mip fixture did not expose a cache-sized fitting backing prefix");
			std::vector<uint32_t> mip_native(mip_guest_size / sizeof(uint32_t));
			std::iota(mip_native.begin(), mip_native.end(), 0x61000000u);
			std::memcpy(memory + mip_prefix_offset, mip_native.data(), mip_guest_size);
			auto mip_desc = MakeLinearDesc(
			    base + mip_prefix_offset, mip_guest_size, vk::Format::eR32Uint, mip_format,
			    Prospero::ImageType::kColor2D, {mip_width, mip_height, 1}, 1, sizeof(uint32_t), 1);
			mip_desc.info.resources.levels = mip_levels;
			mip_desc.info.pitch            = mip_pitch;
			mip_desc.view_info.level_count = mip_levels;
			for (uint32_t level = 0; level < mip_levels; level++) {
				mip_desc.info.mip_layout[level] = {mip_sizes[level].offset, mip_sizes[level].size,
				                                   mip_padded[level].width,
				                                   mip_padded[level].height};
			}
			const auto mip_image = texture_cache.FindImage(mip_desc);
			(void)texture_cache.FindTexture(mip_image, mip_desc);
			texture_cache.MarkGpuWritten(mip_image);
			std::vector<uint32_t> mip_stale(mip_native.size(), 0xdeadbeefu);
			Libs::LibKernel::Memory::WriteBacking(base + mip_prefix_offset, mip_stale.data(),
			                                      mip_guest_size);

			Libs::Graphics::Buffer mip_insufficient(
			    m_runtime_context, scheduler, MemoryUsage::DeviceLocal, mip_desc.info.data.address,
			    AllFlags, mip_prefix_size - 1);
			Libs::Graphics::Buffer mip_prefix(m_runtime_context, scheduler,
			                                  MemoryUsage::DeviceLocal, mip_desc.info.data.address,
			                                  AllFlags, mip_prefix_size);
			Require(name, "formatted mip containment",
			        !BufferCacheTestAccess::SynchronizeBufferFromImage(
			            resources.GetBufferCache(), mip_insufficient, mip_desc.info.data.address,
			            mip_prefix_size - 1) &&
			            BufferCacheTestAccess::SynchronizeBufferFromImage(
			                resources.GetBufferCache(), mip_prefix, mip_desc.info.data.address,
			                mip_prefix_size) &&
			            texture_cache.GetImage(mip_image).IsGpuModified(),
			        "image synchronization accepted a partial first "
			        "mip, rejected a fitting mip "
			        "prefix, or transferred ownership");

			auto& buffer_cache = resources.GetBufferCache();
			BufferBinding mip_formatted;
			bool          formatted_owner_was_absent = false;
			gpu.SendCommandSync([&] {
				formatted_owner_was_absent =
				    !buffer_cache.IsRegionRegistered(mip_desc.info.data.address,
				                                     mip_desc.info.data.size);
				mip_formatted = buffer_cache.ObtainBuffer(command, mip_desc.info.data.address,
				                                            mip_desc.info.data.size, false, true, true);
			});
			Require(name, "formatted cache fixture", formatted_owner_was_absent,
			        "formatted image already had a cached Buffer owner");
			Require(name, "formatted Buffer path",
			        mip_formatted.owner != nullptr && mip_formatted.buffer != nullptr &&
			            texture_cache.GetImage(mip_image).IsGpuModified(),
			        "formatted read bypassed the cached image-copy "
			        "path or transferred ownership");
			command.RetainResourceUntilFence(mip_formatted.owner);
			auto mip_prefix_readback =
			    CreateHostBuffer(name, mip_prefix_size, vk::BufferUsageFlagBits::eTransferDst,
			                     std::vector<u32>(mip_prefix_size / sizeof(uint32_t), 0));
			auto mip_formatted_readback =
			    CreateHostBuffer(name, mip_guest_size, vk::BufferUsageFlagBits::eTransferDst,
			                     std::vector<u32>(mip_guest_size / sizeof(uint32_t), 0));
			TransferReadBarrier(mip_prefix.Handle(), mip_prefix_size);
			const vk::BufferCopy mip_prefix_copy {0, 0, mip_prefix_size};
			command.Handle().copyBuffer(mip_prefix.Handle(), mip_prefix_readback.buffer, 1,
			                            &mip_prefix_copy);
			TransferReadBarrier(mip_formatted.buffer, mip_guest_size);
			const vk::BufferCopy mip_formatted_copy {mip_formatted.offset, 0, mip_guest_size};
			command.Handle().copyBuffer(mip_formatted.buffer, mip_formatted_readback.buffer, 1,
			                            &mip_formatted_copy);
			HostReadBarrier(mip_prefix_readback.buffer, mip_prefix_readback.size,
			                vk::PipelineStageFlagBits::eTransfer,
			                vk::AccessFlagBits::eTransferWrite);
			HostReadBarrier(mip_formatted_readback.buffer, mip_formatted_readback.size,
			                vk::PipelineStageFlagBits::eTransfer,
			                vk::AccessFlagBits::eTransferWrite);
			const std::array<uint32_t, 2> volume_values {0x10203040u, 0x50607080u};
			std::memcpy(memory + 0x1000, volume_values.data(), sizeof(volume_values));
			auto array_desc =
			    MakeLinearDesc(base + 0x1000, sizeof(volume_values), vk::Format::eR32Uint,
			                   Prospero::BufferFormat::k32UInt,
			                   Prospero::ImageType::kColor2D, {1, 1, 1}, 2, 4, 1);
			const auto array_image = texture_cache.FindImage(array_desc);
			(void)texture_cache.FindTexture(array_image, array_desc);
			texture_cache.MarkGpuWritten(array_image);
			auto volume_desc                  = array_desc;
			volume_desc.info.type             = Prospero::ImageType::kColor3D;
			volume_desc.info.extent           = {1, 1, 2};
			volume_desc.info.resources.layers = 1;
			volume_desc.view_info.type        = vk::ImageViewType::e3D;
			volume_desc.view_info.layer_count = 1;
			const auto volume_image           = texture_cache.FindImage(volume_desc);
			Require(name, "2D-array/3D overlap copy",
			        volume_image && volume_image != array_image &&
			            texture_cache.GetImage(volume_image).backing.image_type ==
			                vk::ImageType::e3D &&
			            texture_cache.GetImage(volume_image).IsGpuModified(),
			        "array-to-volume expansion lost slices or GPU "
			        "ownership");

			constexpr uint64_t unique_volume_offset = 0x27b0000;
			std::memcpy(memory + unique_volume_offset, volume_values.data(), sizeof(volume_values));
			auto unique_volume_desc = MakeLinearDesc(
			    base + unique_volume_offset, sizeof(volume_values), vk::Format::eR32Uint,
			    Prospero::BufferFormat::k32UInt,
			    Prospero::ImageType::kColor3D, {1, 1, 2}, 1, sizeof(uint32_t), 1);
			const auto unique_volume_image = texture_cache.FindImage(unique_volume_desc);
			(void)texture_cache.FindTexture(unique_volume_image, unique_volume_desc);
			texture_cache.MarkGpuWritten(unique_volume_image);
			Libs::Graphics::Buffer partial_volume(
			    m_runtime_context, scheduler, MemoryUsage::DeviceLocal,
			    unique_volume_desc.info.data.address, AllFlags,
			    unique_volume_desc.info.data.size - sizeof(uint32_t));
			Libs::Graphics::Buffer full_volume(
			    m_runtime_context, scheduler, MemoryUsage::DeviceLocal,
			    unique_volume_desc.info.data.address, AllFlags, unique_volume_desc.info.data.size);
			Require(name, "formatted volume containment",
			        !BufferCacheTestAccess::SynchronizeBufferFromImage(
			            resources.GetBufferCache(), partial_volume,
			            unique_volume_desc.info.data.address,
			            unique_volume_desc.info.data.size - sizeof(uint32_t)) &&
			            BufferCacheTestAccess::SynchronizeBufferFromImage(
			                resources.GetBufferCache(), full_volume,
			                unique_volume_desc.info.data.address,
			                unique_volume_desc.info.data.size) &&
			            texture_cache.GetImage(unique_volume_image).IsGpuModified(),
			        "partial 3D synchronization was accepted, "
			        "full-volume synchronization was "
			        "rejected, or ownership changed");

			constexpr uint64_t             depth_containment_offset = 0x27c0000;
			constexpr std::array<float, 3> depth_containment_values {0.25f, 0.75f, 0.5f};
			std::memcpy(memory + depth_containment_offset, depth_containment_values.data(),
			            sizeof(depth_containment_values));
			auto depth_containment = MakeLinearDesc(
			    base + depth_containment_offset, sizeof(depth_containment_values),
			    vk::Format::eD32Sfloat, Prospero::BufferFormat::k32Float,
			    Prospero::ImageType::kColor2D, {2, 1, 1}, 1, sizeof(float), 1);
			depth_containment.type                  = BindingType::DepthTarget;
			depth_containment.info.resources.levels = 2;
			depth_containment.info.mip_layout[0]    = {0, 2 * sizeof(float), 2, 1};
			depth_containment.info.mip_layout[1]    = {2 * sizeof(float), sizeof(float), 1, 1};
			depth_containment.view_info.format      = vk::Format::eD32Sfloat;
			depth_containment.view_info.aspect      = vk::ImageAspectFlagBits::eDepth;
			depth_containment.view_info.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
			depth_containment.view_info.level_count = 2;
			const auto depth_containment_image      = texture_cache.FindImage(depth_containment);
			(void)texture_cache.FindDepthTarget(depth_containment_image, depth_containment);
			texture_cache.MarkGpuWritten(depth_containment_image);
			Libs::Graphics::Buffer partial_depth(m_runtime_context, scheduler,
			                                     MemoryUsage::DeviceLocal,
			                                     depth_containment.info.data.address, AllFlags,
			                                     depth_containment.info.mip_layout[0].size);
			Require(name, "formatted depth containment",
			        !BufferCacheTestAccess::SynchronizeBufferFromImage(
			            resources.GetBufferCache(), partial_depth,
			            depth_containment.info.data.address,
			            depth_containment.info.mip_layout[0].size) &&
			            texture_cache.GetImage(depth_containment_image).IsGpuModified(),
			        "partial depth synchronization was accepted or "
			        "transferred ownership");

			auto native_array_info  = array_desc.info;
			native_array_info.data  = {};
			auto native_volume_info = volume_desc.info;
			native_volume_info.data = {};
			Libs::Graphics::Image native_array(m_runtime_context, scheduler, native_array_info);
			Libs::Graphics::Image native_volume(m_runtime_context, scheduler, native_volume_info);
			native_volume.CopyImage(native_array);
			native_array.CopyImage(native_volume);
			Require(name, "Image-owned 2D-array/3D copy state",
			        native_volume.backing.state.layout == vk::ImageLayout::eTransferSrcOptimal &&
			            native_volume.backing.state.access_mask ==
			                vk::AccessFlagBits2::eTransferRead &&
			            native_array.backing.state.layout == vk::ImageLayout::eGeneral &&
			            native_array.backing.state.access_mask ==
			                (vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead),
			        "Image::CopyImage did not retain pinned "
			        "source/destination states");

			constexpr uint64_t                block_alias_offset = 0x23000;
			constexpr std::array<uint32_t, 4> block_alias_data {0x01234567u, 0x89abcdefu,
			                                                    0xfedcba98u, 0x76543210u};
			std::memcpy(memory + block_alias_offset, block_alias_data.data(),
			            sizeof(block_alias_data));
			auto uncompressed_block = MakeLinearDesc(
			    base + block_alias_offset, sizeof(block_alias_data), vk::Format::eR32G32B32A32Uint,
			    Prospero::BufferFormat::k32_32_32_32UInt,
			    Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 16, 1);
			const auto uncompressed_block_image = texture_cache.FindImage(uncompressed_block);
			(void)texture_cache.FindTexture(uncompressed_block_image, uncompressed_block);
			texture_cache.MarkGpuWritten(uncompressed_block_image);
			auto compressed_block = MakeLinearDesc(
			    base + block_alias_offset, sizeof(block_alias_data), vk::Format::eBc3UnormBlock,
			    Prospero::BufferFormat::kBc3UNorm,
			    Prospero::ImageType::kColor2D, {4, 4, 1}, 1, 16, 1);
			const auto compressed_block_image = texture_cache.FindImage(compressed_block);
			const bool compressed_block_download =
			    TextureCacheTestAccess::TryDownload(texture_cache, compressed_block_image);
			scheduler.FinishCurrent();
			scheduler.DrainPriorityOperations();
			std::array<uint32_t, block_alias_data.size()> block_alias_after {};
			std::memcpy(block_alias_after.data(), memory + block_alias_offset,
			            sizeof(block_alias_after));
			Require(name, "compressed view expansion",
			        compressed_block_image && compressed_block_image != uncompressed_block_image &&
			            texture_cache.GetImage(compressed_block_image).backing.format ==
			                vk::Format::eBc3UnormBlock &&
			            compressed_block_download && block_alias_after == block_alias_data,
			        "size-compatible compressed alias did not preserve its native "
			        "contents");

			ImageInfo resolve_source_info {};
			resolve_source_info.pixel_format = vk::Format::eR8G8B8A8Unorm;
			resolve_source_info.guest_format =
			    Prospero::BufferFormat::k8_8_8_8UNorm;
			resolve_source_info.type            = Prospero::ImageType::kColor2D;
			resolve_source_info.extent          = {4, 4, 1};
			resolve_source_info.resources       = {1, 1};
			resolve_source_info.pitch           = 4;
			resolve_source_info.bytes_per_block = 4;
			resolve_source_info.samples         = 2;
			resolve_source_info.tile_mode =
			    Prospero::TileMode::kRenderTarget;
			ImageInfo resolve_destination_info = resolve_source_info;
			resolve_destination_info.type      = Prospero::ImageType::kColor2D;
			resolve_destination_info.samples   = 1;
			Libs::Graphics::Image resolve_source(m_runtime_context, scheduler, resolve_source_info);
			Libs::Graphics::Image resolve_destination(m_runtime_context, scheduler,
			                                          resolve_destination_info);
			resolve_destination.Resolve(resolve_source, {}, {});
			Require(name, "Image-owned MSAA resolve",
			        resolve_source.backing.state.layout == vk::ImageLayout::eTransferSrcOptimal &&
			            resolve_destination.backing.state.layout ==
			                vk::ImageLayout::eTransferDstOptimal,
			        "Image::Resolve did not issue the MSAA-to-single-sample transfer");
			Libs::Graphics::Image copy_destination(m_runtime_context, scheduler,
			                                       resolve_destination_info);
			copy_destination.Resolve(resolve_destination, {}, {});
			Require(
			    name, "Image-owned single-sample resolve copy",
			    resolve_destination.backing.state.layout == vk::ImageLayout::eTransferSrcOptimal &&
			        copy_destination.backing.state.layout == vk::ImageLayout::eTransferDstOptimal,
			    "Image::Resolve did not retain the single-sample copy path");

			const std::array<uint16_t, 2> multisample_source {0x4000u, 0xc000u};
			std::memcpy(memory + 0x2000, &multisample_source, sizeof(multisample_source));
			auto color_desc =
			    MakeLinearDesc(base + 0x2000, sizeof(multisample_source), vk::Format::eR16G16Unorm,
			                   Prospero::BufferFormat::k16_16UNorm,
			                   Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 4, 1);
			const auto color_image = texture_cache.FindImage(color_desc);
			(void)texture_cache.FindTexture(color_image, color_desc);
			texture_cache.MarkGpuWritten(color_image);
			const std::array<uint16_t, 2> refreshed_multisample_source {0x2000u, 0xe000u};
			Require(name, "unequal-sample source CPU write",
			        resources.HandleFault(PageFaultAccess::Write, color_desc.info.data.address),
			        "color-to-MS-depth source did not accept a CPU update");
			std::memcpy(memory + 0x2000, refreshed_multisample_source.data(),
			            sizeof(refreshed_multisample_source));
			constexpr uint64_t ms_stencil_offset = 0x80000;
			constexpr uint64_t ms_stencil_size   = 0x10000;
			auto               ms_stencil_owner  = resources.GetBufferCache().ObtainBuffer(
			    command, base + ms_stencil_offset, ms_stencil_size, false, true);
			Require(name, "MS stencil buffer allocation", ms_stencil_owner.owner != nullptr,
			        "failed to create an unequal-sample stencil source");
			command.RetainResourceUntilFence(ms_stencil_owner.owner);
			resources.GetBufferCache().FillBuffer(base + ms_stencil_offset, ms_stencil_size,
			                                      0x41414141u);
			auto ms_depth_desc              = color_desc;
			ms_depth_desc.type              = BindingType::DepthTarget;
			ms_depth_desc.info.stencil      = {base + ms_stencil_offset, ms_stencil_size};
			ms_depth_desc.info.pixel_format = vk::Format::eD24UnormS8Uint;
			ms_depth_desc.info.guest_format =
			    Prospero::BufferFormat::k16UNorm;
			ms_depth_desc.info.bytes_per_block = 2;
			ms_depth_desc.info.samples         = 2;
			ms_depth_desc.info.type            = Prospero::ImageType::kColor2D;
			ms_depth_desc.view_info.format     = vk::Format::eD24UnormS8Uint;
			ms_depth_desc.view_info.aspect =
			    vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
			ms_depth_desc.view_info.usage       = vk::ImageUsageFlagBits::eDepthStencilAttachment;
			const auto         ms_depth_image   = texture_cache.FindImage(ms_depth_desc);
			constexpr uint64_t ms_htile_address = base + 0x2600000;
			constexpr uint64_t ms_htile_size    = 0x10000;
			ms_depth_desc.info.metadata.range   = {ms_htile_address, ms_htile_size};
			ms_depth_desc.info.metadata.kind    = ImageMetadataKind::Htile;
			const auto ms_depth_with_htile      = texture_cache.FindImage(ms_depth_desc);
			const auto ms_depth_view =
			    texture_cache.FindDepthTarget(ms_depth_with_htile, ms_depth_desc);
			auto sampled_ms_depth             = ms_depth_desc;
			sampled_ms_depth.type             = BindingType::Texture;
			sampled_ms_depth.info.metadata    = {};
			sampled_ms_depth.view_info.usage  = vk::ImageUsageFlagBits::eSampled;
			const auto sampled_ms_depth_image = texture_cache.FindImage(sampled_ms_depth);
			Require(name, "unequal-sample depth overlap",
			        ms_depth_image && ms_depth_with_htile == ms_depth_image &&
			            sampled_ms_depth_image == ms_depth_image && ms_depth_image != color_image &&
			            ms_depth_view != nullptr &&
			            texture_cache.GetImage(ms_depth_image).backing.samples == 2 &&
			            texture_cache.GetImage(ms_depth_image).backing.state.layout ==
			                vk::ImageLayout::eDepthStencilAttachmentOptimal &&
			            texture_cache.GetImage(ms_depth_image).IsGpuModified() &&
			            texture_cache.GetImage(ms_depth_image).info.htile_clear_mask == 0 &&
			            texture_cache.GetImage(ms_depth_image).info.metadata.range ==
			                ms_depth_desc.info.metadata.range &&
			            texture_cache.IsMeta(ms_htile_address) &&
			            !texture_cache.IsMetaCleared(ms_htile_address, 0) &&
			            !resources.GetBufferCache().HasGpuDirtyBytes(base + ms_stencil_offset,
			                                                         ms_stencil_size) &&
			            !texture_cache.QueryRegion(base + ms_stencil_offset, ms_stencil_size)
			                 .gpu_image_bytes,
			        "unequal-sample overlap did not run the color-to-MS-depth pass "
			        "without manufacturing stencil ownership");
			auto&      oversized_ms     = texture_cache.GetImage(ms_depth_image);
			const auto ms_data_size     = oversized_ms.info.data.size;
			oversized_ms.info.data.size = (32ull << 20) + 4;
			const bool oversized_ms_readback =
			    !TextureCacheTestAccess::TryDownload(texture_cache, ms_depth_image);
			oversized_ms.info.data.size = ms_data_size;
			Require(name, "oversized multisample download rejection",
			        oversized_ms_readback && oversized_ms.IsGpuModified() &&
			            !oversized_ms.IsBufferModified() &&
			            !resources.GetBufferCache().HasGpuDirtyBytes(
			                ms_depth_desc.info.data.address, ms_data_size),
			        "an unsupported multisample source reserved download storage or "
			        "published Buffer ownership");

			const std::array<uint16_t, 4> multisample_source_4x {0x0000u, 0x4000u, 0x8000u,
			                                                     0xffffu};
			std::memcpy(memory + 0x7000, multisample_source_4x.data(),
			            sizeof(multisample_source_4x));
			auto color_desc_4x = MakeLinearDesc(
			    base + 0x7000, sizeof(multisample_source_4x), vk::Format::eR16G16B16A16Unorm,
			    Prospero::BufferFormat::k16_16_16_16UNorm,
			    Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 8, 1);
			const auto color_image_4x          = texture_cache.FindImage(color_desc_4x);
			auto       ms_depth_desc_4x        = color_desc_4x;
			ms_depth_desc_4x.type              = BindingType::DepthTarget;
			ms_depth_desc_4x.info.pixel_format = vk::Format::eD16Unorm;
			ms_depth_desc_4x.info.guest_format =
			    Prospero::BufferFormat::k16UNorm;
			ms_depth_desc_4x.info.bytes_per_block = 2;
			ms_depth_desc_4x.info.samples         = 4;
			ms_depth_desc_4x.info.type            = Prospero::ImageType::kColor2D;
			ms_depth_desc_4x.view_info.format     = vk::Format::eD16Unorm;
			ms_depth_desc_4x.view_info.aspect     = vk::ImageAspectFlagBits::eDepth;
			ms_depth_desc_4x.view_info.usage      = vk::ImageUsageFlagBits::eDepthStencilAttachment;
			const auto ms_depth_image_4x          = texture_cache.FindImage(ms_depth_desc_4x);
			Require(name, "four-sample depth overlap",
			        ms_depth_image_4x && ms_depth_image_4x != color_image_4x &&
			            texture_cache.GetImage(ms_depth_image_4x).backing.samples == 4,
			        "four-sample color packing did not produce a depth image");

			const auto ms_depth_backing = texture_cache.GetImage(ms_depth_image).backing.image;
			const auto first_stencil_association =
			    texture_cache.FindImageFromRange(base + ms_stencil_offset, ms_stencil_size, false);
			Require(
			    name, "stencil association shape",
			    first_stencil_association && first_stencil_association != ms_depth_image &&
			        texture_cache.GetImage(first_stencil_association).info.pixel_format ==
			            vk::Format::eUndefined &&
			        texture_cache.GetImage(first_stencil_association).backing.image == nullptr &&
			        texture_cache.GetImage(first_stencil_association).depth_id == ms_depth_image,
			    "stencil address did not create a lightweight depth association");
			Require(name, "stencil association download rejection",
			        !TextureCacheTestAccess::TryDownload(texture_cache, first_stencil_association),
			        "a lightweight stencil association entered image download planning");

			auto exact_ms_depth_alias              = ms_depth_desc;
			exact_ms_depth_alias.type              = BindingType::Texture;
			exact_ms_depth_alias.info.stencil      = {};
			exact_ms_depth_alias.info.pixel_format = vk::Format::eR8G8B8A8Unorm;
			exact_ms_depth_alias.info.guest_format =
			    Prospero::BufferFormat::k8_8_8_8UNorm;
			exact_ms_depth_alias.info.bytes_per_block = 4;
			exact_ms_depth_alias.view_info.format     = vk::Format::eR8G8B8A8Unorm;
			exact_ms_depth_alias.view_info.aspect     = vk::ImageAspectFlagBits::eColor;
			exact_ms_depth_alias.view_info.usage      = vk::ImageUsageFlagBits::eSampled;
			const auto exact_ms_depth_image = texture_cache.FindImage(exact_ms_depth_alias, true);
			Require(name, "exact multisample depth/stencil coexistence",
			        exact_ms_depth_image && exact_ms_depth_image != ms_depth_image &&
			            TextureCacheTestAccess::Contains(texture_cache, ms_depth_image) &&
			            texture_cache.GetImage(ms_depth_image).backing.image == ms_depth_backing &&
			            texture_cache.GetImage(first_stencil_association).depth_id ==
			                ms_depth_image,
			        "exact multisample depth/stencil lookup replaced the old image "
			        "or stole its stencil association");

			constexpr uint64_t second_stencil_offset = 0x90000;
			auto               switched_ms_depth     = ms_depth_desc;
			switched_ms_depth.info.stencil  = {base + second_stencil_offset, ms_stencil_size};
			const auto switched_ms_depth_id = texture_cache.FindImage(switched_ms_depth);
			TextureCacheTestAccess::AssociateStencil(texture_cache, switched_ms_depth_id,
			                                         switched_ms_depth.info.stencil);
			const auto second_stencil_association = texture_cache.FindImageFromRange(
			    base + second_stencil_offset, ms_stencil_size, false);
			Require(
			    name, "stencil association switch",
			    switched_ms_depth_id == ms_depth_image &&
			        texture_cache.GetImage(switched_ms_depth_id).backing.image ==
			            ms_depth_backing &&
			        texture_cache.FindImageFromRange(base + ms_stencil_offset, ms_stencil_size,
			                                         false) == first_stencil_association &&
			        texture_cache.GetImage(first_stencil_association).depth_id == ms_depth_image &&
			        second_stencil_association &&
			        texture_cache.GetImage(second_stencil_association).depth_id == ms_depth_image,
			    "changing stencil address recreated depth or discarded an "
			    "existing lightweight association");

			constexpr uint64_t added_stencil_depth_offset = 0x60000;
			constexpr uint64_t added_stencil_offset       = 0x70000;
			constexpr uint64_t added_stencil_size         = 0x10000;
			constexpr float    added_stencil_depth_value  = 0.75f;
			std::memcpy(memory + added_stencil_depth_offset, &added_stencil_depth_value,
			            sizeof(added_stencil_depth_value));
			auto depth_only_source =
			    MakeLinearDesc(base + added_stencil_depth_offset, 0x10000, vk::Format::eD32Sfloat,
			                   Prospero::BufferFormat::k32Float,
			                   Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 4, 1);
			depth_only_source.type             = BindingType::DepthTarget;
			depth_only_source.view_info.format = vk::Format::eD32Sfloat;
			depth_only_source.view_info.aspect = vk::ImageAspectFlagBits::eDepth;
			depth_only_source.view_info.usage  = vk::ImageUsageFlagBits::eDepthStencilAttachment;
			const auto depth_only_source_image = texture_cache.FindImage(depth_only_source);

			auto combined_destination         = depth_only_source;
			combined_destination.info.stencil = {base + added_stencil_offset, added_stencil_size};
			combined_destination.info.pixel_format = vk::Format::eD32SfloatS8Uint;
			combined_destination.view_info.format  = vk::Format::eD32SfloatS8Uint;
			combined_destination.view_info.aspect =
			    vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
			const auto combined_destination_image = texture_cache.FindImage(combined_destination);
			TextureCacheTestAccess::AssociateStencil(texture_cache, combined_destination_image,
			                                         combined_destination.info.stencil);
			const auto combined_stencil_association = texture_cache.FindImageFromRange(
			    base + added_stencil_offset, added_stencil_size, false);
			Require(name, "depth-only to combined association",
			        combined_destination_image != depth_only_source_image &&
			            combined_stencil_association &&
			            texture_cache.GetImage(combined_stencil_association).depth_id ==
			                combined_destination_image &&
			            texture_cache.GetImage(combined_stencil_association).backing.image ==
			                nullptr,
			        "stencil addition did not recreate depth and register its "
			        "lightweight association");

			const uint32_t mirror_value = 0xa1b2c3d4u;
			std::memcpy(memory + 0x5000, &mirror_value, sizeof(mirror_value));
			auto mirror_desc =
			    MakeLinearDesc(base + 0x5000, sizeof(mirror_value), vk::Format::eR32Uint,
			                   Prospero::BufferFormat::k32UInt,
			                   Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 4, 1);
			const auto mirror_image = texture_cache.FindImage(mirror_desc);
			(void)texture_cache.FindTexture(mirror_image, mirror_desc);
			texture_cache.MarkGpuWritten(mirror_image);
			constexpr uint32_t mirror_cpu_value = 0x0f1e2d3cu;
			Require(name, "CPU-dirty formatted mirror fault",
			        resources.HandleFault(PageFaultAccess::Write, mirror_desc.info.data.address),
			        "GPU image did not accept a CPU write before Buffer mirroring");
			std::memcpy(memory + 0x5000, &mirror_cpu_value, sizeof(mirror_cpu_value));
			auto mirror_binding = resources.GetBufferCache().ObtainBuffer(
			    command, base + 0x5000, sizeof(mirror_value), false, true, true);
			Require(name, "CPU-dirty formatted mirror source",
			        mirror_binding.buffer != nullptr &&
			            !texture_cache.GetImage(mirror_image).IsBufferModified(),
			        "formatted mirror did not expose a readable Buffer source");
			if (mirror_binding.owner != nullptr) {
				command.RetainResourceUntilFence(mirror_binding.owner);
			}
			auto mirror_cpu_readback =
			    CreateHostBuffer(name, sizeof(mirror_cpu_value),
			                     vk::BufferUsageFlagBits::eTransferDst, std::vector<u32> {0});
			const vk::BufferCopy mirror_cpu_copy {mirror_binding.offset, 0,
			                                      sizeof(mirror_cpu_value)};
			command.Handle().copyBuffer(mirror_binding.buffer, mirror_cpu_readback.buffer, 1,
			                            &mirror_cpu_copy);
			HostReadBarrier(mirror_cpu_readback.buffer, mirror_cpu_readback.size,
			                vk::PipelineStageFlagBits::eTransfer,
			                vk::AccessFlagBits::eTransferWrite);
			auto       mirror_refresh_desc = mirror_desc;
			const auto mirror_refresh      = texture_cache.FindImage(mirror_refresh_desc);
			(void)texture_cache.FindTexture(mirror_refresh, mirror_refresh_desc);
			Require(name, "buffer-to-image ownership",
			        mirror_refresh == mirror_image &&
			            !texture_cache.GetImage(mirror_refresh).IsBufferModified(),
			        "buffer-backed refresh incorrectly transferred dirty ownership "
			        "to the image");
			texture_cache.MarkGpuWritten(mirror_refresh);

			constexpr uint64_t exact_buffer_offset = 0x90000;
			constexpr uint32_t exact_buffer_value  = 0x3f234567u;
			std::memcpy(memory + exact_buffer_offset, &exact_buffer_value,
			            sizeof(exact_buffer_value));
			auto exact_buffer_desc = MakeLinearDesc(
			    base + exact_buffer_offset, sizeof(exact_buffer_value), vk::Format::eR32Uint,
			    Prospero::BufferFormat::k32UInt,
			    Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 4, 1);
			const auto exact_buffer_image = texture_cache.FindImage(exact_buffer_desc);
			(void)texture_cache.FindTexture(exact_buffer_image, exact_buffer_desc);
			texture_cache.MarkGpuWritten(exact_buffer_image);
			auto exact_buffer_binding = resources.GetBufferCache().ObtainBuffer(
			    command, exact_buffer_desc.info.data.address, exact_buffer_desc.info.data.size,
			    false, true, true);
			Require(name, "exact replacement Buffer synchronization",
			        exact_buffer_binding.buffer != nullptr &&
			            !texture_cache.GetImage(exact_buffer_image).IsBufferModified() &&
			            texture_cache.GetImage(exact_buffer_image).IsGpuModified(),
			        "exact replacement copy transferred cache ownership");
			if (exact_buffer_binding.owner != nullptr) {
				command.RetainResourceUntilFence(exact_buffer_binding.owner);
			}
			auto exact_float_desc              = exact_buffer_desc;
			exact_float_desc.info.pixel_format = vk::Format::eR32Sfloat;
			exact_float_desc.info.guest_format =
			    Prospero::BufferFormat::k32Float;
			exact_float_desc.view_info.format = vk::Format::eR32Sfloat;
			const auto exact_float_image      = texture_cache.FindImage(exact_float_desc, true);
			(void)texture_cache.FindTexture(exact_float_image, exact_float_desc);
			Require(name, "Buffer-superseded exact coexistence",
			        exact_float_image != exact_buffer_image &&
			            TextureCacheTestAccess::Contains(texture_cache, exact_buffer_image) &&
			            texture_cache.GetImage(exact_buffer_image).IsGpuModified() &&
			            !texture_cache.GetImage(exact_float_image).IsBufferModified() &&
			            !texture_cache.GetImage(exact_float_image).IsGpuModified(),
			        "exact-format lookup retired its old record or transferred Buffer "
			        "ownership");
			auto exact_buffer_readback =
			    CreateHostBuffer(name, sizeof(exact_buffer_value),
			                     vk::BufferUsageFlagBits::eTransferDst, std::vector<u32> {0});
			auto& exact_float_native = texture_cache.GetImage(exact_float_image);
			exact_float_native.Transit(vk::ImageLayout::eTransferSrcOptimal,
			                           vk::AccessFlagBits2::eTransferRead, {}, command.Handle());
			vk::BufferImageCopy exact_float_copy {};
			exact_float_copy.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
			exact_float_copy.imageSubresource.layerCount = 1;
			exact_float_copy.imageExtent                 = {1, 1, 1};
			command.Handle().copyImageToBuffer(exact_float_native.backing.image,
			                                   vk::ImageLayout::eTransferSrcOptimal,
			                                   exact_buffer_readback.buffer, 1, &exact_float_copy);
			HostReadBarrier(exact_buffer_readback.buffer, exact_buffer_readback.size,
			                vk::PipelineStageFlagBits::eTransfer,
			                vk::AccessFlagBits::eTransferWrite);

			const uint32_t clear_source = 0x01010101u;
			std::memcpy(memory + 0x6000, &clear_source, sizeof(clear_source));
			auto clear_desc =
			    MakeLinearDesc(base + 0x6000, sizeof(clear_source), vk::Format::eR8G8B8A8Unorm,
			                   Prospero::BufferFormat::k8_8_8_8UNorm,
			                   Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 4, 1);
			const auto clear_image  = texture_cache.FindImage(clear_desc);
			auto       buffer_write = resources.GetBufferCache().ObtainBuffer(
			    command, base + 0x6000, sizeof(clear_source), true, false, true);
			Require(name, "buffer-write ownership",
			        buffer_write.buffer != nullptr &&
			            texture_cache.GetImage(clear_image).IsBufferModified(),
			        "formatted buffer write did not supersede the image");
			if (buffer_write.owner != nullptr) {
				command.RetainResourceUntilFence(buffer_write.owner);
			}
			Require(name, "buffer-owned full image clear",
			        texture_cache.ClearImageFromBuffer(command, base + 0x6000, sizeof(clear_source),
			                                           0xaabbccddu) &&
			            !texture_cache.GetImage(clear_image).IsBufferModified() &&
			            texture_cache.GetImage(clear_image).IsGpuModified(),
			        "full image clear retained dual buffer/image GPU ownership");
			texture_cache.MarkGpuWritten(clear_image);
			auto clear_read_desc = clear_desc;
			Require(name, "post-clear image reuse",
			        texture_cache.FindImage(clear_read_desc) == clear_image,
			        "cleared image could not be read or written after ownership "
			        "transfer");

			constexpr uint64_t partial_image_offset  = 0xa000;
			constexpr uint64_t partial_buffer_offset = 0xa010;
			constexpr uint64_t partial_clean_offset  = 0xa020;
			constexpr uint32_t partial_image_value   = 0x31415926u;
			constexpr uint32_t partial_buffer_value  = 0x27182818u;
			constexpr uint32_t partial_clean_value   = 0xabcdef01u;
			std::memcpy(memory + partial_clean_offset, &partial_clean_value,
			            sizeof(partial_clean_value));
			auto partial_write = resources.GetBufferCache().ObtainBuffer(
			    command, base + partial_image_offset, sizeof(partial_image_value), true, false);
			Require(name, "partial-page buffer allocation", partial_write.owner != nullptr,
			        "partial-page GPU write did not create a native buffer");
			command.RetainResourceUntilFence(partial_write.owner);
			resources.GetBufferCache().FillBuffer(base + partial_image_offset,
			                                      sizeof(partial_image_value), partial_image_value);
			resources.GetBufferCache().FillBuffer(
			    base + partial_buffer_offset, sizeof(partial_buffer_value), partial_buffer_value);
			auto partial_desc = MakeLinearDesc(
			    base + partial_image_offset, sizeof(partial_image_value), vk::Format::eR32Uint,
			    Prospero::BufferFormat::k32UInt,
			    Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 4, 1);
			const auto partial_image = texture_cache.FindImage(partial_desc);
			Require(name, "partial-page image upload",
			        !texture_cache.GetImage(partial_image).IsGpuModified(),
			        "image upload incorrectly consumed buffer dirty ownership");
			auto partial_image_mirror = resources.GetBufferCache().ObtainBuffer(
			    command, base + partial_image_offset, sizeof(partial_image_value), false, true,
			    true);
			Require(name, "partial-page image mirror",
			        partial_image_mirror.buffer != nullptr &&
			            partial_image_mirror.owner != nullptr &&
			            !texture_cache.GetImage(partial_image).IsGpuModified(),
			        "same-page image upload lost its buffer source");
			command.RetainResourceUntilFence(partial_image_mirror.owner);
			const auto partial_clean_source = resources.GetBufferCache().ObtainBufferForImage(
			    base + partial_clean_offset, sizeof(partial_clean_value));
			Require(name, "partial-page clean source", partial_clean_source.buffer != nullptr,
			        "clean same-page bytes inherited unrelated buffer ownership");
			Require(name, "partial-page remaining fault",
			        resources.HandleFault(PageFaultAccess::Read, base + partial_buffer_offset),
			        "same-page buffer ownership was lost after image transfer");
			uint32_t partial_buffer_backing = 0;
			std::memcpy(&partial_buffer_backing, memory + partial_buffer_offset,
			            sizeof(partial_buffer_backing));
			uint32_t partial_image_backing = 0;
			std::memcpy(&partial_image_backing, memory + partial_image_offset,
			            sizeof(partial_image_backing));
			Require(name, "partial-page readback values",
			        partial_image_backing == partial_image_value &&
			            partial_buffer_backing == partial_buffer_value,
			        "same-page image/buffer readback published incorrect bytes");
			constexpr uint32_t partial_cpu_refresh_value = 0x13579bdfu;
			Require(name, "partial-page CPU write fault",
			        resources.HandleFault(PageFaultAccess::Write, base + partial_clean_offset),
			        "same-page CPU write did not invalidate cached ownership");
			std::memcpy(memory + partial_clean_offset, &partial_cpu_refresh_value,
			            sizeof(partial_cpu_refresh_value));
			const auto partial_cpu_refresh_source = resources.GetBufferCache().ObtainBufferForImage(
			    base + partial_clean_offset, sizeof(partial_cpu_refresh_value));
			Require(name, "partial-page CPU refresh source",
			        partial_cpu_refresh_source.buffer != nullptr,
			        "CPU-dirty same-page bytes did not resolve through the cached "
			        "buffer");
			auto partial_cpu_refresh_readback =
			    CreateHostBuffer(name, sizeof(partial_cpu_refresh_value),
			                     vk::BufferUsageFlagBits::eTransferDst, {0});
			const vk::BufferCopy partial_cpu_refresh_copy {partial_cpu_refresh_source.offset, 0,
			                                               sizeof(partial_cpu_refresh_value)};
			command.Handle().copyBuffer(partial_cpu_refresh_source.buffer->Handle(),
			                            partial_cpu_refresh_readback.buffer, 1,
			                            &partial_cpu_refresh_copy);
			vk::BufferMemoryBarrier partial_cpu_refresh_barrier {};
			partial_cpu_refresh_barrier.sType         = vk::StructureType::eBufferMemoryBarrier;
			partial_cpu_refresh_barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
			partial_cpu_refresh_barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
			partial_cpu_refresh_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			partial_cpu_refresh_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			partial_cpu_refresh_barrier.buffer              = partial_cpu_refresh_readback.buffer;
			partial_cpu_refresh_barrier.size                = partial_cpu_refresh_readback.size;
			command.Handle().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
			                                 vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1,
			                                 &partial_cpu_refresh_barrier, 0, nullptr);

			const uint32_t fault_a = 0x01020304u;
			const uint32_t fault_b = 0x11121314u;
			std::memcpy(memory + 0x8000, &fault_a, sizeof(fault_a));
			std::memcpy(memory + 0x8010, &fault_b, sizeof(fault_b));
			auto fault_a_desc =
			    MakeLinearDesc(base + 0x8000, sizeof(fault_a), vk::Format::eR32Uint,
			                   Prospero::BufferFormat::k32UInt,
			                   Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 4, 1);
			auto fault_b_desc              = fault_a_desc;
			fault_b_desc.info.data.address = base + 0x8010;
			const auto fault_a_image       = texture_cache.FindImage(fault_a_desc);
			const auto fault_b_image       = texture_cache.FindImage(fault_b_desc);
			(void)texture_cache.FindTexture(fault_a_image, fault_a_desc);
			(void)texture_cache.FindTexture(fault_b_image, fault_b_desc);
			texture_cache.MarkGpuWritten(fault_a_image);
			texture_cache.MarkGpuWritten(fault_b_image);
			Require(name, "per-image watcher installation",
			        texture_cache.GetImage(fault_a_image).IsTracked() &&
			            texture_cache.GetImage(fault_b_image).IsTracked(),
			        "same-page images did not install independent write watchers");
			constexpr uint64_t padding_fault_offset = 0x8080;
			uint32_t           write_only_read_a    = 0;
			uint32_t           write_only_read_b    = 0;
			std::memcpy(&write_only_read_a, memory + 0x8000, sizeof(write_only_read_a));
			std::memcpy(&write_only_read_b, memory + 0x8010, sizeof(write_only_read_b));
			Require(name, "write-only image read policy",
			        write_only_read_a == fault_a && write_only_read_b == fault_b &&
			            texture_cache.GetImage(fault_a_image).IsGpuModified() &&
			            texture_cache.GetImage(fault_b_image).IsGpuModified(),
			        "TextureCache claimed a CPU read fault instead of preserving "
			        "GPU image ownership");
			Require(name, "same-page image write invalidation",
			        resources.HandleFault(PageFaultAccess::Write, base + padding_fault_offset) &&
			            texture_cache.GetImage(fault_a_image).IsGpuModified() &&
			            texture_cache.GetImage(fault_b_image).IsGpuModified() &&
			            !texture_cache.GetImage(fault_a_image).IsTracked() &&
			            !texture_cache.GetImage(fault_b_image).IsTracked() &&
			            texture_cache.GetImage(fault_a_image).IsMaybeCpuDirty() &&
			            texture_cache.GetImage(fault_b_image).IsMaybeCpuDirty(),
			        "a byte-disjoint CPU write discarded authoritative images");
			const auto retracked_a    = texture_cache.FindImage(fault_a_desc);
			const auto retracked_b    = texture_cache.FindImage(fault_b_desc);
			(void)texture_cache.FindTexture(retracked_a, fault_a_desc);
			(void)texture_cache.FindTexture(retracked_b, fault_b_desc);
			auto       fault_a_mirror = resources.GetBufferCache().ObtainBuffer(
			    command, base + 0x8000, sizeof(fault_a), false, true, true);
			if (fault_a_mirror.owner != nullptr) {
				command.RetainResourceUntilFence(fault_a_mirror.owner);
			}
			Require(name, "same-page image re-track",
			        retracked_a == fault_a_image && retracked_b == fault_b_image &&
			            texture_cache.GetImage(fault_a_image).IsTracked() &&
			            texture_cache.GetImage(fault_b_image).IsTracked() &&
			            !texture_cache.GetImage(fault_a_image).IsCpuDirty() &&
			            !texture_cache.GetImage(fault_b_image).IsCpuDirty() &&
			            fault_a_mirror.buffer != nullptr &&
			            texture_cache.GetImage(fault_a_image).IsTracked() &&
			            texture_cache.GetImage(fault_b_image).IsTracked() &&
			            texture_cache.GetImage(fault_a_image).IsGpuModified() &&
			            texture_cache.GetImage(fault_b_image).IsGpuModified(),
			        "copying one same-page image lost an authoritative owner");
			Require(name, "same-page survivor write fault",
			        resources.HandleFault(PageFaultAccess::Write, base + 0x8010) &&
			            texture_cache.GetImage(fault_b_image).IsGpuModified() &&
			            texture_cache.GetImage(fault_b_image).IsDefinitelyCpuDirty(),
			        "the surviving image was not protected after its alias retired");
			constexpr uint32_t fault_b_cpu = 0xa5a6a7a8u;
			std::memcpy(memory + 0x8010, &fault_b_cpu, sizeof(fault_b_cpu));
			const auto refreshed_b = texture_cache.FindImage(fault_b_desc);
			(void)texture_cache.FindTexture(refreshed_b, fault_b_desc);
			Require(name, "same-page survivor refresh",
			        refreshed_b == fault_b_image &&
			            !texture_cache.GetImage(fault_b_image).IsDefinitelyCpuDirty() &&
			            texture_cache.GetImage(fault_b_image).IsGpuModified(),
			        "the surviving image could not reconcile its CPU write");

			constexpr uint64_t publish_image_offset  = 0x26000;
			constexpr uint64_t publish_buffer_offset = 0x26010;
			constexpr uint32_t publish_image_value   = 0x31415926u;
			constexpr uint32_t publish_buffer_value  = 0x27182818u;
			std::memcpy(memory + publish_image_offset, &publish_image_value,
			            sizeof(publish_image_value));
			auto publish_image_desc = MakeLinearDesc(
			    base + publish_image_offset, sizeof(publish_image_value), vk::Format::eR32Uint,
			    Prospero::BufferFormat::k32UInt,
			    Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 4, 1);
			const auto publish_image = texture_cache.FindImage(publish_image_desc);
			(void)texture_cache.FindTexture(publish_image, publish_image_desc);
			texture_cache.MarkGpuWritten(publish_image);
			resources.GetBufferCache().FillBuffer(
			    base + publish_buffer_offset, sizeof(publish_buffer_value), publish_buffer_value);
			auto publish_replacement_desc              = publish_image_desc;
			publish_replacement_desc.info.pixel_format = vk::Format::eR32Sfloat;
			publish_replacement_desc.info.guest_format =
			    Prospero::BufferFormat::k32Float;
			publish_replacement_desc.view_info.format = vk::Format::eR32Sfloat;
			const auto publish_replacement =
			    texture_cache.FindImage(publish_replacement_desc, true);
			Require(name, "exact-disjoint image replacement",
			        publish_replacement && publish_replacement != publish_image,
			        "exact-format lookup did not replace the image");
			Require(name, "exact-disjoint image publication",
			        !resources.GetBufferCache().HasGpuDirtyBytes(base + publish_buffer_offset,
			                                                     sizeof(publish_buffer_value)),
			        "clean neighboring buffer unexpectedly became GPU-owned");
			uint32_t published_image_backing  = 0;
			uint32_t published_buffer_backing = 0;
			std::memcpy(&published_image_backing, memory + publish_image_offset,
			            sizeof(published_image_backing));
			std::memcpy(&published_buffer_backing, memory + publish_buffer_offset,
			            sizeof(published_buffer_backing));
			Require(name, "exact-disjoint publication contents",
			        published_image_backing == publish_image_value &&
			            published_buffer_backing == publish_buffer_value,
			        "image publication or neighboring buffer readback changed bytes");

			for (const auto [offset, samples]:
			     std::array<std::pair<uint64_t, uint32_t>, 2> {{{0x10000, 2}, {0x11000, 4}}}) {
				std::memset(memory + offset, 0, samples * sizeof(uint16_t));
				auto standalone_ms =
				    MakeLinearDesc(base + offset, samples * sizeof(uint16_t), vk::Format::eD16Unorm,
				                   Prospero::BufferFormat::k16UNorm,
				                   Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 2, samples);
				standalone_ms.type             = BindingType::DepthTarget;
				standalone_ms.view_info.aspect = vk::ImageAspectFlagBits::eDepth;
				standalone_ms.view_info.usage  = vk::ImageUsageFlagBits::eDepthStencilAttachment;
				const auto image               = texture_cache.FindImage(standalone_ms);
				(void)texture_cache.FindDepthTarget(image, standalone_ms);
				texture_cache.MarkGpuWritten(image);
				uint16_t standalone_cpu_read = 1;
				std::memcpy(&standalone_cpu_read, memory + offset, sizeof(standalone_cpu_read));
				Require(name, "standalone MSAA ownership",
				        texture_cache.GetImage(image).IsGpuModified() && standalone_cpu_read == 0 &&
				            texture_cache.GetImage(image).IsGpuModified() &&
				            resources.HandleFault(PageFaultAccess::Write, base + offset) &&
				            texture_cache.GetImage(image).IsGpuModified() &&
				            texture_cache.GetImage(image).IsDefinitelyCpuDirty(),
				        "fresh multisample target violated write-only ownership");
			}

			constexpr uint64_t metadata_data_a   = 0x12000;
			constexpr uint64_t metadata_data_b   = 0x12100;
			constexpr uint64_t metadata_a        = 0x13000;
			constexpr uint64_t metadata_b        = 0x13100;
			auto               MakeMetadataDepth = [&](uint64_t data, uint64_t metadata) {
				auto desc = MakeLinearDesc(base + data, sizeof(uint32_t), vk::Format::eD32Sfloat,
				                           Prospero::BufferFormat::k32Float,
				                           Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 4, 1);
				desc.type = BindingType::DepthTarget;
				desc.info.metadata.kind  = ImageMetadataKind::Htile;
				desc.info.metadata.range = {base + metadata, 0x80};
				desc.view_info.aspect    = vk::ImageAspectFlagBits::eDepth;
				desc.view_info.usage     = vk::ImageUsageFlagBits::eDepthStencilAttachment;
				return desc;
			};
			auto       metadata_depth_a    = MakeMetadataDepth(metadata_data_a, metadata_a);
			auto       metadata_depth_b    = MakeMetadataDepth(metadata_data_b, metadata_b);
			const auto metadata_depth_a_id = texture_cache.FindImage(metadata_depth_a);
			const auto metadata_depth_b_id = texture_cache.FindImage(metadata_depth_b);
			const auto metadata_depth_a_view =
			    texture_cache.FindDepthTarget(metadata_depth_a_id, metadata_depth_a);
			const auto metadata_depth_b_view =
			    texture_cache.FindDepthTarget(metadata_depth_b_id, metadata_depth_b);
			Require(name, "shared-page metadata state",
			        metadata_depth_a_view != nullptr && metadata_depth_b_view != nullptr &&
			            texture_cache.ClearMeta(base + metadata_a) &&
			            texture_cache.ClearMeta(base + metadata_b),
			        "shared-page metadata did not retain logical clear state");
			auto metadata_alias =
			    MakeLinearDesc(base + metadata_a, sizeof(uint32_t), vk::Format::eR32Sfloat,
			                   Prospero::BufferFormat::k32Float,
			                   Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 4, 1);
			metadata_alias.type            = BindingType::RenderTarget;
			metadata_alias.view_info.usage = vk::ImageUsageFlagBits::eColorAttachment;
			const auto metadata_alias_id   = texture_cache.FindImage(metadata_alias, true);
			const auto metadata_alias_view =
			    texture_cache.FindRenderTarget(metadata_alias_id, metadata_alias);
			Require(name, "exact alias metadata coexistence",
			        metadata_alias_view != nullptr &&
			            texture_cache.GetImage(metadata_depth_a_id).IsGpuModified() &&
			            texture_cache.GetImage(metadata_depth_b_id).IsGpuModified() &&
			            texture_cache.IsMeta(base + metadata_a) &&
			            texture_cache.IsMetaCleared(base + metadata_a, 0) &&
			            texture_cache.IsMeta(base + metadata_b) &&
			            texture_cache.IsMetaCleared(base + metadata_b, 0),
			        "image discovery retired aliased metadata or its live depth image");
			Require(name, "metadata first-touch state",
			        texture_cache.TouchMeta(base + metadata_b, 0, true) &&
			            texture_cache.IsMetaCleared(base + metadata_b, 0) &&
			            texture_cache.TouchMeta(base + metadata_b, 0, false) &&
			            !texture_cache.IsMetaCleared(base + metadata_b, 0),
			        "per-slice metadata update incorrectly required prior GPU ownership");
			resources.GetBufferCache().FillBuffer(base + metadata_b, sizeof(uint32_t), 0);
			Require(name, "metadata buffer fill state",
			        texture_cache.IsMetaCleared(base + metadata_b, 0),
			        "BufferCache fill did not publish an exact-address metadata clear");

			constexpr uint64_t partial_unmap_image_offset = 0x2700000;
			auto               partial_unmap_image =
			    MakeLinearDesc(base + partial_unmap_image_offset, 0x2000, vk::Format::eR32Uint,
			                   Prospero::BufferFormat::k32UInt,
			                   Prospero::ImageType::kColor2D, {2048, 1, 1}, 1, 4, 1);
			const auto partial_unmap_image_id = texture_cache.FindImage(partial_unmap_image);
			texture_cache.UnmapMemory(partial_unmap_image.info.data.address, 0x1000);
			Require(name, "partial image unmap tracking",
			        partial_unmap_image_id &&
			            !texture_cache.FindImageFromRange(partial_unmap_image.info.data.address,
			                                              0x2000, false),
			        "partial unmap left the deleted image's mapped tail tracked");

			constexpr uint64_t unformatted_alias_offset = 0x2500000;
			auto               unformatted_alias        = MakeLinearDesc(
			    base + unformatted_alias_offset, sizeof(uint32_t), vk::Format::eR32Uint,
			    Prospero::BufferFormat::k32UInt,
			    Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 4, 1);
			const auto unformatted_alias_image = texture_cache.FindImage(unformatted_alias);
			(void)texture_cache.FindTexture(unformatted_alias_image, unformatted_alias);
			texture_cache.MarkGpuWritten(unformatted_alias_image);
			auto unformatted_alias_buffer = resources.GetBufferCache().ObtainBuffer(
			    command, unformatted_alias.info.data.address, unformatted_alias.info.data.size,
			    false, true, false);
			Require(name, "unformatted buffer/image alias",
			        unformatted_alias_buffer.buffer != nullptr &&
			            texture_cache.GetImage(unformatted_alias_image).IsGpuModified() &&
			            !texture_cache.GetImage(unformatted_alias_image).IsBufferModified(),
			        "read-only unformatted buffer alias did not follow ordinary "
			        "BufferCache acquisition");
			if (unformatted_alias_buffer.owner != nullptr) {
				command.RetainResourceUntilFence(unformatted_alias_buffer.owner);
			}

			constexpr uint64_t          layered_offset     = 0x14000;
			constexpr uint64_t          layered_guest_size = 0x800;
			const std::array<float, 10> layered_values {0.0f,   0.125f, 0.25f,  0.375f, 0.5f,
			                                            0.625f, 0.75f,  0.875f, 1.0f,   0.0625f};
			std::memset(memory + layered_offset, 0, layered_guest_size);
			const auto layered_layout = TextureCalcUploadLayout(
			    Prospero::BufferFormat::k32Float, 2, 2, 2, 2,
			    Prospero::TileMode::kLinear, layered_guest_size, true,
			    false, "UnifiedTextureCacheFlow");
			const auto layered_upload_regions = TextureBuildImageCopies(layered_layout);
			Require(name, "layered guest layout", layered_upload_regions.size() == 4,
			        "unexpected layered/mipped upload-region count");
			for (const auto& region: layered_upload_regions) {
				for (uint32_t y = 0; y < region.imageExtent.height; y++) {
					for (uint32_t x = 0; x < region.imageExtent.width; x++) {
						const uint32_t logical =
						    region.imageSubresource.mipLevel == 0
						        ? region.imageSubresource.baseArrayLayer * 4 + y * 2 + x
						        : 8 + region.imageSubresource.baseArrayLayer;
						const uint64_t byte_offset =
						    region.bufferOffset +
						    (static_cast<uint64_t>(y) * region.bufferRowLength + x) * sizeof(float);
						Require(name, "layered guest layout bounds",
						        byte_offset + sizeof(float) <= layered_guest_size,
						        "layered guest texel lies outside its backing range");
						std::memcpy(memory + layered_offset + byte_offset, &layered_values[logical],
						            sizeof(float));
					}
				}
			}
			auto layered_color =
			    MakeLinearDesc(base + layered_offset, layered_guest_size, vk::Format::eR32Sfloat,
			                   Prospero::BufferFormat::k32Float,
			                   Prospero::ImageType::kColor2D, {2, 2, 1}, 2, 4, 1);
			layered_color.info.resources.levels = 2;
			layered_color.info.mip_layout[0]    = {0, 32, 2, 2};
			layered_color.info.mip_layout[1]    = {32, 8, 1, 1};
			layered_color.view_info.level_count = 2;
			const auto layered_color_image      = texture_cache.FindImage(layered_color);
			(void)texture_cache.FindTexture(layered_color_image, layered_color);
			texture_cache.MarkGpuWritten(layered_color_image);
			auto layered_depth                  = layered_color;
			layered_depth.type                  = BindingType::DepthTarget;
			layered_depth.info.resources        = {1, 4};
			layered_depth.view_info.level_count = 1;
			layered_depth.info.pixel_format     = vk::Format::eD32Sfloat;
			layered_depth.view_info.format      = vk::Format::eD32Sfloat;
			layered_depth.view_info.aspect      = vk::ImageAspectFlagBits::eDepth;
			layered_depth.view_info.usage       = vk::ImageUsageFlagBits::eDepthStencilAttachment;
			const auto  layered_depth_image     = texture_cache.FindImage(layered_depth);
			const auto& layered_native          = texture_cache.GetImage(layered_depth_image);
			Require(name, "layered mipped depth alias",
			        layered_depth_image != layered_color_image &&
			            layered_native.backing.layers == 2 &&
			            layered_native.backing.mip_levels == 2 &&
			            layered_native.info.resources == layered_color.info.resources &&
			            layered_native.IsGpuModified(),
			        "depth/color conversion did not use the lexicographic "
			        "resource maximum");
			const volatile auto layered_guest_byte =
			    *reinterpret_cast<const volatile uint8_t*>(memory + layered_offset);
			(void)layered_guest_byte;
			Require(name, "layered depth write-only read policy",
			        texture_cache.GetImage(layered_depth_image).IsGpuModified(),
			        "a CPU read invalidated layered depth GPU ownership");

			constexpr uint64_t replacement_offset = 0x18000;
			constexpr uint64_t replacement_size   = 0x2000;
			constexpr uint32_t replacement_pixel  = 0xcafebabeu;
			constexpr uint32_t replacement_tail   = 0x0badf00du;
			std::memset(memory + replacement_offset, 0, replacement_size);
			std::memcpy(memory + replacement_offset, &replacement_pixel, sizeof(replacement_pixel));
			std::memcpy(memory + replacement_offset + replacement_size - sizeof(replacement_tail),
			            &replacement_tail, sizeof(replacement_tail));
			auto replacement_source =
			    MakeLinearDesc(base + replacement_offset, sizeof(uint32_t), vk::Format::eR32Uint,
			                   Prospero::BufferFormat::k32UInt,
			                   Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 4, 1);
			const auto replacement_source_image = texture_cache.FindImage(replacement_source);
			(void)texture_cache.FindTexture(replacement_source_image, replacement_source);
			texture_cache.MarkGpuWritten(replacement_source_image);
			auto replacement_desc =
			    MakeLinearDesc(base + replacement_offset, replacement_size, vk::Format::eR32Uint,
			                   Prospero::BufferFormat::k32UInt,
			                   Prospero::ImageType::kColor2D, {1, 1, 1}, 2, 4, 1);
			const auto replacement            = texture_cache.FindImage(replacement_desc);
			const bool replacement_changed    = replacement != replacement_source_image;
			const bool replacement_gpu_before = texture_cache.GetImage(replacement).IsGpuModified();
			const bool replacement_fault =
			    resources.HandleFault(PageFaultAccess::Write, base + replacement_offset + 0x1000);
			const bool replacement_gpu_after = texture_cache.GetImage(replacement).IsGpuModified();
			uint32_t   replacement_pixel_after = 0;
			uint32_t   replacement_tail_after  = 0;
			std::memcpy(&replacement_pixel_after, memory + replacement_offset,
			            sizeof(replacement_pixel_after));
			std::memcpy(&replacement_tail_after,
			            memory + replacement_offset + replacement_size -
			                sizeof(replacement_tail_after),
			            sizeof(replacement_tail_after));
			Require(name, "replacement tracking",
			        replacement_changed && replacement_gpu_before && replacement_fault &&
			            replacement_gpu_after &&
			            texture_cache.GetImage(replacement).IsDefinitelyCpuDirty() &&
			            replacement_pixel_after == replacement_pixel &&
			            replacement_tail_after == replacement_tail,
			        "replacement write invalidation lost native authority or guest "
			        "backing");

			const uint32_t compressed_value = 0x55667788u;
			std::memcpy(memory + 0xc000, &compressed_value, sizeof(compressed_value));
			auto compressed_desc =
			    MakeLinearDesc(base + 0xc000, sizeof(compressed_value), vk::Format::eR8G8B8A8Srgb,
			                   Prospero::BufferFormat::k8_8_8_8Srgb,
			                   Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 4, 1);
			compressed_desc.type                      = BindingType::RenderTarget;
			compressed_desc.info.metadata.kind        = ImageMetadataKind::Dcc;
			compressed_desc.info.metadata.range       = {base + 0xd000, 0};
			compressed_desc.info.metadata.compression = VideoOutCompression::Dcc256_256_0;
			compressed_desc.view_info.usage           = vk::ImageUsageFlagBits::eColorAttachment;
			const auto compressed_image               = texture_cache.FindImage(compressed_desc);
			(void)texture_cache.FindRenderTarget(compressed_image, compressed_desc);
			uint32_t compressed_cpu_read = 0;
			std::memcpy(&compressed_cpu_read, memory + 0xc000, sizeof(compressed_cpu_read));
			Require(name, "compressed write-only read policy",
			        compressed_cpu_read == compressed_value &&
			            texture_cache.GetImage(compressed_image).IsGpuModified(),
			        "compressed image incorrectly claimed a CPU read fault");
			Require(name, "compressed download rejection",
			        !TextureCacheTestAccess::TryDownload(texture_cache, compressed_image) &&
			            texture_cache.GetImage(compressed_image).IsGpuModified() &&
			            !texture_cache.GetImage(compressed_image).IsBufferModified(),
			        "a compressed image escaped the unified download guard");
			auto compressed_video_desc            = compressed_desc;
			compressed_video_desc.type            = BindingType::VideoOut;
			compressed_video_desc.view_info.usage = vk::ImageUsageFlagBits::eSampled;
			const auto  compressed_video          = texture_cache.FindImage(compressed_video_desc);
			const auto& compressed_video_image    = texture_cache.GetImage(compressed_video);
			Require(name, "cache-only video-out discovery",
			        compressed_video == compressed_image &&
			            !compressed_video_image.binding.is_bound &&
			            !compressed_video_image.binding.is_target &&
			            !compressed_video_image.binding.needs_rebind &&
			            !compressed_video_image.usage.video_out,
			        "FindImage claimed caller-owned video-out usage or binding state");

			constexpr uint64_t mixed_source_offset = 0x20000;
			constexpr uint64_t mixed_source_size   = 0x1004;
			constexpr uint32_t mixed_source_width  = 1025;
			constexpr uint32_t mixed_cpu_value     = 0x1234abcdu;
			constexpr uint32_t mixed_gpu_value     = 0x9876fedcu;
			auto               mixed_owner         = resources.GetBufferCache().ObtainBuffer(
			    command, base + mixed_source_offset, mixed_source_size, true, true);
			Require(name, "mixed-page source allocation", mixed_owner.owner != nullptr,
			        "mixed CPU/GPU image source did not create a containing buffer");
			command.RetainResourceUntilFence(mixed_owner.owner);
			Require(name, "mixed-page CPU write fault",
			        resources.HandleFault(PageFaultAccess::Write, base + mixed_source_offset),
			        "mixed image source could not dirty its first page");
			std::memcpy(memory + mixed_source_offset, &mixed_cpu_value, sizeof(mixed_cpu_value));
			resources.GetBufferCache().FillBuffer(base + mixed_source_offset + 0x1000,
			                                      sizeof(mixed_gpu_value), mixed_gpu_value);
			auto mixed_source_desc =
			    MakeLinearDesc(base + mixed_source_offset, mixed_source_size, vk::Format::eR32Uint,
			                   Prospero::BufferFormat::k32UInt,
			                   Prospero::ImageType::kColor2D, {mixed_source_width, 1, 1}, 1, 4, 1);
			const auto mixed_source_image = texture_cache.FindImage(mixed_source_desc);
			(void)texture_cache.FindTexture(mixed_source_image, mixed_source_desc);
			Require(name, "mixed-page image upload",
			        !texture_cache.GetImage(mixed_source_image).IsGpuModified(),
			        "clean Buffer source manufactured GPU image ownership");
			auto mixed_source_readback =
			    CreateHostBuffer(name, mixed_source_size, vk::BufferUsageFlagBits::eTransferDst,
			                     std::vector<u32>(mixed_source_size / sizeof(u32), 0));
			auto& mixed_source_native = texture_cache.GetImage(mixed_source_image);
			mixed_source_native.Transit(vk::ImageLayout::eTransferSrcOptimal,
			                            vk::AccessFlagBits2::eTransferRead, {}, command.Handle());
			vk::BufferImageCopy mixed_source_copy {};
			mixed_source_copy.bufferRowLength             = mixed_source_width;
			mixed_source_copy.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
			mixed_source_copy.imageSubresource.layerCount = 1;
			mixed_source_copy.imageExtent                 = {mixed_source_width, 1, 1};
			command.Handle().copyImageToBuffer(mixed_source_native.backing.image,
			                                   vk::ImageLayout::eTransferSrcOptimal,
			                                   mixed_source_readback.buffer, 1, &mixed_source_copy);
			HostReadBarrier(mixed_source_readback.buffer, mixed_source_readback.size,
			                vk::PipelineStageFlagBits::eTransfer,
			                vk::AccessFlagBits::eTransferWrite);

			constexpr uint64_t           byte_mirror_page_offset = 0x24000;
			constexpr uint64_t           byte_mirror_offset      = byte_mirror_page_offset + 1;
			const std::array<uint8_t, 4> byte_mirror_guest {0xacu, 0x5au, 0xbdu, 0xceu};
			std::memcpy(memory + byte_mirror_page_offset, byte_mirror_guest.data(),
			            byte_mirror_guest.size());
			auto byte_mirror_desc =
			    MakeLinearDesc(base + byte_mirror_offset, 1, vk::Format::eR8Unorm,
			                   Prospero::BufferFormat::k8UNorm,
			                   Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 1, 1);
			const auto byte_mirror_image = texture_cache.FindImage(byte_mirror_desc);
			(void)texture_cache.FindTexture(byte_mirror_image, byte_mirror_desc);
			texture_cache.MarkGpuWritten(byte_mirror_image);
			auto byte_mirror = resources.GetBufferCache().ObtainBuffer(
			    command, base + byte_mirror_offset, 1, false, true, true);
			Require(name, "byte image mirror",
			        byte_mirror.buffer != nullptr &&
			            !texture_cache.GetImage(byte_mirror_image).IsBufferModified() &&
			            texture_cache.GetImage(byte_mirror_image).IsGpuModified(),
			        "one-byte image copy transferred cache ownership");
			if (byte_mirror.owner != nullptr) {
				command.RetainResourceUntilFence(byte_mirror.owner);
			}

			const std::array<uint16_t, 4> bgra16_guest {0x3c00u, 0x4000u, 0x4200u, 0x4400u};
			std::memcpy(memory + 0xb000, bgra16_guest.data(), sizeof(bgra16_guest));
			auto bgra16_desc =
			    MakeLinearDesc(base + 0xb000, sizeof(bgra16_guest), vk::Format::eR16G16B16A16Sfloat,
			                   Prospero::BufferFormat::k16_16_16_16Float,
			                   Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 8, 1);
			bgra16_desc.type            = BindingType::RenderTarget;
			bgra16_desc.info.bgra16     = true;
			bgra16_desc.view_info.usage = vk::ImageUsageFlagBits::eColorAttachment;
			const auto bgra16_image     = texture_cache.FindImage(bgra16_desc);
			(void)texture_cache.FindRenderTarget(bgra16_image, bgra16_desc);
			auto  bgra16_readback = CreateHostBuffer(name, sizeof(bgra16_guest),
			                                         vk::BufferUsageFlagBits::eTransferDst, {0, 0});
			auto& bgra16_native   = texture_cache.GetImage(bgra16_image);
			bgra16_native.Transit(vk::ImageLayout::eTransferSrcOptimal,
			                      vk::AccessFlagBits2::eTransferRead, {}, command.Handle());
			vk::BufferImageCopy bgra16_copy {};
			bgra16_copy.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
			bgra16_copy.imageSubresource.layerCount = 1;
			bgra16_copy.imageExtent                 = {1, 1, 1};
			command.Handle().copyImageToBuffer(bgra16_native.backing.image,
			                                   vk::ImageLayout::eTransferSrcOptimal,
			                                   bgra16_readback.buffer, 1, &bgra16_copy);
			HostReadBarrier(bgra16_readback.buffer, bgra16_readback.size,
			                vk::PipelineStageFlagBits::eTransfer,
			                vk::AccessFlagBits::eTransferWrite);

			auto layered_readback = CreateHostBuffer(
			    name, sizeof(layered_values), vk::BufferUsageFlagBits::eTransferDst,
			    std::vector<u32>(sizeof(layered_values) / sizeof(u32), 0));
			auto& layered_depth_native = texture_cache.GetImage(layered_depth_image);
			layered_depth_native.Transit(vk::ImageLayout::eTransferSrcOptimal,
			                             vk::AccessFlagBits2::eTransferRead, {}, command.Handle());
			std::array<vk::BufferImageCopy, 2> layered_copies {};
			layered_copies[0].bufferOffset                = 0;
			layered_copies[0].imageSubresource.aspectMask = vk::ImageAspectFlagBits::eDepth;
			layered_copies[0].imageSubresource.mipLevel   = 0;
			layered_copies[0].imageSubresource.layerCount = 2;
			layered_copies[0].imageExtent                 = {2, 2, 1};
			layered_copies[1].bufferOffset                = 32;
			layered_copies[1].imageSubresource.aspectMask = vk::ImageAspectFlagBits::eDepth;
			layered_copies[1].imageSubresource.mipLevel   = 1;
			layered_copies[1].imageSubresource.layerCount = 2;
			layered_copies[1].imageExtent                 = {1, 1, 1};
			command.Handle().copyImageToBuffer(
			    layered_depth_native.backing.image, vk::ImageLayout::eTransferSrcOptimal,
			    layered_readback.buffer, static_cast<uint32_t>(layered_copies.size()),
			    layered_copies.data());
			HostReadBarrier(layered_readback.buffer, layered_readback.size,
			                vk::PipelineStageFlagBits::eTransfer,
			                vk::AccessFlagBits::eTransferWrite);

			vk::ShaderModuleCreateInfo module_info {};
			module_info.sType                = vk::StructureType::eShaderModuleCreateInfo;
			module_info.codeSize             = sizeof(GPU_TEST_MS_DEPTH_SPV);
			module_info.pCode                = GPU_TEST_MS_DEPTH_SPV;
			vk::ShaderModule ms_depth_module = nullptr;
			RequireVk(name, "MS depth content",
			          m_device.createShaderModule(&module_info, nullptr, &ms_depth_module),
			          "vkCreateShaderModule");
			std::array<vk::DescriptorSetLayoutBinding, 2> observer_bindings {};
			observer_bindings[0].binding         = 0;
			observer_bindings[0].descriptorType  = vk::DescriptorType::eSampledImage;
			observer_bindings[0].descriptorCount = 1;
			observer_bindings[0].stageFlags      = vk::ShaderStageFlagBits::eCompute;
			observer_bindings[1].binding         = 1;
			observer_bindings[1].descriptorType  = vk::DescriptorType::eStorageBuffer;
			observer_bindings[1].descriptorCount = 1;
			observer_bindings[1].stageFlags      = vk::ShaderStageFlagBits::eCompute;
			vk::DescriptorSetLayoutCreateInfo observer_layout_info {};
			observer_layout_info.sType        = vk::StructureType::eDescriptorSetLayoutCreateInfo;
			observer_layout_info.bindingCount = static_cast<uint32_t>(observer_bindings.size());
			observer_layout_info.pBindings    = observer_bindings.data();
			vk::DescriptorSetLayout observer_set_layout = nullptr;
			RequireVk(name, "MS depth content",
			          m_device.createDescriptorSetLayout(&observer_layout_info, nullptr,
			                                             &observer_set_layout),
			          "vkCreateDescriptorSetLayout");
			const vk::PushConstantRange  observer_push {vk::ShaderStageFlagBits::eCompute, 0,
			                                            sizeof(uint32_t)};
			vk::PipelineLayoutCreateInfo observer_pipeline_layout_info {};
			observer_pipeline_layout_info.sType = vk::StructureType::ePipelineLayoutCreateInfo;
			observer_pipeline_layout_info.setLayoutCount         = 1;
			observer_pipeline_layout_info.pSetLayouts            = &observer_set_layout;
			observer_pipeline_layout_info.pushConstantRangeCount = 1;
			observer_pipeline_layout_info.pPushConstantRanges    = &observer_push;
			vk::PipelineLayout observer_pipeline_layout          = nullptr;
			RequireVk(name, "MS depth content",
			          m_device.createPipelineLayout(&observer_pipeline_layout_info, nullptr,
			                                        &observer_pipeline_layout),
			          "vkCreatePipelineLayout");
			vk::PipelineShaderStageCreateInfo observer_stage {};
			observer_stage.sType  = vk::StructureType::ePipelineShaderStageCreateInfo;
			observer_stage.stage  = vk::ShaderStageFlagBits::eCompute;
			observer_stage.module = ms_depth_module;
			observer_stage.pName  = "main";
			vk::ComputePipelineCreateInfo observer_pipeline_info {};
			observer_pipeline_info.sType         = vk::StructureType::eComputePipelineCreateInfo;
			observer_pipeline_info.stage         = observer_stage;
			observer_pipeline_info.layout        = observer_pipeline_layout;
			vk::Pipeline observer_depth_pipeline = nullptr;
			RequireVk(name, "MS depth content",
			          m_device.createComputePipelines(nullptr, 1, &observer_pipeline_info, nullptr,
			                                          &observer_depth_pipeline),
			          "vkCreateComputePipelines");
			const std::array<vk::DescriptorPoolSize, 2> observer_pool_sizes {{
			    {vk::DescriptorType::eSampledImage, 2},
			    {vk::DescriptorType::eStorageBuffer, 2},
			}};
			vk::DescriptorPoolCreateInfo                observer_pool_info {};
			observer_pool_info.sType         = vk::StructureType::eDescriptorPoolCreateInfo;
			observer_pool_info.maxSets       = 2;
			observer_pool_info.poolSizeCount = static_cast<uint32_t>(observer_pool_sizes.size());
			observer_pool_info.pPoolSizes    = observer_pool_sizes.data();
			vk::DescriptorPool observer_pool = nullptr;
			RequireVk(name, "MS depth content",
			          m_device.createDescriptorPool(&observer_pool_info, nullptr, &observer_pool),
			          "vkCreateDescriptorPool");
			const std::array<vk::DescriptorSetLayout, 2> observer_layouts {observer_set_layout,
			                                                               observer_set_layout};
			std::array<vk::DescriptorSet, 2>             observer_sets {};
			vk::DescriptorSetAllocateInfo                observer_allocate {};
			observer_allocate.sType              = vk::StructureType::eDescriptorSetAllocateInfo;
			observer_allocate.descriptorPool     = observer_pool;
			observer_allocate.descriptorSetCount = static_cast<uint32_t>(observer_sets.size());
			observer_allocate.pSetLayouts        = observer_layouts.data();
			RequireVk(name, "MS depth content",
			          m_device.allocateDescriptorSets(&observer_allocate, observer_sets.data()),
			          "vkAllocateDescriptorSets");
			std::array<Buffer, 2> ms_observer_outputs {
			    CreateHostBuffer(name, 4 * sizeof(u32), vk::BufferUsageFlagBits::eStorageBuffer,
			                     std::vector<u32>(4, 0)),
			    CreateHostBuffer(name, 4 * sizeof(u32), vk::BufferUsageFlagBits::eStorageBuffer,
			                     std::vector<u32>(4, 0))};
			const std::array<ImageId, 2>  observed_images {ms_depth_image, ms_depth_image_4x};
			const std::array<uint32_t, 2> observed_samples {2, 4};
			for (uint32_t index = 0; index < observed_images.size(); index++) {
				auto& observed = texture_cache.GetImage(observed_images[index]);
				observed.Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
				                 vk::AccessFlagBits2::eShaderRead, {}, command.Handle());
				ImageViewInfo view_info {};
				view_info.format  = observed.backing.format;
				view_info.type    = vk::ImageViewType::e2D;
				view_info.aspect  = vk::ImageAspectFlagBits::eDepth;
				view_info.mapping = {vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eR,
				                     vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eR};
				view_info.usage   = vk::ImageUsageFlagBits::eSampled;
				const auto                     view = observed.FindView(view_info);
				const vk::DescriptorImageInfo  image_info {nullptr, view,
				                                           vk::ImageLayout::eShaderReadOnlyOptimal};
				const vk::DescriptorBufferInfo buffer_info {ms_observer_outputs[index].buffer, 0,
				                                            ms_observer_outputs[index].size};
				std::array<vk::WriteDescriptorSet, 2> writes {};
				writes[0].sType           = vk::StructureType::eWriteDescriptorSet;
				writes[0].dstSet          = observer_sets[index];
				writes[0].dstBinding      = 0;
				writes[0].descriptorCount = 1;
				writes[0].descriptorType  = vk::DescriptorType::eSampledImage;
				writes[0].pImageInfo      = &image_info;
				writes[1].sType           = vk::StructureType::eWriteDescriptorSet;
				writes[1].dstSet          = observer_sets[index];
				writes[1].dstBinding      = 1;
				writes[1].descriptorCount = 1;
				writes[1].descriptorType  = vk::DescriptorType::eStorageBuffer;
				writes[1].pBufferInfo     = &buffer_info;
				m_device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(),
				                              0, nullptr);
				command.Handle().bindPipeline(vk::PipelineBindPoint::eCompute,
				                              observer_depth_pipeline);
				command.Handle().bindDescriptorSets(vk::PipelineBindPoint::eCompute,
				                                    observer_pipeline_layout, 0, 1,
				                                    &observer_sets[index], 0, nullptr);
				command.Handle().pushConstants(
				    observer_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
				    sizeof(observed_samples[index]), &observed_samples[index]);
				command.Handle().dispatch(1, 1, 1);
				HostReadBarrier(ms_observer_outputs[index].buffer, ms_observer_outputs[index].size,
				                vk::PipelineStageFlagBits::eComputeShader,
				                vk::AccessFlagBits::eShaderWrite);
			}

			scheduler.Finish();

			const auto mip_prefix_words =
			    ReadBuffer(name, mip_prefix_readback, mip_prefix_size / sizeof(uint32_t));
			const auto mip_formatted_words =
			    ReadBuffer(name, mip_formatted_readback, mip_guest_size / sizeof(uint32_t));
			const auto mip0_word = mip_sizes[0].offset / sizeof(uint32_t);
			Require(name, "formatted mip prefix content",
			        mip0_word < mip_prefix_words.size() && mip0_word < mip_native.size() &&
			            mip_prefix_words[mip0_word] == mip_native[mip0_word],
			        "fitting mip-prefix synchronization copied "
			        "stale guest backing");
			Require(name, "formatted Buffer content",
			        mip0_word < mip_formatted_words.size() && mip0_word < mip_native.size() &&
			            mip_formatted_words[mip0_word] == mip_native[mip0_word],
			        "formatted Buffer read bypassed authoritative "
			        "native image mip data");
			Require(name, "CPU-dirty formatted mirror content",
			        ReadBuffer(name, mirror_cpu_readback, 1) == std::vector<u32> {mirror_cpu_value},
			        "formatted Buffer mirror published stale "
			        "native image bytes");
			Require(name, "Buffer-superseded exact content",
			        ReadBuffer(name, exact_buffer_readback, 1) ==
			            std::vector<u32> {exact_buffer_value},
			        "exact-format recreation initialized from "
			        "stale guest bytes");
			Require(name, "partial-page CPU refresh content",
			        ReadBuffer(name, partial_cpu_refresh_readback, 1) ==
			            std::vector<u32> {partial_cpu_refresh_value},
			        "cached buffer uploaded bytes outside the exact "
			        "staged guest range");
			const auto mixed_source_words =
			    ReadBuffer(name, mixed_source_readback, mixed_source_size / sizeof(u32));
			Require(name, "mixed-page image content",
			        mixed_source_words.front() == mixed_cpu_value &&
			            mixed_source_words[0x1000 / sizeof(u32)] == mixed_gpu_value,
			        "mixed CPU/GPU source upload lost one "
			        "ownership domain");
			Require(name, "BGRA16 content",
			        ReadBuffer(name, bgra16_readback, 2) ==
			            std::vector<u32> {0x40004200u, 0x44003c00u},
			        "GPU BGRA16 swap did not publish RGBA half-word order");
			scheduler.Finish();
			const auto layered_words = ReadBuffer(name, layered_readback, layered_values.size());
			const std::array<float, 10> layered_expected {
			    layered_values[0], layered_values[1], layered_values[2], layered_values[3],
			    layered_values[4], layered_values[5], layered_values[6], layered_values[7],
			    layered_values[8], layered_values[9]};
			bool layered_content = layered_words.size() == layered_expected.size();
			for (uint32_t index = 0; layered_content && index < layered_expected.size(); index++) {
				layered_content &=
				    layered_words[index] == std::bit_cast<u32>(layered_expected[index]);
			}
			Require(name, "layered mipped depth content", layered_content,
			        "depth/color conversion changed a mip or array-layer value");
			const std::array<std::array<float, 4>, 2> expected_ms_depth {{
			    {0x2000 / 65535.0f, 0xe000 / 65535.0f, 0.0f, 0.0f},
			    {0.0f, 0x4000 / 65535.0f, 0x8000 / 65535.0f, 1.0f},
			}};
			for (uint32_t image = 0; image < expected_ms_depth.size(); image++) {
				const auto words   = ReadBuffer(name, ms_observer_outputs[image], 4);
				bool       content = words.size() == 4;
				for (uint32_t sample = 0; content && sample < observed_samples[image]; sample++) {
					const float actual = std::bit_cast<float>(words[sample]);
					content &=
					    std::abs(actual - expected_ms_depth[image][sample]) <= 1.5f / 65535.0f;
				}
				Require(name, image == 0 ? "two-sample depth content" : "four-sample depth content",
				        content, "color-to-multisample-depth changed a packed sample value");
			}

			auto unrelated_stencil_alias = MakeLinearDesc(
			    base + second_stencil_offset, ms_stencil_size, vk::Format::eR32Uint,
			    Prospero::BufferFormat::k32UInt,
			    Prospero::ImageType::kColor2D,
			    {static_cast<uint32_t>(ms_stencil_size / sizeof(uint32_t)), 1, 1}, 1, 4, 1);
			const auto unrelated_stencil_image = texture_cache.FindImage(unrelated_stencil_alias);
			Require(name, "stencil association filtering",
			        unrelated_stencil_image && unrelated_stencil_image != ms_depth_image &&
			            !texture_cache.GetImage(unrelated_stencil_image).depth_id,
			        "an unrelated color alias was hijacked by the stencil "
			        "association record");

			texture_cache.UnmapMemory(base + second_stencil_offset, ms_stencil_size);
			Require(name, "stencil proxy unmap",
			        !texture_cache.FindImageFromRange(base + second_stencil_offset, ms_stencil_size,
			                                          false) &&
			            texture_cache.GetImage(ms_depth_image).backing.image == ms_depth_backing,
			        "stencil unmap removed the depth backing or retained its proxy");
			const auto reassociated_depth = texture_cache.FindImage(switched_ms_depth);
			TextureCacheTestAccess::AssociateStencil(texture_cache, reassociated_depth,
			                                         switched_ms_depth.info.stencil);
			const auto reassociated_proxy = texture_cache.FindImageFromRange(
			    base + second_stencil_offset, ms_stencil_size, false);
			Require(name, "stencil proxy reassociation",
			        reassociated_depth == ms_depth_image && reassociated_proxy &&
			            texture_cache.GetImage(reassociated_proxy).depth_id == ms_depth_image,
			        "stencil proxy did not re-associate with the surviving depth");
			texture_cache.UnmapMemory(ms_depth_desc.info.data.address,
			                          ms_depth_desc.info.data.size);
			Require(name, "depth unmap proxy retirement",
			        !texture_cache.FindImageFromRange(base + second_stencil_offset, ms_stencil_size,
			                                          false),
			        "unmapping depth retained its re-associated stencil proxy");

			constexpr uint64_t exact_image_offset   = 0x334100;
			constexpr uint64_t dirty_sibling_offset = 0x334200;
			constexpr uint32_t dirty_sibling_value  = 0xc001d00du;
			auto               dirty_sibling        = resources.GetBufferCache().ObtainBuffer(
			    scheduler.Current(), base + dirty_sibling_offset, sizeof(dirty_sibling_value), true,
			    false);
			Require(name, "same-page dirty sibling allocation", dirty_sibling.owner != nullptr,
			        "failed to create the disjoint same-page Buffer owner");
			scheduler.Current().RetainResourceUntilFence(dirty_sibling.owner);
			resources.GetBufferCache().FillBuffer(base + dirty_sibling_offset,
			                                      sizeof(dirty_sibling_value), dirty_sibling_value);
			auto exact_image_desc =
			    MakeLinearDesc(base + exact_image_offset, sizeof(uint32_t), vk::Format::eR32Uint,
			                   Prospero::BufferFormat::k32UInt,
			                   Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 4, 1);
			const auto exact_image = texture_cache.FindImage(exact_image_desc);
			(void)texture_cache.FindTexture(exact_image, exact_image_desc);
			texture_cache.MarkGpuWritten(exact_image);
			Require(name, "same-page exact alias ownership",
			        resources.GetBufferCache().HasGpuDirtyBytes(base + dirty_sibling_offset,
			                                                    sizeof(dirty_sibling_value)) &&
			            !resources.GetBufferCache().HasGpuDirtyBytes(base + exact_image_offset,
			                                                         sizeof(uint32_t)) &&
			            texture_cache.GetImage(exact_image).IsGpuModified(),
			        "image ownership discarded or conflicted with disjoint dirty "
			        "Buffer bytes on the same tracker page");
			resources.GetBufferCache().ReadMemory(base + dirty_sibling_offset,
			                                      sizeof(dirty_sibling_value));

			constexpr std::array<uint64_t, 2> gc_image_offsets {0x330000, 0x332000};
			constexpr std::array<uint32_t, 2> gc_image_values {0x76543210u, 0x89abcdefu};
			constexpr std::array<uint32_t, 2> gc_stale_values {0x10293847u, 0x56473829u};
			for (size_t index = 0; index < gc_image_offsets.size(); index++) {
				std::memcpy(memory + gc_image_offsets[index], &gc_image_values[index],
				            sizeof(uint32_t));
			}
			auto gc_image_desc_a =
			    MakeLinearDesc(base + gc_image_offsets[0], sizeof(uint32_t), vk::Format::eR32Uint,
			                   Prospero::BufferFormat::k32UInt,
			                   Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 4, 1);
			auto gc_image_desc_b              = gc_image_desc_a;
			gc_image_desc_b.info.data.address = base + gc_image_offsets[1];
			auto clean_buffer_alias           = resources.GetBufferCache().ObtainBuffer(
			    scheduler.Current(), gc_image_desc_a.info.data.address,
			    gc_image_desc_a.info.data.size, true, false);
			Require(name, "clean pre-image buffer alias",
			        clean_buffer_alias.owner != nullptr && clean_buffer_alias.buffer != nullptr,
			        "failed to create the clean cached Buffer alias");
			scheduler.Current().RetainResourceUntilFence(clean_buffer_alias.owner);
			resources.GetBufferCache().ReadMemory(gc_image_desc_a.info.data.address,
			                                      gc_image_desc_a.info.data.size);
			const std::array gc_images {texture_cache.FindImage(gc_image_desc_a),
			                            texture_cache.FindImage(gc_image_desc_b)};
			Require(name, "non-GPU image range validity",
			        !texture_cache.FindImageFromRange(gc_image_desc_a.info.data.address,
			                                          gc_image_desc_a.info.data.size),
			        "FindImageFromRange accepted an image without current GPU "
			        "contents");
			(void)texture_cache.FindTexture(gc_images[0], gc_image_desc_a);
			(void)texture_cache.FindTexture(gc_images[1], gc_image_desc_b);
			for (const auto image: gc_images) {
				texture_cache.MarkGpuWritten(image);
			}
			Require(name, "GPU image range validity",
			        texture_cache.FindImageFromRange(gc_image_desc_a.info.data.address,
			                                         gc_image_desc_a.info.data.size) ==
			            gc_images[0],
			        "FindImageFromRange rejected a clean GPU-current image");
			texture_cache.GetImage(gc_images[0])
			    .InvalidateCpuWrite(gc_image_desc_a.info.data.address,
			                        gc_image_desc_a.info.data.size);
			Require(name, "CPU-dirty image range validity",
			        !texture_cache.FindImageFromRange(gc_image_desc_a.info.data.address,
			                                          gc_image_desc_a.info.data.size),
			        "FindImageFromRange accepted CPU-dirty native contents");
			texture_cache.MarkGpuWritten(gc_images[0]);
			for (size_t index = 0; index < gc_image_offsets.size(); index++) {
				Libs::LibKernel::Memory::WriteBacking(base + gc_image_offsets[index],
				                                      &gc_stale_values[index], sizeof(uint32_t));
			}
			TextureCacheTestAccess::ConfigureGarbageCollection(texture_cache, gc_images, 17,
			                                                   UINT64_MAX);
			texture_cache.RunGarbageCollector();
			Require(name, "downloadable image pre-pressure retention",
			        std::ranges::all_of(gc_images,
			                            [&](ImageId image) {
				                            return TextureCacheTestAccess::Contains(texture_cache,
				                                                                    image);
			                            }),
			        "GC retired safely downloadable GPU images before pressure");
			const auto gc_batch_tick = scheduler.CurrentTick();
			TextureCacheTestAccess::ConfigureGarbageCollection(texture_cache, gc_images, 81, 0);
			texture_cache.RunGarbageCollector();
			std::array<uint32_t, 2> gc_before_completion {};
			for (size_t index = 0; index < gc_image_offsets.size(); index++) {
				Libs::LibKernel::Memory::TryReadBacking(
				    base + gc_image_offsets[index], &gc_before_completion[index], sizeof(uint32_t));
			}
			Require(name, "batched image pressure retirement",
			        std::ranges::none_of(gc_images,
			                             [&](ImageId image) {
				                             return TextureCacheTestAccess::Contains(texture_cache,
				                                                                     image);
			                             }) &&
			            scheduler.CurrentTick() == gc_batch_tick &&
			            gc_before_completion == gc_stale_values,
			        "GC submitted per image or published a readback before GPU "
			        "completion");
			scheduler.FinishCurrent();
			scheduler.DrainPriorityOperations();
			auto refreshed_buffer_alias = resources.GetBufferCache().ObtainBuffer(
			    scheduler.Current(), gc_image_desc_a.info.data.address,
			    gc_image_desc_a.info.data.size, false, true);
			Require(name, "post-publication Buffer reacquire",
			        refreshed_buffer_alias.buffer != nullptr,
			        "Buffer lookup failed after the retired image publication");
			if (refreshed_buffer_alias.owner != nullptr) {
				scheduler.Current().RetainResourceUntilFence(refreshed_buffer_alias.owner);
			}
			std::array<uint32_t, 2> gc_after_completion {};
			for (size_t index = 0; index < gc_image_offsets.size(); index++) {
				Libs::LibKernel::Memory::TryReadBacking(
				    base + gc_image_offsets[index], &gc_after_completion[index], sizeof(uint32_t));
			}
			Require(name, "batched image readback publication",
			        scheduler.CurrentTick() == gc_batch_tick + 1 &&
			            gc_after_completion == gc_image_values,
			        "one submission did not publish both deferred image readbacks");
			Require(name, "refreshed post-image buffer alias",
			        refreshed_buffer_alias.buffer != nullptr,
			        "failed to reacquire the cached Buffer alias after image "
			        "publication");
			if (refreshed_buffer_alias.owner != nullptr) {
				scheduler.Current().RetainResourceUntilFence(refreshed_buffer_alias.owner);
			}
			auto alias_readback = CreateHostBuffer(name, sizeof(uint32_t),
			                                       vk::BufferUsageFlagBits::eTransferDst, {0});
			const vk::BufferCopy alias_copy {refreshed_buffer_alias.offset, 0, sizeof(uint32_t)};
			scheduler.Current().Handle().copyBuffer(refreshed_buffer_alias.buffer,
			                                        alias_readback.buffer, 1, &alias_copy);
			HostReadBarrier(alias_readback.buffer, alias_readback.size,
			                vk::PipelineStageFlagBits::eTransfer,
			                vk::AccessFlagBits::eTransferWrite);
			scheduler.FinishCurrent();
			Require(name, "post-image Buffer alias content",
			        ReadBuffer(name, alias_readback, 1) == std::vector<u32> {gc_image_values[0]},
			        "a clean cached Buffer alias survived image ownership with "
			        "stale native bytes");
			DestroyBuffer(&alias_readback);

			constexpr uint64_t submit_readback_offset = 0x336000;
			constexpr uint32_t submit_readback_value  = 0x13579bdfu;
			constexpr uint32_t submit_readback_stale  = 0x2468ace0u;
			auto               submit_readback_desc   = MakeLinearDesc(
			    base + submit_readback_offset, sizeof(uint32_t), vk::Format::eR32Uint,
			    Prospero::BufferFormat::k32UInt,
			    Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 4, 1);
			submit_readback_desc.type            = BindingType::Storage;
			submit_readback_desc.view_info.usage = vk::ImageUsageFlagBits::eStorage;
			TextureCacheTestAccess::SetLinearReadback(texture_cache, true);
			const auto submit_readback_image = texture_cache.FindImage(submit_readback_desc);
			Require(name, "linear submit image clear",
			        texture_cache.ClearImageFromBuffer(
			            command, submit_readback_desc.info.data.address,
			            submit_readback_desc.info.data.size, submit_readback_value),
			        "failed to create GPU-current linear storage contents");
			TextureCacheTestAccess::TrackDownload(texture_cache, submit_readback_image);
			Libs::LibKernel::Memory::WriteBacking(submit_readback_desc.info.data.address,
			                                      &submit_readback_stale,
			                                      sizeof(submit_readback_stale));
			const auto submit_readback_tick = scheduler.CurrentTick();
			texture_cache.ProcessDownloadImages();
			TextureCacheTestAccess::SetLinearReadback(texture_cache, false);
			uint32_t submit_before_completion = 0;
			Libs::LibKernel::Memory::TryReadBacking(submit_readback_desc.info.data.address,
			                                        &submit_before_completion,
			                                        sizeof(submit_before_completion));
			Require(name, "linear submit deferred publication",
			        submit_before_completion == submit_readback_stale &&
			            scheduler.CurrentTick() == submit_readback_tick,
			        "submit-time linear readback published synchronously");
			scheduler.FinishCurrent();
			scheduler.DrainPriorityOperations();
			uint32_t submit_after_completion = 0;
			Libs::LibKernel::Memory::TryReadBacking(submit_readback_desc.info.data.address,
			                                        &submit_after_completion,
			                                        sizeof(submit_after_completion));
			Require(name, "linear submit readback reuse",
			        submit_after_completion == submit_readback_value &&
			            TextureCacheTestAccess::Contains(texture_cache, submit_readback_image) &&
			            texture_cache.GetImage(submit_readback_image).IsGpuModified(),
			        "submit-time readback did not publish or retained no reusable "
			        "GPU-current image");

			constexpr uint64_t linear_depth_offset   = 0x33a000;
			constexpr uint64_t linear_stencil_offset = 0x33b000;
			constexpr uint32_t linear_depth_width    = 3;
			constexpr uint32_t linear_depth_height   = 2;
			constexpr uint32_t linear_depth_pitch    = 4;
			constexpr uint32_t linear_depth_layers   = 2;
			constexpr uint32_t linear_depth_words =
			    linear_depth_pitch * linear_depth_height * linear_depth_layers;
			constexpr uint32_t                       linear_depth_clear = 0x3f400000u;
			std::array<uint32_t, linear_depth_words> linear_depth_guest {};
			for (uint32_t index = 0; index < linear_depth_words; index++) {
				linear_depth_guest[index] = 0x51000000u + index;
			}
			constexpr std::array<uint8_t, 8> linear_stencil_guest {0x91, 0x82, 0x73, 0x64,
			                                                       0x55, 0x46, 0x37, 0x28};
			std::memcpy(memory + linear_depth_offset, linear_depth_guest.data(),
			            sizeof(linear_depth_guest));
			std::memcpy(memory + linear_stencil_offset, linear_stencil_guest.data(),
			            sizeof(linear_stencil_guest));
			auto linear_depth_desc = MakeLinearDesc(
			    base + linear_depth_offset, sizeof(linear_depth_guest),
			    vk::Format::eD32SfloatS8Uint,
			    Prospero::BufferFormat::k32Float,
			    Prospero::ImageType::kColor2D, {linear_depth_width, linear_depth_height, 1},
			    linear_depth_layers, 4, 1);
			linear_depth_desc.type               = BindingType::DepthTarget;
			linear_depth_desc.info.pitch         = linear_depth_pitch;
			linear_depth_desc.info.stencil       = {base + linear_stencil_offset,
			                                        sizeof(linear_stencil_guest)};
			linear_depth_desc.info.mip_layout[0] = {0, sizeof(linear_depth_guest),
			                                        linear_depth_pitch, linear_depth_height};
			linear_depth_desc.view_info.aspect =
			    vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
			linear_depth_desc.view_info.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
			const auto linear_depth_image     = texture_cache.FindImage(linear_depth_desc);
			Require(name, "linear depth native clear",
			        texture_cache.ClearImageFromBuffer(command, linear_depth_desc.info.data.address,
			                                           linear_depth_desc.info.data.size,
			                                           linear_depth_clear),
			        "failed to create GPU-current padded linear depth contents");
			TextureCacheTestAccess::SetLinearReadback(texture_cache, true);
			TextureCacheTestAccess::TrackDownload(texture_cache, linear_depth_image);
			const auto linear_depth_tick = scheduler.CurrentTick();
			texture_cache.ProcessDownloadImages();
			TextureCacheTestAccess::SetLinearReadback(texture_cache, false);
			std::array<uint32_t, linear_depth_words> linear_depth_before {};
			std::memcpy(linear_depth_before.data(), memory + linear_depth_offset,
			            sizeof(linear_depth_before));
			Require(name, "linear depth deferred publication",
			        linear_depth_before == linear_depth_guest &&
			            scheduler.CurrentTick() == linear_depth_tick,
			        "linear depth readback published before GPU completion");
			scheduler.FinishCurrent();
			scheduler.DrainPriorityOperations();
			std::array<uint32_t, linear_depth_words>         linear_depth_after {};
			std::array<uint8_t, linear_stencil_guest.size()> linear_stencil_after {};
			std::memcpy(linear_depth_after.data(), memory + linear_depth_offset,
			            sizeof(linear_depth_after));
			std::memcpy(linear_stencil_after.data(), memory + linear_stencil_offset,
			            sizeof(linear_stencil_after));
			bool     linear_depth_matches  = true;
			uint32_t linear_depth_mismatch = UINT32_MAX;
			for (uint32_t layer = 0; layer < linear_depth_layers; layer++) {
				for (uint32_t y = 0; y < linear_depth_height; y++) {
					for (uint32_t x = 0; x < linear_depth_pitch; x++) {
						const auto index = layer * linear_depth_pitch * linear_depth_height +
						                   y * linear_depth_pitch + x;
						const auto expected =
						    x < linear_depth_width ? linear_depth_clear : linear_depth_guest[index];
						if (linear_depth_after[index] != expected &&
						    linear_depth_mismatch == UINT32_MAX) {
							linear_depth_mismatch = index;
						}
						linear_depth_matches &= linear_depth_after[index] == expected;
					}
				}
			}
			Require(
			    name, "linear depth native contents",
			    linear_depth_matches && linear_stencil_after == linear_stencil_guest &&
			        TextureCacheTestAccess::Contains(texture_cache, linear_depth_image) &&
			        texture_cache.GetImage(linear_depth_image).IsGpuModified(),
			    fmt::format("linear depth mismatch={} actual=0x{:08x} expected=0x{:08x} "
			                "stencil={} contains={} gpu={}",
			                linear_depth_mismatch,
			                linear_depth_mismatch == UINT32_MAX
			                    ? 0
			                    : linear_depth_after[linear_depth_mismatch],
			                linear_depth_mismatch == UINT32_MAX
			                    ? 0
			                    : (linear_depth_mismatch % linear_depth_pitch < linear_depth_width
			                           ? linear_depth_clear
			                           : linear_depth_guest[linear_depth_mismatch]),
			                linear_stencil_after == linear_stencil_guest,
			                TextureCacheTestAccess::Contains(texture_cache, linear_depth_image),
			                texture_cache.GetImage(linear_depth_image).IsGpuModified())
			        .c_str());

			constexpr uint64_t    tiled_depth_offset = 0x380000;
			constexpr uint32_t    tiled_depth_width  = 3;
			constexpr uint32_t    tiled_depth_height = 2;
			constexpr uint32_t    tiled_depth_pitch  = 4;
			constexpr uint32_t    tiled_depth_layers = 2;
			constexpr uint64_t    tiled_depth_slice  = 0x10000;
			constexpr uint64_t    tiled_depth_size   = tiled_depth_slice * tiled_depth_layers;
			constexpr uint32_t    tiled_depth_clear  = 0x3f200000u;
			constexpr uint32_t    tiled_depth_stale  = 0xdeadbeefu;
			std::vector<uint32_t> tiled_depth_guest(tiled_depth_size / sizeof(uint32_t),
			                                        tiled_depth_stale);
			std::memcpy(memory + tiled_depth_offset, tiled_depth_guest.data(), tiled_depth_size);
			auto tiled_depth_desc = MakeLinearDesc(
			    base + tiled_depth_offset, tiled_depth_size, vk::Format::eD32Sfloat,
			    Prospero::BufferFormat::k32Float,
			    Prospero::ImageType::kColor2D, {tiled_depth_width, tiled_depth_height, 1},
			    tiled_depth_layers, 4, 1);
			tiled_depth_desc.type           = BindingType::DepthTarget;
			tiled_depth_desc.info.pitch     = tiled_depth_pitch;
			tiled_depth_desc.info.tile_mode = Prospero::TileMode::kDepth;
			tiled_depth_desc.info.mip_layout[0] = {0, tiled_depth_size, tiled_depth_pitch,
			                                       tiled_depth_height};
			tiled_depth_desc.view_info.aspect   = vk::ImageAspectFlagBits::eDepth;
			tiled_depth_desc.view_info.usage    = vk::ImageUsageFlagBits::eDepthStencilAttachment;
			const auto tiled_depth_image        = texture_cache.FindImage(tiled_depth_desc);
			Require(name, "tiled depth native clear",
			        texture_cache.ClearImageFromBuffer(command, tiled_depth_desc.info.data.address,
			                                           tiled_depth_desc.info.data.size,
			                                           tiled_depth_clear),
			        "failed to create GPU-current layered tiled depth contents");
			const auto [tiled_prefix, tiled_prefix_offset] =
			    TextureCacheTestAccess::MapDownload(texture_cache, 64, 64);
			Require(name, "tiled depth download prefix",
			        tiled_prefix != nullptr && tiled_prefix_offset != 0,
			        "failed to force a nonzero reusable-download offset");
			const auto tiled_depth_tick = scheduler.CurrentTick();
			Require(name, "tiled depth download queue",
			        TextureCacheTestAccess::TryDownload(texture_cache, tiled_depth_image),
			        "layered tiled depth readback was rejected");
			std::vector<uint32_t> tiled_depth_before(tiled_depth_guest.size());
			std::memcpy(tiled_depth_before.data(), memory + tiled_depth_offset, tiled_depth_size);
			Require(name, "tiled depth deferred publication",
			        tiled_depth_before == tiled_depth_guest &&
			            scheduler.CurrentTick() == tiled_depth_tick,
			        "tiled depth readback published before GPU completion");
			scheduler.FinishCurrent();
			scheduler.DrainPriorityOperations();
			std::vector<uint32_t> tiled_depth_after(tiled_depth_guest.size());
			std::memcpy(tiled_depth_after.data(), memory + tiled_depth_offset, tiled_depth_size);
			const auto active_tiled_words =
			    tiled_depth_width * tiled_depth_height * tiled_depth_layers;
			const auto clear_tiled_words = std::ranges::count(tiled_depth_after, tiled_depth_clear);
			const auto stale_tiled_words = std::ranges::count(tiled_depth_after, tiled_depth_stale);
			Require(name, "tiled depth backing preservation",
			        clear_tiled_words == active_tiled_words &&
			            stale_tiled_words == tiled_depth_after.size() - active_tiled_words,
			        fmt::format("tiled depth readback changed clear={}/{} stale={}/{}",
			                    clear_tiled_words, active_tiled_words, stale_tiled_words,
			                    tiled_depth_after.size() - active_tiled_words)
			            .c_str());

			TileBlockLayout depth_block {};
			Require(name, "tiled depth block layout",
			        TileGetBlockLayout(TileBlockFamily::Depth64KB, sizeof(uint32_t), depth_block),
			        "failed to describe the tiled depth test surface");
			std::array<GpuTileInfo, tiled_depth_layers> tiled_depth_tiles {};
			for (uint32_t layer = 0; layer < tiled_depth_layers; layer++) {
				const uint64_t offset              = tiled_depth_slice * layer;
				tiled_depth_tiles[layer]           = {depth_block.family,
				                                      depth_block.bytes_per_element,
				                                      offset,
				                                      tiled_depth_slice,
				                                      offset,
				                                      tiled_depth_slice,
				                                      0,
				                                      tiled_depth_width,
				                                      tiled_depth_height,
				                                      1,
				                                      tiled_depth_pitch};
				tiled_depth_tiles[layer].surface_z = layer;
			}
			auto tiled_depth_input =
			    CreateHostBuffer(name, tiled_depth_size, AllFlags, tiled_depth_after);
			auto tiled_depth_linear = TextureCacheTestAccess::Tiler(texture_cache)
			                              .Detile(tiled_depth_input.buffer, 0, tiled_depth_size,
			                                      tiled_depth_size, tiled_depth_tiles);
			auto tiled_depth_output =
			    CreateHostBuffer(name, tiled_depth_size, AllFlags,
			                     std::vector<uint32_t>(tiled_depth_guest.size(), 0));
			const vk::BufferCopy tiled_depth_copy {tiled_depth_linear.offset, 0, tiled_depth_size};
			scheduler.Current().Handle().copyBuffer(
			    tiled_depth_linear.buffer, tiled_depth_output.buffer, 1, &tiled_depth_copy);
			HostReadBarrier(tiled_depth_output.buffer, tiled_depth_size,
			                vk::PipelineStageFlagBits::eTransfer,
			                vk::AccessFlagBits::eTransferWrite);
			scheduler.FinishCurrent();
			const auto tiled_depth_words =
			    ReadBuffer(name, tiled_depth_output, tiled_depth_guest.size());
			bool tiled_depth_matches = true;
			for (uint32_t layer = 0; layer < tiled_depth_layers; layer++) {
				for (uint32_t y = 0; y < tiled_depth_height; y++) {
					for (uint32_t x = 0; x < tiled_depth_width; x++) {
						const auto index = layer * (tiled_depth_slice / sizeof(uint32_t)) +
						                   y * tiled_depth_pitch + x;
						tiled_depth_matches &= tiled_depth_words[index] == tiled_depth_clear;
					}
				}
			}
			Require(name, "tiled depth layered contents",
			        tiled_depth_matches &&
			            TextureCacheTestAccess::Contains(texture_cache, tiled_depth_image) &&
			            texture_cache.GetImage(tiled_depth_image).IsGpuModified(),
			        "tiled depth readback lost a padded slice or retired its image");
			DestroyBuffer(&tiled_depth_output);
			DestroyBuffer(&tiled_depth_input);

			constexpr uint64_t                tiled_d16_offset         = 0x3a0000;
			constexpr uint64_t                tiled_d16_stencil_offset = 0x3c0000;
			constexpr uint16_t                tiled_d16_clear          = 0x8000u;
			constexpr uint16_t                tiled_d16_stale          = 0xbeefu;
			std::vector<uint16_t>             tiled_d16_guest(tiled_depth_size / sizeof(uint16_t),
			                                                  tiled_d16_stale);
			constexpr std::array<uint8_t, 16> tiled_d16_stencil {0x10, 0x21, 0x32, 0x43, 0x54, 0x65,
			                                                     0x76, 0x87, 0x98, 0xa9, 0xba, 0xcb,
			                                                     0xdc, 0xed, 0xfe, 0x0f};
			std::memcpy(memory + tiled_d16_offset, tiled_d16_guest.data(), tiled_depth_size);
			std::memcpy(memory + tiled_d16_stencil_offset, tiled_d16_stencil.data(),
			            tiled_d16_stencil.size());
			auto tiled_d16_desc = MakeLinearDesc(
			    base + tiled_d16_offset, tiled_depth_size, vk::Format::eD32SfloatS8Uint,
			    Prospero::BufferFormat::k16UNorm,
			    Prospero::ImageType::kColor2D, {tiled_depth_width, tiled_depth_height, 1},
			    tiled_depth_layers, 2, 1);
			tiled_d16_desc.type               = BindingType::DepthTarget;
			tiled_d16_desc.info.pitch         = tiled_depth_pitch;
			tiled_d16_desc.info.tile_mode     = Prospero::TileMode::kDepth;
			tiled_d16_desc.info.stencil       = {base + tiled_d16_stencil_offset,
			                                     tiled_d16_stencil.size()};
			tiled_d16_desc.info.mip_layout[0] = {0, tiled_depth_size, tiled_depth_pitch,
			                                     tiled_depth_height};
			tiled_d16_desc.view_info.aspect =
			    vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
			tiled_d16_desc.view_info.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
			const auto tiled_d16_image     = texture_cache.FindImage(tiled_d16_desc);
			Require(name, "tiled D16 native clear",
			        texture_cache.ClearImageFromBuffer(command, tiled_d16_desc.info.data.address,
			                                           tiled_d16_desc.info.data.size,
			                                           EncodeD16AsD32(tiled_d16_clear)),
			        "failed to clear a layered tiled D16 fallback image");
			Require(name, "tiled D16 download queue",
			        TextureCacheTestAccess::TryDownload(texture_cache, tiled_d16_image),
			        "layered tiled D16 fallback readback was rejected");
			scheduler.FinishCurrent();
			scheduler.DrainPriorityOperations();
			std::vector<uint16_t>                         tiled_d16_after(tiled_d16_guest.size());
			std::array<uint8_t, tiled_d16_stencil.size()> tiled_d16_stencil_after {};
			std::memcpy(tiled_d16_after.data(), memory + tiled_d16_offset, tiled_depth_size);
			std::memcpy(tiled_d16_stencil_after.data(), memory + tiled_d16_stencil_offset,
			            tiled_d16_stencil_after.size());
			Require(name, "tiled D16 backing preservation",
			        std::ranges::count(tiled_d16_after, tiled_d16_clear) == active_tiled_words &&
			            std::ranges::count(tiled_d16_after, tiled_d16_stale) ==
			                tiled_d16_after.size() - active_tiled_words &&
			            tiled_d16_stencil_after == tiled_d16_stencil,
			        "tiled D16 fallback overwrote inactive depth or stencil backing");

			std::vector<uint32_t> tiled_d16_words(tiled_depth_size / sizeof(uint32_t));
			std::memcpy(tiled_d16_words.data(), tiled_d16_after.data(), tiled_depth_size);
			std::array<GpuTileInfo, tiled_depth_layers> tiled_d16_tiles {};
			Require(name, "tiled D16 block layout",
			        TileGetBlockLayout(TileBlockFamily::Depth64KB, sizeof(uint16_t), depth_block),
			        "failed to describe the tiled D16 test surface");
			for (uint32_t layer = 0; layer < tiled_depth_layers; layer++) {
				const uint64_t offset            = tiled_depth_slice * layer;
				tiled_d16_tiles[layer]           = {depth_block.family,
				                                    depth_block.bytes_per_element,
				                                    offset,
				                                    tiled_depth_slice,
				                                    offset,
				                                    tiled_depth_slice,
				                                    0,
				                                    tiled_depth_width,
				                                    tiled_depth_height,
				                                    1,
				                                    tiled_depth_pitch};
				tiled_d16_tiles[layer].surface_z = layer;
			}
			auto tiled_d16_input =
			    CreateHostBuffer(name, tiled_depth_size, AllFlags, tiled_d16_words);
			auto tiled_d16_linear = TextureCacheTestAccess::Tiler(texture_cache)
			                            .Detile(tiled_d16_input.buffer, 0, tiled_depth_size,
			                                    tiled_depth_size, tiled_d16_tiles);
			auto tiled_d16_output = CreateHostBuffer(
			    name, tiled_depth_size, AllFlags, std::vector<uint32_t>(tiled_d16_words.size(), 0));
			const vk::BufferCopy tiled_d16_copy {tiled_d16_linear.offset, 0, tiled_depth_size};
			scheduler.Current().Handle().copyBuffer(tiled_d16_linear.buffer,
			                                        tiled_d16_output.buffer, 1, &tiled_d16_copy);
			HostReadBarrier(tiled_d16_output.buffer, tiled_depth_size,
			                vk::PipelineStageFlagBits::eTransfer,
			                vk::AccessFlagBits::eTransferWrite);
			scheduler.FinishCurrent();
			const auto tiled_d16_linear_words =
			    ReadBuffer(name, tiled_d16_output, tiled_d16_words.size());
			std::vector<uint16_t> tiled_d16_linear_values(tiled_d16_guest.size());
			std::memcpy(tiled_d16_linear_values.data(), tiled_d16_linear_words.data(),
			            tiled_depth_size);
			bool tiled_d16_matches = true;
			for (uint32_t layer = 0; layer < tiled_depth_layers; layer++) {
				for (uint32_t y = 0; y < tiled_depth_height; y++) {
					for (uint32_t x = 0; x < tiled_depth_width; x++) {
						const auto index = layer * (tiled_depth_slice / sizeof(uint16_t)) +
						                   y * tiled_depth_pitch + x;
						tiled_d16_matches &= tiled_d16_linear_values[index] == tiled_d16_clear;
					}
				}
			}
			Require(name, "tiled D16 layered contents",
			        tiled_d16_matches &&
			            TextureCacheTestAccess::Contains(texture_cache, tiled_d16_image) &&
			            texture_cache.GetImage(tiled_d16_image).IsGpuModified(),
			        "tiled D16 fallback lost a converted layer or retired its image");
			DestroyBuffer(&tiled_d16_output);
			DestroyBuffer(&tiled_d16_input);

			constexpr uint64_t                d16_fallback_offset         = 0x33c000;
			constexpr uint64_t                d16_fallback_stencil_offset = 0x33d000;
			constexpr std::array<uint16_t, 4> d16_fallback_values {0x0000u, 0x2468u, 0xabcdu,
			                                                       0xffffu};
			std::memcpy(memory + d16_fallback_offset, d16_fallback_values.data(),
			            sizeof(d16_fallback_values));
			std::memset(memory + d16_fallback_stencil_offset, 0x6d, 4);
			auto d16_depth_desc =
			    MakeLinearDesc(base + d16_fallback_offset, sizeof(d16_fallback_values),
			                   vk::Format::eD24UnormS8Uint,
			                   Prospero::BufferFormat::k16UNorm,
			                   Prospero::ImageType::kColor2D, {4, 1, 1}, 1, 2, 1);
			d16_depth_desc.type         = BindingType::DepthTarget;
			d16_depth_desc.info.stencil = {base + d16_fallback_stencil_offset, 4};
			d16_depth_desc.view_info.aspect =
			    vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
			d16_depth_desc.view_info.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
			const auto d16_depth_image     = texture_cache.FindImage(d16_depth_desc);
			(void)texture_cache.FindDepthTarget(d16_depth_image, d16_depth_desc);
			texture_cache.MarkGpuWritten(d16_depth_image);
			auto d16_storage_desc              = d16_depth_desc;
			d16_storage_desc.type              = BindingType::Storage;
			d16_storage_desc.info.stencil      = {};
			d16_storage_desc.info.pixel_format = vk::Format::eR16Unorm;
			d16_storage_desc.view_info.format  = vk::Format::eR16Unorm;
			d16_storage_desc.view_info.aspect  = vk::ImageAspectFlagBits::eColor;
			d16_storage_desc.view_info.usage   = vk::ImageUsageFlagBits::eStorage;
			const auto d16_storage_image       = texture_cache.FindImage(d16_storage_desc, true);
			auto       d16_storage_readback =
			    CreateHostBuffer(name, sizeof(d16_fallback_values),
			                     vk::BufferUsageFlagBits::eTransferDst, std::vector<u32>(2, 0));
			auto& d16_storage_native = texture_cache.GetImage(d16_storage_image);
			d16_storage_native.Transit(vk::ImageLayout::eTransferSrcOptimal,
			                           vk::AccessFlagBits2::eTransferRead, {},
			                           scheduler.Current().Handle());
			vk::BufferImageCopy d16_storage_copy {};
			d16_storage_copy.bufferRowLength  = 4;
			d16_storage_copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
			d16_storage_copy.imageExtent      = {4, 1, 1};
			scheduler.Current().Handle().copyImageToBuffer(
			    d16_storage_native.backing.image, vk::ImageLayout::eTransferSrcOptimal,
			    d16_storage_readback.buffer, 1, &d16_storage_copy);
			HostReadBarrier(d16_storage_readback.buffer, d16_storage_readback.size,
			                vk::PipelineStageFlagBits::eTransfer,
			                vk::AccessFlagBits::eTransferWrite);
			scheduler.FinishCurrent();
			const auto              d16_storage_words = ReadBuffer(name, d16_storage_readback, 2);
			std::array<uint16_t, 4> d16_storage_values {};
			std::memcpy(d16_storage_values.data(), d16_storage_words.data(),
			            sizeof(d16_storage_values));
			Require(name, "D16 fallback depth/storage copy",
			        d16_storage_values == d16_fallback_values &&
			            d16_storage_image != d16_depth_image &&
			            texture_cache.GetImage(d16_storage_image).IsGpuModified(),
			        "D16 fallback reinterpreted four-byte host depth as two-byte color");
			DestroyBuffer(&d16_storage_readback);

			if (!texture_cache.GetImage(combined_destination_image).IsGpuModified()) {
				texture_cache.MarkGpuWritten(combined_destination_image);
			}
			constexpr float stale_added_stencil_depth = 0.125f;
			Libs::LibKernel::Memory::WriteBacking(combined_destination.info.data.address,
			                                      &stale_added_stencil_depth,
			                                      sizeof(stale_added_stencil_depth));
			const auto depth_gc_tick = scheduler.CurrentTick();
			TextureCacheTestAccess::ConfigureGarbageCollection(
			    texture_cache, std::array {combined_destination_image}, 81, 0);
			texture_cache.RunGarbageCollector();
			float depth_before_completion = 0.0f;
			Libs::LibKernel::Memory::TryReadBacking(combined_destination.info.data.address,
			                                        &depth_before_completion,
			                                        sizeof(depth_before_completion));
			const bool depth_image_retired =
			    !TextureCacheTestAccess::Contains(texture_cache, combined_destination_image);
			const bool depth_proxy_retired = !texture_cache.FindImageFromRange(
			    base + added_stencil_offset, added_stencil_size, false);
			Require(name, "depth/stencil deferred pressure retirement",
			        depth_image_retired && depth_proxy_retired &&
			            scheduler.CurrentTick() == depth_gc_tick &&
			            depth_before_completion == stale_added_stencil_depth,
			        fmt::format("GC failed to retire/defer depth: image={} proxy={} tick={}/{} "
			                    "depth={}/{}",
			                    depth_image_retired, depth_proxy_retired, scheduler.CurrentTick(),
			                    depth_gc_tick, depth_before_completion, stale_added_stencil_depth)
			            .c_str());
			scheduler.FinishCurrent();
			scheduler.DrainPriorityOperations();
			float depth_after_completion = 0.0f;
			Libs::LibKernel::Memory::TryReadBacking(combined_destination.info.data.address,
			                                        &depth_after_completion,
			                                        sizeof(depth_after_completion));
			Require(name, "depth/stencil depth-plane preservation",
			        depth_after_completion == added_stencil_depth_value,
			        "GC discarded the current depth plane of a depth/stencil "
			        "image");

			constexpr size_t                             gc_depth_pair_count  = 6;
			constexpr uint64_t                           gc_depth_pair_offset = 0x350000;
			constexpr uint64_t                           gc_depth_pair_stride = 0x1000;
			std::array<ImageId, gc_depth_pair_count>     gc_depth_images {};
			std::array<ImageId, gc_depth_pair_count>     gc_stencil_images {};
			std::array<ImageId, gc_depth_pair_count * 2> gc_depth_lru {};
			for (size_t index = 0; index < gc_depth_pair_count; index++) {
				auto depth =
				    MakeLinearDesc(base + gc_depth_pair_offset + index * gc_depth_pair_stride,
				                   sizeof(float), vk::Format::eD32SfloatS8Uint,
				                   Prospero::BufferFormat::k32Float,
				                   Prospero::ImageType::kColor2D, {1, 1, 1}, 1, 4, 1);
				depth.type             = BindingType::DepthTarget;
				depth.info.stencil     = {depth.info.data.address + gc_depth_pair_stride / 2,
				                          sizeof(uint32_t)};
				depth.view_info.format = vk::Format::eD32SfloatS8Uint;
				depth.view_info.aspect =
				    vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
				depth.view_info.usage  = vk::ImageUsageFlagBits::eDepthStencilAttachment;
				gc_depth_images[index] = texture_cache.FindImage(depth);
				TextureCacheTestAccess::AssociateStencil(texture_cache, gc_depth_images[index],
				                                         depth.info.stencil);
				gc_stencil_images[index] = texture_cache.FindImageFromRange(
				    depth.info.stencil.address, depth.info.stencil.size, false);
				gc_depth_lru[index * 2]     = gc_depth_images[index];
				gc_depth_lru[index * 2 + 1] = gc_stencil_images[index];
			}
			Require(
			    name, "depth/stencil GC fixtures",
			    std::ranges::all_of(gc_depth_images, [](ImageId image) { return bool {image}; }) &&
			        std::ranges::all_of(gc_stencil_images,
			                            [](ImageId image) { return bool {image}; }),
			    "failed to create six depth/stencil association pairs");
			TextureCacheTestAccess::ConfigureGarbageCollection(texture_cache, gc_depth_lru, 81,
			                                                   UINT64_MAX);
			texture_cache.RunGarbageCollector();
			bool gc_depth_budget = true;
			for (size_t index = 0; index < gc_depth_pair_count; index++) {
				const bool expected_live = index == gc_depth_pair_count - 1;
				gc_depth_budget &=
				    TextureCacheTestAccess::Contains(texture_cache, gc_depth_images[index]) ==
				        expected_live &&
				    TextureCacheTestAccess::Contains(texture_cache, gc_stencil_images[index]) ==
				        expected_live;
			}
			Require(name, "depth/stencil GC traversal budget", gc_depth_budget,
			        "recursive association deletion stopped LRU traversal or "
			        "exceeded the ten-entry deletion budget");

			constexpr uint64_t large_offset = 0x400000;
			constexpr uint32_t large_width  = 4096;
			constexpr uint32_t large_height = 2047;
			constexpr uint64_t large_size =
			    uint64_t {large_width} * large_height * sizeof(uint32_t);
			static_assert(large_size < (32ull << 20));
			std::memset(memory + large_offset, 0, static_cast<size_t>(large_size));
			auto large_desc = MakeLinearDesc(
			    base + large_offset, large_size, vk::Format::eR8G8B8A8Unorm,
			    Prospero::BufferFormat::k8_8_8_8UNorm,
			    Prospero::ImageType::kColor2D, {large_width, large_height, 1}, 1, 4, 1);
			const auto RunLargeReadback = [&](uint32_t clear_value) {
				const auto image = texture_cache.FindImage(large_desc);
				Require(name, "near-capacity image clear",
				        texture_cache.ClearImageFromBuffer(command, large_desc.info.data.address,
				                                           large_desc.info.data.size, clear_value),
				        "failed to write the near-capacity readback source image");
				const std::array<uint64_t, 3> sample_offsets {0, large_size / 2,
				                                              large_size - sizeof(uint32_t)};
				constexpr uint32_t            stale = 0;
				for (const auto offset: sample_offsets) {
					Libs::LibKernel::Memory::WriteBacking(base + large_offset + offset, &stale,
					                                      sizeof(stale));
				}
				TextureCacheTestAccess::ConfigureGarbageCollection(texture_cache,
				                                                   std::array {image}, 81, 0);
				const auto tick = scheduler.CurrentTick();
				texture_cache.RunGarbageCollector();
				const auto handle =
				    BufferCacheTestAccess::DownloadBuffer(resources.GetBufferCache()).Handle();
				Require(name, "near-capacity deferred retirement",
				        !TextureCacheTestAccess::Contains(texture_cache, image) &&
				            scheduler.CurrentTick() == tick,
				        "near-capacity readback was rejected or synchronously submitted");
				scheduler.FinishCurrent();
				scheduler.DrainPriorityOperations();
				bool content = true;
				for (const auto offset: sample_offsets) {
					uint32_t value = 0;
					Libs::LibKernel::Memory::TryReadBacking(base + large_offset + offset, &value,
					                                        sizeof(value));
					content &= value == clear_value;
				}
				Require(name, "near-capacity readback content", content,
				        "near-capacity image readback did not publish its GPU contents");
				return handle;
			};
			const auto large_download        = RunLargeReadback(0xa5a5a5a5u);
			const auto reused_large_download = RunLargeReadback(0x5a5a5a5au);
			Require(name, "near-capacity shared download reuse",
			        large_download == reused_large_download,
			        "successive near-capacity image transfers replaced the shared "
			        "download buffer");

			constexpr uint64_t tile_alias_offset = 0x2000000;
			constexpr uint64_t tile_alias_size   = 0x400000;
			constexpr uint32_t tile_alias_extent = 1024;
			std::memset(memory + tile_alias_offset, 0, static_cast<size_t>(tile_alias_size));
			auto render_target_alias = MakeLinearDesc(
			    base + tile_alias_offset, tile_alias_size, vk::Format::eR8G8B8A8Unorm,
			    Prospero::BufferFormat::k8_8_8_8UNorm,
			    Prospero::ImageType::kColor2D, {tile_alias_extent, tile_alias_extent, 1}, 1, 4, 1);
			render_target_alias.type = BindingType::Storage;
			render_target_alias.info.tile_mode =
			    Prospero::TileMode::kRenderTarget;
			render_target_alias.view_info.usage  = vk::ImageUsageFlagBits::eStorage;
			const auto render_target_alias_image = texture_cache.FindImage(render_target_alias);

			auto standard_4kb_alias = render_target_alias;
			standard_4kb_alias.type = BindingType::Texture;
			standard_4kb_alias.info.tile_mode =
			    Prospero::TileMode::kStandard4KB;
			standard_4kb_alias.view_info.usage     = vk::ImageUsageFlagBits::eSampled;
			const auto standard_4kb_alias_image    = texture_cache.FindImage(standard_4kb_alias);
			auto       repeated_standard_4kb_alias = standard_4kb_alias;
			const auto repeated_standard_4kb_alias_image =
			    texture_cache.FindImage(repeated_standard_4kb_alias);
                        Require(
                            name, "equal-size tile-mode alias",
                            render_target_alias_image &&
                                standard_4kb_alias_image &&
                                standard_4kb_alias_image !=
                                    render_target_alias_image &&
                                repeated_standard_4kb_alias_image ==
                                    standard_4kb_alias_image &&
                                texture_cache
                                        .GetImage(render_target_alias_image)
                                        .info.tile_mode ==
                                    Prospero::TileMode::kRenderTarget &&
                                texture_cache.GetImage(standard_4kb_alias_image)
                                        .info.tile_mode ==
                                    Prospero::TileMode::kStandard4KB,
                            "equal address/size lookup reused an incompatible "
                            "tiled backing");

                        for (auto &output : ms_observer_outputs) {
                          DestroyBuffer(&output);
                        }
                        DestroyBuffer(&layered_readback);
                        DestroyBuffer(&bgra16_readback);
                        DestroyBuffer(&mixed_source_readback);
                        DestroyBuffer(&mip_formatted_readback);
                        DestroyBuffer(&mip_prefix_readback);
                        DestroyBuffer(&partial_cpu_refresh_readback);
                        DestroyBuffer(&mirror_cpu_readback);
                        DestroyBuffer(&exact_buffer_readback);
                        m_device.destroyDescriptorPool(observer_pool, nullptr);
                        m_device.destroyPipeline(observer_depth_pipeline,
                                                 nullptr);
                        m_device.destroyPipelineLayout(observer_pipeline_layout,
                                                       nullptr);
                        m_device.destroyDescriptorSetLayout(observer_set_layout,
                                                            nullptr);
                        m_device.destroyShaderModule(ms_depth_module, nullptr);

                        resources.SetGpu(nullptr);
			resources.UnmapMemory(base, allocation_size);
			scheduler.Finish();
		}
		context.ShutdownGpu();
		Require(name, "unmap direct backing",
		        Libs::LibKernel::Memory::KernelMunmap(base, allocation_size) == 0,
		        "cache direct-memory mapping release failed");
		Require(
		    name, "release direct backing",
		    Libs::LibKernel::Memory::KernelReleaseDirectMemory(direct_offset, allocation_size) == 0,
		    "cache direct-memory allocation release failed");
		std::printf("[host]    %-32s ok\n", name);
	}

	void CheckBgra16Readback() {
		constexpr const char* name            = "Bgra16Readback";
		constexpr uintptr_t   base            = 0x0000000204000000ull;
		constexpr uint64_t    allocation_size = 0x200000;
		constexpr auto format = Prospero::BufferFormat::k16_16_16_16Float;
		constexpr auto tile   = Prospero::TileMode::kRenderTarget;
		EnsureRuntimeContext();

		int64_t direct_offset = -1;
		Require(name, "direct allocation",
		        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
		            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), allocation_size,
		            allocation_size, 0, &direct_offset) == 0,
		        "BGRA16 direct-memory allocation failed");
		void* mapped = reinterpret_cast<void*>(base);
		Require(name, "direct mapping",
		        Libs::LibKernel::Memory::KernelMapDirectMemory(
		            &mapped, allocation_size, 0x3, 0x10, direct_offset, allocation_size) == 0 &&
		            mapped == reinterpret_cast<void*>(base),
		        "BGRA16 fixed mapping failed");

		CommandScheduler scheduler(Renderer(), m_runtime_context);
		HW::Context      registers {};
		HW::UserConfig   user_config {};
		HW::Shader       shaders {};
		scheduler.Begin(registers, user_config, shaders);
		{
			GpuResourceManager resources(m_runtime_context, scheduler);
			resources.MapMemory(base, allocation_size);
			const uint32_t pitch = TileGetTexturePitch(format, 1, tile);
			TileSizeAlign  total {};
			TileSizeOffset mip {};
			TilePaddedSize padded {};
			TileGetTextureSize(format, 1, 1, 1, tile, &total, &mip, &padded);
			Require(name, "tiled layout", total.size >= 8 && total.size <= allocation_size,
			        "BGRA16 tiled layout exceeds its guest allocation");
			std::memset(mapped, 0x5a, total.size);

			ImageDesc desc {};
			desc.type                 = BindingType::RenderTarget;
			desc.info.data            = {base, total.size};
			desc.info.pixel_format    = vk::Format::eR16G16B16A16Sfloat;
			desc.info.guest_format    = format;
			desc.info.type            = Prospero::ImageType::kColor2D;
			desc.info.extent          = {1, 1, 1};
			desc.info.resources       = {1, 1};
			desc.info.pitch           = pitch;
			desc.info.bytes_per_block = 8;
			desc.info.samples         = 1;
			desc.info.tile_mode       = tile;
			desc.info.bgra16          = true;
			desc.info.mip_layout[0]   = {mip.offset, mip.size, pitch, 1};
			desc.view_info.format     = desc.info.pixel_format;
			desc.view_info.type       = vk::ImageViewType::e2D;
			desc.view_info.aspect     = vk::ImageAspectFlagBits::eColor;
			desc.view_info.usage      = vk::ImageUsageFlagBits::eColorAttachment;

			auto&      cache = resources.GetTextureCache();
			const auto id    = cache.FindImage(desc);
			auto&      image = cache.GetImage(id);
			image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
			              {}, scheduler.Current().Handle());
			vk::ClearColorValue clear {};
			clear.float32[0] = 1.0f;
			clear.float32[1] = 2.0f;
			clear.float32[2] = 3.0f;
			clear.float32[3] = 4.0f;
			const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
			scheduler.Current().Handle().clearColorImage(
			    image.backing.image, vk::ImageLayout::eTransferDstOptimal, clear, range);
			cache.MarkGpuWritten(id);
			Require(name, "guest readback queue", TextureCacheTestAccess::TryDownload(cache, id),
			        "tiled BGRA16 guest readback was rejected");
			auto mirror = resources.GetBufferCache().ObtainBuffer(scheduler.Current(), base,
			                                                      total.size, false, true, true);
			Require(name, "mirror owner", mirror.buffer != nullptr && mirror.owner != nullptr,
			        "tiled BGRA16 mirror has no BufferCache owner");
			scheduler.Current().RetainResourceUntilFence(mirror.owner);
			auto mirror_readback =
			    CreateHostBuffer(name, 8, vk::BufferUsageFlagBits::eTransferDst, {0, 0});
			const vk::BufferCopy copy {mirror.offset, 0, 8};
			scheduler.Current().Handle().copyBuffer(mirror.buffer, mirror_readback.buffer, 1,
			                                        &copy);
			vk::BufferMemoryBarrier barrier {};
			barrier.sType               = vk::StructureType::eBufferMemoryBarrier;
			barrier.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
			barrier.dstAccessMask       = vk::AccessFlagBits::eHostRead;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.buffer              = mirror_readback.buffer;
			barrier.size                = mirror_readback.size;
			scheduler.Current().Handle().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
			                                             vk::PipelineStageFlagBits::eHost, {}, 0,
			                                             nullptr, 1, &barrier, 0, nullptr);
			scheduler.Finish();
			scheduler.DrainPriorityOperations();

			std::vector<uint8_t> backing(total.size);
			Require(name, "guest backing",
			        Libs::LibKernel::Memory::TryReadBacking(base, backing.data(), backing.size()),
			        "tiled BGRA16 guest backing is unreadable");
			const std::array<uint16_t, 4> expected {0x4200u, 0x4000u, 0x3c00u, 0x4400u};
			std::array<uint16_t, 4>       observed {};
			std::memcpy(observed.data(), backing.data(), sizeof(observed));
			Require(name, "guest component order",
			        observed == expected &&
			            std::all_of(backing.begin() + sizeof(observed), backing.end(),
			                        [](uint8_t value) { return value == 0x5a; }),
			        "tiled BGRA16 guest readback changed component order or padding");
			Require(name, "mirror component order",
			        ReadBuffer(name, mirror_readback, 2) ==
			            std::vector<u32> {0x40004200u, 0x44003c00u},
			        "tiled BGRA16 Buffer mirror changed guest component order");
			DestroyBuffer(&mirror_readback);
			resources.UnmapMemory(base, allocation_size);
			scheduler.Finish();
		}
		Require(name, "unmap", Libs::LibKernel::Memory::KernelMunmap(base, allocation_size) == 0,
		        "BGRA16 fixed mapping release failed");
		Require(
		    name, "release",
		    Libs::LibKernel::Memory::KernelReleaseDirectMemory(direct_offset, allocation_size) == 0,
		    "BGRA16 direct-memory release failed");
		std::printf("[gpu]     %-32s ok\n", name);
	}

	void CheckRenderExecutorColorVolumeDiscovery() {
		constexpr const char* name                 = "RenderExecutorColorVolumeDiscovery";
		constexpr uintptr_t   base                 = 0x0000000203e00000ull;
		constexpr uint64_t    allocation_size      = 0x200000;
		constexpr uint64_t    allocation_alignment = 0x10000;
		EnsureRuntimeContext();

		int64_t direct_offset = -1;
		Require(name, "direct allocation",
		        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
		            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), allocation_size,
		            allocation_alignment, 0, &direct_offset) == 0,
		        "color-volume direct-memory allocation failed");
		void* mapped = reinterpret_cast<void*>(base);
		Require(name, "direct mapping",
		        Libs::LibKernel::Memory::KernelMapDirectMemory(&mapped, allocation_size, 0x3, 0x10,
		                                                       direct_offset,
		                                                       allocation_alignment) == 0 &&
		            mapped == reinterpret_cast<void*>(base),
		        "color-volume fixed mapping failed");
		std::memset(mapped, 0, allocation_size);
		constexpr uint64_t slice_size = 0x10000;
		std::memset(static_cast<uint8_t*>(mapped) + 31 * slice_size, 0x5a, slice_size);

		{
			RenderContext  context(m_runtime_context);
			auto&          scheduler = context.GetCommandScheduler();
			HW::Context    registers {};
			HW::UserConfig user_config {};
			HW::Shader     shaders {};
			registers.SetColorBase(0, {.addr = base});
			registers.SetColorInfo(
			    0, {.format        = Prospero::ChannelLayout::k10_10_10_2,
			        .channel_type  = Prospero::ChannelType::kUNorm,
			        .channel_order = Prospero::ChannelOrder::kStandard});
			registers.SetColorAttrib2(0, {.height = 31, .width = 31});
			registers.SetColorAttrib3(
			    0, {.depth              = 31,
			        .tile_mode          = Prospero::TileMode::kRenderTarget,
			        .dimension          = 2,
			        .cmask_pipe_aligned = true,
			        .dcc_pipe_aligned   = true});
			registers.SetRenderTargetMask(0x0f);
			scheduler.Begin(registers, user_config, shaders);

			auto& resources     = context.GetGpuResources();
			auto& texture_cache = resources.GetTextureCache();
			auto& executor      = context.GetRenderExecutor();
			resources.MapMemory(base, allocation_size);

			RenderColorInfo color {};
			RenderExecutorTestAccess::ResolveRenderColorTarget(executor, 1, scheduler.Current(),
			                                                   color, 0);
			const auto  attachment = texture_cache.FindRenderTarget(color.image_id, color.desc);
			const auto& image      = texture_cache.GetImage(color.image_id);
			Require(name, "captured 3D target",
			        color.image_id && attachment != nullptr &&
			            color.desc.info.type == Prospero::ImageType::kColor3D &&
			            color.desc.info.extent == vk::Extent3D {32, 32, 32} &&
			            color.desc.info.resources == ImageSubresources {1, 1} &&
			            color.desc.info.pitch == 128 &&
			            color.desc.info.data.size == allocation_size &&
			            color.desc.info.mip_layout[0].size == 0x10000 &&
			            color.desc.view_info.type == vk::ImageViewType::e2D &&
			            color.desc.view_info.layer_count == 1 &&
			            image.backing.image_type == vk::ImageType::e3D &&
			            static_cast<bool>(image.backing.flags &
			                              vk::ImageCreateFlagBits::e2DArrayCompatible) &&
			            image.usage.render_target && image.IsGpuModified(),
			        "dimension=2/depth=31 did not create the SDK-defined 32x32x32 "
			        "backing and 2D "
			        "attachment slice");

			RenderExecutorTestAccess::ResetBindings(executor);
			registers.SetColorView(
			    0, {.base_array_slice_index = 7, .last_array_slice_index = 7});
			RenderColorInfo sliced_color {};
			RenderExecutorTestAccess::ResolveRenderColorTarget(
			    executor, 2, scheduler.Current(), sliced_color, 0);
			RenderDepthInfo no_depth {};
			const auto      sliced_rendering = RenderExecutorTestAccess::AcquireRenderTargets(
			    executor, scheduler.Current(), &sliced_color, 1, no_depth);
			Require(name, "3D slice transition",
			        sliced_color.image_id == color.image_id && sliced_color.image_view != nullptr &&
			            sliced_color.desc.view_info.base_layer == 7 &&
			            sliced_rendering.num_color_attachments == 1 && sliced_rendering.num_layers == 1,
			        "a nonzero 3D attachment slice was treated as a Vulkan array layer");

			auto storage_desc = color.desc;
			storage_desc.type = BindingType::Storage;
			storage_desc.info.guest_format =
			    Prospero::BufferFormat::k10_10_10_2UNorm;
			storage_desc.view_info.type  = vk::ImageViewType::e3D;
			storage_desc.view_info.usage = vk::ImageUsageFlagBits::eStorage;
			const auto  storage_id       = texture_cache.FindImage(storage_desc);
			const auto  storage_view     = texture_cache.FindTexture(storage_id, storage_desc);
			const auto& shared_image     = texture_cache.GetImage(storage_id);
			Require(name, "storage alias reuse",
			        storage_id == color.image_id && storage_view != nullptr &&
			            shared_image.IsGpuModified() && shared_image.usage.render_target,
			        "the matching 3D storage binding did not reuse the live "
			        "render-target image");

			Require(name, "volume readback queue",
			        TextureCacheTestAccess::TryDownload(texture_cache, storage_id),
			        "the 3D render target could not be queued for guest-layout readback");
			auto mirror = resources.GetBufferCache().ObtainBuffer(
			    scheduler.Current(), base, allocation_size, false, true, true);
			Require(name, "volume mirror",
			        mirror.buffer != nullptr && mirror.owner != nullptr,
			        "the 3D render-target readback has no BufferCache owner");
			scheduler.Current().RetainResourceUntilFence(mirror.owner);
			auto slice_probe =
			    CreateHostBuffer(name, 4, vk::BufferUsageFlagBits::eTransferDst, {0});
			const vk::BufferCopy copy {mirror.offset + 31 * slice_size, 0, 4};
			scheduler.Current().Handle().copyBuffer(mirror.buffer, slice_probe.buffer, 1, &copy);
			vk::BufferMemoryBarrier barrier {};
			barrier.sType               = vk::StructureType::eBufferMemoryBarrier;
			barrier.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
			barrier.dstAccessMask       = vk::AccessFlagBits::eHostRead;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.buffer              = slice_probe.buffer;
			barrier.size                = slice_probe.size;
			scheduler.Current().Handle().pipelineBarrier(
			    vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {}, 0, nullptr,
			    1, &barrier, 0, nullptr);
			scheduler.Finish();
			scheduler.DrainPriorityOperations();
			Require(name, "volume Z transfer",
			        ReadBuffer(name, slice_probe, 1) == std::vector<u32> {0x5a5a5a5a},
			        "render-target upload/readback lost the final Z slice");
			DestroyBuffer(&slice_probe);

			RenderExecutorTestAccess::ResetBindings(executor);
			resources.UnmapMemory(base, allocation_size);
			scheduler.Finish();
		}

		Require(name, "unmap direct backing",
		        Libs::LibKernel::Memory::KernelMunmap(base, allocation_size) == 0,
		        "color-volume direct mapping release failed");
		Require(
		    name, "release direct backing",
		    Libs::LibKernel::Memory::KernelReleaseDirectMemory(direct_offset, allocation_size) == 0,
		    "color-volume direct-memory allocation release failed");
		std::printf("[gpu]     %-32s ok\n", name);
	}

	void CheckRenderExecutorStencilBindingDiscovery() {
		constexpr const char* name                 = "RenderExecutorStencilBindingDiscovery";
		constexpr uintptr_t   base                 = 0x0000000203600000ull;
		constexpr uint64_t    allocation_size      = 0x180000;
		constexpr uint64_t    allocation_alignment = 0x10000;
		constexpr uint64_t    depth_address        = base + 0x40000;
		constexpr uint64_t    stencil_address      = base + 0x70000;
		EnsureRuntimeContext();

		int64_t direct_offset = -1;
		Require(name, "direct allocation",
		        Libs::LibKernel::Memory::KernelAllocateDirectMemory(
		            0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), allocation_size,
		            allocation_alignment, 0, &direct_offset) == 0,
		        "descriptor discovery direct-memory allocation failed");
		void* mapped = reinterpret_cast<void*>(base);
		Require(name, "direct mapping",
		        Libs::LibKernel::Memory::KernelMapDirectMemory(&mapped, allocation_size, 0x3, 0x10,
		                                                       direct_offset,
		                                                       allocation_alignment) == 0 &&
		            mapped == reinterpret_cast<void*>(base),
		        "descriptor discovery fixed mapping failed");
		std::memset(mapped, 0, allocation_size);

		{
			RenderContext  context(m_runtime_context);
			auto&          scheduler = context.GetCommandScheduler();
			HW::Context    registers {};
			HW::UserConfig user_config {};
			HW::Shader     shaders {};
			scheduler.Begin(registers, user_config, shaders);
			auto& resources     = context.GetGpuResources();
			auto& texture_cache = resources.GetTextureCache();
			auto& executor      = context.GetRenderExecutor();
			resources.MapMemory(base, allocation_size);

			constexpr auto stencil_format = Prospero::BufferFormat::k8UInt;
			constexpr auto linear         = Prospero::TileMode::kLinear;
			TileSizeAlign  stencil_layout {};
			TileGetTextureTotalSize(stencil_format, 1, 1, 1, 1, linear, false,
			                        stencil_layout);
			Require(name, "stencil footprint",
			        stencil_layout.size != 0 && stencil_layout.align != 0 &&
			            (stencil_address & (stencil_layout.align - 1u)) == 0,
			        "linear stencil descriptor produced an invalid footprint");

			ShaderRecompiler::IR::Program null_program {};
			null_program.stage                      = ShaderType::Compute;
			null_program.resource_tracking_complete = true;
			ShaderRecompiler::IR::ImageResource null_resource {};
			null_resource.kind      = ShaderRecompiler::IR::ResourceKind::ImageUint;
			null_resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
			null_resource.read      = true;
			null_program.info.images.push_back(null_resource);
			auto null_volume      = null_resource;
			null_volume.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim3D;
			null_program.info.images.push_back(null_volume);
			auto null_storage    = null_resource;
			null_storage.kind    = ShaderRecompiler::IR::ResourceKind::StorageImageUint;
			null_storage.read    = false;
			null_storage.written = true;
			null_program.info.images.push_back(null_storage);
			ShaderRecompiler::IR::ResourceSnapshot null_snapshot {};
			ShaderRecompiler::IR::DescriptorValue  null_descriptor {};
			null_descriptor.dword_count = 8;
			null_snapshot.images.assign(3, null_descriptor);
			std::string null_error;
			Require(
			    name, "null specialization",
			    ShaderRecompiler::IR::SpecializeResources(null_program, null_snapshot, &null_error),
			    null_error.c_str());
			ShaderStageRuntime null_runtime {
			    std::make_shared<const ShaderRecompiler::IR::Program>(std::move(null_program)),
			    std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(
			        std::move(null_snapshot))};
			auto null_bindings = executor.PrepareBindings(scheduler.Current(), null_runtime,
			                                              vk::ShaderStageFlagBits::eCompute,
			                                              DescriptorCache::Stage::Compute);
			executor.RebindImages(scheduler.Current(), null_bindings);
			Require(name, "null descriptor count", null_bindings.resources.images.size() == 3,
			        "null descriptor preparation lost an image binding");
			const auto  null_image_id = null_bindings.resources.images[0].image_id;
			const auto& null_image    = texture_cache.GetImage(null_image_id);
			Require(name, "one null image per format",
			        null_bindings.resources.images[1].image_id == null_image_id &&
			            null_bindings.resources.images[2].image_id == null_image_id &&
			            TextureCacheTestAccess::NullImageCount(texture_cache) == 1,
			        "null view dimension or usage created another image allocation");
			Require(name, "null descriptors share final cache acquisition",
			        null_image.info.data.Empty() && !null_image.binding.is_bound &&
			            !null_image.binding.is_target && !null_image.binding.needs_rebind &&
			            null_image.usage.texture && null_image.usage.storage &&
			            null_image.IsGpuModified() &&
			            !TextureCacheTestAccess::PendingDownload(texture_cache, null_image_id),
			        "the shared null image did not preserve texture/storage "
			        "acquisition without guest readback or RenderExecutor ownership");
			RenderExecutorTestAccess::CommitBindings(executor, scheduler.Current(), null_bindings);
			Require(name, "null descriptor general layouts",
			        std::ranges::all_of(null_bindings.resources.images,
			                            [](const auto& binding) {
				                            const auto info =
				                                DescriptorCacheTestAccess::MakeImageInfo(binding);
				                            return binding.layout == vk::ImageLayout::eGeneral &&
				                                   info.imageView == binding.image_view &&
				                                   info.imageLayout == binding.layout;
			                            }) &&
			            null_image.backing.state.layout == vk::ImageLayout::eGeneral,
			        "shared sampled/storage null descriptors did not retain "
			        "the general layout through descriptor generation");

			constexpr uint64_t target_mip_size  = 0x10000;
			const auto         make_target_desc = [&](uint64_t address, uint64_t size,
			                                          vk::Extent3D extent) {
				ImageDesc desc {};
				desc.type                 = BindingType::RenderTarget;
				desc.info.data            = {address, size};
				desc.info.pixel_format    = vk::Format::eR32Uint;
				desc.info.guest_format    = Prospero::BufferFormat::k32UInt;
				desc.info.type            = Prospero::ImageType::kColor2D;
				desc.info.extent          = extent;
				desc.info.resources       = {1, 1};
				desc.info.pitch           = extent.width;
				desc.info.bytes_per_block = 4;
				desc.info.samples         = 1;
				desc.info.tile_mode       = linear;
				desc.info.mip_layout[0]   = {0, size, extent.width, extent.height};
				desc.view_info.format     = desc.info.pixel_format;
				desc.view_info.type       = vk::ImageViewType::e2D;
				desc.view_info.aspect     = vk::ImageAspectFlagBits::eColor;
				desc.view_info.usage      = vk::ImageUsageFlagBits::eColorAttachment;
				return desc;
			};

			TextureCacheTestAccess::SetLinearReadback(texture_cache, true);
			ShaderTextureResource storage {};
			const uint64_t        storage_address      = base + 0x60000;
			constexpr uint32_t    storage_native_value = 0x13579bdfu;
			constexpr uint32_t    storage_stale_value  = 0x2468ace0u;
			std::memcpy(reinterpret_cast<uint8_t*>(mapped) + 0x60000, &storage_native_value,
			            sizeof(storage_native_value));
			const uint64_t encoded_storage_address = storage_address >> 8u;
			storage.fields[0]                      = static_cast<uint32_t>(encoded_storage_address);
			storage.fields[1] =
			    static_cast<uint32_t>(encoded_storage_address >> 32u) |
			    (static_cast<uint32_t>(stencil_format) << 20u);
			storage.fields[3] = DstSel(4, 5, 6, 7) |
			                    (static_cast<uint32_t>(linear) << 20u) |
			                    (static_cast<uint32_t>(Prospero::ImageType::kColor2D) << 28u);
			storage.fields[5] = 0x00700000u;
			ShaderRecompiler::IR::Program storage_program {};
			storage_program.stage                      = ShaderType::Vertex;
			storage_program.resource_tracking_complete = true;
			ShaderRecompiler::IR::ImageResource storage_resource {};
			storage_resource.kind      = ShaderRecompiler::IR::ResourceKind::StorageImageUint;
			storage_resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
			storage_resource.written   = true;
			storage_program.info.images.push_back(storage_resource);
			ShaderRecompiler::IR::DescriptorValue storage_descriptor {};
			std::copy(std::begin(storage.fields), std::end(storage.fields),
			          storage_descriptor.dwords.begin());
			storage_descriptor.dword_count = 8;
			auto           srgb_storage    = storage;
			constexpr auto srgb_format =
			    static_cast<uint32_t>(Prospero::BufferFormat::k8_8_8_8Srgb);
			constexpr uint64_t srgb_storage_address         = base + 0xc0000;
			const auto         encoded_srgb_storage_address = srgb_storage_address >> 8u;
			srgb_storage.fields[0] = static_cast<uint32_t>(encoded_srgb_storage_address);
			srgb_storage.fields[1] =
			    static_cast<uint32_t>(encoded_srgb_storage_address >> 32u) | (srgb_format << 20u);
			ShaderRecompiler::IR::DescriptorValue srgb_storage_descriptor {};
			std::copy(std::begin(srgb_storage.fields), std::end(srgb_storage.fields),
			          srgb_storage_descriptor.dwords.begin());
			srgb_storage_descriptor.dword_count = 8;
			auto srgb_storage_resource          = storage_resource;
			srgb_storage_resource.kind          = ShaderRecompiler::IR::ResourceKind::StorageImage;
			const auto srgb_storage_binding     = RenderExecutorTestAccess::ResolveTexture(
			    executor, srgb_storage_resource, srgb_storage_descriptor);
			const auto srgb_storage_view =
			    texture_cache.FindTexture(srgb_storage_binding.image_id, srgb_storage_binding.desc);
			Require(name, "sRGB storage view",
			        srgb_storage_view != nullptr &&
			            srgb_storage_binding.desc.info.pixel_format == vk::Format::eR8G8B8A8Srgb &&
			            srgb_storage_binding.desc.view_info.format == vk::Format::eR8G8B8A8Unorm &&
			            texture_cache.GetImage(srgb_storage_binding.image_id).backing.format ==
			                vk::Format::eR8G8B8A8Srgb,
			        "storage descriptor did not preserve its sRGB backing and "
			        "select an UNORM Vulkan view");

			ShaderTextureResource sint_storage {{0x01514b00u, 0xc1500000u, 0x000bc00bu, 0x91b00204u,
			                                     0x00000000u, 0x00700000u, 0x102b0000u,
			                                     0x0001514au}};
			Require(name, "PPSA06888 R32 SINT descriptor",
			        sint_storage.Base40() == 0x1514b0000ull && sint_storage.Width5() + 1u == 48 &&
			            sint_storage.Height5() + 1u == 48 && sint_storage.Depth() + 1u == 1 &&
			            sint_storage.Format() ==
			                Prospero::BufferFormat::k32SInt &&
			            sint_storage.Type() ==
			                Prospero::ImageType::kColor2D &&
			            sint_storage.TileMode() ==
			                Prospero::TileMode::kRenderTarget &&
			            sint_storage.DstSelXYZW() == DstSel(4, 0, 0, 1),
			        "captured write-only signed storage descriptor was decoded "
			        "incorrectly");
			const uint64_t mapped_sint_address  = base + 0xe0000;
			const auto     encoded_sint_address = mapped_sint_address >> 8u;
			sint_storage.fields[0]              = static_cast<uint32_t>(encoded_sint_address);
			sint_storage.fields[1] = (sint_storage.fields[1] & ~0xffu) |
			                         static_cast<uint32_t>(encoded_sint_address >> 32u);
			ShaderRecompiler::IR::DescriptorValue sint_storage_descriptor {};
			std::copy(std::begin(sint_storage.fields), std::end(sint_storage.fields),
			          sint_storage_descriptor.dwords.begin());
			sint_storage_descriptor.dword_count = 8;
			auto sint_storage_resource          = srgb_storage_resource;
			sint_storage_resource.kind      = ShaderRecompiler::IR::ResourceKind::StorageImageUint;
			const auto sint_storage_binding = RenderExecutorTestAccess::ResolveTexture(
			    executor, sint_storage_resource, sint_storage_descriptor);
			const auto sint_storage_view =
			    texture_cache.FindTexture(sint_storage_binding.image_id, sint_storage_binding.desc);
			Require(name, "PPSA06888 raw R32 SINT storage view",
			        sint_storage_view != nullptr &&
			            sint_storage_binding.desc.info.data.size == 0x10000 &&
			            sint_storage_binding.desc.info.pixel_format == vk::Format::eR32Sint &&
			            sint_storage_binding.desc.view_info.format == vk::Format::eR32Uint &&
			            texture_cache.GetImage(sint_storage_binding.image_id).backing.format ==
			                vk::Format::eR32Sint,
			        "write-only R32 SINT storage did not select a bit-compatible "
			        "uint view");

			auto               narrowed_storage         = storage;
			constexpr uint64_t narrowed_storage_address = base + 0xd0000;
			const auto         encoded_narrowed_address = narrowed_storage_address >> 8u;
			narrowed_storage.fields[0] = static_cast<uint32_t>(encoded_narrowed_address);
			narrowed_storage.fields[1] =
			    static_cast<uint32_t>(encoded_narrowed_address >> 32u) |
			    (static_cast<uint32_t>(stencil_format) << 20u);
			narrowed_storage.fields[3] =
			    DstSel(4, 5, 6, 7) | (static_cast<uint32_t>(linear) << 20u) |
			    (static_cast<uint32_t>(Prospero::ImageType::kColor1DArray) << 28u);
			narrowed_storage.fields[4] = 1u | (1u << 16u);
			ShaderRecompiler::IR::DescriptorValue narrowed_storage_descriptor {};
			std::copy(std::begin(narrowed_storage.fields), std::end(narrowed_storage.fields),
			          narrowed_storage_descriptor.dwords.begin());
			narrowed_storage_descriptor.dword_count = 8;
			auto narrowed_storage_resource          = storage_resource;
			narrowed_storage_resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim1D;
			const auto narrowed_storage_binding = RenderExecutorTestAccess::ResolveTexture(
			    executor, narrowed_storage_resource, narrowed_storage_descriptor);
			const auto narrowed_storage_view = texture_cache.FindTexture(
			    narrowed_storage_binding.image_id, narrowed_storage_binding.desc);
			const auto& narrowed_storage_image =
			    texture_cache.GetImage(narrowed_storage_binding.image_id);
			Require(name, "narrowed 1D array storage view",
			        narrowed_storage_view != nullptr &&
			            narrowed_storage_binding.desc.info.type == Prospero::ImageType::kColor1D &&
			            narrowed_storage_binding.desc.info.resources.layers == 2 &&
			            narrowed_storage_binding.desc.view_info.type == vk::ImageViewType::e1D &&
			            narrowed_storage_binding.desc.view_info.base_layer == 1 &&
			            narrowed_storage_binding.desc.view_info.layer_count == 1 &&
			            narrowed_storage_image.backing.image_type == vk::ImageType::e1D,
			        "non-array 1D specialization did not select the descriptor "
			        "base layer from its 1D-array backing");

			ShaderRecompiler::IR::ResourceSnapshot storage_snapshot {};
			storage_snapshot.images.push_back(storage_descriptor);
			ShaderStageRuntime storage_runtime {
			    std::make_shared<const ShaderRecompiler::IR::Program>(std::move(storage_program)),
			    std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(
			        std::move(storage_snapshot))};
			auto storage_discovery = executor.PrepareBindings(scheduler.Current(), storage_runtime,
			                                                  vk::ShaderStageFlagBits::eVertex,
			                                                  DescriptorCache::Stage::Vertex);
			const auto storage_id  = storage_discovery.resources.images[0].image_id;
			Require(name, "storage prefetch purity",
			        storage_discovery.resources.images[0].image_view == nullptr &&
			            texture_cache.GetImage(storage_id).binding.is_bound &&
			            !texture_cache.GetImage(storage_id).usage.storage &&
			            !TextureCacheTestAccess::PendingDownload(texture_cache, storage_id),
			        "the first descriptor pass performed final storage acquisition");
			RenderExecutorTestAccess::ResetBindings(executor);

			ShaderRecompiler::IR::Program sampled_program {};
			sampled_program.stage                      = ShaderType::Pixel;
			sampled_program.resource_tracking_complete = true;
			ShaderRecompiler::IR::ImageResource sampled_resource {};
			sampled_resource.kind      = ShaderRecompiler::IR::ResourceKind::ImageUint;
			sampled_resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
			sampled_resource.read      = true;
			sampled_program.info.images.push_back(sampled_resource);
			ShaderRecompiler::IR::ResourceSnapshot sampled_snapshot {};
			sampled_snapshot.images.push_back(storage_descriptor);
			ShaderStageRuntime sampled_runtime {
			    std::make_shared<const ShaderRecompiler::IR::Program>(std::move(sampled_program)),
			    std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(
			        std::move(sampled_snapshot))};

			constexpr uint64_t ordered_sampled_address = base + 0x10000;
			const uint32_t     ordered_sampled_value   = 0x89abcdefu;
			std::memcpy(reinterpret_cast<uint8_t*>(mapped) + 0x10000, &ordered_sampled_value,
			            sizeof(ordered_sampled_value));
			auto       ordered_sampled         = storage;
			const auto encoded_ordered_address = ordered_sampled_address >> 8u;
			ordered_sampled.fields[0]          = static_cast<uint32_t>(encoded_ordered_address);
			ordered_sampled.fields[1] =
			    static_cast<uint32_t>(encoded_ordered_address >> 32u) |
			    (static_cast<uint32_t>(stencil_format) << 20u);
			ShaderRecompiler::IR::DescriptorValue ordered_descriptor {};
			std::copy(std::begin(ordered_sampled.fields), std::end(ordered_sampled.fields),
			          ordered_descriptor.dwords.begin());
			ordered_descriptor.dword_count = 8;
			ShaderRecompiler::IR::ResourceSnapshot ordered_snapshot {};
			ordered_snapshot.images.push_back(ordered_descriptor);
			ShaderStageRuntime ordered_sampled_runtime {
			    sampled_runtime.program,
			    std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(
			        std::move(ordered_snapshot))};
			auto ordered_bindings = RenderExecutorTestAccess::PrepareGraphicsBindings(
			    executor, scheduler.Current(), storage_runtime, ordered_sampled_runtime, true);
			const auto ordered_sampled_id = ordered_bindings.pixel->resources.images[0].image_id;
			Require(name, "VS-before-PS retained-owner order",
			        ordered_bindings.vertex.resources.images[0].image_id == storage_id &&
			            ordered_sampled_id != storage_id &&
			            RenderExecutorTestAccess::BoundImagesInOrder(
			                executor, TextureCacheTestAccess::Owner(texture_cache, storage_id),
			                TextureCacheTestAccess::Owner(texture_cache, ordered_sampled_id)),
			        "production graphics binding did not retain vertex resources "
			        "before pixel resources");
			RenderExecutorTestAccess::ResetBindings(executor);

			auto graphics_bindings = RenderExecutorTestAccess::PrepareGraphicsBindings(
			    executor, scheduler.Current(), storage_runtime, sampled_runtime, true);
			const auto& storage_binding = graphics_bindings.vertex.resources.images[0];
			const auto& sampled_binding = graphics_bindings.pixel->resources.images[0];
			Require(name, "storage final acquisition",
			        storage_binding.image_view != nullptr &&
			            texture_cache.GetImage(storage_id).usage.storage &&
			            texture_cache.GetImage(storage_id).IsGpuModified() &&
			            TextureCacheTestAccess::PendingDownload(texture_cache, storage_id),
			        "the production graphics binding path did not establish GPU "
			        "ownership before storage readback");
			Require(name, "VS-to-PS image acquisition order",
			        storage_binding.image_id == storage_id &&
			            sampled_binding.image_id == storage_id &&
			            texture_cache.GetImage(storage_id).IsGpuModified() &&
			            texture_cache.GetImage(storage_id).usage.storage &&
			            texture_cache.GetImage(storage_id).usage.texture,
			        "the production graphics binding path did not complete vertex "
			        "storage acquisition before pixel sampling");
			RenderExecutorTestAccess::ResetBindings(executor);

			auto vertex_sampled_program =
			    std::make_shared<ShaderRecompiler::IR::Program>(*sampled_runtime.program);
			vertex_sampled_program->stage = ShaderType::Vertex;
			ShaderStageRuntime vertex_sampled_runtime {std::move(vertex_sampled_program),
			                                           sampled_runtime.resources};
			auto               pixel_storage_program =
			    std::make_shared<ShaderRecompiler::IR::Program>(*storage_runtime.program);
			pixel_storage_program->stage = ShaderType::Pixel;
			ShaderStageRuntime pixel_storage_runtime {std::move(pixel_storage_program),
			                                          storage_runtime.resources};
			auto writable_alias_bindings = RenderExecutorTestAccess::PrepareGraphicsBindings(
			    executor, scheduler.Current(), vertex_sampled_runtime, pixel_storage_runtime, true);
			Require(name, "late writable alias promotion",
			        texture_cache.GetImage(storage_id).binding.force_general,
			        "an already sampled image was not promoted when a later "
			        "storage alias bound the same backing");
			RenderExecutorTestAccess::CommitBindings(executor, scheduler.Current(),
			                                         writable_alias_bindings.vertex);
			RenderExecutorTestAccess::CommitBindings(executor, scheduler.Current(),
			                                         *writable_alias_bindings.pixel);
			Require(name, "forced-general descriptor capture",
			        writable_alias_bindings.vertex.resources.images[0].layout ==
			                vk::ImageLayout::eGeneral &&
			            writable_alias_bindings.pixel->resources.images[0].layout ==
			                vk::ImageLayout::eGeneral &&
			            DescriptorCacheTestAccess::MakeImageInfo(
			                writable_alias_bindings.vertex.resources.images[0])
			                    .imageLayout == vk::ImageLayout::eGeneral &&
			            DescriptorCacheTestAccess::MakeImageInfo(
			                writable_alias_bindings.pixel->resources.images[0])
			                    .imageLayout == vk::ImageLayout::eGeneral &&
			            texture_cache.GetImage(storage_id).backing.state.layout ==
			                vk::ImageLayout::eGeneral,
			        "sampled/storage aliases did not retain the promoted general "
			        "layout in both generated descriptors");
			RenderExecutorTestAccess::ResetBindings(executor);

			const std::array<uint32_t, 3> split_mip_data {0x01020304u, 0x05060708u, 0x090a0b0cu};
			std::memcpy(reinterpret_cast<uint8_t*>(mapped) + 0x50000, split_mip_data.data(),
			            sizeof(split_mip_data));
			ImageDesc split_desc {};
			split_desc.type              = BindingType::Texture;
			split_desc.info.data         = {base + 0x50000, sizeof(split_mip_data)};
			split_desc.info.pixel_format = vk::Format::eR32Uint;
			split_desc.info.guest_format = Prospero::BufferFormat::k32UInt;
			split_desc.info.type         = Prospero::ImageType::kColor2D;
			split_desc.info.extent       = {2, 1, 1};
			split_desc.info.resources    = {2, 1};
			split_desc.info.pitch        = 2;
			split_desc.info.bytes_per_block  = 4;
			split_desc.info.samples          = 1;
			split_desc.info.tile_mode        = linear;
			split_desc.info.mip_layout[0]    = {0, 8, 2, 1};
			split_desc.info.mip_layout[1]    = {8, 4, 1, 1};
			split_desc.view_info.format      = split_desc.info.pixel_format;
			split_desc.view_info.type        = vk::ImageViewType::e2D;
			split_desc.view_info.aspect      = vk::ImageAspectFlagBits::eColor;
			split_desc.view_info.level_count = 2;
			split_desc.view_info.layer_count = 1;
			split_desc.view_info.usage       = vk::ImageUsageFlagBits::eSampled;
			const auto split_id              = texture_cache.FindImage(split_desc);

			auto split_storage_desc                  = split_desc;
			split_storage_desc.type                  = BindingType::Storage;
			split_storage_desc.view_info.base_level  = 0;
			split_storage_desc.view_info.level_count = 1;
			split_storage_desc.view_info.usage       = vk::ImageUsageFlagBits::eStorage;
			auto split_sampled_desc                  = split_desc;
			split_sampled_desc.view_info.base_level  = 1;
			split_sampled_desc.view_info.level_count = 1;
			split_sampled_desc.view_info.usage       = vk::ImageUsageFlagBits::eSampled;

			auto split_program   = std::make_shared<ShaderRecompiler::IR::Program>();
			split_program->stage = ShaderType::Vertex;
			split_program->resource_tracking_complete = true;
			split_program->info.images                = {storage_resource, sampled_resource};
			DescriptorCache::PreparedBindings split_bindings {};
			split_bindings.program  = split_program;
			split_bindings.snapshot = std::make_shared<ShaderRecompiler::IR::ResourceSnapshot>();
			split_bindings.shader_stage = vk::ShaderStageFlagBits::eVertex;
			split_bindings.stage        = DescriptorCache::Stage::Vertex;
			split_bindings.resources.images.push_back(
			    {split_id, texture_cache.FindTexture(split_id, split_storage_desc),
			     split_storage_desc});
			split_bindings.resources.images.push_back(
			    {split_id, texture_cache.FindTexture(split_id, split_sampled_desc),
			     split_sampled_desc});
			RenderExecutorTestAccess::CommitBindings(executor, scheduler.Current(), split_bindings);
			const auto& split_image = texture_cache.GetImage(split_id);
			Require(name, "per-binding subresource layouts",
			        split_bindings.resources.images[0].layout == vk::ImageLayout::eGeneral &&
			            split_bindings.resources.images[1].layout ==
			                vk::ImageLayout::eShaderReadOnlyOptimal &&
			            DescriptorCacheTestAccess::MakeImageInfo(split_bindings.resources.images[0])
			                    .imageLayout == vk::ImageLayout::eGeneral &&
			            DescriptorCacheTestAccess::MakeImageInfo(split_bindings.resources.images[1])
			                    .imageLayout == vk::ImageLayout::eShaderReadOnlyOptimal &&
			            split_image.backing.subresource_states.size() == 2 &&
			            split_image.backing.subresource_states[0].layout ==
			                vk::ImageLayout::eGeneral &&
			            split_image.backing.subresource_states[1].layout ==
			                vk::ImageLayout::eShaderReadOnlyOptimal,
			        "descriptor layouts were queried after a later mip transition "
			        "instead of being captured at each binding");

			Libs::LibKernel::Memory::WriteBacking(storage_address, &storage_stale_value,
			                                      sizeof(storage_stale_value));
			texture_cache.ProcessDownloadImages();
			Require(name, "storage acquisition download consumption",
			        !TextureCacheTestAccess::PendingDownload(texture_cache, storage_id),
			        "submit-time processing retained an acquired storage request");
			RenderExecutorTestAccess::ResetBindings(executor);
			scheduler.FinishCurrent();
			scheduler.DrainPriorityOperations();
			uint32_t storage_downloaded_value = 0;
			Libs::LibKernel::Memory::TryReadBacking(storage_address, &storage_downloaded_value,
			                                        sizeof(storage_downloaded_value));
			const uint32_t storage_expected_value =
			    (storage_stale_value & 0xffffff00u) | (storage_native_value & 0xffu);
			Require(name, "storage acquisition readback window",
			        storage_downloaded_value == storage_expected_value &&
			            texture_cache.GetImage(storage_id).IsGpuModified(),
			        "readback enrollment was consumed before final storage "
			        "ownership became visible");

			constexpr uint64_t    phased_depth_address   = base + 0x90000;
			constexpr uint64_t    phased_stencil_address = base + 0xa0000;
			constexpr uint64_t    phased_htile_address   = base + 0xb0000;
			HW::DepthRenderTarget phased_depth_target {};
			phased_depth_target.z_info.format = Prospero::DepthFormat::kZ32F;
			phased_depth_target.z_info.texture_compatibility =
			    Prospero::TextureCompatiblePlaneCompression::kEnable;
			phased_depth_target.z_info.htile_acceleration = true;
			phased_depth_target.z_info.z_compare_base     = Prospero::ZCompareBase::kZMax;
			phased_depth_target.stencil_info.format = Prospero::StencilFormat::k8UInt;
			phased_depth_target.stencil_info.texture_compatibility =
			    Prospero::TextureCompatibleStencil::kEnable;
			phased_depth_target.z_read_base_addr        = phased_depth_address;
			phased_depth_target.z_write_base_addr       = phased_depth_address;
			phased_depth_target.stencil_read_base_addr  = phased_stencil_address;
			phased_depth_target.stencil_write_base_addr = phased_stencil_address;
			phased_depth_target.htile_data_base_addr    = phased_htile_address;
			phased_depth_target.size.valid              = true;
			registers.SetDepthRenderTarget(phased_depth_target);
			HW::DepthControl phased_depth_control {};
			phased_depth_control.z_enable       = true;
			phased_depth_control.z_write_enable = true;
			registers.SetDepthControl(phased_depth_control);
			HW::RenderControl phased_render_control {};
			registers.SetRenderControl(phased_render_control);

			RenderDepthInfo phased_depth {};
			RenderExecutorTestAccess::ResolveRenderDepthTarget(executor, 1, scheduler.Current(),
			                                                   phased_depth);
			auto non_texture_compatible_target = phased_depth_target;
			non_texture_compatible_target.z_info.texture_compatibility =
			    Prospero::TextureCompatiblePlaneCompression::kDisable;
			non_texture_compatible_target.stencil_info.texture_compatibility =
			    Prospero::TextureCompatibleStencil::kDisable;
			registers.SetDepthRenderTarget(non_texture_compatible_target);
			RenderDepthInfo non_texture_compatible_depth {};
			RenderExecutorTestAccess::ResolveRenderDepthTarget(executor, 1, scheduler.Current(),
			                                                   non_texture_compatible_depth);
			Require(name, "depth texture compatibility identity",
			        phased_depth.image_id && phased_depth.htile &&
			            non_texture_compatible_depth.image_id == phased_depth.image_id &&
			            non_texture_compatible_depth.desc.info.data ==
			                phased_depth.desc.info.data &&
			            non_texture_compatible_depth.desc.info.stencil ==
			                phased_depth.desc.info.stencil &&
			            non_texture_compatible_depth.desc.info.metadata.range ==
			                phased_depth.desc.info.metadata.range &&
			            phased_depth.desc.info.metadata.stencil_compressed &&
			            !phased_depth.depth_clear_enable && !phased_depth.depth_meta_clear_enable &&
			            !texture_cache.IsMeta(phased_depth.htile_buffer_vaddr) &&
			            !texture_cache.GetImage(phased_depth.image_id).IsGpuModified() &&
			            !texture_cache.GetImage(phased_depth.image_id).usage.depth_target,
			        "valid PS5 texture-compatibility policy changed logical depth image "
			        "identity or discovery side effects");
			registers.SetDepthRenderTarget(phased_depth_target);
			RenderColorInfo no_color {};
			auto            phased_rendering = RenderExecutorTestAccess::AcquireRenderTargets(
			    executor, scheduler.Current(), &no_color, 0, phased_depth);
			Require(
			    name, "HTile final acquisition",
			    phased_depth.image_view != nullptr && phased_rendering.num_color_attachments == 0 &&
			        phased_rendering.depth_stencil_attachment.image_view ==
			            phased_depth.image_view &&
			        phased_rendering.depth_stencil_attachment.has_depth &&
			        phased_depth.depth_meta_clear_enable && phased_depth.depth_load_clear_enable &&
			        texture_cache.IsMeta(phased_depth.htile_buffer_vaddr) &&
			        !texture_cache.IsMetaCleared(phased_depth.htile_buffer_vaddr,
			                                     phased_depth.desc.view_info.base_layer) &&
			        texture_cache.GetImage(phased_depth.image_id).IsGpuModified() &&
			        texture_cache.GetImage(phased_depth.image_id).usage.depth_target,
			    "final depth acquisition did not publish and consume HTile "
			    "state at the final acquisition boundary");
			RenderExecutorTestAccess::ResetBindings(executor);

			constexpr uint64_t depth_only_address = base + 0x140000;
			HW::DepthRenderTarget depth_only_target {};
			depth_only_target.z_info.format = Prospero::DepthFormat::kZ32F;
			depth_only_target.z_info.z_compare_base = Prospero::ZCompareBase::kZMax;
			depth_only_target.stencil_info.htile_stencil_disabled = true;
			depth_only_target.z_read_base_addr                    = depth_only_address;
			depth_only_target.z_write_base_addr                   = depth_only_address;
			depth_only_target.size                                = {63, 63, true};
			registers.SetDepthRenderTarget(depth_only_target);
			HW::DepthControl depth_only_control {};
			depth_only_control.stencil_enable = true;
			depth_only_control.z_enable       = true;
			depth_only_control.z_write_enable = true;
			depth_only_control.zfunc          = static_cast<uint8_t>(vk::CompareOp::eAlways);
			registers.SetDepthControl(depth_only_control);
			HW::RenderControl depth_only_render_control {};
			depth_only_render_control.depth_clear_enable   = true;
			depth_only_render_control.stencil_clear_enable = true;
			registers.SetRenderControl(depth_only_render_control);
			RenderDepthInfo depth_only {};
			RenderExecutorTestAccess::ResolveRenderDepthTarget(executor, 1, scheduler.Current(),
			                                                   depth_only);
			Require(
			    name, "depth-only target with stale stencil state",
			    depth_only.image_id && depth_only.format == vk::Format::eD32Sfloat &&
			        depth_only.depth_test_enable && depth_only.depth_write_enable &&
			        depth_only.depth_clear_enable && !depth_only.stencil_test_enable &&
			        !depth_only.stencil_clear_enable && depth_only.stencil_buffer_vaddr == 0 &&
			        depth_only.stencil_buffer_size == 0 && depth_only.desc.info.stencil.Empty() &&
			        depth_only.depth_buffer_size != 0 &&
			        depth_only_address + depth_only.depth_buffer_size <= base + allocation_size &&
			        depth_only.vaddr_num == 1 &&
			        depth_only.AttachmentWriteAspects() == vk::ImageAspectFlagBits::eDepth,
			    "raw stencil test or clear state leaked into a depth-only attachment");
			RenderExecutorTestAccess::ResetBindings(executor);

			auto video_subresource = make_target_desc(base + 0x20000, target_mip_size, {1, 1, 1});
			video_subresource.type = BindingType::VideoOut;
			video_subresource.info.pixel_format = vk::Format::eR8G8B8A8Srgb;
			video_subresource.info.guest_format =
			    Prospero::BufferFormat::k8_8_8_8Srgb;
			video_subresource.view_info.format = video_subresource.info.pixel_format;
			video_subresource.view_info.usage  = vk::ImageUsageFlagBits::eSampled;
			const auto video_subresource_id    = texture_cache.FindImage(video_subresource);
			const auto video_subresource_owner =
			    TextureCacheTestAccess::Owner(texture_cache, video_subresource_id);
			auto video_parent = make_target_desc(base + 0x20000, target_mip_size * 2, {2, 2, 1});
			video_parent.type = BindingType::VideoOut;
			video_parent.info.pixel_format = vk::Format::eR8G8B8A8Srgb;
			video_parent.info.guest_format =
			    Prospero::BufferFormat::k8_8_8_8Srgb;
			video_parent.info.resources.levels = 2;
			video_parent.info.mip_layout[0]    = {0, target_mip_size, 2, 2};
			video_parent.info.mip_layout[1]    = {target_mip_size, target_mip_size, 1, 1};
			video_parent.view_info.format      = video_parent.info.pixel_format;
			video_parent.view_info.usage       = vk::ImageUsageFlagBits::eSampled;
			const auto video_parent_id         = texture_cache.FindImage(video_parent);
			const auto video_parent_owner =
			    TextureCacheTestAccess::Owner(texture_cache, video_parent_id);
			Require(name, "video-out replacement remains cache-only",
			        video_subresource_owner != nullptr && video_parent_owner != nullptr &&
			            video_parent_id != video_subresource_id &&
			            !video_subresource_owner->binding.is_bound &&
			            !video_subresource_owner->binding.is_target &&
			            !video_subresource_owner->binding.needs_rebind &&
			            !video_subresource_owner->usage.video_out &&
			            !video_parent_owner->binding.is_bound &&
			            !video_parent_owner->binding.is_target &&
			            !video_parent_owner->binding.needs_rebind &&
			            !video_parent_owner->usage.video_out,
			        "FindImage leaked caller-owned state across a video-out "
			        "replacement");

			constexpr uint64_t array_target_address = base + 0xd0000;
			auto               array_target =
			    make_target_desc(array_target_address, target_mip_size, {128, 128, 1});
			const auto array_target_id = texture_cache.FindImage(array_target);
			const auto array_target_owner =
			    TextureCacheTestAccess::Owner(texture_cache, array_target_id);
			RenderExecutorTestAccess::BindRenderTarget(executor, array_target_id);
			const auto array_target_view =
			    texture_cache.FindRenderTarget(array_target_id, array_target);

			ShaderTextureResource array_texture {};
			const auto            encoded_array_address = array_target_address >> 8u;
			constexpr auto array_format = static_cast<uint32_t>(Prospero::BufferFormat::k32UInt);
			array_texture.fields[0]     = static_cast<uint32_t>(encoded_array_address);
			array_texture.fields[1]     = static_cast<uint32_t>(encoded_array_address >> 32u) |
			                              (array_format << 20u) | (3u << 30u);
			array_texture.fields[2]     = (127u >> 2u) | (127u << 14u);
			array_texture.fields[3] =
			    DstSel(4, 5, 6, 7) | (static_cast<uint32_t>(linear) << 20u) |
			    (static_cast<uint32_t>(Prospero::ImageType::kColor2DArray) << 28u);
			array_texture.fields[4] = 1;

			ShaderRecompiler::IR::Program array_program {};
			array_program.stage                      = ShaderType::Compute;
			array_program.resource_tracking_complete = true;
			ShaderRecompiler::IR::ImageResource array_resource {};
			array_resource.kind      = ShaderRecompiler::IR::ResourceKind::ImageUint;
			array_resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2DArray;
			array_resource.read      = true;
			array_program.info.images.push_back(array_resource);
			ShaderRecompiler::IR::DescriptorValue array_descriptor {};
			std::copy(std::begin(array_texture.fields), std::end(array_texture.fields),
			          array_descriptor.dwords.begin());
			array_descriptor.dword_count = 8;
			ShaderRecompiler::IR::ResourceSnapshot array_snapshot {};
			array_snapshot.images.push_back(array_descriptor);
			ShaderStageRuntime array_runtime {
			    std::make_shared<const ShaderRecompiler::IR::Program>(std::move(array_program)),
			    std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(
			        std::move(array_snapshot))};

			auto array_binding = executor.PrepareBindings(scheduler.Current(), array_runtime,
			                                              vk::ShaderStageFlagBits::eCompute,
			                                              DescriptorCache::Stage::Compute);
			executor.RebindImages(scheduler.Current(), array_binding);
			const auto  expanded_array_id = array_binding.resources.images[0].image_id;
			const auto& expanded_array    = texture_cache.GetImage(expanded_array_id);
			Require(name, "2D target to array backing expansion",
			        array_target_view != nullptr && array_target_owner != nullptr &&
			            array_target_owner->binding.needs_rebind &&
			            expanded_array_id != array_target_id &&
			            array_binding.resources.images[0].image_view != nullptr &&
			            array_binding.resources.images[0].desc.info.type ==
			                Prospero::ImageType::kColor2D &&
			            array_binding.resources.images[0].desc.info.resources ==
			                ImageSubresources {1, 2} &&
			            array_binding.resources.images[0].desc.view_info.type ==
			                vk::ImageViewType::e2DArray &&
			            array_binding.resources.images[0].desc.view_info.layer_count == 2 &&
			            expanded_array.backing.layers == 2 && expanded_array.IsGpuModified() &&
			            expanded_array.usage.texture,
			        "raw array view did not expand and reuse the Color2D backing");

			RenderColorInfo rebound_array_target {};
			rebound_array_target.type     = RenderColorType::RenderTexture;
			rebound_array_target.desc     = array_target;
			rebound_array_target.image_id = array_target_id;
			rebound_array_target.format   = array_target.view_info.format;
			rebound_array_target.extent   = {128, 128};
			rebound_array_target.samples  = 1;
			RenderDepthInfo no_array_depth {};
			auto            array_rendering = RenderExecutorTestAccess::AcquireRenderTargets(
			    executor, scheduler.Current(), &rebound_array_target, 1, no_array_depth);
			Require(name, "expanded array target rebind",
			        rebound_array_target.image_id == expanded_array_id &&
			            rebound_array_target.image_view != nullptr &&
			            array_rendering.num_color_attachments == 1 &&
			            array_rendering.color_attachments[0].image_view ==
			                rebound_array_target.image_view &&
			            array_rendering.width == 128 && array_rendering.height == 128 &&
			            array_rendering.num_layers == 1 &&
			            texture_cache.GetImage(expanded_array_id).binding.is_bound &&
			            texture_cache.GetImage(expanded_array_id).binding.is_target,
			        "the retained single-layer target did not rebind to the expanded "
			        "backing");
			RenderExecutorTestAccess::ResetBindings(executor);

			auto colliding_msaa_texture = array_texture;
			colliding_msaa_texture.fields[3] =
			    DstSel(4, 5, 6, 7) | (1u << 16u) |
			    (static_cast<uint32_t>(Prospero::TileMode::kRenderTarget) << 20u) |
			    (static_cast<uint32_t>(Prospero::ImageType::kColor2DMsaa) << 28u);
			colliding_msaa_texture.fields[4] = 0;
			colliding_msaa_texture.fields[5] = (7u << 20u) | (1u << 4u);
			ShaderRecompiler::IR::DescriptorValue colliding_msaa_descriptor {};
			std::copy(std::begin(colliding_msaa_texture.fields),
			          std::end(colliding_msaa_texture.fields),
			          colliding_msaa_descriptor.dwords.begin());
			colliding_msaa_descriptor.dword_count = 8;
			auto colliding_msaa_program =
			    std::make_shared<ShaderRecompiler::IR::Program>(*array_runtime.program);
			colliding_msaa_program->info.images[0].dimension =
			    ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaa;
			auto colliding_msaa_snapshot =
			    std::make_shared<ShaderRecompiler::IR::ResourceSnapshot>();
			colliding_msaa_snapshot->images.push_back(colliding_msaa_descriptor);
			ShaderStageRuntime colliding_msaa_runtime {std::move(colliding_msaa_program),
			                                           std::move(colliding_msaa_snapshot)};
			auto               colliding_msaa_binding = executor.PrepareBindings(
			    scheduler.Current(), colliding_msaa_runtime, vk::ShaderStageFlagBits::eCompute,
			    DescriptorCache::Stage::Compute);
			executor.RebindImages(scheduler.Current(), colliding_msaa_binding);
			const auto& resolved_colliding_msaa = colliding_msaa_binding.resources.images[0];
			Require(name, "equal-footprint sample-count identity",
			        resolved_colliding_msaa.image_id != expanded_array_id &&
			            resolved_colliding_msaa.image_view != nullptr &&
			            resolved_colliding_msaa.desc.info.type == Prospero::ImageType::kColor2D &&
			            resolved_colliding_msaa.desc.info.resources == ImageSubresources {1, 1} &&
			            resolved_colliding_msaa.desc.info.samples == 2 &&
			            resolved_colliding_msaa.desc.info.data.size == 0x20000 &&
			            texture_cache.GetImage(resolved_colliding_msaa.image_id).backing.samples ==
			                2 &&
			            texture_cache.GetImage(expanded_array_id).backing.samples == 1 &&
			            texture_cache.GetImage(expanded_array_id).backing.layers == 2,
			        "equal-size 1x array and 2x image shared an incompatible backing");
			RenderExecutorTestAccess::ResetBindings(executor);

			constexpr uint64_t msaa_target_address = base + 0x100000;
			constexpr uint32_t msaa_samples        = 4;
			auto msaa_target = make_target_desc(msaa_target_address, 0x40000, {128, 128, 1});
			msaa_target.info.samples   = msaa_samples;
			msaa_target.info.tile_mode = Prospero::TileMode::kRenderTarget;
			msaa_target.info.mip_layout[0].size = msaa_target.info.data.size;
			const auto msaa_target_id           = texture_cache.FindImage(msaa_target);
			RenderExecutorTestAccess::BindRenderTarget(executor, msaa_target_id);
			const auto msaa_target_view =
			    texture_cache.FindRenderTarget(msaa_target_id, msaa_target);

			auto       msaa_texture         = array_texture;
			const auto encoded_msaa_address = msaa_target_address >> 8u;
			msaa_texture.fields[0]          = static_cast<uint32_t>(encoded_msaa_address);
			msaa_texture.fields[1]          = static_cast<uint32_t>(encoded_msaa_address >> 32u) |
			                                  (array_format << 20u) | (3u << 30u);
			msaa_texture.fields[3] =
			    DstSel(4, 5, 6, 7) | (2u << 16u) |
			    (static_cast<uint32_t>(Prospero::TileMode::kRenderTarget) << 20u) |
			    (static_cast<uint32_t>(Prospero::ImageType::kColor2DMsaa) << 28u);
			msaa_texture.fields[4] = 0;
			msaa_texture.fields[5] = (7u << 20u) | (2u << 4u);
			ShaderRecompiler::IR::DescriptorValue msaa_descriptor {};
			std::copy(std::begin(msaa_texture.fields), std::end(msaa_texture.fields),
			          msaa_descriptor.dwords.begin());
			msaa_descriptor.dword_count = 8;
			auto msaa_program =
			    std::make_shared<ShaderRecompiler::IR::Program>(*array_runtime.program);
			msaa_program->info.images[0].dimension =
			    ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaa;
			auto msaa_snapshot = std::make_shared<ShaderRecompiler::IR::ResourceSnapshot>();
			msaa_snapshot->images.push_back(msaa_descriptor);
			ShaderStageRuntime msaa_runtime {std::move(msaa_program), std::move(msaa_snapshot)};
			auto msaa_binding = executor.PrepareBindings(scheduler.Current(), msaa_runtime,
			                                             vk::ShaderStageFlagBits::eCompute,
			                                             DescriptorCache::Stage::Compute);
			executor.RebindImages(scheduler.Current(), msaa_binding);
			const auto& resolved_msaa = msaa_binding.resources.images[0];
			Require(name, "MSAA descriptor backing reuse",
			        msaa_target_view != nullptr && resolved_msaa.image_id == msaa_target_id &&
			            resolved_msaa.image_view != nullptr &&
			            resolved_msaa.desc.info.type == Prospero::ImageType::kColor2D &&
			            resolved_msaa.desc.info.resources == ImageSubresources {1, 1} &&
			            resolved_msaa.desc.info.samples == msaa_samples &&
			            resolved_msaa.desc.info.data.size == 0x40000 &&
			            resolved_msaa.desc.view_info.level_count == 1 &&
			            texture_cache.GetImage(msaa_target_id).backing.samples == msaa_samples,
			        "MSAA view fields did not resolve to the matching multisample "
			        "backing");
			RenderExecutorTestAccess::ResetBindings(executor);

			auto msaa_array_texture = msaa_texture;
			msaa_array_texture.fields[3] =
			    (msaa_array_texture.fields[3] & ~(0xfu << 28u)) |
			    (static_cast<uint32_t>(Prospero::ImageType::kColor2DMsaaArray) << 28u);
			msaa_array_texture.fields[4] = 1;
			ShaderRecompiler::IR::DescriptorValue msaa_array_descriptor {};
			std::copy(std::begin(msaa_array_texture.fields), std::end(msaa_array_texture.fields),
			          msaa_array_descriptor.dwords.begin());
			msaa_array_descriptor.dword_count = 8;
			auto msaa_array_program =
			    std::make_shared<ShaderRecompiler::IR::Program>(*msaa_runtime.program);
			msaa_array_program->info.images[0].dimension =
			    ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaaArray;
			auto msaa_array_snapshot = std::make_shared<ShaderRecompiler::IR::ResourceSnapshot>();
			msaa_array_snapshot->images.push_back(msaa_array_descriptor);
			ShaderStageRuntime msaa_array_runtime {std::move(msaa_array_program),
			                                       std::move(msaa_array_snapshot)};
			auto               msaa_array_binding = executor.PrepareBindings(
			    scheduler.Current(), msaa_array_runtime, vk::ShaderStageFlagBits::eCompute,
			    DescriptorCache::Stage::Compute);
			executor.RebindImages(scheduler.Current(), msaa_array_binding);
			const auto& resolved_msaa_array = msaa_array_binding.resources.images[0];
			Require(name, "MSAA array backing expansion",
			        resolved_msaa_array.image_id != msaa_target_id &&
			            resolved_msaa_array.image_view != nullptr &&
			            resolved_msaa_array.desc.info.type == Prospero::ImageType::kColor2D &&
			            resolved_msaa_array.desc.info.resources == ImageSubresources {1, 2} &&
			            resolved_msaa_array.desc.info.samples == msaa_samples &&
			            resolved_msaa_array.desc.info.data.size == 0x80000 &&
			            resolved_msaa_array.desc.view_info.type == vk::ImageViewType::e2DArray &&
			            resolved_msaa_array.desc.view_info.layer_count == 2 &&
			            texture_cache.GetImage(resolved_msaa_array.image_id).backing.layers == 2 &&
			            texture_cache.GetImage(resolved_msaa_array.image_id).backing.samples ==
			                msaa_samples,
			        "MSAA array view did not expand the matching Color2D backing");
			RenderExecutorTestAccess::ResetBindings(executor);

			ImageDesc depth {};
			depth.type                 = BindingType::DepthTarget;
			depth.info.data            = {depth_address, 0x10000};
			depth.info.stencil         = {stencil_address, stencil_layout.size};
			depth.info.pixel_format    = vk::Format::eD32SfloatS8Uint;
			depth.info.guest_format    = Prospero::BufferFormat::k32Float;
			depth.info.type            = Prospero::ImageType::kColor2D;
			depth.info.extent          = {1, 1, 1};
			depth.info.resources       = {1, 1};
			depth.info.pitch           = 1;
			depth.info.bytes_per_block = 4;
			depth.info.samples         = 1;
			depth.info.tile_mode       = linear;
			depth.info.mip_layout[0]   = {0, depth.info.data.size, 1, 1};
			depth.view_info.format     = depth.info.pixel_format;
			depth.view_info.type       = vk::ImageViewType::e2D;
			depth.view_info.aspect     = vk::ImageAspectFlagBits::eDepth;
			depth.view_info.usage      = vk::ImageUsageFlagBits::eDepthStencilAttachment;
			const auto depth_id        = texture_cache.FindImage(depth);
			Require(
			    name, "cache-only depth discovery",
			    !texture_cache.FindImageFromRange(stencil_address, stencil_layout.size, false) &&
			        !texture_cache.GetImage(depth_id).binding.is_bound &&
			        !texture_cache.GetImage(depth_id).binding.is_target &&
			        !texture_cache.GetImage(depth_id).usage.depth_target,
			    "FindImage created depth acquisition state or a stencil proxy");
			RenderExecutorTestAccess::BindRenderTarget(executor, depth_id);
			Require(
			    name, "depth prefetch purity",
			    texture_cache.GetImage(depth_id).binding.is_target &&
			        !texture_cache.GetImage(depth_id).usage.depth_target &&
			        !texture_cache.FindImageFromRange(stencil_address, stencil_layout.size, false),
			    "depth prefetch performed final target acquisition");

			auto target_parent = make_target_desc(base, target_mip_size * 2, {2, 2, 1});
			target_parent.type = BindingType::RenderTarget;
			target_parent.info.resources.levels = 2;
			target_parent.info.mip_layout[0]    = {0, target_mip_size, 2, 2};
			target_parent.info.mip_layout[1]    = {target_mip_size, target_mip_size, 1, 1};
			target_parent.view_info.usage       = vk::ImageUsageFlagBits::eColorAttachment;
			auto       target_base_subresource = make_target_desc(base, target_mip_size, {2, 2, 1});
			const auto target_base_subresource_id =
			    texture_cache.FindImage(target_base_subresource);
			auto target_subresource =
			    make_target_desc(base + target_mip_size, target_mip_size, {1, 1, 1});
			const auto target_subresource_id = texture_cache.FindImage(target_subresource);
			auto       target_subresource_owner =
			    TextureCacheTestAccess::Owner(texture_cache, target_subresource_id);
			std::weak_ptr<Libs::Graphics::Image> retired_target = target_subresource_owner;
			const auto& discovered_target = texture_cache.GetImage(target_subresource_id);
			Require(
			    name, "cache-only target discovery",
			    !discovered_target.binding.is_bound && !discovered_target.binding.is_target &&
			        !discovered_target.binding.needs_rebind &&
			        !discovered_target.usage.render_target &&
			        !TextureCacheTestAccess::PendingDownload(texture_cache, target_subresource_id),
			    "FindImage claimed RenderExecutor-owned render-target state");
			RenderExecutorTestAccess::BindRenderTarget(executor, target_subresource_id);
			Require(
			    name, "target prefetch purity",
			    texture_cache.GetImage(target_subresource_id).binding.is_target &&
			        !texture_cache.GetImage(target_subresource_id).usage.render_target &&
			        !TextureCacheTestAccess::PendingDownload(texture_cache, target_subresource_id),
			    "target prefetch performed final target acquisition");
			const auto target_parent_id = texture_cache.FindImage(target_parent);
			Require(name, "active target overlap",
			        target_parent_id && target_parent_id != target_base_subresource_id &&
			            target_parent_id != target_subresource_id &&
			            target_subresource_owner != nullptr &&
			            target_subresource_owner->binding.needs_rebind &&
			            texture_cache.GetImage(target_parent_id).binding.is_target &&
			            !TextureCacheTestAccess::PendingDownload(texture_cache, target_parent_id),
			        "target overlap did not transfer target state to the merged "
			        "owner");
			target_subresource_owner.reset();

			RenderColorInfo rebound_color {};
			rebound_color.type     = RenderColorType::RenderTexture;
			rebound_color.desc     = target_subresource;
			rebound_color.image_id = target_subresource_id;
			rebound_color.format   = target_subresource.view_info.format;
			rebound_color.extent   = {1, 1};
			rebound_color.samples  = 1;
			RenderDepthInfo rebound_depth {};
			rebound_depth.desc     = depth;
			rebound_depth.format   = depth.info.pixel_format;
			rebound_depth.width    = 1;
			rebound_depth.height   = 1;
			rebound_depth.samples  = 1;
			rebound_depth.image_id = depth_id;
			scheduler.FinishCurrent();
			auto retired_owner = retired_target.lock();
			Require(name, "deferred target slot erasure",
			        TextureCacheTestAccess::Owner(texture_cache, target_subresource_id) ==
			                nullptr &&
			            retired_owner != nullptr && retired_owner->binding.needs_rebind,
			        "the displaced target did not survive only through RenderExecutor "
			        "ownership");
			auto rebound_rendering = RenderExecutorTestAccess::AcquireRenderTargets(
			    executor, scheduler.Current(), &rebound_color, 1, rebound_depth);
			const auto stencil_proxy_id =
			    texture_cache.FindImageFromRange(stencil_address, stencil_layout.size, false);
			Require(
			    name, "production target rebind",
			    rebound_color.image_id == target_parent_id && rebound_color.image_view != nullptr &&
			        rebound_depth.image_view != nullptr &&
			        rebound_rendering.num_color_attachments == 1 &&
			        rebound_rendering.color_attachments[0].image_view == rebound_color.image_view &&
			        rebound_rendering.depth_stencil_attachment.image_view ==
			            rebound_depth.image_view &&
			        rebound_rendering.width == 1 && rebound_rendering.height == 1 &&
			        texture_cache.GetImage(target_parent_id).binding.is_target &&
			        texture_cache.GetImage(target_parent_id).usage.render_target &&
			        texture_cache.GetImage(target_parent_id).IsGpuModified() &&
			        texture_cache.GetImage(depth_id).usage.depth_target &&
			        texture_cache.GetImage(depth_id).IsGpuModified() &&
			        TextureCacheTestAccess::PendingDownload(texture_cache, target_parent_id) &&
			        stencil_proxy_id &&
			        texture_cache.GetImage(stencil_proxy_id).depth_id == depth_id,
			    "the final target pass did not acquire color before depth and "
			    "build the dynamic render state");
			scheduler.BeginRendering(rebound_rendering);
			scheduler.EndRendering();
			RenderExecutorTestAccess::ResetBindings(executor);
			Require(name, "draw-scoped target reset",
			        !retired_owner->binding.is_target && !retired_owner->binding.needs_rebind &&
			            !texture_cache.GetImage(target_parent_id).binding.is_target &&
			            !texture_cache.GetImage(depth_id).binding.is_target,
			        "RenderExecutor retained target binding state after reset");
			retired_owner.reset();
			Require(name, "RenderExecutor-owned retired lifetime", retired_target.expired(),
			        "a retired target owner survived after RenderExecutor reset");

			constexpr uint64_t ordered_color_address = base + 0x80000;
			auto               ordered_color_desc =
			    make_target_desc(ordered_color_address, target_mip_size, {1, 1, 1});
			const auto stale_ordered_color = texture_cache.FindImage(ordered_color_desc);
			RenderExecutorTestAccess::BindRenderTarget(executor, stale_ordered_color);
			resources.UnmapMemory(ordered_color_address, target_mip_size);
			resources.MapMemory(ordered_color_address, target_mip_size);

			auto ordered_depth_desc         = depth;
			ordered_depth_desc.info.stencil = {ordered_color_address, target_mip_size};
			const auto ordered_depth_id     = texture_cache.FindImage(ordered_depth_desc);
			RenderExecutorTestAccess::BindRenderTarget(executor, ordered_depth_id);
			RenderColorInfo ordered_color {};
			ordered_color.type     = RenderColorType::RenderTexture;
			ordered_color.desc     = ordered_color_desc;
			ordered_color.image_id = stale_ordered_color;
			ordered_color.format   = ordered_color_desc.view_info.format;
			ordered_color.extent   = {1, 1};
			ordered_color.samples  = 1;
			RenderDepthInfo ordered_depth {};
			ordered_depth.desc     = ordered_depth_desc;
			ordered_depth.format   = ordered_depth_desc.info.pixel_format;
			ordered_depth.width    = 1;
			ordered_depth.height   = 1;
			ordered_depth.samples  = 1;
			ordered_depth.image_id = ordered_depth_id;
			auto ordered_rendering = RenderExecutorTestAccess::AcquireRenderTargets(
			    executor, scheduler.Current(), &ordered_color, 1, ordered_depth);
			Require(
			    name, "color-before-depth acquisition order",
			    ordered_color.image_id != stale_ordered_color &&
			        ordered_color.image_view != nullptr && ordered_depth.image_view != nullptr &&
			        ordered_rendering.color_attachments[0].image_view == ordered_color.image_view &&
			        ordered_rendering.depth_stencil_attachment.image_view ==
			            ordered_depth.image_view &&
			        texture_cache.GetImage(ordered_color.image_id).depth_id ==
			            ordered_depth.image_id,
			    "depth acquisition ran before the stale color target was "
			    "recreated and associated");
			RenderExecutorTestAccess::ResetBindings(executor);
			TextureCacheTestAccess::SetLinearReadback(texture_cache, false);

			ShaderTextureResource stencil {};
			const uint64_t        encoded_address = stencil_address >> 8u;
			stencil.fields[0]                     = static_cast<uint32_t>(encoded_address);
			stencil.fields[1] =
			    static_cast<uint32_t>(encoded_address >> 32u) |
			    (static_cast<uint32_t>(stencil_format) << 20u);
			stencil.fields[3] = DstSel(4, 5, 6, 7) |
			                    (static_cast<uint32_t>(linear) << 20u) |
			                    (static_cast<uint32_t>(Prospero::ImageType::kColor2D) << 28u);

			ShaderRecompiler::IR::Program program {};
			program.stage                      = ShaderType::Compute;
			program.resource_tracking_complete = true;
			ShaderRecompiler::IR::ImageResource stencil_resource {};
			stencil_resource.kind      = ShaderRecompiler::IR::ResourceKind::ImageUint;
			stencil_resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
			stencil_resource.read      = true;
			program.info.images.push_back(stencil_resource);

			ShaderRecompiler::IR::ResourceSnapshot snapshot {};
			ShaderRecompiler::IR::DescriptorValue  descriptor {};
			std::copy(std::begin(stencil.fields), std::end(stencil.fields),
			          descriptor.dwords.begin());
			descriptor.dword_count = 8;
			snapshot.images.push_back(descriptor);
			ShaderStageRuntime runtime {
			    std::make_shared<const ShaderRecompiler::IR::Program>(std::move(program)),
			    std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(
			        std::move(snapshot))};

			auto prepared = context.GetRenderExecutor().PrepareBindings(
			    scheduler.Current(), runtime, vk::ShaderStageFlagBits::eCompute,
			    DescriptorCache::Stage::Compute);
			const auto sampled_stencil_id = prepared.resources.images[0].image_id;
			Require(name, "first stencil discovery",
			        prepared.resources.images.size() == 1 && sampled_stencil_id != depth_id &&
			            prepared.resources.images[0].image_view == nullptr &&
			            texture_cache.GetImage(sampled_stencil_id).binding.is_bound &&
			            !texture_cache.GetImage(stencil_proxy_id).binding.is_bound &&
			            !texture_cache.GetImage(stencil_proxy_id).binding.is_target,
			        "the first sampled-stencil lookup did not remain an ordinary "
			        "discovery before final depth acquisition");

			context.GetRenderExecutor().RebindImages(scheduler.Current(), prepared);
			Require(name, "first stencil acquisition",
			        prepared.resources.images[0].image_id == sampled_stencil_id &&
			            prepared.resources.images[0].image_view != nullptr &&
			            texture_cache.GetImage(sampled_stencil_id).usage.texture,
			        "the first sampled-stencil image was not finally acquired");

			RenderExecutorTestAccess::ResetBindings(executor);
			RenderDepthInfo reassociated_depth = rebound_depth;
			reassociated_depth.image_view      = nullptr;
			RenderColorInfo no_reassociated_color {};
			(void)RenderExecutorTestAccess::AcquireRenderTargets(
			    executor, scheduler.Current(), &no_reassociated_color, 0, reassociated_depth);
			Require(name, "existing stencil association selection",
			        texture_cache.GetImage(sampled_stencil_id).depth_id ==
			            reassociated_depth.image_id,
			        "final depth acquisition did not associate the existing image "
			        "at the stencil guest address");
			RenderExecutorTestAccess::ResetBindings(executor);

			auto redirected = context.GetRenderExecutor().PrepareBindings(
			    scheduler.Current(), runtime, vk::ShaderStageFlagBits::eCompute,
			    DescriptorCache::Stage::Compute);
			Require(name, "redirected owner discovery",
			        redirected.resources.images.size() == 1 &&
			            redirected.resources.images[0].image_id == depth_id &&
			            redirected.resources.images[0].image_view == nullptr &&
			            texture_cache.GetImage(depth_id).binding.is_bound &&
			            !texture_cache.GetImage(sampled_stencil_id).binding.is_bound,
			        "the established stencil association did not redirect the next "
			        "discovery to the depth owner");
			context.GetRenderExecutor().RebindImages(scheduler.Current(), redirected);
			Require(name, "second-pass stencil acquisition",
			        redirected.resources.images[0].image_id == depth_id &&
			            redirected.resources.images[0].image_view != nullptr &&
			            texture_cache.GetImage(depth_id).usage.texture,
			        "RebindImages did not acquire the associated depth owner");
			RenderExecutorTestAccess::ResetBindings(executor);

			auto stencil_storage_program =
			    std::make_shared<ShaderRecompiler::IR::Program>(*runtime.program);
			stencil_storage_program->info.images[0].kind =
			    ShaderRecompiler::IR::ResourceKind::StorageImageUint;
			stencil_storage_program->info.images[0].read    = false;
			stencil_storage_program->info.images[0].written = true;
			auto storage_stencil                            = stencil;
			storage_stencil.fields[5]                       = 0x00700000u;
			ShaderRecompiler::IR::DescriptorValue stencil_storage_descriptor {};
			std::copy(std::begin(storage_stencil.fields), std::end(storage_stencil.fields),
			          stencil_storage_descriptor.dwords.begin());
			stencil_storage_descriptor.dword_count = 8;
			ShaderRecompiler::IR::ResourceSnapshot stencil_storage_snapshot {};
			stencil_storage_snapshot.images.push_back(stencil_storage_descriptor);
			ShaderStageRuntime stencil_storage_runtime {
			    std::move(stencil_storage_program),
			    std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(
			        std::move(stencil_storage_snapshot))};
			auto storage_redirected = executor.PrepareBindings(
			    scheduler.Current(), stencil_storage_runtime, vk::ShaderStageFlagBits::eCompute,
			    DescriptorCache::Stage::Compute);
			executor.RebindImages(scheduler.Current(), storage_redirected);
			Require(name, "storage stencil acquisition",
			        storage_redirected.resources.images[0].image_id == depth_id &&
			            storage_redirected.resources.images[0].image_view != nullptr &&
			            texture_cache.GetImage(depth_id).usage.storage,
			        "storage stencil binding did not acquire the associated depth owner");
			RenderExecutorTestAccess::ResetBindings(executor);
			resources.UnmapMemory(base, allocation_size);
			scheduler.Finish();
		}

		Require(name, "unmap direct backing",
		        Libs::LibKernel::Memory::KernelMunmap(base, allocation_size) == 0,
		        "descriptor discovery direct mapping release failed");
		Require(
		    name, "release direct backing",
		    Libs::LibKernel::Memory::KernelReleaseDirectMemory(direct_offset, allocation_size) == 0,
		    "descriptor discovery direct-memory allocation release failed");
		std::printf("[host]    %-32s ok\n", name);
	}

	Buffer CreateStorageBuffer(const char* shader_name, const std::vector<u32>& initial,
	                           size_t dword_count) {
		Buffer ret;
		ret.size = static_cast<vk::DeviceSize>(std::max<size_t>(dword_count, 1u) * sizeof(u32));

		vk::BufferCreateInfo buffer_info {};
		buffer_info.sType       = vk::StructureType::eBufferCreateInfo;
		buffer_info.size        = ret.size;
		buffer_info.usage       = vk::BufferUsageFlagBits::eStorageBuffer;
		buffer_info.sharingMode = vk::SharingMode::eExclusive;
		RequireVk(shader_name, "dispatch",
		          m_device.createBuffer(&buffer_info, nullptr, &ret.buffer), "vkCreateBuffer");

		vk::MemoryRequirements req {};
		m_device.getBufferMemoryRequirements(ret.buffer, &req);
		u32 memory_type = 0;
		ret.coherent    = FindMemoryType(req.memoryTypeBits,
		                                 vk::MemoryPropertyFlagBits::eHostVisible |
		                                     vk::MemoryPropertyFlagBits::eHostCoherent,
		                                 &memory_type);
		if (!ret.coherent) {
			Require(shader_name, "dispatch",
			        FindMemoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible,
			                       &memory_type),
			        "no host-visible memory type for storage buffer");
		}

		vk::MemoryAllocateInfo alloc {};
		alloc.sType           = vk::StructureType::eMemoryAllocateInfo;
		alloc.allocationSize  = req.size;
		alloc.memoryTypeIndex = memory_type;
		RequireVk(shader_name, "dispatch", m_device.allocateMemory(&alloc, nullptr, &ret.memory),
		          "vkAllocateMemory");
		RequireVk(shader_name, "dispatch", m_device.bindBufferMemory(ret.buffer, ret.memory, 0),
		          "vkBindBufferMemory");

		std::vector<u32> contents(dword_count, 0);
		for (size_t i = 0; i < initial.size() && i < contents.size(); i++) {
			contents[i] = initial[i];
		}
		WriteBuffer(shader_name, ret, contents);
		return ret;
	}

	void DestroyBuffer(Buffer* buffer) {
		if (buffer == nullptr) {
			return;
		}
		if (buffer->buffer != nullptr) {
			m_device.destroyBuffer(buffer->buffer, nullptr);
			buffer->buffer = nullptr;
		}
		if (buffer->memory != nullptr) {
			m_device.freeMemory(buffer->memory, nullptr);
			buffer->memory = nullptr;
		}
	}

	Image CreateImage2D(const char* shader_name, u32 width, u32 height, vk::Format format,
	                    vk::ImageUsageFlags usage, const std::vector<u32>& initial,
	                    u32 dwords_per_pixel, vk::ImageLayout final_layout) {
		std::vector<std::vector<u32>> mips;
		if (!initial.empty()) {
			mips.push_back(initial);
		}
		return CreateImageMips(shader_name, width, height, format, usage, mips, dwords_per_pixel,
		                       final_layout, vk::ImageType::e2D, vk::ImageViewType::e2D, 1);
	}

	static u32 MipExtent(u32 value, u32 level) {
		for (u32 i = 0; i < level && value > 1u; i++) {
			value >>= 1u;
		}
		return std::max(value, 1u);
	}

	static size_t ImageMipDwordCount(u32 width, u32 height, u32 dwords_per_pixel, u32 level,
	                                 u32 layers = 1) {
		return static_cast<size_t>(MipExtent(width, level)) *
		       static_cast<size_t>(MipExtent(height, level)) * layers * dwords_per_pixel;
	}

	Image CreateImageMips(const char* shader_name, u32 width, u32 height, vk::Format format,
	                      vk::ImageUsageFlags                  usage,
	                      const std::vector<std::vector<u32>>& initial_mips, u32 dwords_per_pixel,
	                      vk::ImageLayout final_layout, vk::ImageType image_type,
	                      vk::ImageViewType view_type, u32 layers, u32 view_base_layer = 0,
	                      u32 view_layers = 0) {
		Image ret;
		ret.format           = format;
		ret.width            = width;
		ret.height           = height;
		ret.layers           = layers;
		ret.mip_levels       = std::max<u32>(static_cast<u32>(initial_mips.size()), 1u);
		ret.dwords_per_pixel = dwords_per_pixel;

		vk::ImageCreateInfo image_info {};
		image_info.sType         = vk::StructureType::eImageCreateInfo;
		image_info.imageType     = image_type;
		image_info.format        = format;
		image_info.extent.width  = width;
		image_info.extent.height = height;
		image_info.extent.depth  = 1;
		image_info.mipLevels     = ret.mip_levels;
		image_info.arrayLayers   = layers;
		image_info.samples       = vk::SampleCountFlagBits::e1;
		image_info.tiling        = vk::ImageTiling::eOptimal;
		image_info.usage =
		    usage | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
		image_info.sharingMode   = vk::SharingMode::eExclusive;
		image_info.initialLayout = vk::ImageLayout::eUndefined;
		RequireVk(shader_name, "dispatch", m_device.createImage(&image_info, nullptr, &ret.image),
		          "vkCreateImage");

		vk::MemoryRequirements req {};
		m_device.getImageMemoryRequirements(ret.image, &req);
		u32 memory_type = 0;
		if (!FindMemoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal,
		                    &memory_type)) {
			Require(shader_name, "dispatch", FindMemoryType(req.memoryTypeBits, {}, &memory_type),
			        "no memory type for image");
		}

		vk::MemoryAllocateInfo alloc {};
		alloc.sType           = vk::StructureType::eMemoryAllocateInfo;
		alloc.allocationSize  = req.size;
		alloc.memoryTypeIndex = memory_type;
		RequireVk(shader_name, "dispatch", m_device.allocateMemory(&alloc, nullptr, &ret.memory),
		          "vk::Device::allocateMemory(image)");
		RequireVk(shader_name, "dispatch", m_device.bindImageMemory(ret.image, ret.memory, 0),
		          "vkBindImageMemory");

		vk::ImageViewCreateInfo view_info {};
		view_info.sType                         = vk::StructureType::eImageViewCreateInfo;
		view_info.image                         = ret.image;
		view_info.viewType                      = view_type;
		view_info.format                        = format;
		view_info.subresourceRange.aspectMask   = vk::ImageAspectFlagBits::eColor;
		view_info.subresourceRange.baseMipLevel = 0;
		view_info.subresourceRange.levelCount   = ret.mip_levels;
		Require(shader_name, "dispatch", view_base_layer < layers,
		        "image view base layer is out of bounds");
		if (view_layers == 0) {
			view_layers = layers - view_base_layer;
		}
		Require(shader_name, "dispatch", view_layers <= layers - view_base_layer,
		        "image view layer count is out of bounds");
		view_info.subresourceRange.baseArrayLayer = view_base_layer;
		view_info.subresourceRange.layerCount     = view_layers;
		RequireVk(shader_name, "dispatch", m_device.createImageView(&view_info, nullptr, &ret.view),
		          "vkCreateImageView");

		if (!initial_mips.empty()) {
			size_t total_dwords = 0;
			for (u32 level = 0; level < ret.mip_levels; level++) {
				total_dwords += ImageMipDwordCount(width, height, dwords_per_pixel, level, layers);
			}
			std::vector<u32> contents(total_dwords, 0);
			size_t           offset = 0;
			for (u32 level = 0; level < ret.mip_levels; level++) {
				const auto level_dwords =
				    ImageMipDwordCount(width, height, dwords_per_pixel, level, layers);
				const auto& src = initial_mips[level];
				for (size_t i = 0; i < src.size() && i < level_dwords; i++) {
					contents[offset + i] = src[i];
				}
				offset += level_dwords;
			}
			auto staging = CreateHostBuffer(shader_name, contents.size() * sizeof(u32),
			                                vk::BufferUsageFlagBits::eTransferSrc, contents);
			UploadImageMips(shader_name, &ret, staging.buffer, final_layout);
			DestroyBuffer(&staging);
		} else {
			TransitionImage(shader_name, &ret, final_layout, vk::PipelineStageFlagBits::eTopOfPipe,
			                vk::PipelineStageFlagBits::eComputeShader, {},
			                AccessForLayout(final_layout));
		}
		return ret;
	}

	void DestroyImage(Image* image) {
		if (image == nullptr) {
			return;
		}
		if (image->view != nullptr) {
			m_device.destroyImageView(image->view, nullptr);
			image->view = nullptr;
		}
		if (image->image != nullptr) {
			m_device.destroyImage(image->image, nullptr);
			image->image = nullptr;
		}
		if (image->memory != nullptr) {
			m_device.freeMemory(image->memory, nullptr);
			image->memory = nullptr;
		}
	}

	std::vector<u32> ReadImage(const char* shader_name, Image* image) {
		const auto dword_count = static_cast<size_t>(image->width) *
		                         static_cast<size_t>(image->height) * image->layers *
		                         image->dwords_per_pixel;
		auto       staging     = CreateHostBuffer(shader_name, dword_count * sizeof(u32),
		                                          vk::BufferUsageFlagBits::eTransferDst, {});

		vk::CommandBuffer cmd = BeginCommands(shader_name, "readback");
		AddImageBarrier(cmd, image->image, image->layout, vk::ImageLayout::eTransferSrcOptimal,
		                vk::PipelineStageFlagBits::eAllCommands,
		                vk::PipelineStageFlagBits::eTransfer,
		                vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
		                vk::AccessFlagBits::eTransferRead, image->mip_levels, image->layers);
		vk::BufferImageCopy copy {};
		copy.bufferOffset                    = 0;
		copy.imageSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
		copy.imageSubresource.mipLevel       = 0;
		copy.imageSubresource.baseArrayLayer = 0;
		copy.imageSubresource.layerCount     = image->layers;
		copy.imageExtent.width               = image->width;
		copy.imageExtent.height              = image->height;
		copy.imageExtent.depth               = 1;
		cmd.copyImageToBuffer(image->image, vk::ImageLayout::eTransferSrcOptimal, staging.buffer, 1,
		                      &copy);
		EndSubmitAndFree(shader_name, "readback", cmd);
		image->layout = vk::ImageLayout::eTransferSrcOptimal;

		auto ret = ReadBuffer(shader_name, staging, dword_count);
		DestroyBuffer(&staging);
		return ret;
	}

	vk::Sampler CreateNearestSampler(const char* shader_name) {
		vk::SamplerCreateInfo sampler_info {};
		sampler_info.sType        = vk::StructureType::eSamplerCreateInfo;
		sampler_info.magFilter    = vk::Filter::eNearest;
		sampler_info.minFilter    = vk::Filter::eNearest;
		sampler_info.mipmapMode   = vk::SamplerMipmapMode::eNearest;
		sampler_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
		sampler_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
		sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		sampler_info.minLod       = 0.0f;
		sampler_info.maxLod       = 0.0f;
		vk::Sampler sampler       = nullptr;
		RequireVk(shader_name, "dispatch", m_device.createSampler(&sampler_info, nullptr, &sampler),
		          "vkCreateSampler");
		return sampler;
	}

	std::vector<u32> ReadBuffer(const char* shader_name, const Buffer& buffer, size_t dword_count) {
		if (!buffer.coherent) {
			vk::MappedMemoryRange range {};
			range.sType  = vk::StructureType::eMappedMemoryRange;
			range.memory = buffer.memory;
			range.offset = 0;
			range.size   = VK_WHOLE_SIZE;
			RequireVk(shader_name, "readback", m_device.invalidateMappedMemoryRanges(1, &range),
			          "vkInvalidateMappedMemoryRanges");
		}

		void* data = nullptr;
		RequireVk(shader_name, "readback",
		          m_device.mapMemory(buffer.memory, 0, buffer.size, {}, &data), "vkMapMemory");
		std::vector<u32> ret(dword_count, 0);
		std::memcpy(ret.data(), data, dword_count * sizeof(u32));
		m_device.unmapMemory(buffer.memory);
		return ret;
	}

	void Dispatch(const TestCase& test, const CompiledShader& compiled, const Buffer& buffer,
	              const Buffer* gds_buffer = nullptr, const Image* sampled_image = nullptr,
	              const Image* storage_image = nullptr, const Image* storage_image_uint = nullptr,
	              vk::Sampler sampler = nullptr) {
		using Kind         = ShaderRecompiler::IR::DescriptorBindingKind;
		const auto& layout = compiled.program.bindings;
		auto Binding = [&](Kind kind) { return ShaderRecompiler::IR::FindBinding(layout, kind); };
		auto Count   = [&](Kind kind) {
			const auto* binding = Binding(kind);
			if (binding == nullptr) {
				return 0u;
			}
			if (kind == Kind::Gds) {
				return 1u;
			}
			return static_cast<u32>(binding->resources.size());
		};
		Require(test.name, "dispatch",
		        Count(Kind::Sampled2DArray) == 0 && Count(Kind::Sampled3D) == 0 &&
		            Count(Kind::SampledUint2DArray) == 0 && Count(Kind::SampledUint3D) == 0 &&
		            Count(Kind::Storage1D) == 0 && Count(Kind::Storage1DArray) == 0 &&
		            Count(Kind::Storage2DArray) == 0 && Count(Kind::Storage3D) == 0 &&
		            Count(Kind::StorageUint1D) == 0 && Count(Kind::StorageUint1DArray) == 0 &&
		            Count(Kind::StorageUint2DArray) == 0 && Count(Kind::StorageUint3D) == 0,
		        "unsupported array/3D image cases must provide matching Vulkan test "
		        "views "
		        "before dispatch");

		vk::ShaderModuleCreateInfo module_info {};
		module_info.sType       = vk::StructureType::eShaderModuleCreateInfo;
		module_info.codeSize    = compiled.spirv.size() * sizeof(u32);
		module_info.pCode       = compiled.spirv.data();
		vk::ShaderModule module = nullptr;
		RequireVk(test.name, "SPIR-V module",
		          m_device.createShaderModule(&module_info, nullptr, &module),
		          "vkCreateShaderModule");

		std::vector<vk::DescriptorSetLayoutBinding> layout_bindings;
		auto add_layout_binding = [&layout_bindings](u32 binding, vk::DescriptorType type,
		                                             u32 count) {
			if (count == 0) {
				return;
			}
			vk::DescriptorSetLayoutBinding item {};
			item.binding         = binding;
			item.descriptorType  = type;
			item.descriptorCount = count;
			item.stageFlags      = vk::ShaderStageFlagBits::eCompute;
			layout_bindings.push_back(item);
		};
		for (const auto& binding: layout.descriptors) {
			vk::DescriptorType type  = vk::DescriptorType::eStorageBuffer;
			u32                count = static_cast<u32>(binding.resources.size());
			switch (binding.kind) {
				case Kind::Sampled1D:
				case Kind::Sampled1DArray:
				case Kind::Sampled2D:
				case Kind::Sampled2DArray:
				case Kind::Sampled3D:
				case Kind::SampledUint1D:
				case Kind::SampledUint1DArray:
				case Kind::SampledUint2D:
				case Kind::SampledUint2DArray:
				case Kind::SampledUint3D: type = vk::DescriptorType::eSampledImage; break;
				case Kind::Storage1D:
				case Kind::Storage1DArray:
				case Kind::Storage2D:
				case Kind::Storage2DArray:
				case Kind::Storage3D:
				case Kind::StorageUint1D:
				case Kind::StorageUint1DArray:
				case Kind::StorageUint2D:
				case Kind::StorageUint2DArray:
				case Kind::StorageUint3D: type = vk::DescriptorType::eStorageImage; break;
				case Kind::Samplers: type = vk::DescriptorType::eSampler; break;
				case Kind::Gds:
				case Kind::FlattenedSrt:
				case Kind::UserData: count = 1; break;
				default: break;
			}
			add_layout_binding(binding.binding, type, count);
		}

		vk::DescriptorSetLayoutCreateInfo layout_info {};
		layout_info.sType        = vk::StructureType::eDescriptorSetLayoutCreateInfo;
		layout_info.bindingCount = static_cast<u32>(layout_bindings.size());
		layout_info.pBindings    = layout_bindings.empty() ? nullptr : layout_bindings.data();
		vk::DescriptorSetLayout descriptor_layout = nullptr;
		RequireVk(test.name, "dispatch",
		          m_device.createDescriptorSetLayout(&layout_info, nullptr, &descriptor_layout),
		          "vkCreateDescriptorSetLayout");

		vk::PipelineLayoutCreateInfo pipeline_layout_info {};
		pipeline_layout_info.sType          = vk::StructureType::ePipelineLayoutCreateInfo;
		pipeline_layout_info.setLayoutCount = 1;
		pipeline_layout_info.pSetLayouts    = &descriptor_layout;
		vk::PushConstantRange push_range {};
		if (layout.push_constant_size != 0) {
			push_range.stageFlags                       = vk::ShaderStageFlagBits::eCompute;
			push_range.offset                           = layout.push_constant_offset;
			push_range.size                             = layout.push_constant_size;
			pipeline_layout_info.pushConstantRangeCount = 1;
			pipeline_layout_info.pPushConstantRanges    = &push_range;
		}
		vk::PipelineLayout pipeline_layout = nullptr;
		RequireVk(test.name, "dispatch",
		          m_device.createPipelineLayout(&pipeline_layout_info, nullptr, &pipeline_layout),
		          "vkCreatePipelineLayout");

		vk::PipelineShaderStageCreateInfo stage {};
		stage.sType  = vk::StructureType::ePipelineShaderStageCreateInfo;
		stage.stage  = vk::ShaderStageFlagBits::eCompute;
		stage.module = module;
		stage.pName  = "main";

		vk::ComputePipelineCreateInfo pipeline_info {};
		pipeline_info.sType   = vk::StructureType::eComputePipelineCreateInfo;
		pipeline_info.stage   = stage;
		pipeline_info.layout  = pipeline_layout;
		vk::Pipeline pipeline = nullptr;
		RequireVk(test.name, "dispatch",
		          m_device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr, &pipeline),
		          "vkCreateComputePipelines");

		std::vector<vk::DescriptorPoolSize> pool_sizes;
		auto add_pool_size = [&pool_sizes](vk::DescriptorType type, u32 count) {
			if (count == 0) {
				return;
			}
			for (auto& size: pool_sizes) {
				if (size.type == type) {
					size.descriptorCount += count;
					return;
				}
			}
			pool_sizes.push_back({type, count});
		};
		add_pool_size(vk::DescriptorType::eStorageBuffer,
		              Count(Kind::Buffers) + Count(Kind::AddressMemory) + Count(Kind::Gds) +
		                  (Binding(Kind::FlattenedSrt) != nullptr ? 1u : 0u) +
		                  (Binding(Kind::UserData) != nullptr ? 1u : 0u));
		add_pool_size(vk::DescriptorType::eSampledImage,
		              Count(Kind::Sampled1D) + Count(Kind::Sampled1DArray) +
		                  Count(Kind::Sampled2D) + Count(Kind::Sampled2DArray) +
		                  Count(Kind::Sampled3D) + Count(Kind::SampledUint1D) +
		                  Count(Kind::SampledUint1DArray) + Count(Kind::SampledUint2D) +
		                  Count(Kind::SampledUint2DArray) + Count(Kind::SampledUint3D));
		add_pool_size(vk::DescriptorType::eStorageImage,
		              Count(Kind::Storage1D) + Count(Kind::Storage1DArray) +
		                  Count(Kind::Storage2D) + Count(Kind::Storage2DArray) +
		                  Count(Kind::Storage3D) + Count(Kind::StorageUint1D) +
		                  Count(Kind::StorageUint1DArray) + Count(Kind::StorageUint2D) +
		                  Count(Kind::StorageUint2DArray) + Count(Kind::StorageUint3D));
		add_pool_size(vk::DescriptorType::eSampler, Count(Kind::Samplers));
		vk::DescriptorPoolCreateInfo pool_info {};
		pool_info.sType                    = vk::StructureType::eDescriptorPoolCreateInfo;
		pool_info.maxSets                  = 1;
		pool_info.poolSizeCount            = static_cast<u32>(pool_sizes.size());
		pool_info.pPoolSizes               = pool_sizes.empty() ? nullptr : pool_sizes.data();
		vk::DescriptorPool descriptor_pool = nullptr;
		RequireVk(test.name, "dispatch",
		          m_device.createDescriptorPool(&pool_info, nullptr, &descriptor_pool),
		          "vkCreateDescriptorPool");

		vk::DescriptorSetAllocateInfo set_info {};
		set_info.sType                   = vk::StructureType::eDescriptorSetAllocateInfo;
		set_info.descriptorPool          = descriptor_pool;
		set_info.descriptorSetCount      = 1;
		set_info.pSetLayouts             = &descriptor_layout;
		vk::DescriptorSet descriptor_set = nullptr;
		RequireVk(test.name, "dispatch",
		          m_device.allocateDescriptorSets(&set_info, &descriptor_set),
		          "vkAllocateDescriptorSets");

		std::vector<vk::WriteDescriptorSet>   writes;
		std::vector<vk::DescriptorBufferInfo> buffer_infos;
		std::vector<vk::DescriptorBufferInfo> address_memory_infos;
		std::vector<vk::DescriptorImageInfo>  sampled_infos;
		std::vector<vk::DescriptorImageInfo>  storage_infos;
		std::vector<vk::DescriptorImageInfo>  storage_uint_infos;
		std::vector<vk::DescriptorImageInfo>  sampler_infos;
		Buffer                                flattened_buffer;
		Buffer                                user_data_buffer;
		vk::DescriptorBufferInfo              flattened_info {};
		vk::DescriptorBufferInfo              user_data_info {};
		vk::DescriptorBufferInfo              gds_info {};

		const auto* buffers = Binding(Kind::Buffers);
		if (buffers != nullptr) {
			buffer_infos.resize(buffers->resources.size());
			for (u32 i = 0; i < buffer_infos.size(); i++) {
				auto& info  = buffer_infos[i];
				info.buffer = buffer.buffer;
				info.offset = 0;
				info.range  = buffer.size;
				if (test.storage_buffer_range_dwords != 0) {
					const auto offset = i < test.storage_buffer_offsets.size()
					                        ? test.storage_buffer_offsets[i]
					                        : 0u;
					info.range        = static_cast<vk::DeviceSize>(
					    test.storage_buffer_range_dwords * sizeof(u32) + offset);
					Require(test.name, "dispatch", info.range <= buffer.size,
					        "storage buffer descriptor range exceeds backing buffer");
				}
			}
			vk::WriteDescriptorSet write {};
			write.sType           = vk::StructureType::eWriteDescriptorSet;
			write.dstSet          = descriptor_set;
			write.dstBinding      = buffers->binding;
			write.descriptorCount = static_cast<u32>(buffer_infos.size());
			write.descriptorType  = vk::DescriptorType::eStorageBuffer;
			write.pBufferInfo     = buffer_infos.data();
			writes.push_back(write);
		}
		if (const auto* address = Binding(Kind::AddressMemory); address != nullptr) {
			address_memory_infos.resize(address->resources.size());
			for (auto& info: address_memory_infos) {
				info = {buffer.buffer, 0, buffer.size};
			}
			vk::WriteDescriptorSet write {};
			write.sType           = vk::StructureType::eWriteDescriptorSet;
			write.dstSet          = descriptor_set;
			write.dstBinding      = address->binding;
			write.descriptorCount = static_cast<u32>(address_memory_infos.size());
			write.descriptorType  = vk::DescriptorType::eStorageBuffer;
			write.pBufferInfo     = address_memory_infos.data();
			writes.push_back(write);
		}
		if (const auto* flattened = Binding(Kind::FlattenedSrt); flattened != nullptr) {
			flattened_buffer = CreateStorageBuffer(test.name, compiled.flattened_srt,
			                                       compiled.flattened_srt.size());
			flattened_info   = {flattened_buffer.buffer, 0, flattened_buffer.size};
			vk::WriteDescriptorSet write {};
			write.sType           = vk::StructureType::eWriteDescriptorSet;
			write.dstSet          = descriptor_set;
			write.dstBinding      = flattened->binding;
			write.descriptorCount = 1;
			write.descriptorType  = vk::DescriptorType::eStorageBuffer;
			write.pBufferInfo     = &flattened_info;
			writes.push_back(write);
		}
		if (const auto* user = Binding(Kind::UserData); user != nullptr) {
			user_data_buffer = CreateStorageBuffer(test.name, compiled.packed_user_data,
			                                       compiled.packed_user_data.size());
			user_data_info   = {user_data_buffer.buffer, 0, user_data_buffer.size};
			vk::WriteDescriptorSet write {};
			write.sType           = vk::StructureType::eWriteDescriptorSet;
			write.dstSet          = descriptor_set;
			write.dstBinding      = user->binding;
			write.descriptorCount = 1;
			write.descriptorType  = vk::DescriptorType::eStorageBuffer;
			write.pBufferInfo     = &user_data_info;
			writes.push_back(write);
		}
		if (const auto* gds = Binding(Kind::Gds); gds != nullptr) {
			Require(test.name, "dispatch", gds_buffer != nullptr,
			        "GDS descriptor requested but no GDS buffer was provided");
			gds_info = {gds_buffer->buffer, 0, gds_buffer->size};
			vk::WriteDescriptorSet write {};
			write.sType           = vk::StructureType::eWriteDescriptorSet;
			write.dstSet          = descriptor_set;
			write.dstBinding      = gds->binding;
			write.descriptorCount = 1;
			write.descriptorType  = vk::DescriptorType::eStorageBuffer;
			write.pBufferInfo     = &gds_info;
			writes.push_back(write);
		}
		const ShaderRecompiler::IR::DescriptorBinding* sampled = nullptr;
		constexpr std::array                           sampled_kinds {
		    Kind::Sampled1D,     Kind::Sampled1DArray,     Kind::Sampled2D,
		    Kind::SampledUint1D, Kind::SampledUint1DArray, Kind::SampledUint2D,
		};
		for (const auto kind: sampled_kinds) {
			if (const auto* candidate = Binding(kind); candidate != nullptr) {
				Require(test.name, "dispatch", sampled == nullptr,
				        "Vulkan test harness needs separate sampled images for mixed "
				        "descriptor classes");
				sampled = candidate;
			}
		}
		if (sampled != nullptr) {
			Require(test.name, "dispatch", sampled_image != nullptr,
			        "sampled image descriptor requested but no sampled image was "
			        "provided");
			sampled_infos.resize(sampled->resources.size());
			for (auto& info: sampled_infos) {
				info.imageView   = sampled_image->view;
				info.imageLayout = sampled_image->layout;
			}
			vk::WriteDescriptorSet write {};
			write.sType           = vk::StructureType::eWriteDescriptorSet;
			write.dstSet          = descriptor_set;
			write.dstBinding      = sampled->binding;
			write.descriptorCount = static_cast<u32>(sampled_infos.size());
			write.descriptorType  = vk::DescriptorType::eSampledImage;
			write.pImageInfo      = sampled_infos.data();
			writes.push_back(write);
		}
		const auto* storage      = Binding(Kind::Storage2D);
		const auto* storage_uint = Binding(Kind::StorageUint2D);
		if (storage != nullptr || storage_uint != nullptr) {
			Require(test.name, "dispatch", storage_image != nullptr,
			        "storage image descriptor requested but no storage image was "
			        "provided");
			Require(test.name, "dispatch", storage_image_uint != nullptr,
			        "uint storage image descriptor requested but no uint storage "
			        "image was provided");
			storage_infos.resize(storage != nullptr ? storage->resources.size() : 0u);
			storage_uint_infos.resize(storage_uint != nullptr ? storage_uint->resources.size()
			                                                  : 0u);
			for (auto& info: storage_infos) {
				info.imageView   = storage_image->view;
				info.imageLayout = storage_image->layout;
			}
			for (auto& info: storage_uint_infos) {
				info.imageView   = storage_image_uint->view;
				info.imageLayout = storage_image_uint->layout;
			}
			if (storage != nullptr) {
				vk::WriteDescriptorSet write {};
				write.sType           = vk::StructureType::eWriteDescriptorSet;
				write.dstSet          = descriptor_set;
				write.dstBinding      = storage->binding;
				write.descriptorCount = static_cast<u32>(storage_infos.size());
				write.descriptorType  = vk::DescriptorType::eStorageImage;
				write.pImageInfo      = storage_infos.data();
				writes.push_back(write);
			}
			if (storage_uint != nullptr) {
				vk::WriteDescriptorSet write {};
				write.sType           = vk::StructureType::eWriteDescriptorSet;
				write.dstSet          = descriptor_set;
				write.dstBinding      = storage_uint->binding;
				write.descriptorCount = static_cast<u32>(storage_uint_infos.size());
				write.descriptorType  = vk::DescriptorType::eStorageImage;
				write.pImageInfo      = storage_uint_infos.data();
				writes.push_back(write);
			}
		}
		const auto* samplers = Binding(Kind::Samplers);
		if (samplers != nullptr) {
			Require(test.name, "dispatch", sampler != nullptr,
			        "sampler descriptor requested but no sampler was provided");
			sampler_infos.resize(samplers->resources.size());
			for (auto& info: sampler_infos) {
				info.sampler = sampler;
			}
			vk::WriteDescriptorSet write {};
			write.sType           = vk::StructureType::eWriteDescriptorSet;
			write.dstSet          = descriptor_set;
			write.dstBinding      = samplers->binding;
			write.descriptorCount = static_cast<u32>(sampler_infos.size());
			write.descriptorType  = vk::DescriptorType::eSampler;
			write.pImageInfo      = sampler_infos.data();
			writes.push_back(write);
		}
		if (!writes.empty()) {
			m_device.updateDescriptorSets(static_cast<u32>(writes.size()), writes.data(), 0,
			                              nullptr);
		}

		vk::CommandBuffer cmd = BeginCommands(test.name, "dispatch");
		cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline_layout, 0, 1,
		                       &descriptor_set, 0, nullptr);
		if (layout.push_constant_size != 0) {
			Require(test.name, "dispatch",
			        compiled.packed_user_data.size() * sizeof(u32) == layout.push_constant_size,
			        "native user-data size does not match push-constant range");
			cmd.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eCompute,
			                  layout.push_constant_offset, layout.push_constant_size,
			                  compiled.packed_user_data.data());
		}
		cmd.dispatch(test.dispatch_x, test.dispatch_y, test.dispatch_z);

		if (buffers != nullptr) {
			vk::BufferMemoryBarrier barrier {};
			barrier.sType = vk::StructureType::eBufferMemoryBarrier;
			barrier.srcAccessMask =
			    vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
			barrier.dstAccessMask       = vk::AccessFlagBits::eHostRead;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.buffer              = buffer.buffer;
			barrier.offset              = 0;
			barrier.size                = buffer.size;
			cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
			                    vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1, &barrier, 0,
			                    nullptr);
		}
		if (gds_buffer != nullptr) {
			vk::BufferMemoryBarrier barrier {};
			barrier.sType = vk::StructureType::eBufferMemoryBarrier;
			barrier.srcAccessMask =
			    vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
			barrier.dstAccessMask       = vk::AccessFlagBits::eHostRead;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.buffer              = gds_buffer->buffer;
			barrier.offset              = 0;
			barrier.size                = gds_buffer->size;
			cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
			                    vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1, &barrier, 0,
			                    nullptr);
		}
		EndSubmitAndFree(test.name, "dispatch", cmd);
		if (flattened_buffer.buffer != nullptr) {
			DestroyBuffer(&flattened_buffer);
		}
		if (user_data_buffer.buffer != nullptr) {
			DestroyBuffer(&user_data_buffer);
		}
		m_device.destroyDescriptorPool(descriptor_pool, nullptr);
		m_device.destroyPipeline(pipeline, nullptr);
		m_device.destroyPipelineLayout(pipeline_layout, nullptr);
		m_device.destroyDescriptorSetLayout(descriptor_layout, nullptr);
		m_device.destroyShaderModule(module, nullptr);
	}

	std::vector<u32> RenderFragment(const GraphicsCase& test, const CompiledShader& fragment) {
		const auto vertex_spirv = TestSpv::MakePassthroughVertexSpirv();
		ValidateSpirv(test.name, vertex_spirv);

		Image target = CreateImage2D(test.name, 1, 1, vk::Format::eR32G32B32A32Sfloat,
		                             vk::ImageUsageFlagBits::eColorAttachment, {}, 4,
		                             vk::ImageLayout::eGeneral);
		const std::vector<u32> default_vertices = {
		    0xbf800000u, 0xbf800000u, 0x3e800000u, 0x3f000000u, 0x3f400000u, 0x3f800000u,
		    0x40400000u, 0xbf800000u, 0x3e800000u, 0x3f000000u, 0x3f400000u, 0x3f800000u,
		    0xbf800000u, 0x40400000u, 0x3e800000u, 0x3f000000u, 0x3f400000u, 0x3f800000u,
		};
		const auto& vertices = test.vertices.empty() ? default_vertices : test.vertices;
		Require(test.name, "graphics", vertices.size() == 18u,
		        "graphics vertex buffer must contain three pos2/color4 vertices");
		auto vertex_buffer = CreateHostBuffer(test.name, vertices.size() * sizeof(u32),
		                                      vk::BufferUsageFlagBits::eVertexBuffer, vertices);

		auto make_module = [&](const std::vector<u32>& spirv) {
			vk::ShaderModuleCreateInfo module_info {};
			module_info.sType       = vk::StructureType::eShaderModuleCreateInfo;
			module_info.codeSize    = spirv.size() * sizeof(u32);
			module_info.pCode       = spirv.data();
			vk::ShaderModule module = nullptr;
			RequireVk(test.name, "graphics",
			          m_device.createShaderModule(&module_info, nullptr, &module),
			          "vkCreateShaderModule");
			return module;
		};
		vk::ShaderModule vertex_module   = make_module(vertex_spirv);
		vk::ShaderModule fragment_module = make_module(fragment.spirv);

		const auto&           fragment_bind = fragment.program.bindings;
		vk::PushConstantRange push_constant_range {};
		if (fragment_bind.push_constant_size > 0) {
			Require(test.name, "graphics",
			        test.push_constants.size() * sizeof(u32) == fragment_bind.push_constant_size,
			        "fragment push constant data size does not match reflection");
			push_constant_range.stageFlags = vk::ShaderStageFlagBits::eFragment;
			push_constant_range.offset     = fragment_bind.push_constant_offset;
			push_constant_range.size       = fragment_bind.push_constant_size;
		}

		vk::PipelineLayoutCreateInfo pipeline_layout_info {};
		pipeline_layout_info.sType                  = vk::StructureType::ePipelineLayoutCreateInfo;
		pipeline_layout_info.pushConstantRangeCount = push_constant_range.size != 0 ? 1u : 0u;
		pipeline_layout_info.pPushConstantRanges =
		    push_constant_range.size != 0 ? &push_constant_range : nullptr;
		vk::PipelineLayout pipeline_layout = nullptr;
		RequireVk(test.name, "graphics",
		          m_device.createPipelineLayout(&pipeline_layout_info, nullptr, &pipeline_layout),
		          "vkCreatePipelineLayout");

		vk::PipelineShaderStageCreateInfo stages[2] = {};
		stages[0].sType  = vk::StructureType::ePipelineShaderStageCreateInfo;
		stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
		stages[0].module = vertex_module;
		stages[0].pName  = "main";
		stages[1].sType  = vk::StructureType::ePipelineShaderStageCreateInfo;
		stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
		stages[1].module = fragment_module;
		stages[1].pName  = "main";

		vk::VertexInputBindingDescription vertex_binding {};
		vertex_binding.binding                            = 0;
		vertex_binding.stride                             = 6u * sizeof(float);
		vertex_binding.inputRate                          = vk::VertexInputRate::eVertex;
		vk::VertexInputAttributeDescription attributes[2] = {};
		attributes[0].location                            = 0;
		attributes[0].binding                             = 0;
		attributes[0].format                              = vk::Format::eR32G32Sfloat;
		attributes[0].offset                              = 0;
		attributes[1].location                            = 1;
		attributes[1].binding                             = 0;
		attributes[1].format                              = vk::Format::eR32G32B32A32Sfloat;
		attributes[1].offset                              = 2u * sizeof(float);
		vk::PipelineVertexInputStateCreateInfo vertex_input {};
		vertex_input.sType = vk::StructureType::ePipelineVertexInputStateCreateInfo;
		vertex_input.vertexBindingDescriptionCount   = 1;
		vertex_input.pVertexBindingDescriptions      = &vertex_binding;
		vertex_input.vertexAttributeDescriptionCount = 2;
		vertex_input.pVertexAttributeDescriptions    = attributes;

		vk::PipelineInputAssemblyStateCreateInfo input_assembly {};
		input_assembly.sType    = vk::StructureType::ePipelineInputAssemblyStateCreateInfo;
		input_assembly.topology = vk::PrimitiveTopology::eTriangleList;

		vk::Viewport viewport {};
		viewport.x        = 0.0f;
		viewport.y        = 0.0f;
		viewport.width    = 1.0f;
		viewport.height   = 1.0f;
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		vk::Rect2D scissor {};
		scissor.extent.width  = 1;
		scissor.extent.height = 1;
		vk::PipelineViewportStateCreateInfo viewport_state {};
		viewport_state.sType         = vk::StructureType::ePipelineViewportStateCreateInfo;
		viewport_state.viewportCount = 1;
		viewport_state.pViewports    = &viewport;
		viewport_state.scissorCount  = 1;
		viewport_state.pScissors     = &scissor;

		vk::PipelineRasterizationStateCreateInfo raster {};
		raster.sType       = vk::StructureType::ePipelineRasterizationStateCreateInfo;
		raster.polygonMode = vk::PolygonMode::eFill;
		raster.cullMode    = vk::CullModeFlagBits::eNone;
		raster.frontFace   = vk::FrontFace::eCounterClockwise;
		raster.lineWidth   = 1.0f;

		vk::PipelineMultisampleStateCreateInfo multisample {};
		multisample.sType                = vk::StructureType::ePipelineMultisampleStateCreateInfo;
		multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;

		vk::PipelineColorBlendAttachmentState color_attachment {};
		color_attachment.colorWriteMask =
		    vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
		    vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
		vk::PipelineColorBlendStateCreateInfo color_blend {};
		color_blend.sType           = vk::StructureType::ePipelineColorBlendStateCreateInfo;
		color_blend.attachmentCount = 1;
		color_blend.pAttachments    = &color_attachment;

		vk::GraphicsPipelineCreateInfo  pipeline_info {};
		const vk::Format                color_format = vk::Format::eR32G32B32A32Sfloat;
		vk::PipelineRenderingCreateInfo rendering_pipeline {};
		rendering_pipeline.sType                = vk::StructureType::ePipelineRenderingCreateInfo;
		rendering_pipeline.colorAttachmentCount = 1;
		rendering_pipeline.pColorAttachmentFormats = &color_format;
		pipeline_info.sType                        = vk::StructureType::eGraphicsPipelineCreateInfo;
		pipeline_info.pNext                        = &rendering_pipeline;
		pipeline_info.stageCount                   = 2;
		pipeline_info.pStages                      = stages;
		pipeline_info.pVertexInputState            = &vertex_input;
		pipeline_info.pInputAssemblyState          = &input_assembly;
		pipeline_info.pViewportState               = &viewport_state;
		pipeline_info.pRasterizationState          = &raster;
		pipeline_info.pMultisampleState            = &multisample;
		pipeline_info.pColorBlendState             = &color_blend;
		pipeline_info.layout                       = pipeline_layout;
		vk::Pipeline pipeline                      = nullptr;
		RequireVk(test.name, "graphics",
		          m_device.createGraphicsPipelines(nullptr, 1, &pipeline_info, nullptr, &pipeline),
		          "vkCreateGraphicsPipelines");

		vk::CommandBuffer           cmd = BeginCommands(test.name, "graphics");
		vk::RenderingAttachmentInfo color {};
		color.sType       = vk::StructureType::eRenderingAttachmentInfo;
		color.imageView   = target.view;
		color.imageLayout = vk::ImageLayout::eGeneral;
		color.loadOp      = vk::AttachmentLoadOp::eClear;
		color.storeOp     = vk::AttachmentStoreOp::eStore;
		vk::RenderingInfo rendering {};
		rendering.sType                = vk::StructureType::eRenderingInfo;
		rendering.renderArea.extent    = {1, 1};
		rendering.layerCount           = 1;
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachments    = &color;
		cmd.beginRendering(rendering);
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
		if (push_constant_range.size != 0) {
			cmd.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eFragment,
			                  fragment_bind.push_constant_offset, fragment_bind.push_constant_size,
			                  test.push_constants.data());
		}
		vk::DeviceSize offset = 0;
		cmd.bindVertexBuffers(0, 1, &vertex_buffer.buffer, &offset);
		cmd.draw(3, 1, 0, 0);
		cmd.endRendering();
		EndSubmitAndFree(test.name, "graphics", cmd);
		target.layout = vk::ImageLayout::eGeneral;

		auto pixel = ReadImage(test.name, &target);
		pixel.resize(4);

		m_device.destroyPipeline(pipeline, nullptr);
		m_device.destroyPipelineLayout(pipeline_layout, nullptr);
		m_device.destroyShaderModule(fragment_module, nullptr);
		m_device.destroyShaderModule(vertex_module, nullptr);
		DestroyBuffer(&vertex_buffer);
		DestroyImage(&target);
		return pixel;
	}

	void CheckGpuTilerCpuParity() {
		constexpr const char* name = "GpuTilerCpuParity";
		EnsureRuntimeContext();

		CommandScheduler scheduler(Renderer(), m_runtime_context);
		HW::Context      registers {};
		HW::UserConfig   user_config {};
		HW::Shader       shaders {};
		scheduler.Begin(registers, user_config, shaders);
		StreamBuffer       parameters(m_runtime_context, scheduler, MemoryUsage::Stream, 1u << 20);
		TileManager        tile_manager(m_runtime_context, scheduler, parameters);
		constexpr uint64_t conversion_limit = 128ull << 20;
		Require(name, "D16 conversion chunk boundaries",
		        TileManagerTestAccess::ConversionRows(0, 32ull << 10, 32ull << 10, 4096, 256,
		                                              conversion_limit, 65535) == 4096 &&
		            TileManagerTestAccess::ConversionRows(0, 32ull << 10, 32ull << 10, 4097, 256,
		                                                  conversion_limit, 65535) == 4096 &&
		            TileManagerTestAccess::ConversionRows(257, 32ull << 10, 32ull << 10, 4097, 256,
		                                                  conversion_limit, 65535) == 4095,
		        "D16 conversion planner exceeds descriptor or dispatch limits");

		const auto align_dword = [](uint64_t size) {
			return (size + sizeof(u32) - 1u) & ~(uint64_t {sizeof(u32) - 1u});
		};
		const auto to_dwords = [&](const std::vector<uint8_t>& bytes, uint64_t padded_size) {
			std::vector<u32> words(static_cast<size_t>(padded_size / sizeof(u32)), 0);
			std::memcpy(words.data(), bytes.data(), bytes.size());
			return words;
		};
		const auto read_bytes = [&](const Buffer& buffer, uint64_t size) {
			const auto words =
			    ReadBuffer(name, buffer, static_cast<size_t>(align_dword(size) / sizeof(u32)));
			std::vector<uint8_t> bytes(static_cast<size_t>(size));
			std::memcpy(bytes.data(), words.data(), bytes.size());
			return bytes;
		};
		const auto host_barrier = [&](vk::Buffer buffer, uint64_t size,
		                              vk::PipelineStageFlags source_stage,
		                              vk::AccessFlags        source_access) {
			vk::BufferMemoryBarrier barrier {};
			barrier.sType               = vk::StructureType::eBufferMemoryBarrier;
			barrier.srcAccessMask       = source_access;
			barrier.dstAccessMask       = vk::AccessFlagBits::eHostRead;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.buffer              = buffer;
			barrier.offset              = 0;
			barrier.size                = size;
			scheduler.Current().Handle().pipelineBarrier(source_stage,
			                                             vk::PipelineStageFlagBits::eHost, {}, 0,
			                                             nullptr, 1, &barrier, 0, nullptr);
		};
		const auto gpu_detile = [&](const std::vector<uint8_t>& tiled, std::vector<uint8_t>* linear,
		                            uint64_t tiled_capacity, uint64_t linear_capacity,
		                            std::span<const GpuTileInfo> infos) {
			const uint64_t padded_tiled  = align_dword(tiled_capacity);
			const uint64_t padded_linear = align_dword(linear_capacity);
			auto           input =
			    CreateHostBuffer(name, padded_tiled, AllFlags, to_dwords(tiled, padded_tiled));
			auto output = CreateHostBuffer(
			    name, padded_linear, AllFlags,
			    std::vector<u32>(static_cast<size_t>(padded_linear / sizeof(u32)), 0xabababab));
			const auto result =
			    tile_manager.Detile(input.buffer, 0, padded_tiled, padded_linear, infos);
			const vk::BufferCopy copy {result.offset, 0, padded_linear};
			scheduler.Current().Handle().copyBuffer(result.buffer, output.buffer, 1, &copy);
			host_barrier(output.buffer, padded_linear, vk::PipelineStageFlagBits::eTransfer,
			             vk::AccessFlagBits::eTransferWrite);
			scheduler.Finish();
			*linear = read_bytes(output, linear_capacity);
			DestroyBuffer(&output);
			DestroyBuffer(&input);
		};
		const auto gpu_tile = [&](const std::vector<uint8_t>& linear, std::vector<uint8_t>* tiled,
		                          uint64_t tiled_capacity, uint64_t linear_capacity,
		                          std::span<const GpuTileInfo> infos) {
			const uint64_t padded_tiled  = align_dword(tiled_capacity);
			const uint64_t padded_linear = align_dword(linear_capacity);
			auto           input =
			    CreateHostBuffer(name, padded_linear, AllFlags, to_dwords(linear, padded_linear));
			auto output = CreateHostBuffer(
			    name, padded_tiled, AllFlags,
			    std::vector<u32>(static_cast<size_t>(padded_tiled / sizeof(u32)), 0xabababab));
			tile_manager.Tile(input.buffer, 0, padded_linear, output.buffer, 0, padded_tiled,
			                  infos);
			host_barrier(output.buffer, padded_tiled, vk::PipelineStageFlagBits::eComputeShader,
			             vk::AccessFlagBits::eShaderWrite);
			scheduler.Finish();
			*tiled = read_bytes(output, tiled_capacity);
			DestroyBuffer(&output);
			DestroyBuffer(&input);
		};

		for (const bool d32: std::array {true, false}) {
			constexpr uint32_t width     = 5;
			constexpr uint32_t height    = 3;
			constexpr uint32_t pitch     = 8;
			constexpr uint32_t layers    = 2;
			constexpr uint64_t d16_row   = pitch * sizeof(uint16_t);
			constexpr uint64_t d32_row   = pitch * sizeof(uint32_t);
			constexpr uint64_t d16_slice = d16_row * height;
			constexpr uint64_t d32_slice = d32_row * height;
			constexpr uint64_t d16_size  = d16_slice * layers;
			constexpr uint64_t d32_size  = d32_slice * layers;
			StreamBuffer       conversion_upload(m_runtime_context, scheduler, MemoryUsage::Upload,
			                                     1u << 20);
			auto [prefix, prefix_offset] = conversion_upload.Map(4096, 256);
			Require(name, "D16 conversion prefix", prefix != nullptr && prefix_offset == 0,
			        "failed to advance the D16 conversion upload ring");
			std::memset(prefix, 0x7b, 4096);
			conversion_upload.Commit();
			auto [source_bytes, source_offset] = conversion_upload.Map(d16_size, 4);
			Require(name, "D16 conversion source", source_bytes != nullptr && source_offset != 0,
			        "failed to reserve a nonzero D16 conversion source");
			std::memset(source_bytes, 0x3c, d16_size);
			for (uint32_t layer = 0; layer < layers; layer++) {
				for (uint32_t y = 0; y < height; y++) {
					auto* row =
					    reinterpret_cast<uint16_t*>(source_bytes + layer * d16_slice + y * d16_row);
					for (uint32_t x = 0; x < width; x++) {
						row[x] = static_cast<uint16_t>(
						    0x1111u * (1u + x + y * width + layer * width * height));
					}
				}
			}
			conversion_upload.Commit();
			auto promoted =
			    CreateHostBuffer(name, d32_size, AllFlags,
			                     std::vector<u32>(static_cast<size_t>(d32_size / sizeof(u32)), 0));
			auto round_trip = CreateHostBuffer(
			    name, d16_size, AllFlags,
			    std::vector<u32>(static_cast<size_t>(d16_size / sizeof(u32)), 0xa5a5a5a5u));
			const TileManager::D16Layout promote_layout {
			    .width               = width,
			    .height              = height,
			    .layers              = layers,
			    .source_row_stride   = d16_row,
			    .target_row_stride   = d32_row,
			    .source_slice_stride = d16_slice,
			    .target_slice_stride = d32_slice,
			};
			tile_manager.ConvertD16({conversion_upload.Handle(), source_offset,
			                         conversion_upload.Size() - source_offset},
			                        {promoted.buffer, 0, d32_size},
			                        TileManager::D16Direction::Promote, d32, promote_layout);
			tile_manager.ConvertD16({promoted.buffer, 0, d32_size},
			                        {round_trip.buffer, 0, d16_size},
			                        TileManager::D16Direction::Demote, d32,
			                        {.width               = width,
			                         .height              = height,
			                         .layers              = layers,
			                         .source_row_stride   = d32_row,
			                         .target_row_stride   = d16_row,
			                         .source_slice_stride = d32_slice,
			                         .target_slice_stride = d16_slice});
			host_barrier(round_trip.buffer, d16_size, vk::PipelineStageFlagBits::eComputeShader,
			             vk::AccessFlagBits::eShaderWrite);
			scheduler.Finish();
			const auto result             = read_bytes(round_trip, d16_size);
			bool       conversion_matches = true;
			for (uint32_t layer = 0; layer < layers; layer++) {
				for (uint32_t y = 0; y < height; y++) {
					for (uint32_t x = 0; x < pitch; x++) {
						const auto offset = layer * d16_slice + y * d16_row + x * sizeof(uint16_t);
						conversion_matches &=
						    x < width ? std::memcmp(result.data() + offset, source_bytes + offset,
						                            sizeof(uint16_t)) == 0
						              : result[offset] == 0xa5 && result[offset + 1] == 0xa5;
					}
				}
			}
			Require(name, d32 ? "D32 conversion round trip" : "D24 conversion round trip",
			        conversion_matches,
			        "D16 conversion lost rows, layers, active values, or padding");
			DestroyBuffer(&round_trip);
			DestroyBuffer(&promoted);
		}

		auto fill = [](std::vector<uint8_t>* bytes, u32 salt) {
			for (size_t i = 0; i < bytes->size(); i++) {
				uint64_t value = i + static_cast<uint64_t>(salt) * 0x9e3779b97f4a7c15ull;
				value          = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
				value          = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
				(*bytes)[i]    = static_cast<uint8_t>((value ^ (value >> 31u)) >> 56u);
			}
		};
		auto compare = [&](const char* stage, const std::vector<uint8_t>& expected,
		                   const std::vector<uint8_t>& actual) {
			if (expected != actual) {
				const auto mismatch = static_cast<size_t>(
				    std::mismatch(expected.begin(), expected.end(), actual.begin()).first -
				    expected.begin());
				std::ostringstream out;
				out << "first mismatch at " << mismatch << " of " << expected.size();
				Fail(name, stage, out.str());
			}
		};
		auto convert_reference = [&](bool to_tiled, std::vector<uint8_t>* dst,
		                             const std::vector<uint8_t>& src, const GpuTileInfo& info) {
			TileBlockLayout block {};
			Require(name, "reference layout",
			        TileGetBlockLayout(info.family, info.bytes_per_element, block),
			        "CPU reference rejected GPU tile info");
			const u32      tiled_width  = info.tiled_width != 0 ? info.tiled_width : info.pitch;
			const u32      tiled_height = info.tiled_height != 0 ? info.tiled_height : info.height;
			const uint64_t columns = (tiled_width + block.block_width - 1u) / block.block_width;
			const uint64_t rows    = (tiled_height + block.block_height - 1u) / block.block_height;
			const uint64_t slice =
			    info.linear_slice_stride != 0
			        ? info.linear_slice_stride
			        : static_cast<uint64_t>(info.pitch) * info.height * info.bytes_per_element;
			for (u32 z = 0; z < info.depth; ++z) {
				for (u32 y = 0; y < info.height; ++y) {
					for (u32 x = 0; x < info.width; ++x) {
						const u32 bx    = info.tail ? 0 : x / block.block_width;
						const u32 by    = info.tail ? 0 : y / block.block_height;
						const u32 bz    = info.tail ? 0 : z / block.block_depth;
						const u32 lx    = info.tail ? x + info.tail_x : x % block.block_width;
						const u32 ly    = info.tail ? y + info.tail_y : y % block.block_height;
						const u32 lz    = z % block.block_depth;
						u32       local = 0, block_xor = 0;
						Require(name, "reference offset",
						        TileGetBlockOffset(block, lx, ly, lz, local) &&
						            TileGetBlockXor(block, bx, by, info.surface_z + bz, block_xor),
						        "CPU reference address lookup failed");
						const uint64_t block_index =
						    static_cast<uint64_t>(bz) * columns * rows + by * columns + bx;
						const uint64_t tiled = info.tiled_offset + block_index * block.block_size +
						                       (local ^ block_xor);
						const uint64_t linear =
						    info.linear_offset + static_cast<uint64_t>(z) * slice +
						    static_cast<uint64_t>(y) * info.pitch * info.bytes_per_element +
						    static_cast<uint64_t>(x) * info.bytes_per_element;
						const uint64_t dst_offset = to_tiled ? tiled : linear;
						const uint64_t src_offset = to_tiled ? linear : tiled;
						Require(name, "reference range",
						        dst_offset + info.bytes_per_element <= dst->size() &&
						            src_offset + info.bytes_per_element <= src.size(),
						        "CPU reference address escaped storage");
						std::memcpy(dst->data() + dst_offset, src.data() + src_offset,
						            info.bytes_per_element);
					}
				}
			}
		};
		struct FamilyCase {
			TileBlockFamily family;
			u32             max_bpe;
		};
		constexpr FamilyCase families[] = {
		    {TileBlockFamily::Standard256B, 16},   {TileBlockFamily::Standard4KB, 16},
		    {TileBlockFamily::Standard4KB3D, 16},  {TileBlockFamily::Standard64KB, 16},
		    {TileBlockFamily::Standard64KB3D, 16}, {TileBlockFamily::Prt64KB, 16},
		    {TileBlockFamily::Prt64KB3D, 16},      {TileBlockFamily::RenderTarget64KB, 16},
		    {TileBlockFamily::Depth64KB, 8},
		};

		{
			TileBlockLayout standard {}, prt {}, standard_3d {}, prt_3d {}, color {}, depth {};
			u32  standard_offset = 0, prt_offset = 0, standard_3d_offset = 0, prt_3d_offset = 0,
			     color_z = 0, depth_z = 0;
			bool fixed_addresses =
			    TileGetBlockLayout(TileBlockFamily::Standard64KB, 4, standard) &&
			    TileGetBlockLayout(TileBlockFamily::Prt64KB, 4, prt) &&
			    TileGetBlockLayout(TileBlockFamily::Standard64KB3D, 4, standard_3d) &&
			    TileGetBlockLayout(TileBlockFamily::Prt64KB3D, 4, prt_3d) &&
			    TileGetBlockLayout(TileBlockFamily::RenderTarget64KB, 4, color) &&
			    TileGetBlockLayout(TileBlockFamily::Depth64KB, 4, depth) &&
			    TileGetBlockOffset(standard, 64, 0, 0, standard_offset) &&
			    TileGetBlockOffset(prt, 64, 0, 0, prt_offset) &&
			    TileGetBlockOffset(standard_3d, 16, 0, 0, standard_3d_offset) &&
			    TileGetBlockOffset(prt_3d, 16, 0, 0, prt_3d_offset) &&
			    TileGetBlockXor(color, 0, 0, 1, color_z) &&
			    TileGetBlockXor(depth, 0, 0, 15, depth_z);
			Require(name, "fixed address vectors",
			        fixed_addresses && standard_offset == 0x8000 && prt_offset == 0x8100 &&
			            standard_3d_offset == 0x8000 && prt_3d_offset == 0x8400 &&
			            standard_3d.block_width == 32 && standard_3d.block_height == 32 &&
			            standard_3d.block_depth == 16 && color_z == 0x800 && depth_z == 0xf00,
			        "a fixed block address changed");

			constexpr auto  format = Prospero::BufferFormat::k32Float;
			constexpr u32  levels = 7;
			TileSizeAlign  total {};
			TileSizeOffset mip[levels] {};
			TilePaddedSize padded[levels] {};
			TileGetTextureSize(format, 65, 33, levels,
			                   Prospero::TileMode::kStandard256B, &total,
			                   mip, padded);
			constexpr u32 offsets[levels] = {0x1a00, 0xb00, 0x500, 0x300, 0x200, 0x100, 0};
			constexpr u32 sizes[levels]   = {0x2d00, 0xf00, 0x600, 0x200, 0x100, 0x100, 0x100};
			bool          layout_matches  = total.size == 0x4700 && total.align == 0x100 &&
			                                padded[0].width == 72 && padded[0].height == 40;
			for (u32 level = 0; level < levels; ++level) {
				layout_matches &=
				    mip[level].offset == offsets[level] && mip[level].size == sizes[level];
			}
			Require(name, "fixed mip vector", layout_matches,
			        "the reverse-packed small-block mip layout changed");
		}

		u32  case_index       = 0;
		auto check_round_trip = [&](const char* stage, uint64_t tiled_size,
		                            std::span<const GpuTileInfo> infos) {
			uint64_t linear_size = 0;
			for (const auto& info: infos) {
				linear_size = std::max(linear_size, info.linear_offset + info.linear_size);
			}
			std::vector<uint8_t> tiled(tiled_size);
			std::vector<uint8_t> cpu(linear_size, 0);
			std::vector<uint8_t> gpu(linear_size, 0xab);
			fill(&tiled, ++case_index);
			for (const auto& info: infos) {
				convert_reference(false, &cpu, tiled, info);
			}
			gpu_detile(tiled, &gpu, tiled_size, linear_size, infos);
			compare((std::string(stage) + " detile bytes").c_str(), cpu, gpu);

			std::vector<uint8_t> linear(linear_size);
			std::vector<uint8_t> cpu_tiled(tiled_size, 0xab);
			std::vector<uint8_t> gpu_tiled(tiled_size, 0xab);
			fill(&linear, 0x280u + case_index);
			for (const auto& info: infos) {
				convert_reference(true, &cpu_tiled, linear, info);
			}
			gpu_tile(linear, &gpu_tiled, tiled_size, linear_size, infos);
			compare((std::string(stage) + " tile bytes").c_str(), cpu_tiled, gpu_tiled);
		};
		for (const auto family: families) {
			for (u32 bpe = 1; bpe <= family.max_bpe; bpe <<= 1u) {
				TileBlockLayout block {};
				Require(name, "block layout", TileGetBlockLayout(family.family, bpe, block),
				        "admitted family/BPE has no block layout");
				const bool     volume = block.block_depth > 1;
				const u32      width  = block.block_width * 3u + std::min(block.block_width, 3u);
				const u32      height = block.block_height * 3u + std::min(block.block_height, 3u);
				const u32      depth  = volume ? block.block_depth + 1u : 1u;
				const u32      pitch  = block.block_width * 4u;
				const uint64_t block_columns = (pitch + block.block_width - 1u) / block.block_width;
				const uint64_t block_rows = (height + block.block_height - 1u) / block.block_height;
				const uint64_t block_slices = (depth + block.block_depth - 1u) / block.block_depth;
				const uint64_t storage_size =
				    block_columns * block_rows * block_slices * block.block_size;
				const uint64_t slice_stride = static_cast<uint64_t>(pitch) * height * bpe;

				std::vector<uint8_t> tiled(storage_size);
				std::vector<uint8_t> cpu(storage_size, 0);
				std::vector<uint8_t> gpu(storage_size, 0xab);
				fill(&tiled, ++case_index);

				GpuTileInfo info {};
				info.family              = block.family;
				info.bytes_per_element   = block.bytes_per_element;
				info.linear_size         = storage_size;
				info.tiled_size          = storage_size;
				info.linear_slice_stride = volume ? slice_stride : 0;
				info.width               = width;
				info.height              = height;
				info.depth               = depth;
				info.pitch               = pitch;
				info.surface_z           = family.family == TileBlockFamily::RenderTarget64KB ||
				                                   family.family == TileBlockFamily::Depth64KB
				                               ? 3
				                               : 0;
				convert_reference(false, &cpu, tiled, info);
				gpu_detile(tiled, &gpu, storage_size, storage_size,
				           std::span<const GpuTileInfo>(&info, 1));
				const auto family_label = [&](const char* operation) {
					std::ostringstream out;
					out << operation << " family=" << static_cast<u32>(family.family)
					    << " bpe=" << bpe;
					return out.str();
				};
				compare(family_label("detile bytes").c_str(), cpu, gpu);

				{
					std::vector<uint8_t> linear(storage_size);
					std::vector<uint8_t> cpu_tiled(storage_size, 0xab);
					std::vector<uint8_t> gpu_tiled(storage_size, 0xab);
					fill(&linear, 0x80u + case_index);
					convert_reference(true, &cpu_tiled, linear, info);
					gpu_tile(linear, &gpu_tiled, storage_size, storage_size,
					         std::span<const GpuTileInfo>(&info, 1));
					compare(family_label("tile bytes").c_str(), cpu_tiled, gpu_tiled);
				}
			}
		}

		// Exercise the real texture-layout/info-building seam for every format
		// admitted by each standard tile mode.  Formats sharing a byte/block width
		// intentionally share a shader, but this loop still validates their
		// texel-to-element conversion (notably every BCn format).
		struct StandardMode {
			Prospero::TileMode tile;
			TileBlockFamily family;
		};
		constexpr StandardMode standard_modes[] = {
		    {Prospero::TileMode::kStandard256B,
		     TileBlockFamily::Standard256B},
		    {Prospero::TileMode::kStandard4KB,
		     TileBlockFamily::Standard4KB},
		    {Prospero::TileMode::kStandard64KB,
		     TileBlockFamily::Standard64KB},
		    {Prospero::TileMode::kPrt, TileBlockFamily::Prt64KB},
		};
		for (const auto format: {Prospero::BufferFormat::k8Srgb,
		                         Prospero::BufferFormat::k8_8Srgb,
		                         Prospero::BufferFormat::k9_9_9_5Float}) {
			for (const auto tile: {Prospero::TileMode::kDepth,
			                       Prospero::TileMode::kRenderTarget}) {
				TileTextureBlockLayout texture {};
				Require(name, "RT format policy",
				        !TileGetTextureBlockLayout(format, tile, false, texture),
				        "non-render-target format admitted by an RT/depth tile family");
			}
		}
		{
			TileSurfaceLayout invalid {};
			const TileSurfaceDescription description {
			    Prospero::BufferFormat::k32Float,
			    Prospero::TileMode::kStandard256B,
			    TileSurfaceDimension::Dim3D,
			    16,
			    16,
			    16,
			    1,
			    1,
			};
			Require(name, "3D tile policy", !TileGetTiledTextureLayout(description, invalid),
			        "Standard256B volume silently fell back to a 2D array layout");
		}
		{
			constexpr auto format =
			    Prospero::BufferFormat::kFmask8_S4_F4;
			constexpr auto tile = Prospero::TileMode::kStandard64KB;
			TileSizeAlign total {};
			TileGetTextureSize(format, 128, 128, 1, tile, &total, nullptr, nullptr);
			const auto layout = TextureCalcUploadLayout(format, 128, 128, 1, 1, tile, total.size,
			                                            false, false, name);
			const auto regions = TextureBuildImageCopies(layout);
			std::vector<GpuTileInfo> infos;
			Require(name, "FMASK policy",
			        !TextureBuildGpuTileInfos(total.size, regions, layout, 1, infos),
			        "FMASK entered the texel tiler through a non-depth family");
		}
		u32 format_cases = 0;
		for (u32 raw_format = 1;
		     raw_format <= static_cast<uint32_t>(Prospero::BufferFormat::kBc7Srgb);
		     ++raw_format) {
			const auto format = static_cast<Prospero::BufferFormat>(raw_format);
			// FMASK is synthesized as identity metadata by TextureUploadFmask; it is
			// deliberately not a texel surface and must never enter the GPU tiler.
			if (Prospero::IsFmaskTextureFormat(format)) {
				continue;
			}
			for (const auto& mode: standard_modes) {
				TileTextureBlockLayout texture {};
				if (!TileGetTextureBlockLayout(format, mode.tile, false, texture)) {
					continue;
				}
				const u32 bpe = texture.block.bytes_per_element;
				Require(name, "format block", texture.block.family == mode.family,
				        "CPU-supported format selected the wrong block family");

				constexpr u32 width  = 67;
				constexpr u32 height = 51;
				const u32     pitch  = TileGetTexturePitch(format, width, mode.tile);
				TileSizeAlign total {};
				TileGetTextureSize(format, width, height, 1, mode.tile, &total, nullptr,
				                   nullptr);
				Require(name, "format size", total.size != 0,
				        "supported format has an empty layout");

				const auto layout = TextureCalcUploadLayout(
				    format, width, height, 1, 1, mode.tile, total.size, false, false, name);
				const auto regions = TextureBuildImageCopies(layout);
				std::vector<GpuTileInfo> infos;
				if (!TextureBuildGpuTileInfos(total.size, regions, layout, 1, infos)) {
					std::ostringstream out;
					out << "format=" << raw_format
					    << " tile=" << static_cast<uint32_t>(mode.tile)
					    << " size=" << total.size << " pitch=" << pitch;
					Fail(name, "format infos", out.str());
				}
				Require(name, "format family",
				        infos.size() == 1 && infos[0].family == mode.family &&
				            infos[0].bytes_per_element == bpe,
				        "texture info selected the wrong shader family");

				std::vector<uint8_t> tiled(total.size);
				std::vector<uint8_t> cpu(total.size, 0);
				std::vector<uint8_t> gpu(total.size, 0xab);
				fill(&tiled, ++case_index);
				convert_reference(false, &cpu, tiled, infos[0]);
				gpu_detile(tiled, &gpu, total.size, total.size, infos);
				compare("format bytes", cpu, gpu);
				++format_cases;
			}
		}
		Require(name, "format coverage", format_cases != 0,
		        "no CPU-supported standard formats were tested");

		{
			constexpr auto format = Prospero::BufferFormat::k32_32_32Float;
			TileTextureElementLayout element {};
			Require(name, "RGB32 texture policy", !TileGetTextureElementLayout(format, element),
			        "12-byte texel layouts must be rejected");
		}

		{
			constexpr auto format = Prospero::BufferFormat::kBc1UNorm;
			constexpr auto tile = Prospero::TileMode::kStandard64KB;
			constexpr u32 width = 256, height = 256, levels = 9;
			TileSizeAlign total {};
			TileGetTextureSize(format, width, height, levels, tile, &total, nullptr,
			                   nullptr);
			const auto layout = TextureCalcUploadLayout(format, width, height, levels, 1, tile,
			                                            total.size, false, false, name);
			const auto regions = TextureBuildImageCopies(layout);
			std::vector<GpuTileInfo> infos;
			const bool built = TextureBuildGpuTileInfos(total.size, regions, layout, levels, infos);
			uint64_t linear_size = 0;
			for (const auto& info: infos) {
				linear_size = std::max(linear_size, info.linear_offset + info.linear_size);
			}
			Require(name, "BC1 mip-tail capacities",
			        built && total.size == 0x10000 && layout.surface.first_tail_level == 0 &&
			            linear_size == 0x15560 && linear_size > total.size,
			        "BC1 mip tail conflated tiled and linear capacities");
			check_round_trip("BC1 mip tail", total.size, infos);
		}

		{
			constexpr auto format = Prospero::BufferFormat::k32Float;
			constexpr auto tile   = Prospero::TileMode::kRenderTarget;
			constexpr u32 width = 129, height = 65, layers = 3;
			TileSizeAlign total {};
			TileGetTextureTotalSize(format, width, height, layers, 1, tile, false, total);
			const auto layout = TextureCalcUploadLayout(format, width, height, 1, layers, tile,
			                                            total.size, true, false, name);
			const auto regions = TextureBuildImageCopies(layout);
			std::vector<GpuTileInfo> infos;
			const bool built = TextureBuildGpuTileInfos(total.size, regions, layout, 1, infos);
			Require(name, "array infos",
			        built && infos.size() == layers && infos[0].surface_z == 0 &&
			            infos[1].surface_z == 1 && infos[2].surface_z == 2,
			        "array slices lost their absolute surface Z");
			check_round_trip("array", total.size, infos);
		}

		for (const auto& mode: standard_modes) {
			constexpr auto   format = Prospero::BufferFormat::k32Float;
			constexpr u32   levels = 2;
			TileBlockLayout block {};
			Require(name, "odd mip block", TileGetBlockLayout(mode.family, 4, block),
			        "odd multi-mip format has no block layout");
			const u32     width  = block.block_width * 2u + 1u;
			const u32     height = block.block_height * 2u + 1u;
			TileSizeAlign total {};
			TileGetTextureSize(format, width, height, levels, mode.tile, &total, nullptr,
			                   nullptr);
			const auto layout = TextureCalcUploadLayout(format, width, height, levels, 1, mode.tile,
			                                            total.size, false, false, name);
			const auto regions = TextureBuildImageCopies(layout);
			std::vector<GpuTileInfo> infos;
			Require(
			    name, "odd mip infos",
			    TextureBuildGpuTileInfos(total.size, regions, layout, levels, infos) &&
			        infos.size() == 2 && infos[1].tiled_width >= infos[1].pitch &&
			        infos[1].tiled_height >= infos[1].height &&
			        (mode.family == TileBlockFamily::Standard256B ||
			         infos[1].tiled_height > infos[1].height),
			    "odd multi-mip physical stride collapsed to the active linear "
			    "extent");
			check_round_trip("odd multi-mip", total.size, infos);
		}

		struct TailMode {
			Prospero::TileMode tile;
			TileBlockFamily family;
		};
		constexpr TailMode tail_modes[] = {
		    {Prospero::TileMode::kStandard4KB,
		     TileBlockFamily::Standard4KB},
		    {Prospero::TileMode::kStandard64KB,
		     TileBlockFamily::Standard64KB},
		    {Prospero::TileMode::kPrt, TileBlockFamily::Prt64KB},
		    {Prospero::TileMode::kRenderTarget,
		     TileBlockFamily::RenderTarget64KB},
		    {Prospero::TileMode::kDepth, TileBlockFamily::Depth64KB},
		};
		for (const auto& mode: tail_modes) {
			constexpr auto   format = Prospero::BufferFormat::k32Float;
			constexpr u32   levels = 7;
			TileBlockLayout block {};
			Require(name, "2D tail block", TileGetBlockLayout(mode.family, 4, block),
			        "2D tail mode has no block layout");
			const u32     width  = block.block_width * 2u;
			const u32     height = block.block_height * 2u;
			TileSizeAlign total {};
			TileGetTextureSize(format, width, height, levels, mode.tile, &total, nullptr,
			                   nullptr);
			const auto layout = TextureCalcUploadLayout(format, width, height, levels, 1, mode.tile,
			                                            total.size, true, false, name);
			const auto regions = TextureBuildImageCopies(layout);
			std::vector<GpuTileInfo> infos;
			Require(name, "2D mip tail seam",
			        layout.surface.first_tail_level == 2 &&
			            TextureBuildGpuTileInfos(total.size, regions, layout, levels, infos) &&
			            infos.size() == levels && !infos[0].tail && !infos[1].tail &&
			            std::all_of(infos.begin() + 2, infos.end(),
			                        [](const auto& info) { return info.tail; }),
			        "2D mip chain lost its linear/tiled tail boundary");
			check_round_trip("2D mip tail seam", total.size, infos);
		}

		{
			constexpr auto format = Prospero::BufferFormat::k32Float;
			constexpr auto tile   = Prospero::TileMode::kStandard4KB;
			constexpr u32 width  = 65;
			constexpr u32 height = 129;
			constexpr u32 depth  = 17;
			constexpr u32 levels = 2;
			TileSizeAlign total {};
			TileGetTextureTotalSize(format, width, height, depth, levels, tile, true, total);
			const auto layout = TextureCalcUploadLayout(format, width, height, levels, depth, tile,
			                                            total.size, false, true, name);
			const auto regions = TextureBuildImageCopies(layout);
			std::vector<GpuTileInfo> infos;
			const bool built = TextureBuildGpuTileInfos(total.size, regions, layout, levels, infos);
			Require(name, "3D mip infos",
			        regions.size() == depth + (depth >> 1u) && built && infos.size() == 4 &&
			            infos[0].depth == 8 && infos[1].depth == 8 && infos[2].depth == 1 &&
			            infos[3].depth == 8 && infos[0].tiled_offset == 0x19000 &&
			            infos[1].tiled_offset == 0x83000 && infos[2].tiled_offset == 0xed000 &&
			            infos[3].tiled_offset == 0 && infos[3].pitch == 32 &&
			            infos[3].height == 64 && infos[3].tiled_width == 40 &&
			            infos[3].tiled_height == 80,
			        "Standard4KB3D mip depth, packing, or physical stride changed");
			check_round_trip("3D mip", total.size, infos);
		}

		{
			constexpr auto format = Prospero::BufferFormat::kBc1UNorm;
			constexpr auto tile   = Prospero::TileMode::kStandard4KB;
			constexpr u32 width  = 65;
			constexpr u32 height = 129;
			constexpr u32 depth  = 17;
			constexpr u32 levels = 2;
			TileSizeAlign total {};
			TileGetTextureTotalSize(format, width, height, depth, levels, tile, true, total);
			const auto layout = TextureCalcUploadLayout(format, width, height, levels, depth, tile,
			                                            total.size, false, true, name);
			const auto regions = TextureBuildImageCopies(layout);
			std::vector<GpuTileInfo> infos;
			Require(name, "3D BC mip infos",
			        TextureBuildGpuTileInfos(total.size, regions, layout, levels, infos) &&
			            infos.size() == 4 && infos[3].pitch == 8 && infos[3].height == 16 &&
			            infos[3].tiled_width == 16 && infos[3].tiled_height == 24,
			        "block-compressed 3D mip lost its physical row or slice stride");
			check_round_trip("3D BC mip", total.size, infos);
		}

		{
			constexpr auto format = Prospero::BufferFormat::kBc3UNorm;
			constexpr auto tile   = Prospero::TileMode::kLinear;
			constexpr u32 width = 8, height = 8, levels = 4;
			TileSizeAlign total {};
			TileGetTextureSize(format, width, height, levels, tile, &total, nullptr,
			                   nullptr);
			const auto layout = TextureCalcUploadLayout(format, width, height, levels, 1, tile,
			                                            total.size, false, false, name);
			const auto regions = TextureBuildImageCopies(layout);
			Require(name, "linear BC native regions",
			        regions.size() == levels &&
			            std::all_of(regions.begin(), regions.end(),
			                        [](const auto& region) {
				                        return region.bufferRowLength >= region.imageExtent.width &&
				                               region.bufferImageHeight >=
				                                   region.imageExtent.height &&
				                               region.bufferRowLength % 4 == 0 &&
				                               region.bufferImageHeight % 4 == 0;
			                        }),
			        "linear BC mip tails emitted invalid Vulkan copy strides");
		}

		struct VolumeModeCase {
			Prospero::TileMode tile;
			TileBlockFamily family;
			Prospero::BufferFormat format;
		};
		constexpr VolumeModeCase volume_modes[] = {
		    {Prospero::TileMode::kStandard64KB,
		     TileBlockFamily::Standard64KB3D,
		     Prospero::BufferFormat::k32Float},
		    {Prospero::TileMode::kPrt, TileBlockFamily::Prt64KB3D,
		     Prospero::BufferFormat::k32Float},
		    {Prospero::TileMode::kRenderTarget,
		     TileBlockFamily::RenderTarget64KB,
		     Prospero::BufferFormat::k32Float},
		    {Prospero::TileMode::kDepth, TileBlockFamily::Depth64KB,
		     Prospero::BufferFormat::k32Float},
		    {Prospero::TileMode::kStandard64KB,
		     TileBlockFamily::Standard64KB3D,
		     Prospero::BufferFormat::kBc1UNorm},
		    {Prospero::TileMode::kPrt, TileBlockFamily::Prt64KB3D,
		     Prospero::BufferFormat::kBc3UNorm},
		};
		for (const auto test: volume_modes) {
			constexpr u32 width = 65, height = 33, depth = 37, levels = 6;
			TileSizeAlign total {};
			TileGetTextureTotalSize(test.format, width, height, depth, levels, test.tile, true,
			                        total);
			const auto layout =
			    TextureCalcUploadLayout(test.format, width, height, levels, depth, test.tile,
			                            total.size, true, true, name);
			const auto regions = TextureBuildImageCopies(layout);
			std::vector<GpuTileInfo> infos;
			const bool built = TextureBuildGpuTileInfos(total.size, regions, layout, levels, infos);
			const bool uses_z = test.family == TileBlockFamily::RenderTarget64KB ||
			                    test.family == TileBlockFamily::Depth64KB;
			Require(
			    name, "volume family infos",
			    built && !infos.empty() &&
			        std::all_of(infos.begin(), infos.end(),
			                    [&](const auto& info) { return info.family == test.family; }) &&
			        (!uses_z || std::any_of(infos.begin(), infos.end(),
			                                [](const auto& info) { return info.surface_z != 0; })),
			    "volume mode selected the wrong family or lost its surface Z");
			check_round_trip("volume family", total.size, infos);
		}

		struct VolumeTailCase {
			Prospero::BufferFormat format;
			u32                    bytes;
		};
		constexpr VolumeTailCase volume_tails[] = {
		    {Prospero::BufferFormat::k8UNorm, 1},
		    {Prospero::BufferFormat::k16UNorm, 2},
		    {Prospero::BufferFormat::k32Float, 4},
		    {Prospero::BufferFormat::k16_16_16_16Float, 8},
		    {Prospero::BufferFormat::k32_32_32_32Float, 16},
		    {Prospero::BufferFormat::kBc1UNorm, 8},
		    {Prospero::BufferFormat::kBc3UNorm, 16},
		};
		constexpr u32 volume_tail_xy[5][5][2] = {
		    {{0, 8}, {8, 4}, {8, 0}, {0, 4}, {0, 0}}, {{0, 8}, {4, 4}, {4, 0}, {0, 4}, {0, 0}},
		    {{0, 8}, {4, 4}, {4, 0}, {0, 4}, {0, 0}}, {{0, 4}, {4, 2}, {4, 0}, {0, 2}, {0, 0}},
		    {{0, 4}, {2, 2}, {2, 0}, {0, 2}, {0, 0}},
		};
		for (const auto& tail: volume_tails) {
			constexpr auto   tile   = Prospero::TileMode::kStandard4KB;
			constexpr u32   levels = 5;
			TileBlockLayout block {};
			Require(name, "3D tail block",
			        TileGetBlockLayout(TileBlockFamily::Standard4KB3D, tail.bytes, block),
			        "Standard4KB3D tail format has no block layout");
			const bool    compressed = Prospero::BlockCompressedBytesPerBlock(tail.format) != 0;
			const u32     width      = block.block_width * (compressed ? 4u : 1u);
			const u32     height     = block.block_height / 2u * (compressed ? 4u : 1u);
			const u32     depth      = block.block_depth + 1u;
			TileSizeAlign total {};
			TileGetTextureTotalSize(tail.format, width, height, depth, levels, tile, true,
			                        total);
			const auto layout = TextureCalcUploadLayout(tail.format, width, height, levels, depth,
			                                            tile, total.size, false, true, name);
			const auto regions = TextureBuildImageCopies(layout);
			std::vector<GpuTileInfo> infos;
			bool                     valid =
			    total.size == 8192 &&
			    regions.size() == depth + std::max(depth >> 1u, 1u) + std::max(depth >> 2u, 1u) +
			                          std::max(depth >> 3u, 1u) + std::max(depth >> 4u, 1u) &&
			    TextureBuildGpuTileInfos(total.size, regions, layout, levels, infos) &&
			    infos.size() == 6;
			const u32 table = std::countr_zero(tail.bytes);
			for (u32 level = 0; level < levels && valid; level++) {
				const auto& info = infos[level == 0 ? 0 : level + 1];
				valid &= info.tail && info.tail_x == volume_tail_xy[table][level][0] &&
				         info.tail_y == volume_tail_xy[table][level][1] && info.tiled_offset == 0;
			}
			if (valid) {
				valid = infos[1].tail && infos[1].tail_x == volume_tail_xy[table][0][0] &&
				        infos[1].tail_y == volume_tail_xy[table][0][1] &&
				        infos[1].tiled_offset == 4096;
			}
			Require(name, "3D mip tail infos", valid,
			        "Standard4KB3D mip tail coordinates or block-slice packing changed");
			check_round_trip("3D mip tail", total.size, infos);
		}

		for (const auto family:
		     {TileBlockFamily::Standard4KB, TileBlockFamily::Standard64KB, TileBlockFamily::Prt64KB,
		      TileBlockFamily::RenderTarget64KB, TileBlockFamily::Depth64KB}) {
			for (u32 bpe = 1; bpe <= 16; bpe <<= 1u) {
				if (family == TileBlockFamily::Depth64KB && bpe == 16) continue;
				TileBlockLayout block {};
				Require(name, "tail layout", TileGetBlockLayout(family, bpe, block),
				        "tail family/BPE has no block layout");
				const u32            width       = std::max(block.block_width / 4u, 1u);
				const u32            height      = std::max(block.block_height / 4u, 1u);
				const u32            x           = block.block_width / 2u;
				const u32            y           = block.block_height / 2u;
				const u32            pitch       = width;
				const uint64_t       linear_size = static_cast<uint64_t>(pitch) * height * bpe;
				std::vector<uint8_t> tiled(block.block_size);
				std::vector<uint8_t> cpu(linear_size, 0xcd);
				std::vector<uint8_t> gpu(linear_size, 0xab);
				fill(&tiled, ++case_index);
				GpuTileInfo info {};
				info.family            = block.family;
				info.bytes_per_element = block.bytes_per_element;
				info.linear_size       = linear_size;
				info.tiled_size        = block.block_size;
				info.width             = width;
				info.height            = height;
				info.pitch             = pitch;
				info.tail              = true;
				info.tail_x            = x;
				info.tail_y            = y;
				info.surface_z         = family == TileBlockFamily::RenderTarget64KB ||
				                                 family == TileBlockFamily::Depth64KB
				                             ? 2
				                             : 0;
				convert_reference(false, &cpu, tiled, info);
				gpu_detile(tiled, &gpu, block.block_size, linear_size,
				           std::span<const GpuTileInfo>(&info, 1));
				compare("tail bytes", cpu, gpu);

				std::vector<uint8_t> linear(linear_size);
				std::vector<uint8_t> cpu_tiled(block.block_size, 0xab);
				std::vector<uint8_t> gpu_tiled(block.block_size, 0xab);
				fill(&linear, 0x400u + case_index);
				convert_reference(true, &cpu_tiled, linear, info);
				gpu_tile(linear, &gpu_tiled, block.block_size, linear_size,
				         std::span<const GpuTileInfo>(&info, 1));
				compare("tail tile bytes", cpu_tiled, gpu_tiled);
			}
		}

		TileBlockLayout small_block {};
		Require(name, "small layout",
		        TileGetBlockLayout(TileBlockFamily::Standard256B, 4, small_block),
		        "small fixture layout is unavailable");
		std::vector<uint8_t> small_input(small_block.block_size);
		std::vector<uint8_t> small_expected(small_block.block_size, 0);
		std::vector<uint8_t> small_output(small_block.block_size, 0xab);
		fill(&small_input, 0xee);
		GpuTileInfo small_info {};
		small_info.family            = small_block.family;
		small_info.bytes_per_element = small_block.bytes_per_element;
		small_info.linear_size       = small_output.size();
		small_info.tiled_size        = small_input.size();
		small_info.width             = 1;
		small_info.height            = 1;
		small_info.pitch             = small_block.block_width;
		convert_reference(false, &small_expected, small_input, small_info);
		gpu_detile(small_input, &small_output, small_input.size(), small_output.size(),
		           std::span<const GpuTileInfo>(&small_info, 1));
		compare("small detile", small_expected, small_output);

		std::fill(small_output.begin(), small_output.end(), 0xab);
		gpu_detile(small_input, &small_output, small_input.size(), small_output.size(),
		           std::span<const GpuTileInfo>(&small_info, 1));
		compare("scheduler-owned reuse", small_expected, small_output);

		constexpr auto volume_format = Prospero::BufferFormat::k32UInt;
		constexpr auto volume_tile   = Prospero::TileMode::kLinear;
		constexpr u32 volume_width = 8, volume_height = 4, volume_depth = 5;
		constexpr u32 volume_levels = 3;
		const u32     volume_pitch =
		    TileGetTexturePitch(volume_format, volume_width, volume_tile);
		TileSizeAlign volume_size {};
		TileGetTextureTotalSize(volume_format, volume_width, volume_height, volume_depth,
		                        volume_levels, volume_tile, true, volume_size);
		const auto volume_layout = TextureCalcUploadLayout(
		    volume_format, volume_width, volume_height, volume_levels, volume_depth, volume_tile,
		    volume_size.size, true, true, name);
		const auto volume_copies = TextureBuildImageCopies(volume_layout);
		std::vector<u32>                    volume_source(volume_size.size / sizeof(u32), 0);
		std::vector<std::pair<size_t, u32>> volume_probes;
		for (const auto& copy: volume_copies) {
			for (u32 y = 0; y < copy.imageExtent.height; y++) {
				for (u32 x = 0; x < copy.imageExtent.width; x++) {
					const auto index =
					    (copy.bufferOffset +
					     (static_cast<uint64_t>(y) * copy.bufferRowLength + x) * sizeof(u32)) /
					    sizeof(u32);
					const u32 value = 0xa0000000u | (copy.imageSubresource.mipLevel << 24u) |
					                  (static_cast<u32>(copy.imageOffset.z) << 16u) | (y << 8u) | x;
					Require(name, "linear volume native bounds", index < volume_source.size(),
					        "linear volume copy escaped its padded guest layout");
					volume_source[index] = value;
					volume_probes.emplace_back(static_cast<size_t>(index), value);
				}
			}
		}
		auto volume_upload = CreateHostBuffer(name, volume_size.size,
		                                      vk::BufferUsageFlagBits::eTransferSrc, volume_source);
		auto volume_download =
		    CreateHostBuffer(name, volume_size.size, vk::BufferUsageFlagBits::eTransferDst,
		                     std::vector<u32>(volume_source.size(), 0));
		ImageInfo volume_info {};
		volume_info.data            = {0x10000, volume_size.size};
		volume_info.pixel_format    = vk::Format::eR32Uint;
		volume_info.guest_format    = volume_format;
		volume_info.type            = Prospero::ImageType::kColor3D;
		volume_info.extent          = {volume_width, volume_height, volume_depth};
		volume_info.resources       = {volume_levels, 1};
		volume_info.pitch           = volume_pitch;
		volume_info.bytes_per_block = sizeof(u32);
		volume_info.tile_mode       = volume_tile;
		for (u32 level = 0; level < volume_levels; level++) {
			volume_info.mip_layout[level] = {volume_layout.mips[level].offset,
			                                 volume_layout.mips[level].size,
			                                 volume_layout.mips[level].row_length,
			                                 volume_layout.mips[level].image_height};
		}
		Libs::Graphics::Image volume_image(m_runtime_context, scheduler, volume_info);
		volume_image.Upload(volume_copies, volume_upload.buffer, 0, volume_size.size);
		volume_image.Download(volume_copies, volume_download.buffer, 0, volume_size.size);
		scheduler.Finish();
		const auto volume_observed = ReadBuffer(name, volume_download, volume_source.size());
		Require(name, "linear volume native content",
		        std::ranges::all_of(volume_probes,
		                            [&](const auto& probe) {
			                            return volume_observed[probe.first] == probe.second;
		                            }),
		        "native upload/download did not preserve padded 3D mip rows");
		DestroyBuffer(&volume_download);
		DestroyBuffer(&volume_upload);
		std::printf("[gpu]     %-32s ok (%u cases, %u format/mode pairs)\n", name, case_index,
		            format_cases);
	}

private:
	RenderContext& Renderer() {
		EXIT_IF(m_renderer == nullptr);
		return *m_renderer;
	}

	void EnsureRuntimeContext() {
		if (m_runtime_context.allocator != nullptr) {
			return;
		}
		m_runtime_context.instance        = m_instance;
		m_runtime_context.physical_device = m_physical_device;
		m_runtime_context.device          = m_device;
		m_physical_device.getProperties(&m_runtime_context.physical_device_properties);
		m_runtime_context.physical_device_memory_properties = m_memory_properties;
		m_runtime_context.queue_family                      = m_queue_family;
		m_runtime_context.queue                             = m_queue;

		VmaVulkanFunctions functions {};
		functions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
		functions.vkGetDeviceProcAddr   = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

		VmaAllocatorCreateInfo allocator_info {};
		allocator_info.instance         = m_instance;
		allocator_info.physicalDevice   = m_physical_device;
		allocator_info.device           = m_device;
		allocator_info.pVulkanFunctions = &functions;
		allocator_info.vulkanApiVersion = VK_API_VERSION_1_3;
		RequireVk("VulkanHarness", "runtime context",
		          static_cast<vk::Result>(
		              vmaCreateAllocator(&allocator_info, &m_runtime_context.allocator)),
		          "vmaCreateAllocator");
		m_renderer = std::make_unique<RenderContext>(m_runtime_context);
	}

	void Init() {
		static vk::detail::DynamicLoader loader;
		const auto                       get_instance_proc_addr =
		    loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
		Require("VulkanHarness", "dispatch", get_instance_proc_addr != nullptr,
		        "could not load the Vulkan loader");
		VULKAN_HPP_DEFAULT_DISPATCHER.init(get_instance_proc_addr);

		vk::ApplicationInfo app {};
		app.sType            = vk::StructureType::eApplicationInfo;
		app.pApplicationName = "ShaderRecompilerComputeTests";
		app.apiVersion       = VK_API_VERSION_1_3;

		vk::InstanceCreateInfo instance_info {};
		instance_info.sType            = vk::StructureType::eInstanceCreateInfo;
		instance_info.pApplicationInfo = &app;
		RequireVk("VulkanHarness", "dispatch",
		          vk::createInstance(&instance_info, nullptr, &m_instance), "vkCreateInstance");
		VULKAN_HPP_DEFAULT_DISPATCHER.init(m_instance);

		u32 physical_count = 0;
		RequireVk("VulkanHarness", "dispatch",
		          m_instance.enumeratePhysicalDevices(&physical_count, nullptr),
		          "vkEnumeratePhysicalDevices");
		Require("VulkanHarness", "dispatch", physical_count != 0, "no Vulkan physical devices");
		std::vector<vk::PhysicalDevice> physical_devices(physical_count);
		RequireVk("VulkanHarness", "dispatch",
		          m_instance.enumeratePhysicalDevices(&physical_count, physical_devices.data()),
		          "vkEnumeratePhysicalDevices");

		for (auto physical: physical_devices) {
			u32 queue_count = 0;
			physical.getQueueFamilyProperties(&queue_count, nullptr);
			std::vector<vk::QueueFamilyProperties> queues(queue_count);
			physical.getQueueFamilyProperties(&queue_count, queues.data());
			for (u32 i = 0; i < queue_count; i++) {
				if ((queues[i].queueFlags &
				     (vk::QueueFlagBits::eCompute | vk::QueueFlagBits::eGraphics)) ==
				    (vk::QueueFlagBits::eCompute | vk::QueueFlagBits::eGraphics)) {
					m_physical_device = physical;
					m_queue_family    = i;
					break;
				}
			}
			if (m_physical_device != nullptr) {
				break;
			}
		}
		Require("VulkanHarness", "dispatch", m_physical_device != nullptr,
		        "no Vulkan graphics+compute queue family");
		m_physical_device.getMemoryProperties(&m_memory_properties);

		vk::PhysicalDeviceFeatures available_features {};
		m_physical_device.getFeatures(&available_features);
		vk::PhysicalDeviceVulkan12Features available_features12 {};
		available_features12.sType = vk::StructureType::ePhysicalDeviceVulkan12Features;
		vk::PhysicalDeviceVulkan13Features available_features13 {};
		available_features13.sType = vk::StructureType::ePhysicalDeviceVulkan13Features;
		available_features13.pNext = &available_features12;
		vk::PhysicalDeviceFeatures2 available_features2 {};
		available_features2.sType = vk::StructureType::ePhysicalDeviceFeatures2;
		available_features2.pNext = &available_features13;
		m_physical_device.getFeatures2(&available_features2);
		Require("VulkanHarness", "dispatch",
		        available_features.shaderStorageImageWriteWithoutFormat == true,
		        "shaderStorageImageWriteWithoutFormat is not supported");
		Require("VulkanHarness", "dispatch",
		        available_features.shaderStorageImageReadWithoutFormat == true,
		        "shaderStorageImageReadWithoutFormat is not supported");
		Require("VulkanHarness", "dispatch", available_features12.timelineSemaphore == true,
		        "timeline semaphores are not supported");
		Require("VulkanHarness", "dispatch", available_features13.dynamicRendering == true,
		        "dynamic rendering is not supported");
		Require("VulkanHarness", "dispatch", available_features13.synchronization2 == true,
		        "synchronization2 is not supported");
		Require("VulkanHarness", "dispatch", available_features.sampleRateShading == true,
		        "sample-rate shading is not supported");

		float                     priority = 1.0f;
		vk::DeviceQueueCreateInfo queue_info {};
		queue_info.sType            = vk::StructureType::eDeviceQueueCreateInfo;
		queue_info.queueFamilyIndex = m_queue_family;
		queue_info.queueCount       = 1;
		queue_info.pQueuePriorities = &priority;

		vk::DeviceCreateInfo device_info {};
		device_info.sType                = vk::StructureType::eDeviceCreateInfo;
		device_info.queueCreateInfoCount = 1;
		device_info.pQueueCreateInfos    = &queue_info;
		vk::PhysicalDeviceVulkan12Features device_features12 {};
		device_features12.sType             = vk::StructureType::ePhysicalDeviceVulkan12Features;
		device_features12.timelineSemaphore = true;
		vk::PhysicalDeviceVulkan13Features device_features13 {};
		device_features13.sType            = vk::StructureType::ePhysicalDeviceVulkan13Features;
		device_features13.pNext            = &device_features12;
		device_features13.dynamicRendering = true;
		device_features13.synchronization2 = true;
		device_info.pNext                  = &device_features13;
		vk::PhysicalDeviceFeatures device_features {};
		device_features.shaderStorageImageWriteWithoutFormat = true;
		device_features.shaderStorageImageReadWithoutFormat  = true;
		device_features.sampleRateShading                    = true;
		device_info.pEnabledFeatures                         = &device_features;
		constexpr const char* device_extensions[] = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
		device_info.enabledExtensionCount         = std::size(device_extensions);
		device_info.ppEnabledExtensionNames       = device_extensions;
		RequireVk("VulkanHarness", "dispatch",
		          m_physical_device.createDevice(&device_info, nullptr, &m_device),
		          "vkCreateDevice");
		VULKAN_HPP_DEFAULT_DISPATCHER.init(m_device);
		m_device.getQueue(m_queue_family, 0, &m_queue);

		vk::CommandPoolCreateInfo pool_info {};
		pool_info.sType            = vk::StructureType::eCommandPoolCreateInfo;
		pool_info.queueFamilyIndex = m_queue_family;
		pool_info.flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
		RequireVk("VulkanHarness", "dispatch",
		          m_device.createCommandPool(&pool_info, nullptr, &m_command_pool),
		          "vkCreateCommandPool");
	}

	void Destroy() {
		if (m_device != nullptr) {
			RequireVulkanSuccess(m_device.waitIdle(), "vkDeviceWaitIdle");
			if (m_runtime_context.allocator != nullptr) {
				m_renderer.reset();
				vmaDestroyAllocator(m_runtime_context.allocator);
				m_runtime_context.allocator = nullptr;
			}
			if (m_command_pool != nullptr) {
				m_device.destroyCommandPool(m_command_pool, nullptr);
			}
			m_device.destroy(nullptr);
		}
		if (m_instance != nullptr) {
			m_instance.destroy(nullptr);
		}
	}

	bool FindMemoryType(u32 type_bits, vk::MemoryPropertyFlags required, u32* index) const {
		for (u32 i = 0; i < m_memory_properties.memoryTypeCount; i++) {
			if ((type_bits & (1u << i)) == 0) {
				continue;
			}
			if ((m_memory_properties.memoryTypes[i].propertyFlags & required) == required) {
				*index = i;
				return true;
			}
		}
		return false;
	}

	static vk::AccessFlags AccessForLayout(vk::ImageLayout layout) {
		switch (layout) {
			case vk::ImageLayout::eTransferDstOptimal: return vk::AccessFlagBits::eTransferWrite;
			case vk::ImageLayout::eTransferSrcOptimal: return vk::AccessFlagBits::eTransferRead;
			case vk::ImageLayout::eShaderReadOnlyOptimal: return vk::AccessFlagBits::eShaderRead;
			case vk::ImageLayout::eGeneral:
				return vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
			default: return {};
		}
	}

	vk::CommandBuffer BeginCommands(const char* shader_name, const char* stage) {
		vk::CommandBufferAllocateInfo cmd_alloc {};
		cmd_alloc.sType              = vk::StructureType::eCommandBufferAllocateInfo;
		cmd_alloc.commandPool        = m_command_pool;
		cmd_alloc.level              = vk::CommandBufferLevel::ePrimary;
		cmd_alloc.commandBufferCount = 1;
		vk::CommandBuffer cmd        = nullptr;
		RequireVk(shader_name, stage, m_device.allocateCommandBuffers(&cmd_alloc, &cmd),
		          "vkAllocateCommandBuffers");

		vk::CommandBufferBeginInfo begin {};
		begin.sType = vk::StructureType::eCommandBufferBeginInfo;
		begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
		RequireVk(shader_name, stage, cmd.begin(&begin), "vkBeginCommandBuffer");
		return cmd;
	}

	void EndSubmitAndFree(const char* shader_name, const char* stage, vk::CommandBuffer cmd) {
		RequireVk(shader_name, stage, cmd.end(), "vkEndCommandBuffer");

		vk::FenceCreateInfo fence_info {};
		fence_info.sType = vk::StructureType::eFenceCreateInfo;
		vk::Fence fence  = nullptr;
		RequireVk(shader_name, stage, m_device.createFence(&fence_info, nullptr, &fence),
		          "vkCreateFence");

		vk::SubmitInfo submit {};
		submit.sType              = vk::StructureType::eSubmitInfo;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers    = &cmd;
		RequireVk(shader_name, stage, m_queue.submit(1, &submit, fence), "vkQueueSubmit");
		RequireVk(shader_name, stage, m_device.waitForFences(1, &fence, true, UINT64_MAX),
		          "vkWaitForFences");

		m_device.destroyFence(fence, nullptr);
		m_device.freeCommandBuffers(m_command_pool, 1, &cmd);
	}

	void AddImageBarrier(vk::CommandBuffer cmd, vk::Image image, vk::ImageLayout old_layout,
	                     vk::ImageLayout new_layout, vk::PipelineStageFlags src_stage,
	                     vk::PipelineStageFlags dst_stage, vk::AccessFlags src_access,
	                     vk::AccessFlags dst_access, u32 mip_levels = 1, u32 layers = 1) {
		vk::ImageMemoryBarrier barrier {};
		barrier.sType                           = vk::StructureType::eImageMemoryBarrier;
		barrier.srcAccessMask                   = src_access;
		barrier.dstAccessMask                   = dst_access;
		barrier.oldLayout                       = old_layout;
		barrier.newLayout                       = new_layout;
		barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		barrier.image                           = image;
		barrier.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
		barrier.subresourceRange.baseMipLevel   = 0;
		barrier.subresourceRange.levelCount     = mip_levels;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount     = layers;
		cmd.pipelineBarrier(src_stage, dst_stage, {}, 0, nullptr, 0, nullptr, 1, &barrier);
	}

	void TransitionImage(const char* shader_name, Image* image, vk::ImageLayout new_layout,
	                     vk::PipelineStageFlags src_stage, vk::PipelineStageFlags dst_stage,
	                     vk::AccessFlags src_access, vk::AccessFlags dst_access) {
		vk::CommandBuffer cmd = BeginCommands(shader_name, "dispatch");
		AddImageBarrier(cmd, image->image, image->layout, new_layout, src_stage, dst_stage,
		                src_access, dst_access, image->mip_levels, image->layers);
		EndSubmitAndFree(shader_name, "dispatch", cmd);
		image->layout = new_layout;
	}

	void UploadImage(const char* shader_name, Image* image, vk::Buffer staging,
	                 vk::ImageLayout final_layout) {
		UploadImageMips(shader_name, image, staging, final_layout);
	}

	void UploadImageMips(const char* shader_name, Image* image, vk::Buffer staging,
	                     vk::ImageLayout final_layout) {
		vk::CommandBuffer cmd = BeginCommands(shader_name, "dispatch");
		AddImageBarrier(cmd, image->image, vk::ImageLayout::eUndefined,
		                vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits::eTopOfPipe,
		                vk::PipelineStageFlagBits::eTransfer, {},
		                vk::AccessFlagBits::eTransferWrite, image->mip_levels, image->layers);
		std::vector<vk::BufferImageCopy> copies;
		copies.reserve(image->mip_levels);
		vk::DeviceSize offset = 0;
		for (u32 level = 0; level < image->mip_levels; level++) {
			vk::BufferImageCopy copy {};
			copy.bufferOffset                    = offset;
			copy.imageSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
			copy.imageSubresource.mipLevel       = level;
			copy.imageSubresource.baseArrayLayer = 0;
			copy.imageSubresource.layerCount     = image->layers;
			copy.imageExtent.width               = MipExtent(image->width, level);
			copy.imageExtent.height              = MipExtent(image->height, level);
			copy.imageExtent.depth               = 1;
			copies.push_back(copy);
			offset += static_cast<vk::DeviceSize>(ImageMipDwordCount(image->width, image->height,
			                                                         image->dwords_per_pixel, level,
			                                                         image->layers) *
			                                      sizeof(u32));
		}
		cmd.copyBufferToImage(staging, image->image, vk::ImageLayout::eTransferDstOptimal,
		                      static_cast<u32>(copies.size()), copies.data());
		AddImageBarrier(cmd, image->image, vk::ImageLayout::eTransferDstOptimal, final_layout,
		                vk::PipelineStageFlagBits::eTransfer,
		                vk::PipelineStageFlagBits::eComputeShader,
		                vk::AccessFlagBits::eTransferWrite, AccessForLayout(final_layout),
		                image->mip_levels, image->layers);
		EndSubmitAndFree(shader_name, "dispatch", cmd);
		image->layout = final_layout;
	}

	Buffer CreateHostBuffer(const char* shader_name, vk::DeviceSize size,
	                        vk::BufferUsageFlags usage, const std::vector<u32>& contents) {
		Buffer ret;
		ret.size = std::max<vk::DeviceSize>(size, sizeof(u32));

		vk::BufferCreateInfo buffer_info {};
		buffer_info.sType       = vk::StructureType::eBufferCreateInfo;
		buffer_info.size        = ret.size;
		buffer_info.usage       = usage;
		buffer_info.sharingMode = vk::SharingMode::eExclusive;
		RequireVk(shader_name, "dispatch",
		          m_device.createBuffer(&buffer_info, nullptr, &ret.buffer), "vkCreateBuffer");

		vk::MemoryRequirements req {};
		m_device.getBufferMemoryRequirements(ret.buffer, &req);
		u32 memory_type = 0;
		ret.coherent    = FindMemoryType(req.memoryTypeBits,
		                                 vk::MemoryPropertyFlagBits::eHostVisible |
		                                     vk::MemoryPropertyFlagBits::eHostCoherent,
		                                 &memory_type);
		if (!ret.coherent) {
			Require(shader_name, "dispatch",
			        FindMemoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible,
			                       &memory_type),
			        "no host-visible memory type for staging buffer");
		}

		vk::MemoryAllocateInfo alloc {};
		alloc.sType           = vk::StructureType::eMemoryAllocateInfo;
		alloc.allocationSize  = req.size;
		alloc.memoryTypeIndex = memory_type;
		RequireVk(shader_name, "dispatch", m_device.allocateMemory(&alloc, nullptr, &ret.memory),
		          "vkAllocateMemory");
		RequireVk(shader_name, "dispatch", m_device.bindBufferMemory(ret.buffer, ret.memory, 0),
		          "vkBindBufferMemory");
		if (!contents.empty()) {
			WriteBuffer(shader_name, ret, contents);
		}
		return ret;
	}

	void WriteBuffer(const char* shader_name, const Buffer& buffer,
	                 const std::vector<u32>& contents) {
		void* data = nullptr;
		RequireVk(shader_name, "dispatch",
		          m_device.mapMemory(buffer.memory, 0, buffer.size, {}, &data), "vkMapMemory");
		std::memcpy(data, contents.data(), contents.size() * sizeof(u32));
		if (!buffer.coherent) {
			vk::MappedMemoryRange range {};
			range.sType  = vk::StructureType::eMappedMemoryRange;
			range.memory = buffer.memory;
			range.offset = 0;
			range.size   = VK_WHOLE_SIZE;
			RequireVk(shader_name, "dispatch", m_device.flushMappedMemoryRanges(1, &range),
			          "vkFlushMappedMemoryRanges");
		}
		m_device.unmapMemory(buffer.memory);
	}

	vk::Instance                       m_instance        = nullptr;
	vk::PhysicalDevice                 m_physical_device = nullptr;
	vk::Device                         m_device          = nullptr;
	vk::Queue                          m_queue           = nullptr;
	vk::CommandPool                    m_command_pool    = nullptr;
	u32                                m_queue_family    = 0;
	vk::PhysicalDeviceMemoryProperties m_memory_properties {};
	GraphicContext                     m_runtime_context {};
	std::unique_ptr<RenderContext>     m_renderer;
};

void CompareWords(const TestCase& test, const char* stage, const std::vector<u32>& expected,
                  const std::vector<u32>& actual) {
	if (actual == expected) {
		return;
	}
	std::ostringstream out;
	out << "expected [";
	for (size_t i = 0; i < expected.size(); i++) {
		out << (i == 0 ? "" : ", ") << Hex(expected[i]);
	}
	out << "] actual [";
	for (size_t i = 0; i < actual.size(); i++) {
		out << (i == 0 ? "" : ", ") << Hex(actual[i]);
	}
	out << "]";
	Fail(test.name, stage, out.str());
}

void CompareGraphicsWords(const GraphicsCase& test, const std::vector<u32>& actual) {
	if (actual == test.expected_pixel) {
		return;
	}
	std::ostringstream out;
	out << "expected [";
	for (size_t i = 0; i < test.expected_pixel.size(); i++) {
		out << (i == 0 ? "" : ", ") << Hex(test.expected_pixel[i]);
	}
	out << "] actual [";
	for (size_t i = 0; i < actual.size(); i++) {
		out << (i == 0 ? "" : ", ") << Hex(actual[i]);
	}
	out << "]";
	Fail(test.name, "graphics readback", out.str());
}

void RunCase(VulkanHarness* vulkan, const TestCase& test) {
	auto compiled = CompileCase(test);
	if (test.image_descriptor_swizzle != DstSel(4, 5, 6, 7)) {
		Require(test.name, "resource specialization",
		        !compiled.program.info.images.empty() &&
		            compiled.program.info.images[0].storage_swizzle ==
		                test.image_descriptor_swizzle,
		        "storage image descriptor swizzle did not reach the specialized "
		        "program");
	}
	if (test.compile_only) {
		std::printf("[compute] %-32s ok\n", test.name);
		return;
	}
	const auto dwords =
	    std::max<size_t>({test.initial.size(), test.expected.size(), static_cast<size_t>(1)});
	auto buffer = vulkan->CreateStorageBuffer(test.name, test.initial, dwords);

	using Kind = ShaderRecompiler::IR::DescriptorBindingKind;
	auto Has   = [&](Kind kind) {
		return ShaderRecompiler::IR::FindBinding(compiled.program.bindings, kind) != nullptr;
	};
	VulkanHarness::Image  sampled_image;
	VulkanHarness::Image  storage_image;
	VulkanHarness::Image  storage_image_uint;
	VulkanHarness::Buffer gds_buffer;
	vk::Sampler           sampler  = nullptr;
	const bool needs_sampled_image = Has(Kind::Sampled1D) || Has(Kind::Sampled1DArray) ||
	                                 Has(Kind::Sampled2D) || Has(Kind::Sampled2DArray) ||
	                                 Has(Kind::Sampled3D) || Has(Kind::SampledUint2D) ||
	                                 Has(Kind::SampledUint1D) || Has(Kind::SampledUint1DArray) ||
	                                 Has(Kind::SampledUint2DArray) || Has(Kind::SampledUint3D);
	const bool needs_storage_image = Has(Kind::Storage1D) || Has(Kind::Storage1DArray) ||
	                                 Has(Kind::Storage2D) || Has(Kind::Storage2DArray) ||
	                                 Has(Kind::Storage3D) || Has(Kind::StorageUint2D) ||
	                                 Has(Kind::StorageUint1D) || Has(Kind::StorageUint1DArray) ||
	                                 Has(Kind::StorageUint2DArray) || Has(Kind::StorageUint3D);
	const bool needs_sampler       = Has(Kind::Samplers);
	const bool needs_gds           = Has(Kind::Gds);
	if (needs_gds) {
		const auto gds_dwords =
		    std::max<size_t>({test.gds_initial.size(), test.expected_gds.size(), 1u});
		gds_buffer = vulkan->CreateStorageBuffer(test.name, test.gds_initial, gds_dwords);
	}
	if (needs_sampled_image) {
		auto sampled_mips             = test.sampled_image_rgba_mips;
		auto sampled_format           = test.sampled_image_format;
		auto sampled_dwords_per_pixel = test.sampled_image_dwords_per_pixel;
		if (!test.sampled_image_rgba_mips.empty()) {
			sampled_format           = vk::Format::eR32G32B32A32Sfloat;
			sampled_dwords_per_pixel = 4;
		} else if (!test.sampled_image_rgba.empty()) {
			sampled_mips.push_back(test.sampled_image_rgba);
		}
		sampled_image = vulkan->CreateImageMips(
		    test.name, test.image_width, test.image_height, sampled_format,
		    vk::ImageUsageFlagBits::eSampled, sampled_mips, sampled_dwords_per_pixel,
		    vk::ImageLayout::eShaderReadOnlyOptimal, test.sampled_image_type,
		    test.sampled_image_view_type, test.sampled_image_layers,
		    test.sampled_image_view_base_layer, test.sampled_image_view_layers);
	}
	if (needs_storage_image) {
		storage_image = vulkan->CreateImage2D(
		    test.name, test.image_width, test.image_height, test.storage_image_format,
		    vk::ImageUsageFlagBits::eStorage, test.storage_image_rgba,
		    test.storage_image_dwords_per_pixel, vk::ImageLayout::eGeneral);
		storage_image_uint =
		    vulkan->CreateImage2D(test.name, test.image_width, test.image_height,
		                          vk::Format::eR32Uint, vk::ImageUsageFlagBits::eStorage,
		                          test.storage_image_r32ui, 1, vk::ImageLayout::eGeneral);
	}
	if (needs_sampler) {
		sampler = vulkan->CreateNearestSampler(test.name);
	}

	vulkan->Dispatch(test, compiled, buffer, needs_gds ? &gds_buffer : nullptr,
	                 needs_sampled_image ? &sampled_image : nullptr,
	                 needs_storage_image ? &storage_image : nullptr,
	                 needs_storage_image ? &storage_image_uint : nullptr, sampler);
	auto actual = vulkan->ReadBuffer(test.name, buffer, test.expected.size());
	if (!test.expected_gds.empty()) {
		const auto gds_actual = vulkan->ReadBuffer(test.name, gds_buffer, test.expected_gds.size());
		CompareWords(test, "GDS readback", test.expected_gds, gds_actual);
	}
	if (!test.expected_storage_image_rgba.empty()) {
		auto image_actual = vulkan->ReadImage(test.name, &storage_image);
		image_actual.resize(test.expected_storage_image_rgba.size());
		CompareWords(test, "storage image readback", test.expected_storage_image_rgba,
		             image_actual);
	}
	if (!test.expected_storage_image_r32ui.empty()) {
		auto image_actual = vulkan->ReadImage(test.name, &storage_image_uint);
		image_actual.resize(test.expected_storage_image_r32ui.size());
		CompareWords(test, "uint storage image readback", test.expected_storage_image_r32ui,
		             image_actual);
	}
	if (sampler != nullptr) {
		vulkan->Device().destroySampler(sampler, nullptr);
	}
	vulkan->DestroyImage(&sampled_image);
	vulkan->DestroyImage(&storage_image);
	vulkan->DestroyImage(&storage_image_uint);
	vulkan->DestroyBuffer(&gds_buffer);
	vulkan->DestroyBuffer(&buffer);
	CompareWords(test, "readback", test.expected, actual);
	std::printf("[compute] %-32s ok\n", test.name);
}

void RunGraphicsCase(VulkanHarness* vulkan, const GraphicsCase& test) {
	auto compiled = CompileFragmentCase(test);
	auto actual   = vulkan->RenderFragment(test, compiled);
	CompareGraphicsWords(test, actual);
	std::printf("[graphics] %-31s ok\n", test.name);
}

enum class CoverageClass {
	Covered,
	ControlOrMarker,
	NeedsAluCase,
	NeedsFloatCase,
	NeedsMemoryCase,
	NeedsImageCase,
	NeedsGraphicsStageCase,
};

bool IsCovered(const std::set<ShaderOpcode>& covered, ShaderOpcode opcode) {
	return covered.find(opcode) != covered.end();
}

CoverageClass ClassifyOpcode(ShaderOpcode opcode, const std::set<ShaderOpcode>& covered) {
	using ShaderRecompiler::Decoder::Opcode;

	if (IsCovered(covered, opcode)) {
		return CoverageClass::Covered;
	}

	switch (opcode) {
		case Opcode::SGetpcB64:
		case Opcode::SSetpcB64:
		case Opcode::SNop:
		case Opcode::SWaitcnt:
		case Opcode::SBarrier:
		case Opcode::SBranch:
		case Opcode::SCbranchScc0:
		case Opcode::SCbranchScc1:
		case Opcode::SCbranchVccz:
		case Opcode::SCbranchVccnz:
		case Opcode::SCbranchExecz:
		case Opcode::SCbranchExecnz:
		case Opcode::SSendmsg:
		case Opcode::SSetregB32:
		case Opcode::SSleep:
		case Opcode::STtraceData:
		case Opcode::SInstPrefetch:
		case Opcode::SEndpgm: return CoverageClass::ControlOrMarker;

		case Opcode::VAddF32:
		case Opcode::VSubF32:
		case Opcode::VSubrevF32:
		case Opcode::VMulF32:
		case Opcode::VMacF32:
		case Opcode::VMadmkF32:
		case Opcode::VMadakF32:
		case Opcode::VMinF32:
		case Opcode::VMaxF32:
		case Opcode::VMadF32:
		case Opcode::VFmaF32:
		case Opcode::VMin3F32:
		case Opcode::VMax3F32:
		case Opcode::VMed3F32:
		case Opcode::VDot2cF32F16:
		case Opcode::VCvtF32I32:
		case Opcode::VCvtF32U32:
		case Opcode::VCvtU32F32:
		case Opcode::VCvtI32F32:
		case Opcode::VCvtF16F32:
		case Opcode::VCvtF32F16:
		case Opcode::VCvtU16F16:
		case Opcode::VCvtRpiI32F32:
		case Opcode::VCvtFlrI32F32:
		case Opcode::VCvtOffF32I4:
		case Opcode::VCvtF32Ubyte0:
		case Opcode::VCvtF32Ubyte1:
		case Opcode::VCvtF32Ubyte2:
		case Opcode::VCvtF32Ubyte3:
		case Opcode::VRcpF32:
		case Opcode::VFractF32:
		case Opcode::VTruncF32:
		case Opcode::VCeilF32:
		case Opcode::VRndneF32:
		case Opcode::VFloorF32:
		case Opcode::VExpF32:
		case Opcode::VLogF32:
		case Opcode::VRsqF32:
		case Opcode::VSqrtF32:
		case Opcode::VSinF32:
		case Opcode::VCosF32:
		case Opcode::VCubeidF32:
		case Opcode::VCubescF32:
		case Opcode::VCubetcF32:
		case Opcode::VCubemaF32:
		case Opcode::VLdexpF32:
		case Opcode::VCvtPkU8F32:
		case Opcode::VCvtPknormI16F32:
		case Opcode::VCvtPknormU16F32:
		case Opcode::VCvtPkrtzF16F32:
		case Opcode::VPkAddF16:
		case Opcode::VPkMulF16:
		case Opcode::VPkMinF16:
		case Opcode::VPkMaxF16:
		case Opcode::VPkFmaF16:
		case Opcode::VAddF16:
		case Opcode::VSubF16:
		case Opcode::VSubrevF16:
		case Opcode::VMulF16:
		case Opcode::VMaxF16:
		case Opcode::VMinF16:
		case Opcode::VMin3F16:
		case Opcode::VMax3F16:
		case Opcode::VMed3F16:
		case Opcode::VRcpF16:
		case Opcode::VRsqF16:
		case Opcode::VLogF16:
		case Opcode::VExpF16:
		case Opcode::VMadMixloF16:
		case Opcode::VMadMixhiF16:
		case Opcode::DsMinF32:
		case Opcode::DsMaxF32:
		case Opcode::VCmpFF32:
		case Opcode::VCmpLtF32:
		case Opcode::VCmpEqF32:
		case Opcode::VCmpLeF32:
		case Opcode::VCmpGtF32:
		case Opcode::VCmpLgF32:
		case Opcode::VCmpGeF32:
		case Opcode::VCmpOF32:
		case Opcode::VCmpUF32:
		case Opcode::VCmpNgeF32:
		case Opcode::VCmpNlgF32:
		case Opcode::VCmpNgtF32:
		case Opcode::VCmpNleF32:
		case Opcode::VCmpNeqF32:
		case Opcode::VCmpNltF32:
		case Opcode::VCmpTruF32:
		case Opcode::VCmpxLtF32:
		case Opcode::VCmpxEqF32:
		case Opcode::VCmpxLeF32:
		case Opcode::VCmpxGtF32:
		case Opcode::VCmpxLgF32:
		case Opcode::VCmpxGeF32:
		case Opcode::VCmpxNgeF32:
		case Opcode::VCmpxNlgF32:
		case Opcode::VCmpxNgtF32:
		case Opcode::VCmpxNleF32:
		case Opcode::VCmpxNeqF32:
		case Opcode::VCmpxNltF32:
		case Opcode::VCmpClassF32:
		case Opcode::VCmpLtF16:
		case Opcode::VCmpEqF16:
		case Opcode::VCmpLeF16:
		case Opcode::VCmpGtF16:
		case Opcode::VCmpLgF16:
		case Opcode::VCmpGeF16:
		case Opcode::VCmpNeqF16:
		case Opcode::VCmpxLtF16:
		case Opcode::VCmpxEqF16:
		case Opcode::VCmpxLeF16:
		case Opcode::VCmpxGtF16:
		case Opcode::VCmpxGeF16:
		case Opcode::VCmpxNeqF16:
		case Opcode::VCmpxNltF16: return CoverageClass::NeedsFloatCase;

		case Opcode::SLoadDword:
		case Opcode::SLoadDwordx2:
		case Opcode::SLoadDwordx4:
		case Opcode::SLoadDwordx8:
		case Opcode::SLoadDwordx16:
		case Opcode::SBufferLoadDword:
		case Opcode::SBufferLoadDwordx2:
		case Opcode::SBufferLoadDwordx4:
		case Opcode::SBufferLoadDwordx8:
		case Opcode::SBufferLoadDwordx16:
		case Opcode::BufferLoadFormatX:
		case Opcode::BufferLoadFormatXy:
		case Opcode::BufferLoadFormatXyz:
		case Opcode::BufferLoadFormatXyzw:
		case Opcode::BufferStoreFormatX:
		case Opcode::BufferStoreFormatXy:
		case Opcode::BufferStoreFormatXyz:
		case Opcode::BufferStoreFormatXyzw:
		case Opcode::BufferLoadUbyte:
		case Opcode::BufferLoadSbyte:
		case Opcode::BufferLoadUshort:
		case Opcode::BufferLoadSshort:
		case Opcode::BufferLoadDwordx2:
		case Opcode::BufferLoadDwordx3:
		case Opcode::BufferLoadDwordx4:
		case Opcode::BufferStoreByte:
		case Opcode::BufferStoreShort:
		case Opcode::BufferStoreDwordx2:
		case Opcode::BufferStoreDwordx3:
		case Opcode::BufferStoreDwordx4:
		case Opcode::TBufferLoadFormatX:
		case Opcode::TBufferLoadFormatXy:
		case Opcode::TBufferLoadFormatXyz:
		case Opcode::TBufferLoadFormatXyzw:
		case Opcode::TBufferStoreFormatX:
		case Opcode::TBufferStoreFormatXy:
		case Opcode::TBufferStoreFormatXyz:
		case Opcode::TBufferStoreFormatXyzw:
		case Opcode::BufferAtomicSwap:
		case Opcode::BufferAtomicAdd:
		case Opcode::BufferAtomicSub:
		case Opcode::BufferAtomicSMin:
		case Opcode::BufferAtomicUMin:
		case Opcode::BufferAtomicSMax:
		case Opcode::BufferAtomicUMax:
		case Opcode::BufferAtomicAnd:
		case Opcode::BufferAtomicOr:
		case Opcode::BufferAtomicXor:
		case Opcode::BufferAtomicFMin:
		case Opcode::BufferAtomicFMax:
		case Opcode::FlatLoadUbyte:
		case Opcode::FlatLoadSbyte:
		case Opcode::FlatLoadUshort:
		case Opcode::FlatLoadSshort:
		case Opcode::FlatLoadDword:
		case Opcode::FlatLoadDwordx2:
		case Opcode::FlatLoadDwordx3:
		case Opcode::FlatLoadDwordx4:
		case Opcode::FlatStoreByte:
		case Opcode::FlatStoreShort:
		case Opcode::FlatStoreDword:
		case Opcode::FlatStoreDwordx2:
		case Opcode::FlatStoreDwordx3:
		case Opcode::FlatStoreDwordx4:
		case Opcode::DsAddU32:
		case Opcode::DsAddRtnU32:
		case Opcode::DsSubU32:
		case Opcode::DsSubRtnU32:
		case Opcode::DsMinI32:
		case Opcode::DsMinRtnI32:
		case Opcode::DsMaxI32:
		case Opcode::DsMaxRtnI32:
		case Opcode::DsMinU32:
		case Opcode::DsMinRtnU32:
		case Opcode::DsMaxU32:
		case Opcode::DsMaxRtnU32:
		case Opcode::DsAndB32:
		case Opcode::DsAndRtnB32:
		case Opcode::DsOrB32:
		case Opcode::DsOrRtnB32:
		case Opcode::DsXorB32:
		case Opcode::DsXorRtnB32:
		case Opcode::DsWrxchgRtnB32:
		case Opcode::DsSwizzleB32:
		case Opcode::DsReadSbyte:
		case Opcode::DsReadUbyte:
		case Opcode::DsReadSshort:
		case Opcode::DsReadUshort:
		case Opcode::DsRead2B32:
		case Opcode::DsReadB32:
		case Opcode::DsReadB64:
		case Opcode::DsRead2B64:
		case Opcode::DsReadB96:
		case Opcode::DsReadB128:
		case Opcode::DsWriteByte:
		case Opcode::DsWriteShort:
		case Opcode::DsWrite2B32:
		case Opcode::DsWrite2St64B32:
		case Opcode::DsWriteB32:
		case Opcode::DsWriteB64:
		case Opcode::DsWriteB96:
		case Opcode::DsWriteB128:
		case Opcode::DsWriteAddtidB32:
		case Opcode::DsReadAddtidB32: return CoverageClass::NeedsMemoryCase;

		case Opcode::ImageGetResinfo:
		case Opcode::ImageGetLod:
		case Opcode::ImageLoad:
		case Opcode::ImageLoadMip:
		case Opcode::ImageStore:
		case Opcode::ImageStoreMip:
		case Opcode::ImageAtomicAdd:
		case Opcode::ImageAtomicUMin:
		case Opcode::ImageAtomicAnd:
		case Opcode::ImageAtomicOr:
		case Opcode::ImageAtomicXor:
		case Opcode::ImageSample:
		case Opcode::ImageGather4Lz:
		case Opcode::ImageGather4C:
		case Opcode::ImageGather4CLz:
		case Opcode::ImageGather4LzO:
		case Opcode::ImageGather4CO:
		case Opcode::ImageGather4CLzO: return CoverageClass::NeedsImageCase;

		case Opcode::VInterpP1F32:
		case Opcode::VInterpP2F32:
		case Opcode::VInterpMovF32:
		case Opcode::Exp: return CoverageClass::NeedsGraphicsStageCase;

		default: return CoverageClass::NeedsAluCase;
	}
}

const char* CoverageClassName(CoverageClass status) {
	switch (status) {
		case CoverageClass::Covered: return "covered";
		case CoverageClass::ControlOrMarker: return "control";
		case CoverageClass::NeedsAluCase: return "alu";
		case CoverageClass::NeedsFloatCase: return "float";
		case CoverageClass::NeedsMemoryCase: return "memory";
		case CoverageClass::NeedsImageCase: return "image";
		case CoverageClass::NeedsGraphicsStageCase: return "graphics";
		default: return "unknown";
	}
}

void PrintPendingOpcodes(CoverageClass status, const std::vector<ShaderOpcode>& opcodes) {
	if (opcodes.empty()) {
		return;
	}

	std::printf("[coverage] pending_%s:", CoverageClassName(status));
	for (auto opcode: opcodes) {
		const auto name = ShaderRecompiler::Decoder::OpcodeToString(opcode);
		std::printf(" %s", name.c_str());
	}
	std::printf("\n");
}

void CheckOpcodeCoverage(const std::vector<TestCase>&     tests,
                         const std::vector<GraphicsCase>& graphics_tests) {
	using ShaderRecompiler::Decoder::Opcode;

	std::set<Opcode> covered;
	for (const auto& test: tests) {
		for (auto opcode: test.opcodes) {
			covered.insert(opcode);
		}
	}
	for (const auto& test: graphics_tests) {
		for (auto opcode: test.opcodes) {
			covered.insert(opcode);
		}
	}

	uint32_t                  counts[7] = {};
	std::vector<ShaderOpcode> pending[7];
	for (auto value = static_cast<int>(Opcode::SMovB32); value <= static_cast<int>(Opcode::Exp);
	     value++) {
		const auto opcode = static_cast<Opcode>(value);
		const auto status = ClassifyOpcode(opcode, covered);
		counts[static_cast<uint32_t>(status)]++;
		if (status != CoverageClass::Covered && status != CoverageClass::ControlOrMarker) {
			pending[static_cast<uint32_t>(status)].push_back(opcode);
		}
	}

	std::printf("[coverage] decoder opcodes: covered=%u control=%u alu_pending=%u "
	            "float_pending=%u memory_pending=%u image_pending=%u "
	            "graphics_pending=%u\n",
	            counts[static_cast<uint32_t>(CoverageClass::Covered)],
	            counts[static_cast<uint32_t>(CoverageClass::ControlOrMarker)],
	            counts[static_cast<uint32_t>(CoverageClass::NeedsAluCase)],
	            counts[static_cast<uint32_t>(CoverageClass::NeedsFloatCase)],
	            counts[static_cast<uint32_t>(CoverageClass::NeedsMemoryCase)],
	            counts[static_cast<uint32_t>(CoverageClass::NeedsImageCase)],
	            counts[static_cast<uint32_t>(CoverageClass::NeedsGraphicsStageCase)]);
	PrintPendingOpcodes(CoverageClass::NeedsAluCase,
	                    pending[static_cast<uint32_t>(CoverageClass::NeedsAluCase)]);
	PrintPendingOpcodes(CoverageClass::NeedsFloatCase,
	                    pending[static_cast<uint32_t>(CoverageClass::NeedsFloatCase)]);
	PrintPendingOpcodes(CoverageClass::NeedsMemoryCase,
	                    pending[static_cast<uint32_t>(CoverageClass::NeedsMemoryCase)]);
	PrintPendingOpcodes(CoverageClass::NeedsImageCase,
	                    pending[static_cast<uint32_t>(CoverageClass::NeedsImageCase)]);
	PrintPendingOpcodes(CoverageClass::NeedsGraphicsStageCase,
	                    pending[static_cast<uint32_t>(CoverageClass::NeedsGraphicsStageCase)]);
}

TestCase IntegerAddSubMul() {
	using O = ShaderOpcode;

	std::vector<u32> code = {
	    EncodeSMovB32(0, InlineU32(7)),
	    EncodeSMovB32(1, InlineU32(3)),
	    EncodeSop2(0x00, 2, 0, 1),
	    EncodeSop2(0x01, 3, 2, InlineU32(1)),
	    EncodeSop2(0x26, 4, 3, InlineU32(2)),
	    EncodeVop1(0x01, 0, 4),
	};
	AppendBufferStoreDword(&code, 0, 30);
	AppendEnd(&code);
	return {"IntegerAddSubMul",
	        code,
	        {},
	        {18},
	        {O::SMovB32, O::SAddU32, O::SSubU32, O::SMulI32, O::VMovB32, O::BufferStoreDword,
	         O::SEndpgm}};
}

TestCase BitwiseOps() {
	using O = ShaderOpcode;

	std::vector<u32> code = {
	    EncodeSMovB32(0, InlineU32(60)), EncodeSMovB32(1, InlineU32(15)), EncodeSop2(0x0e, 2, 0, 1),
	    EncodeSop2(0x10, 3, 0, 1),       EncodeSop2(0x12, 4, 3, 2),       EncodeVop1(0x37, 0, 4),
	};
	AppendBufferStoreDword(&code, 0, 30);
	AppendEnd(&code);
	return {"BitwiseAndOrXorNot",
	        code,
	        {},
	        {~0x33u},
	        {O::SMovB32, O::SAndB32, O::SOrB32, O::SXorB32, O::VNotB32, O::BufferStoreDword,
	         O::SEndpgm}};
}

TestCase Shifts() {
	using O = ShaderOpcode;

	std::vector<u32> code = {
	    EncodeSMovB32(0, InlineU32(3)),
	    EncodeSop2(0x1e, 1, 0, InlineU32(2)),
	    EncodeSop2(0x20, 2, 1, InlineU32(1)),
	    EncodeVop1(0x01, 0, 2),
	};
	AppendBufferStoreDword(&code, 0, 30);
	AppendEnd(&code);
	return {"Shifts",
	        code,
	        {},
	        {6},
	        {O::SMovB32, O::SLshlB32, O::SLshrB32, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase ExactPushConstantExtent() {
	using O = ShaderOpcode;

	constexpr std::array<u32, 10> values {0x10203040u, 0x21314151u, 0x32425262u, 0x43536373u,
	                                      0x54647484u, 0x65758595u, 0x768696a6u, 0x8797a7b7u,
	                                      0x98a8b8c8u, 0xa9b9c9d9u};
	std::vector<u32>              code;
	for (u32 i = 0; i < values.size(); i++) {
		AppendStoreSgpr(&code, 4u + i, i);
	}
	AppendEnd(&code);

	TestCase test;
	test.name = "ExactPushConstantExtent";
	test.code = std::move(code);
	test.initial.resize(values.size());
	test.expected.assign(values.begin(), values.end());
	test.opcodes   = {O::VMovB32, O::BufferStoreDword, O::SEndpgm};
	test.user_data = MakeStructuredStorageBufferData(0, values.size() * sizeof(u32));
	std::copy(values.begin(), values.end(), test.user_data.begin() + 4);
	test.has_user_data = true;
	return test;
}

TestCase ScalarShiftCountsMaskLowBits() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	code.push_back(EncodeSMovB32(0, InlineU32(1)));
	AppendSMovLiteral(&code, 1, 0x80000000u);
	code.push_back(EncodeSMovB32(2, InlineU32(32)));
	code.push_back(EncodeSMovB32(3, InlineU32(33)));
	code.push_back(EncodeSop2(0x1e, 10, 0, 2));
	code.push_back(EncodeSop2(0x1e, 11, 0, 3));
	code.push_back(EncodeSop2(0x20, 12, 1, 2));
	code.push_back(EncodeSop2(0x20, 13, 1, 3));
	code.push_back(EncodeSop2(0x22, 14, 1, 2));
	code.push_back(EncodeSop2(0x22, 15, 1, 3));

	for (u32 i = 0; i < 6u; i++) {
		AppendStoreSgpr(&code, 10u + i, i);
	}
	AppendEnd(&code);

	return {"ScalarShiftCountsMaskLowBits",
	        code,
	        {},
	        {1, 2, 0x80000000u, 0x40000000u, 0x80000000u, 0xc0000000u},
	        {O::SMovB32, O::SLshlB32, O::SLshrB32, O::SAshrI32, O::VMovB32, O::BufferStoreDword,
	         O::SEndpgm}};
}

TestCase Rdna2ScalarOpcodes() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	code.push_back(EncodeSMovB32(2, InlineU32(7)));
	code.push_back(EncodeSMovB32(106, InlineU32(16)));
	code.push_back(EncodeSop1(0x1d, 106, InlineU32(0)));
	AppendStoreSgpr(&code, 106, 0);
	code.push_back(EncodeSopk(0x13, 106, 0x1019u));
	AppendStoreSgpr(&code, 106, 1);
	code.push_back(EncodeSop2(0x02, 106, 2, 239u));
	AppendStoreSgpr(&code, 106, 2);
	code.push_back(EncodeSopp(0x0e, 0));
	code.push_back(EncodeSop2(0x02, 106, 239u, 2));
	AppendStoreSgpr(&code, 106, 3);
	AppendEnd(&code);

	return {"Rdna2ScalarOpcodes",
	        code,
	        {},
	        {0x11u, 0x11u, 7u, 7u},
	        {O::SMovB32, O::SBitset1B32, O::SSetregB32, O::SAddI32, O::SSleep, O::VMovB32,
	         O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarExtendedArithmetic() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendSMovLiteral(&code, 0, 0xffffffffu);
	code.push_back(EncodeSMovB32(1, InlineU32(1)));
	code.push_back(EncodeSop2(0x00, 2, 0, 1));
	code.push_back(EncodeSMovB32(3, InlineU32(5)));
	code.push_back(EncodeSMovB32(4, InlineU32(6)));
	code.push_back(EncodeSop2(0x04, 5, 3, 4));
	code.push_back(EncodeSop2(0x02, 6, InlineU32(7), InlineU32(8)));
	code.push_back(EncodeSop2(0x03, 7, InlineU32(7), InlineU32(9)));
	AppendSMovLiteral(&code, 8, 0xfffffffbu);
	code.push_back(EncodeSMovB32(9, InlineU32(3)));
	code.push_back(EncodeSop2(0x06, 10, 8, 9));
	code.push_back(EncodeSop2(0x08, 11, 8, 9));
	AppendSMovLiteral(&code, 12, 0xfffffffeu);
	code.push_back(EncodeSMovB32(13, InlineU32(3)));
	code.push_back(EncodeSop2(0x07, 14, 12, 13));
	code.push_back(EncodeSop2(0x09, 15, 12, 13));
	code.push_back(EncodeSMovB32(16, 8));
	code.push_back(EncodeSop1(0x34, 16, 16));
	code.push_back(EncodeSopk(0x00, 17, 0xfff5u));
	code.push_back(EncodeSopk(0x10, 17, 0xfffdu));

	const u32 results[] = {2, 5, 6, 7, 10, 11, 14, 15, 16, 17};
	for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
		AppendStoreSgpr(&code, results[i], i);
	}
	AppendEnd(&code);

	return {"ScalarExtendedArithmetic",
	        code,
	        {},
	        {0, 12, 15, 0xfffffffeu, 0xfffffffbu, 3, 3, 0xfffffffeu, 5, 33},
	        {O::SMovB32, O::SAddU32, O::SAddcU32, O::SAddI32, O::SSubI32, O::SMinI32, O::SMaxI32,
	         O::SMinU32, O::SMaxU32, O::SAbsI32, O::SMovkI32, O::SMulkI32, O::VMovB32,
	         O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarArithmeticSccCarryBorrowOverflow() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	auto             set_scc = [&](bool value) {
		code.push_back(EncodeSopc(0x06, InlineU32(1), InlineU32(value ? 1u : 0u)));
	};
	auto capture_scc = [&](u32 dst_sgpr) {
		code.push_back(EncodeSop2(0x0a, dst_sgpr, InlineU32(1), InlineU32(0)));
	};
	auto append_case = [&](bool prior_scc, u32 instruction, u32 out_sgpr) {
		set_scc(prior_scc);
		code.push_back(instruction);
		capture_scc(out_sgpr);
	};

	code.push_back(EncodeSMovB32(0, InlineU32(0)));
	code.push_back(EncodeSMovB32(1, InlineU32(1)));
	append_case(false, EncodeSop2(0x01, 10, 0, 1), 20);

	code.push_back(EncodeSMovB32(2, InlineU32(5)));
	code.push_back(EncodeSMovB32(3, InlineU32(3)));
	append_case(true, EncodeSop2(0x01, 11, 2, 3), 21);

	AppendSMovLiteral(&code, 4, 0x7fffffffu);
	code.push_back(EncodeSMovB32(5, InlineU32(1)));
	append_case(false, EncodeSop2(0x02, 12, 4, 5), 22);

	code.push_back(EncodeSMovB32(6, InlineU32(1)));
	code.push_back(EncodeSMovB32(7, InlineU32(2)));
	append_case(true, EncodeSop2(0x02, 13, 6, 7), 23);

	AppendSMovLiteral(&code, 8, 0x80000000u);
	code.push_back(EncodeSMovB32(9, InlineU32(1)));
	append_case(false, EncodeSop2(0x03, 14, 8, 9), 24);

	code.push_back(EncodeSMovB32(15, InlineU32(5)));
	code.push_back(EncodeSMovB32(16, InlineU32(3)));
	append_case(true, EncodeSop2(0x03, 17, 15, 16), 25);

	AppendSMovLiteral(&code, 18, 0x7fffffffu);
	set_scc(false);
	code.push_back(EncodeSopk(0x0f, 18, 1));
	capture_scc(26);

	code.push_back(EncodeSMovB32(19, InlineU32(4)));
	set_scc(true);
	code.push_back(EncodeSopk(0x0f, 19, 0xfffeu));
	capture_scc(27);

	for (u32 i = 0; i < 8u; i++) {
		AppendStoreSgpr(&code, 20u + i, i);
	}
	AppendEnd(&code);

	return {"ScalarArithmeticSccCarryBorrowOverflow",
	        code,
	        {},
	        {1, 0, 1, 0, 1, 0, 1, 0},
	        {O::SMovB32, O::SSubU32, O::SAddI32, O::SSubI32, O::SCmpEqU32, O::SCselectB32,
	         O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarMinMaxSccComparisonEdges() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	auto             set_scc = [&](bool value) {
		code.push_back(EncodeSopc(0x06, InlineU32(1), InlineU32(value ? 1u : 0u)));
	};
	auto capture_scc = [&](u32 dst_sgpr) {
		code.push_back(EncodeSop2(0x0a, dst_sgpr, InlineU32(1), InlineU32(0)));
	};
	auto append_case = [&](bool prior_scc, u32 instruction, u32 out_sgpr) {
		set_scc(prior_scc);
		code.push_back(instruction);
		capture_scc(out_sgpr);
	};

	AppendSMovLiteral(&code, 0, 0xfffffffeu);
	code.push_back(EncodeSMovB32(1, InlineU32(3)));
	append_case(false, EncodeSop2(0x06, 10, 0, 1), 20);
	append_case(true, EncodeSop2(0x06, 11, 1, 0), 21);
	append_case(false, EncodeSop2(0x08, 12, 1, 0), 22);
	append_case(true, EncodeSop2(0x08, 13, 0, 1), 23);

	code.push_back(EncodeSMovB32(2, InlineU32(2)));
	code.push_back(EncodeSMovB32(3, InlineU32(3)));
	append_case(false, EncodeSop2(0x07, 14, 2, 3), 24);
	append_case(true, EncodeSop2(0x07, 15, 3, 2), 25);
	append_case(false, EncodeSop2(0x09, 16, 3, 2), 26);
	append_case(true, EncodeSop2(0x09, 17, 2, 3), 27);

	for (u32 i = 0; i < 8u; i++) {
		AppendStoreSgpr(&code, 20u + i, i);
	}
	AppendEnd(&code);

	return {"ScalarMinMaxSccComparisonEdges",
	        code,
	        {},
	        {1, 0, 1, 0, 1, 0, 1, 0},
	        {O::SMovB32, O::SMinI32, O::SMaxI32, O::SMinU32, O::SMaxU32, O::SCmpEqU32,
	         O::SCselectB32, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarAbsI32UpdatesScc() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	auto             set_scc = [&](bool value) {
		code.push_back(EncodeSopc(0x06, InlineU32(1), InlineU32(value ? 1u : 0u)));
	};
	auto capture_scc = [&](u32 dst_sgpr) {
		code.push_back(EncodeSop2(0x0a, dst_sgpr, InlineU32(1), InlineU32(0)));
	};

	code.push_back(EncodeSMovB32(0, InlineU32(0)));
	set_scc(true);
	code.push_back(EncodeSop1(0x34, 1, 0));
	capture_scc(2);

	AppendSMovLiteral(&code, 3, 0xfffffffbu);
	set_scc(false);
	code.push_back(EncodeSop1(0x34, 4, 3));
	capture_scc(5);

	AppendStoreSgpr(&code, 1, 0);
	AppendStoreSgpr(&code, 2, 1);
	AppendStoreSgpr(&code, 4, 2);
	AppendStoreSgpr(&code, 5, 3);
	AppendEnd(&code);

	return {"ScalarAbsI32UpdatesScc",
	        code,
	        {},
	        {0, 0, 5, 1},
	        {O::SMovB32, O::SCmpEqU32, O::SAbsI32, O::SCselectB32, O::VMovB32, O::BufferStoreDword,
	         O::SEndpgm}};
}

TestCase ScalarShiftLeftAddSccCarryEdges() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	auto             set_scc = [&](bool value) {
		code.push_back(EncodeSopc(0x06, InlineU32(1), InlineU32(value ? 1u : 0u)));
	};
	auto capture_scc = [&](u32 dst_sgpr) {
		code.push_back(EncodeSop2(0x0a, dst_sgpr, InlineU32(1), InlineU32(0)));
	};
	auto append_case = [&](bool prior_scc, u32 instruction, u32 out_sgpr) {
		set_scc(prior_scc);
		code.push_back(instruction);
		capture_scc(out_sgpr);
	};

	AppendSMovLiteral(&code, 0, 0x80000000u);
	AppendSMovLiteral(&code, 1, 0x40000000u);
	AppendSMovLiteral(&code, 2, 0x20000000u);
	AppendSMovLiteral(&code, 3, 0x10000000u);
	code.push_back(EncodeSMovB32(4, InlineU32(1)));
	code.push_back(EncodeSMovB32(5, InlineU32(0)));
	append_case(false, EncodeSop2(0x2e, 10, 0, 5), 20);
	append_case(true, EncodeSop2(0x2e, 11, 4, 4), 21);
	append_case(false, EncodeSop2(0x2f, 12, 1, 5), 22);
	append_case(true, EncodeSop2(0x2f, 13, 4, 4), 23);
	append_case(false, EncodeSop2(0x30, 14, 2, 5), 24);
	append_case(true, EncodeSop2(0x30, 15, 4, 4), 25);
	append_case(false, EncodeSop2(0x31, 16, 3, 5), 26);
	append_case(true, EncodeSop2(0x31, 17, 4, 4), 27);

	for (u32 i = 0; i < 8u; i++) {
		AppendStoreSgpr(&code, 20u + i, i);
	}
	AppendEnd(&code);

	return {"ScalarShiftLeftAddSccCarryEdges",
	        code,
	        {},
	        {1, 0, 1, 0, 1, 0, 1, 0},
	        {O::SMovB32, O::SLshl1AddU32, O::SLshl2AddU32, O::SLshl3AddU32, O::SLshl4AddU32,
	         O::SCmpEqU32, O::SCselectB32, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarCompareOps() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendSMovLiteral(&code, 0, 0xfffffffeu);
	code.push_back(EncodeSMovB32(1, InlineU32(1)));
	code.push_back(EncodeSMovB32(2, InlineU32(5)));
	code.push_back(EncodeSMovB32(3, InlineU32(2)));
	code.push_back(EncodeSMovB32(4, InlineU32(3)));
	AppendSMovLiteral(&code, 10, 0x00000001u);
	code.push_back(EncodeSMovB32(11, InlineU32(0)));
	code.push_back(EncodeSMovB32(12, InlineU32(2)));
	code.push_back(EncodeSMovB32(13, InlineU32(0)));
	code.push_back(EncodeSMovB32(14, InlineU32(1)));
	code.push_back(EncodeSMovB32(15, InlineU32(1)));

	u32  dst            = 20;
	auto append_compare = [&](u32 opcode, u32 src0, u32 src1) {
		code.push_back(EncodeSopc(opcode, src0, src1));
		code.push_back(EncodeSop2(0x0a, dst++, InlineU32(1), InlineU32(0)));
	};

	append_compare(0x00, 2, 2);
	append_compare(0x01, 0, 1);
	append_compare(0x02, 1, 0);
	append_compare(0x03, 1, 1);
	append_compare(0x04, 0, 1);
	append_compare(0x05, 0, 0);
	append_compare(0x07, 3, 4);
	append_compare(0x08, 4, 3);
	append_compare(0x09, 4, 4);
	append_compare(0x0b, 3, 4);
	append_compare(0x12, 10, 10);
	append_compare(0x12, 10, 12);
	append_compare(0x12, 10, 14);
	append_compare(0x13, 10, 12);
	append_compare(0x13, 10, 14);

	for (u32 i = 0; i < 15u; i++) {
		AppendStoreSgpr(&code, 20u + i, i);
	}
	AppendEnd(&code);

	return {"ScalarCompareOps",
	        code,
	        {},
	        {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1},
	        {O::SMovB32, O::SCmpEqI32, O::SCmpLgI32, O::SCmpGtI32, O::SCmpGeI32, O::SCmpLtI32,
	         O::SCmpLeI32, O::SCmpLgU32, O::SCmpGtU32, O::SCmpGeU32, O::SCmpLeU32, O::SCmpEqU64,
	         O::SCmpLgU64, O::SCselectB32, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarShiftAddAndMaskOps() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	code.push_back(EncodeSMovB32(0, InlineU32(2)));
	code.push_back(EncodeSMovB32(1, InlineU32(3)));
	code.push_back(EncodeSop2(0x2e, 2, 0, 1));
	code.push_back(EncodeSop2(0x2f, 3, 0, 1));
	code.push_back(EncodeSop2(0x30, 4, 0, 1));
	code.push_back(EncodeSop2(0x31, 5, 0, 1));
	AppendSMovLiteral(&code, 6, 0xfffffff8u);
	code.push_back(EncodeSop2(0x22, 7, 6, InlineU32(2)));
	AppendSMovLiteral(&code, 8, 0xffffffffu);
	code.push_back(EncodeSMovB32(9, InlineU32(2)));
	code.push_back(EncodeSop2(0x35, 10, 8, 9));
	AppendSMovLiteral(&code, 12, 0x0f0f0f0fu);
	AppendSMovLiteral(&code, 13, 0x00ff00ffu);
	code.push_back(EncodeSop1(0x08, 14, 12));
	code.push_back(EncodeSop1(0x0a, 16, 12));

	AppendStoreSgpr(&code, 2, 0);
	AppendStoreSgpr(&code, 3, 1);
	AppendStoreSgpr(&code, 4, 2);
	AppendStoreSgpr(&code, 5, 3);
	AppendStoreSgpr(&code, 7, 4);
	AppendStoreSgpr(&code, 10, 5);
	AppendStoreSgprPair(&code, 14, 6);
	AppendStoreSgprPair(&code, 16, 8);
	AppendEnd(&code);

	return {"ScalarShiftAddAndMaskOps",
	        code,
	        {},
	        {7, 11, 19, 35, 0xfffffffeu, 1, 0xf0f0f0f0u, 0xff00ff00u, 0x0f0f0f0fu, 0x00ff00ffu},
	        {O::SMovB32, O::SLshl1AddU32, O::SLshl2AddU32, O::SLshl3AddU32, O::SLshl4AddU32,
	         O::SAshrI32, O::SMulHiU32, O::SNotB64, O::SWqmB64, O::VMovB32, O::BufferStoreDword,
	         O::SEndpgm}};
}

TestCase ScalarNotB64UpdatesScc() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	auto             set_scc = [&](bool value) {
		code.push_back(EncodeSopc(0x06, InlineU32(1), InlineU32(value ? 1u : 0u)));
	};
	auto capture_scc = [&](u32 dst_sgpr) {
		code.push_back(EncodeSop2(0x0a, dst_sgpr, InlineU32(1), InlineU32(0)));
	};

	AppendSMovLiteral(&code, 0, 0xffffffffu);
	AppendSMovLiteral(&code, 1, 0xffffffffu);
	set_scc(true);
	code.push_back(EncodeSop1(0x08, 2, 0));
	capture_scc(4);

	AppendSMovLiteral(&code, 6, 0xfffffffeu);
	AppendSMovLiteral(&code, 7, 0xffffffffu);
	set_scc(false);
	code.push_back(EncodeSop1(0x08, 8, 6));
	capture_scc(10);

	AppendStoreSgpr(&code, 4, 0);
	AppendStoreSgpr(&code, 10, 1);
	AppendStoreSgprPair(&code, 2, 2);
	AppendStoreSgprPair(&code, 8, 4);
	AppendEnd(&code);

	return {"ScalarNotB64UpdatesScc",
	        code,
	        {},
	        {0, 1, 0, 0, 1, 0},
	        {O::SMovB32, O::SCmpEqU32, O::SNotB64, O::SCselectB32, O::VMovB32, O::BufferStoreDword,
	         O::SEndpgm}};
}

TestCase ScalarFlbitI32B64Gpu() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendSMovLiteral(&code, 0, 0x00000000u);
	AppendSMovLiteral(&code, 1, 0x80000000u);
	code.push_back(EncodeSop1(0x16, 20, 0));

	AppendSMovLiteral(&code, 2, 0x00008000u);
	AppendSMovLiteral(&code, 3, 0x00000000u);
	code.push_back(EncodeSop1(0x16, 21, 2));

	AppendSMovLiteral(&code, 4, 0x00000000u);
	AppendSMovLiteral(&code, 5, 0x00000000u);
	code.push_back(EncodeSop1(0x16, 22, 4));

	AppendSMovLiteral(&code, 14, 0x00000008u);
	AppendSMovLiteral(&code, 15, 0x00000000u);
	code.push_back(EncodeSop1(0x16, 106, 14));

	AppendStoreSgpr(&code, 20, 0);
	AppendStoreSgpr(&code, 21, 1);
	AppendStoreSgpr(&code, 22, 2);
	AppendStoreSgpr(&code, 106, 3);
	AppendEnd(&code);

	return {"ScalarFlbitI32B64Gpu",
	        code,
	        {},
	        {0, 48, 0xffffffffu, 60},
	        {O::SMovB32, O::SFlbitI32B64, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarSaveExecOps() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendSMovLiteral(&code, 106, 0xffffffffu);
	AppendSMovLiteral(&code, 107, 0xffffffffu);
	code.push_back(0xbe80246au);
	code.push_back(EncodeSop1(0x28, 2, 106));
	code.push_back(EncodeSMovB32(106, InlineU32(0)));
	code.push_back(EncodeSMovB32(107, InlineU32(0)));
	code.push_back(EncodeSop1(0x37, 4, 106));
	AppendSMovLiteral(&code, 106, 0x00000003u);
	code.push_back(EncodeSop1(0x3c, 8, 106));
	code.push_back(EncodeSop1(0x04, 10, 126));
	code.push_back(EncodeSMovB32(106, InlineU32(1)));
	code.push_back(EncodeSop1(0x44, 12, 106));
	code.push_back(EncodeSop1(0x04, 14, 126));
	AppendSMovLiteral(&code, 126, 0xffffffffu);
	AppendSMovLiteral(&code, 127, 0xffffffffu);

	AppendStoreSgprPair(&code, 0, 0);
	AppendStoreSgprPair(&code, 2, 2);
	AppendStoreSgprPair(&code, 4, 4);
	AppendStoreSgpr(&code, 8, 6);
	AppendStoreSgprPair(&code, 10, 7);
	AppendStoreSgpr(&code, 12, 9);
	AppendStoreSgprPair(&code, 14, 10);
	AppendStoreSgpr(&code, 253, 12);
	AppendEnd(&code);

	return {"ScalarSaveExecOps",
	        code,
	        {},
	        {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
	         0xffffffffu, 0x00000003u, 0xffffffffu, 0x00000003u, 0x00000002u, 0xffffffffu, 1},
	        {O::SMovB32, O::SAndSaveexecB64, O::SOrn2SaveexecB64, O::SAndn1SaveexecB64,
	         O::SAndSaveexecB32, O::SAndn1SaveexecB32, O::SMovB64, O::VMovB32, O::BufferStoreDword,
	         O::SEndpgm}};
}

TestCase ScalarOrn2SaveexecUsesSourceOrNotExec() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendSMovLiteral(&code, 126, 0x0000000cu);
	AppendSMovLiteral(&code, 127, 0x80000000u);
	AppendSMovLiteral(&code, 0, 0x00000001u);
	AppendSMovLiteral(&code, 1, 0x00000001u);
	code.push_back(EncodeSop1(0x28, 2, 0));
	code.push_back(EncodeSop1(0x04, 4, 126));
	code.push_back(EncodeSMovB32(126, InlineU32(1)));
	code.push_back(EncodeSMovB32(127, InlineU32(0)));
	AppendStoreSgprPair(&code, 2, 0);
	AppendStoreSgprPair(&code, 4, 2);
	AppendStoreSgpr(&code, 253, 4);
	AppendEnd(&code);

	return {
	    "ScalarOrn2SaveexecUsesSourceOrNotExec",
	    code,
	    {},
	    {0x0000000cu, 0x80000000u, 0xfffffff3u, 0x7fffffffu, 1},
	    {O::SMovB32, O::SOrn2SaveexecB64, O::SMovB64, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarGetpcWritesNextInstructionPc() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	code.push_back(EncodeSop1(0x1f, 0, 0));
	AppendStoreSgprPair(&code, 0, 0);
	AppendEnd(&code);

	return {"ScalarGetpcWritesNextInstructionPc",
	        code,
	        {},
	        {4, 0},
	        {O::SGetpcB64, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarBitfieldPack() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	code.push_back(EncodeSMovB32(0, InlineU32(15)));
	code.push_back(EncodeSop1(0x0b, 1, 0));
	AppendSMovLiteral(&code, 2, 0xf0000000u);
	code.push_back(EncodeSop1(0x10, 3, 0));
	AppendSMovLiteral(&code, 19, 0xf0f00001u);
	code.push_back(EncodeSop1(0x0f, 20, 19));
	code.push_back(EncodeSop2(0x0a, 21, InlineU32(1), InlineU32(0)));
	code.push_back(EncodeSMovB32(22, InlineU32(0)));
	code.push_back(EncodeSop1(0x0f, 23, 22));
	code.push_back(EncodeSop2(0x0a, 24, InlineU32(1), InlineU32(0)));
	code.push_back(EncodeSMovB32(4, InlineU32(3)));
	code.push_back(EncodeSop1(0x3b, 5, 4));
	code.push_back(EncodeSop2(0x24, 7, InlineU32(4), InlineU32(8)));
	AppendSMovLiteral(&code, 8, 0x00f00000u);
	AppendSMovLiteral(&code, 9, 0x00040014u);
	code.push_back(EncodeSop2(0x27, 10, 8, 9));
	AppendSMovLiteral(&code, 11, 0xaaaabbbbu);
	AppendSMovLiteral(&code, 12, 0xccccddddu);
	code.push_back(EncodeSop2(0x32, 13, 11, 12));
	code.push_back(EncodeSop2(0x33, 14, 11, 12));
	code.push_back(EncodeSop2(0x34, 15, 11, 12));
	code.push_back(EncodeSopc(0x0d, InlineU32(4), InlineU32(2)));
	code.push_back(EncodeSop2(0x0a, 17, InlineU32(1), InlineU32(0)));
	code.push_back(EncodeSopc(0x0c, InlineU32(4), InlineU32(1)));
	code.push_back(EncodeSop2(0x0a, 18, InlineU32(1), InlineU32(0)));

	AppendStoreSgpr(&code, 1, 0);
	AppendStoreSgpr(&code, 3, 1);
	AppendStoreSgpr(&code, 20, 2);
	AppendStoreSgpr(&code, 21, 3);
	AppendStoreSgpr(&code, 23, 4);
	AppendStoreSgpr(&code, 24, 5);
	AppendStoreSgprPair(&code, 5, 6);
	AppendStoreSgpr(&code, 7, 8);
	AppendStoreSgpr(&code, 10, 9);
	AppendStoreSgpr(&code, 13, 10);
	AppendStoreSgpr(&code, 14, 11);
	AppendStoreSgpr(&code, 15, 12);
	AppendStoreSgpr(&code, 17, 13);
	AppendStoreSgpr(&code, 18, 14);
	AppendEnd(&code);

	return {"ScalarBitfieldPack",
	        code,
	        {},
	        {0xf0000000u, 8, 9, 1, 0, 0, 0x0000000fu, 0, 0x00000f00u, 0x0000000fu, 0xddddbbbbu,
	         0xccccbbbbu, 0xccccaaaau, 1, 1},
	        {O::SMovB32, O::SBrevB32, O::SBcnt1I32B32, O::SBcnt1I32B64, O::SBitreplicateB64B32,
	         O::SBfmB32, O::SBfeU32, O::SPackLlB32B16, O::SPackLhB32B16, O::SPackHhB32B16,
	         O::SBitcmp0B32, O::SBitcmp1B32, O::SCselectB32, O::VMovB32, O::BufferStoreDword,
	         O::SEndpgm}};
}

TestCase ScalarBrevB32PreservesScc() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	auto             set_scc = [&](bool value) {
		code.push_back(EncodeSopc(0x06, InlineU32(1), InlineU32(value ? 1u : 0u)));
	};
	auto capture_scc = [&](u32 dst_sgpr) {
		code.push_back(EncodeSop2(0x0a, dst_sgpr, InlineU32(1), InlineU32(0)));
	};

	code.push_back(EncodeSMovB32(0, InlineU32(1)));
	set_scc(false);
	code.push_back(EncodeSop1(0x0b, 1, 0));
	capture_scc(2);

	code.push_back(EncodeSMovB32(3, InlineU32(0)));
	set_scc(true);
	code.push_back(EncodeSop1(0x0b, 4, 3));
	capture_scc(5);

	AppendStoreSgpr(&code, 1, 0);
	AppendStoreSgpr(&code, 2, 1);
	AppendStoreSgpr(&code, 4, 2);
	AppendStoreSgpr(&code, 5, 3);
	AppendEnd(&code);

	return {"ScalarBrevB32PreservesScc",
	        code,
	        {},
	        {0x80000000u, 0, 0, 1},
	        {O::SMovB32, O::SCmpEqU32, O::SBrevB32, O::SCselectB32, O::VMovB32, O::BufferStoreDword,
	         O::SEndpgm}};
}

TestCase BitfieldExtractWidthPastEndEdges() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendSMovLiteral(&code, 0, 0xf0000000u);
	AppendSMovLiteral(&code, 1, (20u << 16u) | 20u);
	code.push_back(EncodeSop2(0x27, 2, 0, 1));
	AppendSMovLiteral(&code, 3, 0x80000000u);
	AppendSMovLiteral(&code, 4, (7u << 16u) | 31u);
	code.push_back(EncodeSop2(0x27, 5, 3, 4));
	AppendVMovLiteral(&code, 6, 0xf0000000u);
	AppendVMovU32(&code, 7, 28);
	AppendVMovU32(&code, 8, 8);
	AppendVop3(&code, 0x148, 9, Vgpr(6), Vgpr(7), Vgpr(8));
	AppendStoreSgpr(&code, 2, 0);
	AppendStoreSgpr(&code, 5, 1);
	AppendStoreVgpr(&code, 9, 2);
	AppendEnd(&code);

	return {"BitfieldExtractWidthPastEndEdges",
	        code,
	        {},
	        {0x00000f00u, 1, 0x0000000fu},
	        {O::SMovB32, O::SBfeU32, O::VMovB32, O::VBfeU32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase Scalar64BitOps() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendSMovLiteral(&code, 0, 0x0f0f0f0fu);
	AppendSMovLiteral(&code, 1, 0x00ff00ffu);
	AppendSMovLiteral(&code, 2, 0x33333333u);
	AppendSMovLiteral(&code, 3, 0x0f0f0f0fu);
	code.push_back(EncodeSop2(0x0f, 4, 0, 2));
	code.push_back(EncodeSop2(0x15, 6, 0, 2));
	code.push_back(EncodeSop2(0x11, 8, 0, 2));
	code.push_back(EncodeSop2(0x17, 10, 0, 2));
	code.push_back(EncodeSop2(0x13, 12, 0, 2));
	code.push_back(EncodeSop2(0x19, 14, 0, 2));
	code.push_back(EncodeSop2(0x1b, 16, 0, 2));
	code.push_back(EncodeSop2(0x1d, 18, 0, 2));
	code.push_back(EncodeSop1(0x04, 20, 0));
	code.push_back(EncodeSop2(0x1f, 22, 0, InlineU32(4)));
	code.push_back(EncodeSop2(0x21, 24, 0, InlineU32(8)));
	code.push_back(EncodeSop2(0x25, 26, InlineU32(36), InlineU32(4)));
	code.push_back(EncodeSop2(0x21, 34, 193u, InlineU32(1)));
	AppendSMovLiteral(&code, 28, 0x000c0004u);
	code.push_back(EncodeSop2(0x29, 30, 0, 28));
	code.push_back(EncodeSopc(0x06, 0, 0));
	code.push_back(EncodeSop2(0x0b, 32, 0, 2));

	const u32 result_pairs[] = {4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 34, 30, 32};
	u32       out            = 0;
	for (auto sgpr: result_pairs) {
		AppendStoreSgprPair(&code, sgpr, out);
		out += 2;
	}
	AppendEnd(&code);

	return {"Scalar64BitOps",
	        code,
	        {},
	        {0x03030303u, 0x000f000fu, 0x0c0c0c0cu, 0x00f000f0u, 0x3f3f3f3fu, 0x0fff0fffu,
	         0xcfcfcfcfu, 0xf0fff0ffu, 0x3c3c3c3cu, 0x0ff00ff0u, 0xfcfcfcfcu, 0xfff0fff0u,
	         0xc0c0c0c0u, 0xf000f000u, 0xc3c3c3c3u, 0xf00ff00fu, 0x0f0f0f0fu, 0x00ff00ffu,
	         0xf0f0f0f0u, 0x0ff00ff0u, 0xff0f0f0fu, 0x0000ff00u, 0xfffffff0u, 0x000000ffu,
	         0xffffffffu, 0x7fffffffu, 0x000000f0u, 0,           0x0f0f0f0fu, 0x00ff00ffu},
	        {O::SMovB32, O::SMovB64, O::SAndB64, O::SAndn2B64, O::SOrB64, O::SOrn2B64, O::SXorB64,
	         O::SNandB64, O::SNorB64, O::SXnorB64, O::SLshlB64, O::SLshrB64, O::SBfmB64, O::SBfeU64,
	         O::SCmpEqU32, O::SCselectB64, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarAndn2B64SccBranch() {
	using O = ShaderOpcode;

	std::vector<u32> code = {
	    EncodeSMovB32(0, InlineU32(3)),
	    EncodeSMovB32(1, InlineU32(0)),
	    EncodeSMovB32(2, InlineU32(1)),
	    EncodeSMovB32(3, InlineU32(0)),
	    EncodeSop2(0x15, 4, 0, 2),
	    EncodeVop1(0x01, 0, InlineU32(1)),
	    EncodeSopp(0x04, 1),
	    EncodeVop1(0x01, 0, InlineU32(7)),
	};
	AppendStoreVgpr(&code, 0, 0);
	AppendEnd(&code);

	return {
	    "ScalarAndn2B64SccBranch",
	    code,
	    {},
	    {7},
	    {O::SMovB32, O::SAndn2B64, O::VMovB32, O::SCbranchScc0, O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarLiteral() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendSMovLiteral(&code, 0, 0x12345678u);
	code.push_back(EncodeVop1(0x01, 0, 0));
	AppendBufferStoreDword(&code, 0, 30);
	AppendEnd(&code);
	return {"ScalarLiteral",
	        code,
	        {},
	        {0x12345678u},
	        {O::SMovB32, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorMoves() {
	using O = ShaderOpcode;

	std::vector<u32> code = {
	    EncodeVop1(0x00, 0, 0),
	    EncodeVop1(0x01, 0, InlineU32(5)),
	    EncodeVop1(0x01, 2, Vgpr(0)),
	};
	AppendBufferStoreDword(&code, 2, 30);
	AppendEnd(&code);
	return {"VectorRegisterMoves",
	        code,
	        {},
	        {5},
	        {O::VNop, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorVop3MoveAppliesFloatSourceModifiers() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendSMovLiteral(&code, 24, 0x40000000u);
	AppendVop3(&code, 0x181, 1, 24, 0, 0, 0, 0, false, 0, 0x1);
	AppendStoreVgpr(&code, 1, 0);
	AppendEnd(&code);

	TestCase test;
	test.name           = "VectorVop3MoveAppliesFloatSourceModifiers";
	test.code           = code;
	test.expected       = {0xc0000000u};
	test.opcodes        = {O::SMovB32, O::VMovB32, O::BufferStoreDword, O::SEndpgm};
	test.required_spirv = {"OpFNegate"};
	return test;
}

TestCase VectorIntegerOps() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0xfffffff8u);
	code.push_back(EncodeVop1(0x01, 1, InlineU32(5)));
	code.push_back(EncodeVop1(0x01, 4, InlineU32(7)));
	AppendVMovLiteral(&code, 10, 0x01000001u);
	code.push_back(EncodeVop1(0x01, 11, InlineU32(2)));
	AppendVMovLiteral(&code, 12, 0x0f0f0f0fu);
	AppendVMovLiteral(&code, 13, 0x33333333u);
	AppendVMovLiteral(&code, 14, 0xfffffff0u);
	code.push_back(EncodeVop1(0x01, 15, InlineU32(4)));
	code.push_back(EncodeVop1(0x01, 29, InlineU32(15)));

	code.push_back(EncodeVop2(0x11, 2, Vgpr(0), 1));
	code.push_back(EncodeVop2(0x12, 3, Vgpr(0), 1));
	code.push_back(EncodeVop2(0x13, 5, Vgpr(0), 1));
	code.push_back(EncodeVop2(0x14, 6, Vgpr(0), 1));
	code.push_back(EncodeVop2(0x25, 7, Vgpr(4), 1));
	code.push_back(EncodeVop2(0x26, 8, Vgpr(4), 1));
	code.push_back(EncodeVop2(0x27, 9, InlineU32(3), 4));
	code.push_back(EncodeVop2(0x26, 35, 249, 11));
	code.push_back(EncodeVop2Sdwa(12, 6, 0, 0, 6));
	code.push_back(EncodeVop2(0x27, 36, 249, 11));
	code.push_back(EncodeVop2Sdwa(12, 6, 0, 0, 6));
	code.push_back(EncodeVop2(0x0b, 16, Vgpr(10), 11));
	code.push_back(EncodeVop2(0x09, 37, Vgpr(0), 1));
	code.push_back(EncodeVop2(0x1b, 17, Vgpr(12), 13));
	code.push_back(EncodeVop2(0x1c, 18, Vgpr(12), 13));
	code.push_back(EncodeVop2(0x1d, 19, Vgpr(12), 13));
	code.push_back(EncodeVop2(0x1e, 20, Vgpr(12), 13));
	code.push_back(EncodeVop2(0x19, 21, Vgpr(11), 15));
	code.push_back(EncodeVop2(0x1a, 22, InlineU32(4), 11));
	code.push_back(EncodeVop2(0x15, 23, Vgpr(14), 15));
	code.push_back(EncodeVop2(0x16, 24, InlineU32(4), 14));
	code.push_back(EncodeVop2(0x17, 25, Vgpr(14), 15));
	code.push_back(EncodeVop2(0x18, 26, InlineU32(4), 14));
	code.push_back(EncodeVop1(0x37, 27, Vgpr(12)));
	code.push_back(EncodeVop1(0x38, 28, Vgpr(29)));
	code.push_back(EncodeVop1(0x3a, 30, Vgpr(11)));
	code.push_back(EncodeVop1(0x39, 32, Vgpr(11)));
	code.push_back(EncodeVopc(0xc2, Vgpr(4), 4));
	code.push_back(EncodeVop2(0x01, 33, InlineU32(3), 4));
	code.push_back(EncodeVop2(0x11, 34, 249, 4));
	code.push_back(EncodeVop2Sdwa(0));

	const u32 results[] = {2,  3,  5,  6,  7,  8,  9,  35, 36, 16, 37, 17, 18, 19,
	                       20, 21, 22, 23, 24, 25, 26, 27, 28, 30, 32, 33, 34};
	for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
		AppendStoreVgpr(&code, results[i], i);
	}
	AppendEnd(&code);

	return {"VectorIntegerOps",
	        code,
	        {},
	        {0xfffffff8u, 5,           5,  0xfffffff8u, 12,          2,           4,
	         13,          0xfffffff3u, 2,  0xffffffd8u, 0x03030303u, 0x3f3f3f3fu, 0x3c3c3c3cu,
	         0xc3c3c3c3u, 32,          32, 0x0fffffffu, 0x0fffffffu, 0xffffffffu, 0xffffffffu,
	         0xf0f0f0f0u, 0xf0000000u, 1,  30,          7,           0xfffffff8u},
	        {O::VMovB32,     O::VMinI32,          O::VMaxI32,      O::VMinU32,    O::VMaxU32,
	         O::VAddNcU32,   O::VSubNcU32,        O::VSubrevNcU32, O::VMulU32U24, O::VMulI32I24,
	         O::VAndB32,     O::VOrB32,           O::VXorB32,      O::VXnorB32,   O::VLshlB32,
	         O::VLshlrevB32, O::VLshrB32,         O::VLshrrevB32,  O::VAshrI32,   O::VAshrrevI32,
	         O::VNotB32,     O::VBfrevB32,        O::VFfblB32,     O::VFfbhU32,   O::VCmpEqU32,
	         O::VCndmaskB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase Vop2SdwaSubNcExactByte2Destination() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 11, 2);
	code.push_back(0x4c1616f9u);
	code.push_back(0x0686128du);
	AppendStoreVgpr(&code, 11, 0);
	AppendEnd(&code);

	TestCase test;
	test.name           = "Vop2SdwaSubNcExactByte2Destination";
	test.code           = code;
	test.expected       = {0x000b0002u};
	test.opcodes        = {O::VMovB32, O::VSubNcU32, O::BufferStoreDword, O::SEndpgm};
	test.required_spirv = {"OpISub", "OpShiftLeftLogical", "OpBitwiseOr"};
	return test;
}

TestCase Vop2SdwaSubNcPreservesByteAndWordDestinations() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 0, 3);
	for (u32 dst = 10; dst <= 15; dst++) {
		AppendVMovLiteral(&code, dst, 0xa1b2c3d4u);
	}
	for (u32 sel = 0; sel <= 5; sel++) {
		code.push_back(EncodeVop2(0x26, 10 + sel, 249, 0));
		code.push_back(EncodeVop2Sdwa(InlineU32(20), sel, 2, 6, 6, 0, 0, 0, 0, 0, 0, 1));
		AppendStoreVgpr(&code, 10 + sel, sel);
	}
	AppendEnd(&code);

	return {"Vop2SdwaSubNcPreservesByteAndWordDestinations",
	        code,
	        {},
	        {0xa1b2c311u, 0xa1b211d4u, 0xa111c3d4u, 0x11b2c3d4u, 0xa1b20011u,
	         0x0011c3d4u},
	        {O::VMovB32, O::VSubNcU32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase Vop2SdwaMinU32PreservesWordDestination() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 10, 0xa1b2c3d4u);
	AppendVMovU32(&code, 12, 7);
	code.push_back(0x261418f9u);
	code.push_back(0x0686149fu);
	AppendStoreVgpr(&code, 10, 0);
	AppendVMovLiteral(&code, 10, 0xa1b2c3d4u);
	AppendVMovU32(&code, 12, 64);
	code.push_back(0x261418f9u);
	code.push_back(0x0686149fu);
	AppendStoreVgpr(&code, 10, 1);
	AppendEnd(&code);

	TestCase test;
	test.name           = "Vop2SdwaMinU32PreservesWordDestination";
	test.code           = code;
	test.expected       = {0xa1b20007u, 0xa1b2001fu};
	test.opcodes        = {O::VMovB32, O::VMinU32, O::BufferStoreDword, O::SEndpgm};
	test.required_spirv = {"OpULessThan", "OpSelect", "OpBitwiseOr"};
	return test;
}

TestCase VectorShiftCountsMaskLowBits() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 0, 1);
	AppendVMovU32(&code, 1, 32);
	AppendVMovU32(&code, 2, 33);
	AppendVMovLiteral(&code, 3, 0x80000000u);
	code.push_back(EncodeVop2(0x19, 10, Vgpr(0), 1));
	code.push_back(EncodeVop2(0x1a, 11, Vgpr(2), 0));
	code.push_back(EncodeVop2(0x15, 12, Vgpr(3), 1));
	code.push_back(EncodeVop2(0x16, 13, Vgpr(2), 3));
	code.push_back(EncodeVop2(0x17, 14, Vgpr(3), 1));
	code.push_back(EncodeVop2(0x18, 15, Vgpr(2), 3));

	for (u32 i = 0; i < 6u; i++) {
		AppendStoreVgpr(&code, 10u + i, i);
	}
	AppendEnd(&code);

	return {"VectorShiftCountsMaskLowBits",
	        code,
	        {},
	        {1, 2, 0x80000000u, 0x40000000u, 0x80000000u, 0xc0000000u},
	        {O::VMovB32, O::VLshlB32, O::VLshlrevB32, O::VLshrB32, O::VLshrrevB32, O::VAshrI32,
	         O::VAshrrevI32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorVop3IntegerOps() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	code.push_back(EncodeVop1(0x01, 0, InlineU32(2)));
	code.push_back(EncodeVop1(0x01, 1, InlineU32(3)));
	code.push_back(EncodeVop1(0x01, 2, InlineU32(4)));
	AppendVMovLiteral(&code, 3, 0xfffffff8u);
	code.push_back(EncodeVop1(0x01, 4, InlineU32(5)));
	AppendVMovLiteral(&code, 5, 0x11223344u);
	AppendVMovLiteral(&code, 6, 0x55667788u);
	AppendVMovLiteral(&code, 7, 0x0f0f0f0fu);
	AppendVMovLiteral(&code, 8, 0x33333333u);
	AppendVMovLiteral(&code, 9, 0xaaaaaaaau);
	AppendVMovLiteral(&code, 14, 0x00f00000u);
	code.push_back(EncodeVop1(0x01, 15, InlineU32(8)));
	AppendVMovLiteral(&code, 30, 0x00010000u);
	AppendVMovLiteral(&code, 31, 0x00010000u);

	AppendVop3(&code, 0x36d, 10, Vgpr(0), Vgpr(1), Vgpr(2));
	AppendVop3(&code, 0x152, 11, Vgpr(3), Vgpr(4), Vgpr(0));
	AppendVop3(&code, 0x155, 12, Vgpr(3), Vgpr(4), Vgpr(0));
	AppendVop3(&code, 0x158, 13, Vgpr(3), Vgpr(4), Vgpr(0));
	AppendVop3(&code, 0x153, 16, Vgpr(3), Vgpr(4), Vgpr(0));
	AppendVop3(&code, 0x156, 17, Vgpr(3), Vgpr(4), Vgpr(0));
	AppendVop3(&code, 0x159, 18, Vgpr(3), Vgpr(4), Vgpr(0));
	AppendVop3(&code, 0x15d, 19, Vgpr(0), Vgpr(4), Vgpr(2));
	AppendVop3(&code, 0x346, 20, Vgpr(1), Vgpr(0), Vgpr(2));
	AppendVop3(&code, 0x347, 21, Vgpr(0), Vgpr(1), Vgpr(0));
	AppendVop3(&code, 0x345, 22, Vgpr(7), Vgpr(8), Vgpr(0));
	AppendVop3(&code, 0x36f, 23, Vgpr(1), Vgpr(2), Vgpr(0));
	AppendVop3(&code, 0x371, 24, Vgpr(7), Vgpr(8), Vgpr(2));
	AppendVop3(&code, 0x372, 25, Vgpr(7), Vgpr(8), Vgpr(2));
	AppendVop3(&code, 0x178, 26, Vgpr(7), Vgpr(8), Vgpr(2));
	AppendVop3(&code, 0x148, 27, Vgpr(14), InlineU32(20), Vgpr(2));
	AppendVop3(&code, 0x149, 28, Vgpr(3), Vgpr(2), Vgpr(2));
	AppendVop3(&code, 0x14a, 29, Vgpr(8), Vgpr(7), Vgpr(9));
	AppendVop3(&code, 0x14e, 32, Vgpr(5), Vgpr(6), Vgpr(15));
	AppendVop3(&code, 0x363, 33, Vgpr(2), Vgpr(15));
	AppendVop3(&code, 0x169, 34, Vgpr(30), Vgpr(31));
	AppendVop3(&code, 0x16a, 35, Vgpr(30), Vgpr(31));
	AppendVop3(&code, 0x16b, 36, Vgpr(30), Vgpr(31));
	AppendVop3(&code, 0x16c, 44, Vgpr(3), Vgpr(30));
	AppendVop3(&code, 0x142, 37, Vgpr(0), Vgpr(1), Vgpr(2));
	AppendVop3(&code, 0x143, 38, Vgpr(0), Vgpr(1), Vgpr(2));
	AppendVop3(&code, 0x30f, 39, Vgpr(0), Vgpr(1));
	AppendVop3(&code, 0x310, 40, Vgpr(0), Vgpr(1));
	AppendVop3(&code, 0x319, 41, Vgpr(0), Vgpr(1));

	const u32 results[] = {10, 11, 12, 13, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
	                       27, 28, 29, 32, 33, 34, 35, 36, 44, 37, 38, 39, 40, 41};
	for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
		AppendStoreVgpr(&code, results[i], i);
	}
	AppendEnd(&code);

	return {"VectorVop3IntegerOps",
	        code,
	        {},
	        {9,
	         0xfffffff8u,
	         5,
	         2,
	         2,
	         0xfffffff8u,
	         5,
	         7,
	         16,
	         20,
	         0x3c3c3c3eu,
	         0x32,
	         0x03030307u,
	         0x3f3f3f3fu,
	         0x3c3c3c38u,
	         0x0fu,
	         0x0fu,
	         0x8b8b8b8bu,
	         0x44556677u,
	         0x00000f00u,
	         0,
	         1,
	         0,
	         0xffffffffu,
	         10,
	         10,
	         5,
	         0xffffffffu,
	         1},
	        {O::VMovB32,          O::VAdd3U32,   O::VMin3I32,   O::VMax3I32,  O::VMed3I32,
	         O::VMin3U32,         O::VMax3U32,   O::VMed3U32,   O::VSadU32,   O::VLshlAddU32,
	         O::VAddLshlU32,      O::VXadU32,    O::VLshlOrB32, O::VAndOrB32, O::VOr3B32,
	         O::VXor3B32,         O::VBfeU32,    O::VBfeI32,    O::VBfiB32,   O::VAlignbitB32,
	         O::VBfmB32,          O::VMulLoU32,  O::VMulHiU32,  O::VMulLoI32, O::VMulHiI32,
	         O::VMadI32I24,       O::VMadU32U24, O::VAddI32,    O::VSubI32,   O::VSubrevI32,
	         O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorBfeI32ArithmeticShiftMasksField() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x80000000u);
	AppendVMovLiteral(&code, 1, 0xfffffff8u);
	AppendVMovU32(&code, 2, 31);
	AppendVMovU32(&code, 3, 1);
	AppendVMovU32(&code, 4, 3);
	AppendVMovU32(&code, 5, 4);
	AppendVop3(&code, 0x149, 10, Vgpr(0), Vgpr(2), Vgpr(3));
	AppendVop3(&code, 0x149, 11, Vgpr(1), Vgpr(4), Vgpr(5));
	AppendStoreVgpr(&code, 10, 0);
	AppendStoreVgpr(&code, 11, 1);
	AppendEnd(&code);

	return {"VectorBfeI32ArithmeticShiftMasksField",
	        code,
	        {},
	        {1, 0x0fu},
	        {O::VMovB32, O::VBfeI32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorCarryAndBitCountOps() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0xffffffffu);
	code.push_back(EncodeVop1(0x01, 1, InlineU32(0)));
	AppendVMovLiteral(&code, 3, 0x0000f0f0u);
	code.push_back(EncodeVop1(0x01, 4, InlineU32(5)));

	code.push_back(EncodeVopc(0xc7, Vgpr(1), 1));
	code.push_back(EncodeVop2(0x28, 2, Vgpr(0), 1));
	code.push_back(EncodeVop2(0x22, 5, Vgpr(3), 4));
	AppendVMovLiteral(&code, 6, 0xffffffffu);
	code.push_back(EncodeVop2(0x23, 7, Vgpr(6), 4));
	code.push_back(EncodeVop2(0x24, 8, Vgpr(6), 4));

	AppendStoreVgpr(&code, 2, 0);
	AppendStoreSgpr(&code, 106, 1);
	AppendStoreVgpr(&code, 5, 2);
	AppendStoreVgpr(&code, 7, 3);
	AppendStoreVgpr(&code, 8, 4);
	AppendEnd(&code);

	return {"VectorCarryAndBitCountOps",
	        code,
	        {},
	        {0, 1, 13, 5, 5},
	        {O::VMovB32, O::VCmpTU32, O::VAddcU32, O::VBcntU32B32, O::VMbcntLoU32B32,
	         O::VMbcntHiU32B32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorMbcntUsesThreadMask() {
	using O = ShaderOpcode;

	std::vector<u32> code = {
	    EncodeVop1(0x01, 1, 0),
	    EncodeVop2(0x1a, 1, InlineU32(2), 1),
	    EncodeVop2(0x25, 1, Vgpr(0), 1),
	    EncodeVop2(0x1a, 3, InlineU32(2), 1),
	};
	AppendVMovLiteral(&code, 2, 0xffffffffu);
	code.push_back(EncodeVop1(0x01, 4, InlineU32(0)));
	code.push_back(EncodeVop2(0x23, 5, Vgpr(2), 4));
	code.push_back(EncodeVop2(0x24, 6, Vgpr(2), 5));
	AppendBufferStoreDword(&code, 6, 3);
	AppendEnd(&code);

	TestCase test;
	test.name     = "VectorMbcntUsesThreadMask";
	test.code     = code;
	test.expected = {0, 1, 2, 3, 0, 1, 2, 3};
	test.opcodes  = {O::VMovB32,        O::VLshlrevB32,      O::VAddNcU32, O::VMbcntLoU32B32,
	                 O::VMbcntHiU32B32, O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0]     = 4;
	test.compute_info.threads_num[1]     = 1;
	test.compute_info.threads_num[2]     = 1;
	test.compute_info.group_id[0]        = true;
	test.compute_info.thread_ids_num     = 1;
	test.compute_info.workgroup_register = 0;
	test.has_compute_info                = true;
	test.dispatch_x                      = 2;
	return test;
}

TestCase VectorAddcUsesPerLaneCarryIn() {
	using O = ShaderOpcode;

	std::vector<u32> code = {
	    EncodeVop1(0x01, 1, 0),
	    EncodeVop2(0x1a, 1, InlineU32(2), 1),
	    EncodeVop2(0x25, 1, Vgpr(0), 1),
	    EncodeVopc(0xc7, Vgpr(0), 0),
	    EncodeVop1(0x01, 2, InlineU32(0)),
	    EncodeVop2(0x28, 3, Vgpr(2), 2),
	    EncodeVop2(0x1a, 4, InlineU32(2), 1),
	};
	AppendBufferStoreDword(&code, 3, 4);
	AppendEnd(&code);

	TestCase test;
	test.name                        = "VectorAddcUsesPerLaneCarryIn";
	test.code                        = code;
	test.expected                    = std::vector<u32>(8, 1);
	test.opcodes                     = {O::VMovB32,  O::VLshlrevB32,      O::VAddNcU32, O::VCmpTU32,
	                                    O::VAddcU32, O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0] = 4;
	test.compute_info.threads_num[1] = 1;
	test.compute_info.threads_num[2] = 1;
	test.compute_info.group_id[0]    = true;
	test.compute_info.thread_ids_num = 1;
	test.compute_info.workgroup_register = 0;
	test.has_compute_info                = true;
	test.dispatch_x                      = 2;
	return test;
}

TestCase VectorAddcWritesPerLaneCarryOut() {
	using O = ShaderOpcode;

	std::vector<u32> code = {
	    EncodeVop1(0x01, 1, 0),
	    EncodeVop2(0x1a, 1, InlineU32(2), 1),
	    EncodeVop2(0x25, 1, Vgpr(0), 1),
	    EncodeVopc(0xc0, Vgpr(0), 0),
	};
	AppendVMovLiteral(&code, 2, 0xffffffffu);
	code.push_back(EncodeVop1(0x01, 3, InlineU32(1)));
	code.push_back(EncodeVop2(0x28, 5, Vgpr(2), 3));
	code.push_back(EncodeVop1(0x01, 6, 106));
	code.push_back(EncodeVop2(0x1a, 4, InlineU32(2), 1));
	AppendBufferStoreDword(&code, 6, 4);
	AppendEnd(&code);

	TestCase test;
	test.name                        = "VectorAddcWritesPerLaneCarryOut";
	test.code                        = code;
	test.expected                    = std::vector<u32>(8, 0x0fu);
	test.opcodes                     = {O::VMovB32,  O::VLshlrevB32,      O::VAddNcU32, O::VCmpFU32,
	                                    O::VAddcU32, O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0] = 4;
	test.compute_info.threads_num[1] = 1;
	test.compute_info.threads_num[2] = 1;
	test.compute_info.group_id[0]    = true;
	test.compute_info.thread_ids_num = 1;
	test.compute_info.workgroup_register = 0;
	test.has_compute_info                = true;
	test.dispatch_x                      = 2;
	return test;
}

TestCase VectorVop3BCarryOutWritesSgprMask() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 1, 0xffffffffu);
	AppendVMovU32(&code, 2, 1);
	AppendVMovU32(&code, 3, 0);

	AppendVop3B(&code, 0x30fu, 10, 0, Vgpr(1), Vgpr(2));
	AppendStoreSgprAtLaneDwordOffset(&code, 0, 0, 0);
	AppendVop3B(&code, 0x310u, 11, 0, Vgpr(3), Vgpr(2));
	AppendStoreSgprAtLaneDwordOffset(&code, 0, 0, 4);
	AppendVop3B(&code, 0x319u, 12, 0, Vgpr(2), Vgpr(3));
	AppendStoreSgprAtLaneDwordOffset(&code, 0, 0, 8);
	AppendEnd(&code);

	TestCase test;
	test.name                            = "VectorVop3BCarryOutWritesSgprMask";
	test.code                            = code;
	test.expected                        = std::vector<u32>(12, 0x0fu);
	test.opcodes                         = {O::VMovB32,    O::VAddI32,          O::VSubI32,
	                                        O::VSubrevI32, O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0]     = 4;
	test.compute_info.threads_num[1]     = 1;
	test.compute_info.threads_num[2]     = 1;
	test.compute_info.group_id[0]        = true;
	test.compute_info.thread_ids_num     = 1;
	test.compute_info.workgroup_register = 0;
	test.has_compute_info                = true;
	return test;
}

TestCase VectorVop3BCarryOutUsesEncodedSdst() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 1, 0xffffffffu);
	AppendVMovU32(&code, 2, 1);
	AppendVMovU32(&code, 3, 0);

	AppendVop3B(&code, 0x30fu, 10, 20, Vgpr(1), Vgpr(2));
	AppendStoreSgprAtLaneDwordOffset(&code, 20, 0, 0);
	AppendVop3B(&code, 0x310u, 11, 22, Vgpr(3), Vgpr(2));
	AppendStoreSgprAtLaneDwordOffset(&code, 22, 0, 4);
	AppendVop3B(&code, 0x319u, 12, 24, Vgpr(2), Vgpr(3));
	AppendStoreSgprAtLaneDwordOffset(&code, 24, 0, 8);
	AppendEnd(&code);

	TestCase test;
	test.name                            = "VectorVop3BCarryOutUsesEncodedSdst";
	test.code                            = code;
	test.expected                        = std::vector<u32>(12, 0x0fu);
	test.opcodes                         = {O::VMovB32,    O::VAddI32,          O::VSubI32,
	                                        O::VSubrevI32, O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0]     = 4;
	test.compute_info.threads_num[1]     = 1;
	test.compute_info.threads_num[2]     = 1;
	test.compute_info.group_id[0]        = true;
	test.compute_info.thread_ids_num     = 1;
	test.compute_info.workgroup_register = 0;
	test.has_compute_info                = true;
	return test;
}

TestCase VectorVop3BSubCoU32UsesRdna2Opcode310() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 1, 0);
	AppendVMovU32(&code, 2, 1);
	AppendVMovU32(&code, 3, 1);
	AppendVMovLiteral(&code, 4, 0xffffffffu);
	AppendVMovLiteral(&code, 5, 0x80000000u);

	code.push_back(EncodeVopc(0xc2, InlineU32(1), 0));
	code.push_back(EncodeVop2(0x01, 1, Vgpr(1), Vgpr(3)));
	code.push_back(EncodeVopc(0xc2, InlineU32(2), 0));
	code.push_back(EncodeVop2(0x01, 1, Vgpr(1), Vgpr(4)));
	code.push_back(EncodeVopc(0xc2, InlineU32(3), 0));
	code.push_back(EncodeVop2(0x01, 1, Vgpr(1), Vgpr(5)));
	code.push_back(EncodeVop2(0x01, 2, Vgpr(2), Vgpr(4)));

	AppendVop3B(&code, 0x310u, 10, 20, Vgpr(1), Vgpr(2));
	AppendStoreVgprAtLaneDwordOffset(&code, 10, 0, 0);
	AppendStoreSgprAtLaneDwordOffset(&code, 20, 0, 4);
	AppendEnd(&code);

	TestCase test;
	test.name                            = "VectorVop3BSubCoU32UsesRdna2Opcode310";
	test.code                            = code;
	test.expected                        = {0xffffffffu, 0, 0xfffffffeu, 0x80000001u, 9, 9, 9, 9};
	test.opcodes                         = {O::VMovB32, O::VCmpEqU32,        O::VCndmaskB32,
	                                        O::VSubI32, O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0]     = 4;
	test.compute_info.threads_num[1]     = 1;
	test.compute_info.threads_num[2]     = 1;
	test.compute_info.group_id[0]        = true;
	test.compute_info.thread_ids_num     = 1;
	test.compute_info.workgroup_register = 0;
	test.has_compute_info                = true;
	return test;
}

TestCase VectorMadU64U32UnsignedCarryOut() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 1, 0xffffffffu);
	AppendVMovU32(&code, 2, 2);
	AppendVMovU32(&code, 4, 2);
	AppendVMovLiteral(&code, 5, 0xffffffffu);
	AppendVop3B(&code, 0x176u, 10, 20, Vgpr(1), Vgpr(2), Vgpr(4));
	AppendStoreVgpr(&code, 10, 0);
	AppendStoreVgpr(&code, 11, 1);
	AppendStoreSgprPair(&code, 20, 2);
	AppendEnd(&code);

	return {"VectorMadU64U32UnsignedCarryOut",
	        code,
	        {},
	        {0x00000000u, 0x00000001u, 0x00000001u, 0x00000000u},
	        {O::VMovB32, O::VMadU64U32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorLaneAndPackedOps() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x12345678u);
	code.push_back(EncodeVop1(0x01, 1, InlineU32(0)));
	AppendVMovLiteral(&code, 4, 0x40003c00u);
	AppendVMovLiteral(&code, 5, 0x44004200u);
	AppendVMovLiteral(&code, 6, 0x3c003c00u);
	AppendVMovLiteral(&code, 7, 0x3f800000u);
	AppendVMovLiteral(&code, 8, 0x40000000u);
	AppendVMovLiteral(&code, 9, 0x12345678u);
	AppendVMovLiteral(&code, 10, 0xabcdef01u);

	code.push_back(EncodeVop1(0x02, 0, Vgpr(0)));
	AppendVop3(&code, 0x360, 1, Vgpr(0), InlineU32(0));
	AppendVop3(&code, 0x361, 1, 0, InlineU32(0));
	AppendVop3(&code, 0x377, 2, Vgpr(0), InlineU32(0), InlineU32(0));
	code.push_back(EncodeVop2(0x2f, 3, Vgpr(7), 8));
	AppendVop3p(&code, 0x0f, 11, Vgpr(4), Vgpr(5), 0, 0x3);
	AppendVop3p(&code, 0x10, 12, Vgpr(4), Vgpr(5), 0, 0x3);
	AppendVop3p(&code, 0x11, 13, Vgpr(4), Vgpr(5), 0, 0x3);
	AppendVop3p(&code, 0x12, 14, Vgpr(4), Vgpr(5), 0, 0x3);
	AppendVop3p(&code, 0x0e, 15, Vgpr(4), Vgpr(5), Vgpr(6), 0x7);
	AppendVop3(&code, 0x362, 16, Vgpr(7), InlineU32(1));
	AppendVop3(&code, 0x36a, 17, Vgpr(9), Vgpr(10));

	AppendStoreSgpr(&code, 0, 0);
	AppendStoreSgpr(&code, 1, 1);
	const u32 results[] = {1, 2, 3, 11, 12, 13, 14, 15, 16, 17};
	for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
		AppendStoreVgpr(&code, results[i], i + 2u);
	}
	AppendEnd(&code);

	return {"VectorLaneAndPackedOps",
	        code,
	        {},
	        {0x12345678u, 0x12345678u, 0x12345678u, 0x12345678u, 0x40003c00u, 0x46004400u,
	         0x48004200u, 0x40003c00u, 0x44004200u, 0x48804400u, 0x40000000u, 0xef015678u},
	        {O::VMovB32, O::VReadfirstlaneB32, O::VReadlaneB32, O::VWritelaneB32, O::VPermlane16B32,
	         O::VCvtPkrtzF16F32, O::VPkAddF16, O::VPkMulF16, O::VPkMinF16, O::VPkMaxF16,
	         O::VPkFmaF16, O::VLdexpF32, O::VCvtPkU16U32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase CvtPkU8F32PacksSelectedByte() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	// RDNA2 V_CVT_PK_U8_F32: uint8(S0) replaces byte S1[1:0] in S2.
	AppendVMovLiteral(&code, 0, 0x414c0000u); // 12.75f truncates to 12.
	AppendVMovLiteral(&code, 1, 0x43960000u); // 300.0f saturates to 255.
	AppendVMovLiteral(&code, 2, 0xbf800000u); // Negative values saturate to zero.
	AppendVMovLiteral(&code, 3, 0x7fc00000u); // NaN saturates to zero.
	AppendVMovLiteral(&code, 4, 0x11223344u);

	AppendVop3(&code, 0x15eu, 10, Vgpr(0), InlineU32(0), Vgpr(4));
	AppendVop3(&code, 0x15eu, 11, Vgpr(1), InlineU32(5), Vgpr(4));
	AppendVop3(&code, 0x15eu, 12, Vgpr(2), InlineU32(2), Vgpr(4));
	AppendVop3(&code, 0x15eu, 13, Vgpr(3), InlineU32(3), Vgpr(4));
	for (u32 i = 0; i < 4; i++) {
		AppendStoreVgpr(&code, 10 + i, i);
	}
	AppendEnd(&code);

	return {"CvtPkU8F32PacksSelectedByte",
	        code,
	        {},
	        {0x1122330cu, 0x1122ff44u, 0x11003344u, 0x00223344u},
	        {O::VMovB32, O::VCvtPkU8F32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase CvtPkrtzF16F32SubnormalRoundsTowardZero() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x33c00000u);
	AppendVMovLiteral(&code, 1, 0xb3c00000u);
	code.push_back(EncodeVop2(0x2f, 2, Vgpr(0), 1));
	AppendStoreVgpr(&code, 2, 0);
	AppendEnd(&code);

	return {"CvtPkrtzF16F32SubnormalRoundsTowardZero",
	        code,
	        {},
	        {0x80010001u},
	        {O::VMovB32, O::VCvtPkrtzF16F32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase PackedMinMaxF16NanAndSignedZeroEdges() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x80004000u);
	AppendVMovLiteral(&code, 1, 0x00007e00u);
	AppendVMovLiteral(&code, 2, 0x00004000u);
	AppendVMovLiteral(&code, 3, 0x80007e00u);
	AppendVMovLiteral(&code, 4, 0x40007d01u);
	AppendVMovLiteral(&code, 5, 0x7d014000u);

	AppendVop3p(&code, 0x11, 10, Vgpr(0), Vgpr(1), 0, 0x3);
	AppendVop3p(&code, 0x12, 11, Vgpr(2), Vgpr(3), 0, 0x3);
	AppendVop3p(&code, 0x11, 12, Vgpr(4), Vgpr(5), 0, 0x3);
	AppendVop3p(&code, 0x12, 13, Vgpr(4), Vgpr(5), 0, 0x3);

	for (u32 i = 0; i < 4; i++) {
		AppendStoreVgpr(&code, 10 + i, i);
	}
	AppendEnd(&code);

	return {"PackedMinMaxF16NanAndSignedZeroEdges",
	        code,
	        {},
	        {0x80004000u, 0x00004000u, 0x7f017f01u, 0x7f017f01u},
	        {O::VMovB32, O::VPkMinF16, O::VPkMaxF16, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorMinMaxF16Ops() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0xaaaa4000u); // low=2.0h
	AppendVMovLiteral(&code, 1, 0xbbbb3c00u); // low=1.0h
	AppendVMovLiteral(&code, 2, 0x12345678u);
	AppendVMovLiteral(&code, 3, 0x87654321u);
	AppendVMovLiteral(&code, 4, 0x5555aaaau);
	AppendVMovLiteral(&code, 5, 0x6666bbbbu);
	AppendVMovLiteral(&code, 6, 0x7777bbbbu);
	AppendVMovLiteral(&code, 7, 0x8888bbbbu);
	AppendVMovLiteral(&code, 8, 0x9999bbbbu);
	AppendVMovLiteral(&code, 9, 0x99990000u);
	code.push_back(EncodeVop2(0x32, 5, Vgpr(0), 1));
	code.push_back(EncodeVop2(0x33, 6, Vgpr(0), 1));
	code.push_back(EncodeVop2(0x34, 7, Vgpr(0), 1));
	code.push_back(EncodeVop2(0x35, 4, Vgpr(0), 1));
	code.push_back(EncodeVop2(0x39, 2, Vgpr(0), 1));
	code.push_back(EncodeVop2(0x3a, 3, Vgpr(0), 1));
	code.push_back(EncodeVop2(0x35, 9, 249, 1));
	code.push_back(EncodeVop2Sdwa(0, 4, 2, 4, 4));
	code.push_back(0x660000f9u);
	code.push_back(EncodeVop2Sdwa(0, 4, 2, 4, 4));
	AppendStoreVgpr(&code, 5, 0);
	AppendStoreVgpr(&code, 6, 1);
	AppendStoreVgpr(&code, 7, 2);
	AppendStoreVgpr(&code, 0, 3);
	AppendStoreVgpr(&code, 4, 4);
	AppendStoreVgpr(&code, 2, 5);
	AppendStoreVgpr(&code, 3, 6);
	AppendStoreVgpr(&code, 9, 7);
	AppendEnd(&code);

	return {"VectorMinMaxF16Ops",
	        code,
	        {},
	        {0x66664200u, 0x77773c00u, 0x8888bc00u, 0xaaaa0000u, 0x55554000u, 0x12344000u,
	         0x87653c00u, 0x99994000u},
	        {O::VMovB32, O::VAddF16, O::VSubF16, O::VSubrevF16, O::VMulF16, O::VMaxF16, O::VMinF16,
	         O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorCvtU16F16Sdwa() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x45004200u); // low=3.0h, high=5.0h
	AppendVMovLiteral(&code, 1, 0xbc007c00u); // low=+inf, high=-1.0h
	AppendVMovLiteral(&code, 2, 0x00007e00u); // low=qNaN
	AppendVMovLiteral(&code, 10, 0x12345678u);
	AppendVMovLiteral(&code, 11, 0x87654321u);
	AppendVMovLiteral(&code, 12, 0xabcd1111u);
	AppendVMovLiteral(&code, 13, 0x77772222u);
	AppendVMovLiteral(&code, 14, 0x5555aaaau);
	code.push_back(EncodeVop1(0x52, 10, 249));
	code.push_back(EncodeVop1Sdwa(0, 4, 2, 4));
	code.push_back(EncodeVop1(0x52, 11, 249));
	code.push_back(EncodeVop1Sdwa(0, 5, 2, 5));
	code.push_back(EncodeVop1(0x52, 12, 249));
	code.push_back(EncodeVop1Sdwa(1, 4, 2, 4));
	code.push_back(EncodeVop1(0x52, 13, 249));
	code.push_back(EncodeVop1Sdwa(1, 4, 2, 5));
	code.push_back(EncodeVop1(0x52, 14, 249));
	code.push_back(EncodeVop1Sdwa(2, 4, 2, 4));
	for (u32 i = 0; i < 5; i++) {
		AppendStoreVgpr(&code, 10 + i, i);
	}
	AppendEnd(&code);

	return {"VectorCvtU16F16Sdwa",
	        code,
	        {},
	        {0x12340003u, 0x00054321u, 0xabcdffffu, 0x77770000u, 0x55550000u},
	        {O::VMovB32, O::VCvtU16F16, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorMinMaxMed3F16Ops() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x40003c00u); // low=1.0h, high=2.0h
	AppendVMovLiteral(&code, 1, 0x44004200u); // low=3.0h, high=4.0h
	AppendVMovLiteral(&code, 2, 0xc8004000u); // low=2.0h, high=-8.0h
	AppendVMovLiteral(&code, 3, 0x00007e00u); // low=qNaN
	AppendVMovLiteral(&code, 4, 0x00004000u); // low=2.0h
	AppendVMovLiteral(&code, 5, 0x00003c00u); // low=1.0h
	AppendVMovLiteral(&code, 10, 0xaaaa5555u);
	AppendVMovLiteral(&code, 11, 0x12345678u);
	AppendVMovLiteral(&code, 12, 0x77772222u);

	AppendVop3(&code, 0x351, 10, Vgpr(0), Vgpr(1), Vgpr(2));
	AppendVop3(&code, 0x354, 11, Vgpr(0), Vgpr(1), Vgpr(2), 0, 0xf, false, 0, 0x4);
	AppendVop3(&code, 0x357, 12, Vgpr(3), Vgpr(4), Vgpr(5));
	for (u32 i = 0; i < 3; i++) {
		AppendStoreVgpr(&code, 10 + i, i);
	}
	AppendEnd(&code);

	return {"VectorMinMaxMed3F16Ops",
	        code,
	        {},
	        {0xaaaa3c00u, 0x48005678u, 0x77773c00u},
	        {O::VMovB32, O::VMin3F16, O::VMax3F16, O::VMed3F16, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorSpecialF16Ops() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0xc000fc00u); // low=-inf, high=-2.0h
	AppendVMovLiteral(&code, 1, 0x00004400u); // low=+4.0h
	AppendVMovLiteral(&code, 2, 0x00003c00u); // low=+1.0h
	AppendVMovLiteral(&code, 3, 0x0000bc00u); // low=-1.0h
	AppendVMovLiteral(&code, 4, 0x0000fc00u); // low=-inf
	AppendVMovLiteral(&code, 11, 0x12345678u);

	code.push_back(EncodeVop1(0x54, 10, Vgpr(0)));
	code.push_back(EncodeVop1(0x54, 11, 249));
	code.push_back(EncodeVop1Sdwa(0, 5, 2, 5));
	code.push_back(EncodeVop1(0x56, 12, Vgpr(1)));
	code.push_back(EncodeVop1(0x57, 13, Vgpr(2)));
	code.push_back(EncodeVop1(0x57, 14, Vgpr(3)));
	code.push_back(EncodeVop1(0x58, 15, Vgpr(4)));
	for (u32 i = 0; i < 6; i++) {
		AppendStoreVgpr(&code, 10 + i, i);
	}
	AppendEnd(&code);

	return {"VectorSpecialF16Ops",
	        code,
	        {},
	        {0x00008000u, 0xb8005678u, 0x00003800u, 0x00000000u, 0x0000fe00u, 0x00000000u},
	        {O::VMovB32, O::VRcpF16, O::VRsqF16, O::VLogF16, O::VExpF16, O::BufferStoreDword,
	         O::SEndpgm}};
}

TestCase VectorWritelaneIgnoresExecMask() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 2, 0xaaaaaaaau);
	AppendSMovLiteral(&code, 4, 0x12345678u);
	code.push_back(EncodeSMovB32(126, InlineU32(1)));
	code.push_back(EncodeSMovB32(127, InlineU32(0)));
	AppendVop3(&code, 0x361, 2, 4, InlineU32(1));
	code.push_back(EncodeSMovB32(126, InlineU32(0xf)));
	code.push_back(EncodeSMovB32(127, InlineU32(0)));
	code.push_back(EncodeVop2(0x1a, 3, InlineU32(2), 0));
	AppendBufferStoreDword(&code, 2, 3);
	AppendEnd(&code);

	TestCase test;
	test.name                        = "VectorWritelaneIgnoresExecMask";
	test.code                        = code;
	test.expected                    = {0xaaaaaaaau, 0x12345678u, 0xaaaaaaaau, 0xaaaaaaaau};
	test.opcodes                     = {O::VMovB32,     O::SMovB32,          O::VWritelaneB32,
	                                    O::VLshlrevB32, O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0] = 4;
	test.compute_info.threads_num[1] = 1;
	test.compute_info.threads_num[2] = 1;
	test.compute_info.thread_ids_num = 1;
	test.has_compute_info            = true;
	return test;
}

TestCase VectorReadlaneFromInactiveWrittenLane() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendSMovLiteral(&code, 4, 0x12345678u);
	AppendVop3(&code, 0x361, 2, 4, InlineU32(4));
	AppendSMovLiteral(&code, 4, 0xdeadbeefu);
	AppendVop3(&code, 0x360, 5, Vgpr(2), InlineU32(4));
	AppendStoreSgpr(&code, 5, 0);
	AppendEnd(&code);

	TestCase test;
	test.name                        = "VectorReadlaneInactiveWrittenLane";
	test.code                        = code;
	test.expected                    = {0x12345678u};
	test.opcodes                     = {O::SMovB32, O::VWritelaneB32,    O::VReadlaneB32,
	                                    O::VMovB32, O::BufferStoreDword, O::SEndpgm};
	test.forbidden_spirv             = {"OpGroupNonUniformShuffle"};
	test.compute_info.threads_num[0] = 4;
	test.compute_info.threads_num[1] = 1;
	test.compute_info.threads_num[2] = 1;
	test.compute_info.thread_ids_num = 1;
	test.has_compute_info            = true;
	return test;
}

TestCase VectorLaneWave32RuntimeSelectorWraps() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	code.push_back(EncodeSop2(0x00, 5, 4, InlineU32(33)));
	AppendSMovLiteral(&code, 6, 0x12345678u);
	AppendVMovU32(&code, 2, 0);
	AppendVop3(&code, 0x361, 2, 6, 5);
	AppendVop3(&code, 0x360, 7, Vgpr(2), InlineU32(1));
	AppendStoreSgpr(&code, 7, 0);
	AppendEnd(&code);

	TestCase test;
	test.name           = "VectorLaneWave32RuntimeSelectorWraps";
	test.code           = code;
	test.expected       = {0x12345678u};
	test.opcodes        = {O::SAddU32, O::SMovB32,          O::VWritelaneB32, O::VReadlaneB32,
	                       O::VMovB32, O::BufferStoreDword, O::SEndpgm};
	test.required_spirv = {"OpGroupNonUniformShuffle"};
	test.compute_info.threads_num[0]     = 4;
	test.compute_info.threads_num[1]     = 1;
	test.compute_info.threads_num[2]     = 1;
	test.compute_info.group_id[0]        = true;
	test.compute_info.wave_size          = 32;
	test.compute_info.thread_ids_num     = 1;
	test.compute_info.workgroup_register = 4;
	test.has_compute_info                = true;
	return test;
}

TestCase VectorPermlanex16() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 3, 0x76543210u);
	AppendVMovLiteral(&code, 4, 0xfedcba98u);
	code.push_back(EncodeVop2(0x1a, 2, InlineU32(2), 0));
	AppendVMovLiteral(&code, 5, 0xfeedbabeu);
	AppendVop3(&code, 0x378, 1, Vgpr(5), Vgpr(3), Vgpr(4));
	AppendBufferStoreDword(&code, 1, 2);
	AppendEnd(&code);

	TestCase test;
	test.name     = "VectorPermlanex16";
	test.code     = code;
	test.expected = std::vector<u32>(32, 0xfeedbabeu);
	test.opcodes  = {O::VMovB32, O::VPermlanex16B32, O::VLshlrevB32, O::BufferStoreDword,
	                 O::SEndpgm};
	test.compute_info.threads_num[0] = 32;
	test.compute_info.threads_num[1] = 1;
	test.compute_info.threads_num[2] = 1;
	test.compute_info.thread_ids_num = 1;
	test.has_compute_info            = true;
	return test;
}

TestCase VectorPermlane16FetchInactiveZero() {
	using O = ShaderOpcode;

	std::vector<u32> code = {
	    EncodeVop1(0x01, 1, 0),           EncodeVop2(0x25, 1, InlineU32(10), 1),
	    EncodeSMovB32(126, InlineU32(1)), EncodeSMovB32(127, InlineU32(0)),
	    EncodeSMovB32(0, InlineU32(1)),   EncodeSMovB32(1, InlineU32(0)),
	};
	AppendVop3(&code, 0x377, 2, Vgpr(1), 0, 1);
	AppendStoreVgpr(&code, 2, 0);
	AppendEnd(&code);

	TestCase test;
	test.name                        = "VectorPermlane16FetchInactiveZero";
	test.code                        = code;
	test.expected                    = {0};
	test.opcodes                     = {O::VMovB32,        O::VAddNcU32,        O::SMovB32,
	                                    O::VPermlane16B32, O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0] = 4;
	test.compute_info.threads_num[1] = 1;
	test.compute_info.threads_num[2] = 1;
	test.compute_info.thread_ids_num = 1;
	test.has_compute_info            = true;
	return test;
}

TestCase VectorPermlane16FetchInactiveFi() {
	using O = ShaderOpcode;

	std::vector<u32> code = {
	    EncodeVop1(0x01, 1, Vgpr(0)),     EncodeVop2(0x25, 1, InlineU32(10), 1),
	    EncodeSMovB32(126, InlineU32(1)), EncodeSMovB32(127, InlineU32(0)),
	    EncodeSMovB32(0, InlineU32(1)),   EncodeSMovB32(1, InlineU32(0)),
	};
	AppendVop3(&code, 0x377, 2, Vgpr(1), 0, 1, 0, 1);
	AppendStoreVgpr(&code, 2, 0);
	AppendEnd(&code);

	TestCase test;
	test.name                        = "VectorPermlane16FetchInactiveFi";
	test.code                        = code;
	test.expected                    = {11};
	test.opcodes                     = {O::VMovB32,        O::VAddNcU32,        O::SMovB32,
	                                    O::VPermlane16B32, O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0] = 4;
	test.compute_info.threads_num[1] = 1;
	test.compute_info.threads_num[2] = 1;
	test.compute_info.thread_ids_num = 1;
	test.has_compute_info            = true;
	return test;
}

TestCase VectorDppQuadPermuteReverse() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 1, 100);
	code.push_back(EncodeVop2(0x25, 2, 250, 1));
	code.push_back(EncodeVop2Dpp(0, 0x01b));
	code.push_back(EncodeVop2(0x1a, 3, InlineU32(2), 0));
	AppendBufferStoreDword(&code, 2, 3);
	AppendEnd(&code);

	TestCase test;
	test.name     = "VectorDppQuadPermuteReverse";
	test.code     = code;
	test.expected = {103, 102, 101, 100, 107, 106, 105, 104};
	test.opcodes  = {O::VMovB32, O::VAddNcU32, O::VLshlrevB32, O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0] = 8;
	test.compute_info.threads_num[1] = 1;
	test.compute_info.threads_num[2] = 1;
	test.compute_info.thread_ids_num = 1;
	test.has_compute_info            = true;
	return test;
}

TestCase VectorDppBankMaskPreservesDestination() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 2, 0xaaaaaaaau);
	AppendVMovU32(&code, 1, 100);
	code.push_back(EncodeVop2(0x25, 2, 250, 1));
	code.push_back(EncodeVop2Dpp(0, 0x0e4, 0xf, 0xe));
	code.push_back(EncodeVop2(0x1a, 3, InlineU32(2), 0));
	AppendBufferStoreDword(&code, 2, 3);
	AppendEnd(&code);

	TestCase test;
	test.name     = "VectorDppBankMaskPreservesDestination";
	test.code     = code;
	test.expected = {0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 104, 105, 106, 107};
	test.opcodes  = {O::VMovB32, O::VAddNcU32, O::VLshlrevB32, O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0] = 8;
	test.compute_info.threads_num[1] = 1;
	test.compute_info.threads_num[2] = 1;
	test.compute_info.thread_ids_num = 1;
	test.has_compute_info            = true;
	return test;
}

TestCase VectorDppBoundsControlZeroPreservesDestination() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 2, 0xaaaaaaaau);
	AppendVMovU32(&code, 1, 100);
	code.push_back(EncodeVop2(0x25, 2, 250, 1));
	code.push_back(EncodeVop2Dpp(0, 0x111));
	code.push_back(EncodeVop2(0x1a, 3, InlineU32(2), 0));
	AppendBufferStoreDword(&code, 2, 3);
	AppendEnd(&code);

	TestCase test;
	test.name     = "VectorDppBoundsControlZeroPreservesDestination";
	test.code     = code;
	test.expected = {0xaaaaaaaau, 100, 101, 102, 103, 104, 105, 106};
	test.opcodes  = {O::VMovB32, O::VAddNcU32, O::VLshlrevB32, O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0] = 8;
	test.compute_info.threads_num[1] = 1;
	test.compute_info.threads_num[2] = 1;
	test.compute_info.thread_ids_num = 1;
	test.has_compute_info            = true;
	return test;
}

TestCase Vop3LdexpSourceModifier() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 9, 0xbfc00000u);
	code.push_back(0xd7620107u);
	code.push_back(0x00018509u);
	AppendStoreVgpr(&code, 7, 0);
	AppendEnd(&code);

	return {"Vop3LdexpSourceModifier",
	        code,
	        {},
	        {0x3ec00000u},
	        {O::VMovB32, O::VLdexpF32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase Vop1MoveRelSource() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 12, 0x11111111u);
	AppendVMovLiteral(&code, 13, 0x22222222u);
	AppendVMovLiteral(&code, 14, 0x33333333u);
	code.push_back(EncodeSMovB32(124, InlineU32(2)));
	code.push_back(0x7e6e870cu);
	AppendStoreVgpr(&code, 55, 0);
	AppendEnd(&code);

	return {"Vop1MoveRelSource",
	        code,
	        {},
	        {0x33333333u},
	        {O::VMovB32, O::SMovB32, O::VMovrelsB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase Vop1MoveRelDestination() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 1, 0x12345678u);
	AppendVMovLiteral(&code, 5, 0xaaaaaaaau);
	AppendVMovLiteral(&code, 6, 0xbbbbbbbbu);
	AppendVMovLiteral(&code, 7, 0xccccccccu);
	code.push_back(EncodeSMovB32(124, InlineU32(2)));
	code.push_back(EncodeSMovB32(126, InlineU32(1)));
	code.push_back(EncodeSMovB32(127, InlineU32(0)));
	code.push_back(EncodeVop1(0x42, 5, Vgpr(1)));
	code.push_back(EncodeSMovB32(126, InlineU32(0xf)));
	code.push_back(EncodeSMovB32(127, InlineU32(0)));
	code.push_back(EncodeVop2(0x1a, 4, InlineU32(2), 0));
	AppendBufferStoreDword(&code, 7, 4);
	AppendEnd(&code);

	TestCase test;
	test.name                        = "Vop1MoveRelDestination";
	test.code                        = code;
	test.expected                    = {0x12345678u, 0xccccccccu, 0xccccccccu, 0xccccccccu};
	test.opcodes                     = {O::VMovB32,     O::SMovB32,          O::VMovreldB32,
	                                    O::VLshlrevB32, O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0] = 4;
	test.compute_info.threads_num[1] = 1;
	test.compute_info.threads_num[2] = 1;
	test.compute_info.thread_ids_num = 1;
	test.has_compute_info            = true;
	return test;
}

TestCase VectorFloatSpecialOps() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x3f800000u);
	AppendVMovLiteral(&code, 1, 0x40000000u);
	AppendVMovLiteral(&code, 2, 0x40400000u);
	AppendVMovLiteral(&code, 3, 0x40800000u);
	AppendVMovLiteral(&code, 4, 0x40003c00u);
	AppendVMovLiteral(&code, 5, 0x44004200u);
	AppendVMovLiteral(&code, 6, 0x3f800000u);
	AppendVMovLiteral(&code, 7, 0x3f800000u);
	AppendVMovLiteral(&code, 8, 0xbf800000u);
	AppendVMovLiteral(&code, 10, 0x3f000000u);
	AppendVMovLiteral(&code, 12, 0xaaaabbbbu);
	AppendVMovLiteral(&code, 13, 0xaaaabbbbu);

	code.push_back(EncodeVop2(0x02, 6, Vgpr(4), 5));
	AppendVop3(&code, 0x368, 9, Vgpr(7), Vgpr(8));
	AppendVop3(&code, 0x369, 11, Vgpr(7), Vgpr(10));
	AppendVop3p(&code, 0x21, 12, Vgpr(1), Vgpr(2), Vgpr(0));
	AppendVop3p(&code, 0x22, 13, Vgpr(1), Vgpr(2), Vgpr(0));
	AppendVop3(&code, 0x144, 14, Vgpr(0), Vgpr(1), Vgpr(2));
	AppendVop3(&code, 0x145, 15, Vgpr(0), Vgpr(1), Vgpr(2));
	AppendVop3(&code, 0x146, 16, Vgpr(0), Vgpr(1), Vgpr(2));
	AppendVop3(&code, 0x147, 17, Vgpr(0), Vgpr(1), Vgpr(2));

	const u32 results[] = {6, 9, 11, 12, 13, 14, 15, 16, 17};
	for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
		AppendStoreVgpr(&code, results[i], i);
	}
	AppendEnd(&code);

	return {"VectorFloatSpecialOps",
	        code,
	        {},
	        {0x41400000u, 0x80017fffu, 0x8000ffffu, 0xaaaa4700u, 0x4700bbbbu, 0x40800000u,
	         0x3f800000u, 0xc0000000u, 0x40c00000u},
	        {O::VMovB32, O::VDot2cF32F16, O::VCvtPknormI16F32, O::VCvtPknormU16F32, O::VMadMixloF16,
	         O::VMadMixhiF16, O::VCubeidF32, O::VCubescF32, O::VCubetcF32, O::VCubemaF32,
	         O::BufferStoreDword, O::SEndpgm}};
}

TestCase MadMixF16LiteralHalfSourceUsesOpsel() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x3f800000u);
	AppendVMovLiteral(&code, 12, 0xaaaabbbbu);
	AppendVMovLiteral(&code, 13, 0x11112222u);

	AppendVop3p(&code, 0x21, 12, 255u, Vgpr(0), Vgpr(0), 0x1, 0x1);
	code.push_back(0x40003c00u);
	AppendVop3p(&code, 0x22, 13, 255u, Vgpr(0), Vgpr(0), 0x1, 0x0);
	code.push_back(0x40003c00u);

	AppendStoreVgpr(&code, 12, 0);
	AppendStoreVgpr(&code, 13, 1);
	AppendEnd(&code);

	return {"MadMixF16LiteralHalfSourceUsesOpsel",
	        code,
	        {},
	        {0xaaaa4200u, 0x40002222u},
	        {O::VMovB32, O::VMadMixloF16, O::VMadMixhiF16, O::BufferStoreDword, O::SEndpgm}};
}

TestCase MadMixF16NegHiIsAbsAndNegIsIndependent() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x3f800000u);
	AppendVMovLiteral(&code, 1, 0xc0000000u);
	AppendVMovLiteral(&code, 2, 0x40000000u);
	AppendVMovLiteral(&code, 12, 0xaaaabbbbu);
	AppendVMovLiteral(&code, 13, 0x11112222u);

	AppendVop3p(&code, 0x21, 12, Vgpr(1), Vgpr(0), Vgpr(0), 0, 0, 0x1);
	AppendVop3p(&code, 0x22, 13, Vgpr(2), Vgpr(0), Vgpr(0), 0, 0, 0, 0x1);

	AppendStoreVgpr(&code, 12, 0);
	AppendStoreVgpr(&code, 13, 1);
	AppendEnd(&code);

	return {"MadMixF16NegHiIsAbsAndNegIsIndependent",
	        code,
	        {},
	        {0xaaaa4200u, 0xbc002222u},
	        {O::VMovB32, O::VMadMixloF16, O::VMadMixhiF16, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorVop3FmaF16UsesRdna2Opcode34b() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 1, 0x40003c00u);
	AppendVMovLiteral(&code, 2, 0x44004200u);
	AppendVMovLiteral(&code, 3, 0x3c003c00u);
	AppendVMovLiteral(&code, 10, 0xaaaa5555u);
	AppendVMovLiteral(&code, 11, 0xbbbb5555u);

	AppendVop3(&code, 0x34bu, 10, Vgpr(1), Vgpr(2), Vgpr(3));
	AppendVop3(&code, 0x34bu, 11, Vgpr(1), Vgpr(2), Vgpr(3), 0, 0xfu);

	AppendStoreVgpr(&code, 10, 0);
	AppendStoreVgpr(&code, 11, 1);
	AppendEnd(&code);

	return {"VectorVop3FmaF16UsesRdna2Opcode34b",
	        code,
	        {},
	        {0xaaaa4400u, 0x48805555u},
	        {O::VMovB32, O::VFmaF16, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorFloatArithmeticOps() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x3f800000u);
	AppendVMovLiteral(&code, 1, 0x40000000u);
	AppendVMovLiteral(&code, 2, 0x40800000u);
	AppendVMovLiteral(&code, 3, 0x40a00000u);
	AppendVMovLiteral(&code, 4, 0x40400000u);
	AppendVMovLiteral(&code, 11, 0x40800000u);
	AppendVMovLiteral(&code, 17, 0x40800000u);

	code.push_back(EncodeVop2(0x03, 5, Vgpr(0), 1));
	code.push_back(EncodeVop2(0x04, 6, Vgpr(3), 1));
	code.push_back(EncodeVop2(0x05, 7, Vgpr(1), 3));
	code.push_back(EncodeVop2(0x08, 8, Vgpr(1), 2));
	code.push_back(EncodeVop1(0x01, 21, Vgpr(8)));
	code.push_back(EncodeVop2(0x0f, 9, Vgpr(3), 1));
	code.push_back(EncodeVop2(0x10, 10, Vgpr(3), 1));
	code.push_back(EncodeVop2(0x1f, 11, Vgpr(1), 4));
	AppendVop3(&code, 0x141, 12, Vgpr(1), Vgpr(4), Vgpr(2));
	AppendVop3(&code, 0x14b, 13, Vgpr(1), Vgpr(4), Vgpr(2));
	AppendVop3(&code, 0x151, 14, Vgpr(3), Vgpr(1), Vgpr(2));
	AppendVop3(&code, 0x154, 15, Vgpr(3), Vgpr(1), Vgpr(2));
	AppendVop3(&code, 0x157, 16, Vgpr(3), Vgpr(1), Vgpr(2));
	code.push_back(EncodeVop2(0x20, 18, Vgpr(1), 17));
	code.push_back(0x3f800000u);
	code.push_back(EncodeVop2(0x21, 19, Vgpr(1), 4));
	code.push_back(0x40000000u);
	code.push_back(EncodeVop2(0x03, 20, 249, 1));
	code.push_back(EncodeVop2Sdwa(0));
	AppendSMovLiteral(&code, 70, 0x40000000u);
	code.push_back(0x06088cf9u);
	code.push_back(0x868606f2u);
	AppendSMovLiteral(&code, 26, 0x40000000u);
	code.push_back(0x081034f9u);
	code.push_back(0x868606f2u);
	code.push_back(0xd51f8011u);
	code.push_back(0x00020d0bu);
	AppendVMovLiteral(&code, 22, 0xbf800000u);
	code.push_back(EncodeVop2(0x05, 23, 249, 26));
	code.push_back(EncodeVop2Sdwa(22, 6, 0, 6, 6, 0, 0, 0, 1, 0, 0, 0, 1));
	AppendVMovLiteral(&code, 24, 0x3f800000u);
	AppendVMovLiteral(&code, 25, 0x3f800000u);
	code.push_back(EncodeVop2(0x03, 26, 249, 25));
	code.push_back(EncodeVop2Sdwa(24, 6, 0, 6, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1));
	AppendVMovLiteral(&code, 27, 0xbf800000u);
	AppendVMovLiteral(&code, 28, 0x40000000u);
	AppendVop3(&code, 0x104, 29, Vgpr(27), Vgpr(28), 0, 1);
	AppendVop3(&code, 0x103, 30, Vgpr(0), Vgpr(1), 0, 0, 0, false, 1);
	AppendVop3(&code, 0x151, 32, Vgpr(0), Vgpr(1), Vgpr(2), 0, 0, false, 1);
	AppendVMovLiteral(&code, 33, 0xbf800000u);
	AppendVMovLiteral(&code, 34, 0x40000000u);
	code.push_back(EncodeVop2(0x03, 35, 250, 34));
	code.push_back(EncodeVop2Dpp(33, 0, 0xf, 0xf, 0, 1));

	const u32 results[] = {5,  6,  7,  21, 9, 10, 11, 12, 13, 14, 15, 16,
	                       18, 19, 20, 4,  8, 17, 23, 26, 29, 30, 32, 35};
	for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
		AppendStoreVgpr(&code, results[i], i);
	}
	AppendEnd(&code);

	return {"VectorFloatArithmeticOps",
	        code,
	        {},
	        {0x40400000u, 0x40400000u, 0x40400000u, 0x41000000u, 0x40000000u, 0x40a00000u,
	         0x41200000u, 0x41200000u, 0x41200000u, 0x40000000u, 0x40a00000u, 0x40800000u,
	         0x40c00000u, 0x41000000u, 0x40400000u, 0x40400000u, 0xbf800000u, 0x3f800000u,
	         0x3f800000u, 0x40800000u, 0xbf800000u, 0x40c00000u, 0x40000000u, 0x40400000u},
	        {O::SMovB32, O::VMovB32, O::VAddF32, O::VSubF32, O::VSubrevF32, O::VMulF32, O::VMinF32,
	         O::VMaxF32, O::VMacF32, O::VMadF32, O::VFmaF32, O::VMin3F32, O::VMax3F32, O::VMed3F32,
	         O::VMadmkF32, O::VMadakF32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorMinMaxF32NanAndSignedZeroEdges() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x40000000u);
	AppendVMovLiteral(&code, 1, 0x7fc00000u);
	AppendVMovLiteral(&code, 2, 0x00000000u);
	AppendVMovLiteral(&code, 3, 0x80000000u);
	AppendVMovLiteral(&code, 4, 0x40400000u);
	AppendVMovLiteral(&code, 5, 0x3f800000u);
	AppendVMovLiteral(&code, 6, 0x7fa00001u);

	code.push_back(EncodeVop2(0x0f, 10, Vgpr(0), 1));
	code.push_back(EncodeVop2(0x10, 11, Vgpr(0), 1));
	code.push_back(EncodeVop2(0x0f, 12, Vgpr(3), 2));
	code.push_back(EncodeVop2(0x10, 13, Vgpr(2), 3));
	code.push_back(EncodeVop2(0x0f, 14, Vgpr(6), 0));
	code.push_back(EncodeVop2(0x10, 15, Vgpr(0), 6));
	AppendVop3(&code, 0x151, 16, Vgpr(0), Vgpr(1), Vgpr(4));
	AppendVop3(&code, 0x154, 17, Vgpr(0), Vgpr(1), Vgpr(5));

	for (u32 i = 0; i < 8; i++) {
		AppendStoreVgpr(&code, 10 + i, i);
	}
	AppendEnd(&code);

	return {"VectorMinMaxF32NanAndSignedZeroEdges",
	        code,
	        {},
	        {0x40000000u, 0x40000000u, 0x80000000u, 0x00000000u, 0x7fe00001u, 0x7fe00001u,
	         0x40000000u, 0x40000000u},
	        {O::VMovB32, O::VMinF32, O::VMaxF32, O::VMin3F32, O::VMax3F32, O::BufferStoreDword,
	         O::SEndpgm}};
}

TestCase VectorMed3F32NanUsesMin3Path() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x40000000u);
	AppendVMovLiteral(&code, 1, 0x40400000u);
	AppendVMovLiteral(&code, 2, 0x7fa00001u);
	AppendVMovLiteral(&code, 3, 0x7fc00000u);

	AppendVop3(&code, 0x157, 10, Vgpr(0), Vgpr(1), Vgpr(2));
	AppendVop3(&code, 0x157, 11, Vgpr(0), Vgpr(1), Vgpr(3));
	AppendVop3(&code, 0x157, 12, Vgpr(2), Vgpr(0), Vgpr(1));

	AppendStoreVgpr(&code, 10, 0);
	AppendStoreVgpr(&code, 11, 1);
	AppendStoreVgpr(&code, 12, 2);
	AppendEnd(&code);

	return {"VectorMed3F32NanUsesMin3Path",
	        code,
	        {},
	        {0x7fe00001u, 0x40000000u, 0x40400000u},
	        {O::VMovB32, O::VMed3F32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorFloatConversionOps() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0xfffffffdu);
	code.push_back(EncodeVop1(0x01, 1, InlineU32(7)));
	AppendVMovLiteral(&code, 2, 0x40e00000u);
	AppendVMovLiteral(&code, 3, 0xc0400000u);
	AppendVMovLiteral(&code, 4, 0x3f800000u);
	AppendVMovLiteral(&code, 5, 0x00003c00u);
	AppendVMovLiteral(&code, 6, 0x40700000u);
	AppendVMovLiteral(&code, 7, 0x44332211u);
	AppendVMovLiteral(&code, 8, 0x40000000u);
	AppendVMovLiteral(&code, 9, 0x40800000u);
	AppendVMovLiteral(&code, 10, 0x40100000u);
	AppendVMovLiteral(&code, 11, 0x40300000u);
	code.push_back(EncodeVop1(0x01, 12, InlineU32(0)));
	code.push_back(EncodeVop1(0x01, 37, InlineU32(15)));

	code.push_back(EncodeVop1(0x05, 13, Vgpr(0)));
	code.push_back(EncodeVop1(0x06, 14, Vgpr(1)));
	code.push_back(EncodeVop1(0x07, 15, Vgpr(2)));
	code.push_back(EncodeVop1(0x08, 16, Vgpr(3)));
	code.push_back(EncodeVop1(0x0a, 17, Vgpr(4)));
	code.push_back(EncodeVop1(0x0b, 18, Vgpr(5)));
	code.push_back(EncodeVop1(0x0c, 42, Vgpr(11)));
	code.push_back(EncodeVop1(0x0d, 19, Vgpr(6)));
	code.push_back(EncodeVop1(0x11, 20, Vgpr(7)));
	code.push_back(EncodeVop1(0x12, 21, Vgpr(7)));
	code.push_back(EncodeVop1(0x13, 22, Vgpr(7)));
	code.push_back(EncodeVop1(0x14, 23, Vgpr(7)));
	code.push_back(EncodeVop1(0x2a, 24, Vgpr(8)));
	code.push_back(EncodeVop1(0x20, 25, Vgpr(10)));
	code.push_back(EncodeVop1(0x21, 26, Vgpr(11)));
	code.push_back(EncodeVop1(0x22, 27, Vgpr(10)));
	code.push_back(EncodeVop1(0x23, 28, Vgpr(10)));
	code.push_back(EncodeVop1(0x24, 29, Vgpr(11)));
	code.push_back(EncodeVop1(0x25, 30, Vgpr(4)));
	code.push_back(EncodeVop1(0x27, 36, Vgpr(9)));
	code.push_back(EncodeVop1(0x2e, 32, Vgpr(9)));
	code.push_back(EncodeVop1(0x33, 33, Vgpr(9)));
	code.push_back(EncodeVop1(0x35, 34, Vgpr(12)));
	code.push_back(EncodeVop1(0x36, 35, Vgpr(12)));
	code.push_back(EncodeVop1(0x0e, 38, Vgpr(37)));
	code.push_back(EncodeVop1(0x2a, 39, 249));
	code.push_back(EncodeVop1Sdwa(8, 6, 0, 6, 0, 0, 0, 0, 0, 1));
	AppendVMovLiteral(&code, 40, 0xc0000000u);
	AppendVop3(&code, 0x1aa, 41, Vgpr(40), 0, 0, 1, 0, false, 1);
	AppendVMovLiteral(&code, 2, 0xc0003c00u); // low=1.0h, high=-2.0h
	code.push_back(0x7e1016f9u);
	code.push_back(0x00250602u);

	const u32 results[] = {13, 14, 15, 16, 17, 18, 42, 19, 20, 21, 22, 23, 24, 25,
	                       26, 27, 28, 29, 30, 36, 32, 33, 34, 35, 38, 39, 41, 8};
	for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
		AppendStoreVgpr(&code, results[i], i);
	}
	AppendEnd(&code);

	return {
	    "VectorFloatConversionOps",
	    code,
	    {},
	    {0xc0400000u, 0x40e00000u, 7,           0xfffffffdu, 0x00003c00u, 0x3f800000u, 3,
	     3,           0x41880000u, 0x42080000u, 0x424c0000u, 0x42880000u, 0x3f000000u, 0x3e800000u,
	     0x40000000u, 0x40400000u, 0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u, 0x3f000000u,
	     0x40000000u, 0,           0x3f800000u, 0xbd800000u, 0x3f800000u, 0x3f800000u, 0x40000000u},
	    {O::VMovB32,       O::VCvtF32I32,       O::VCvtF32U32,    O::VCvtU32F32,    O::VCvtI32F32,
	     O::VCvtF16F32,    O::VCvtF32F16,       O::VCvtRpiI32F32, O::VCvtFlrI32F32, O::VCvtOffF32I4,
	     O::VCvtF32Ubyte0, O::VCvtF32Ubyte1,    O::VCvtF32Ubyte2, O::VCvtF32Ubyte3, O::VRcpF32,
	     O::VFractF32,     O::VTruncF32,        O::VCeilF32,      O::VRndneF32,     O::VFloorF32,
	     O::VExpF32,       O::VLogF32,          O::VRsqF32,       O::VSqrtF32,      O::VSinF32,
	     O::VCosF32,       O::BufferStoreDword, O::SEndpgm}};
}

TestCase CvtF32ToIntSaturatesNaNAndOutOfRange() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x7fc00000u);
	AppendVMovLiteral(&code, 1, 0xbf800000u);
	AppendVMovLiteral(&code, 2, 0x7f800000u);
	AppendVMovLiteral(&code, 3, 0x4f800000u);
	AppendVMovLiteral(&code, 4, 0xff800000u);
	AppendVMovLiteral(&code, 5, 0x4f000000u);

	code.push_back(EncodeVop1(0x07, 10, Vgpr(0)));
	code.push_back(EncodeVop1(0x07, 11, Vgpr(1)));
	code.push_back(EncodeVop1(0x07, 12, Vgpr(2)));
	code.push_back(EncodeVop1(0x07, 13, Vgpr(3)));
	code.push_back(EncodeVop1(0x08, 14, Vgpr(0)));
	code.push_back(EncodeVop1(0x08, 15, Vgpr(2)));
	code.push_back(EncodeVop1(0x08, 16, Vgpr(4)));
	code.push_back(EncodeVop1(0x08, 17, Vgpr(5)));

	for (u32 i = 0; i < 8; i++) {
		AppendStoreVgpr(&code, 10 + i, i);
	}
	AppendEnd(&code);

	return {"CvtF32ToIntSaturatesNaNAndOutOfRange",
	        code,
	        {},
	        {0, 0, 0xffffffffu, 0xffffffffu, 0, 0x7fffffffu, 0x80000000u, 0x7fffffffu},
	        {O::VMovB32, O::VCvtU32F32, O::VCvtI32F32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorSpecialF32FlushesDenormalInputs() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x00000001u);
	code.push_back(EncodeVop1(0x27, 1, Vgpr(0)));
	code.push_back(EncodeVop1(0x2a, 2, Vgpr(0)));
	code.push_back(EncodeVop1(0x2e, 3, Vgpr(0)));
	code.push_back(EncodeVop1(0x33, 4, Vgpr(0)));
	AppendStoreVgpr(&code, 1, 0);
	AppendStoreVgpr(&code, 2, 1);
	AppendStoreVgpr(&code, 3, 2);
	AppendStoreVgpr(&code, 4, 3);
	AppendEnd(&code);

	return {"VectorSpecialF32FlushesDenormalInputs",
	        code,
	        {},
	        {0xff800000u, 0x7f800000u, 0x7f800000u, 0x00000000u},
	        {O::VMovB32, O::VLogF32, O::VRcpF32, O::VRsqF32, O::VSqrtF32, O::BufferStoreDword,
	         O::SEndpgm}};
}

TestCase VectorSinCosMaxFiniteSpecialCases() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0xff7fffffu);
	code.push_back(EncodeVop1(0x35, 1, Vgpr(0)));
	code.push_back(EncodeVop1(0x36, 2, Vgpr(0)));
	AppendStoreVgpr(&code, 1, 0);
	AppendStoreVgpr(&code, 2, 1);
	AppendEnd(&code);

	return {"VectorSinCosMaxFiniteSpecialCases",
	        code,
	        {},
	        {0x00000000u, 0x3f800000u},
	        {O::VMovB32, O::VSinF32, O::VCosF32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorCompareOps() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	std::vector<u32> results;
	std::vector<u32> expected;
	u32              dst = 10;

	code.push_back(EncodeVop1(0x01, 1, InlineU32(1)));
	AppendVMovLiteral(&code, 2, 0x40000000u);
	AppendVMovLiteral(&code, 3, 0x40400000u);
	code.push_back(EncodeVop1(0x01, 4, InlineU32(1)));
	AppendVMovLiteral(&code, 5, 0xffffffffu);

	auto append_compare = [&](u32 opcode, u32 src0, u32 src1, bool value) {
		if (dst == 31u) {
			dst++;
		}
		code.push_back(EncodeVopc(opcode, src0, src1));
		code.push_back(EncodeVop2(0x01, dst, InlineU32(0), 1));
		results.push_back(dst);
		expected.push_back(value ? 1u : 0u);
		dst++;
	};

	append_compare(0x00, Vgpr(2), 3, false);
	append_compare(0x01, Vgpr(2), 3, true);
	append_compare(0x02, Vgpr(2), 3, false);
	append_compare(0x03, Vgpr(2), 3, true);
	append_compare(0x04, Vgpr(2), 3, false);
	append_compare(0x05, Vgpr(2), 3, true);
	append_compare(0x06, Vgpr(2), 3, false);
	append_compare(0x07, Vgpr(2), 3, true);
	append_compare(0x08, Vgpr(2), 3, false);
	append_compare(0x09, Vgpr(2), 3, true);
	append_compare(0x0a, Vgpr(2), 3, false);
	append_compare(0x0b, Vgpr(2), 3, true);
	append_compare(0x0c, Vgpr(2), 3, false);
	append_compare(0x0d, Vgpr(2), 3, true);
	append_compare(0x0e, Vgpr(2), 3, false);
	append_compare(0x0f, Vgpr(2), 3, true);

	append_compare(0x80, Vgpr(5), 4, false);
	append_compare(0x81, Vgpr(5), 4, true);
	append_compare(0x82, Vgpr(5), 4, false);
	append_compare(0x83, Vgpr(5), 4, true);
	append_compare(0x84, Vgpr(5), 4, false);
	append_compare(0x85, Vgpr(5), 4, true);
	append_compare(0x86, Vgpr(5), 4, false);
	append_compare(0x87, Vgpr(5), 4, true);

	append_compare(0xc0, Vgpr(4), 5, false);
	append_compare(0xc1, Vgpr(4), 5, true);
	append_compare(0xc2, Vgpr(4), 5, false);
	append_compare(0xc3, Vgpr(4), 5, true);
	append_compare(0xc4, Vgpr(4), 5, false);
	append_compare(0xc5, Vgpr(4), 5, true);
	append_compare(0xc6, Vgpr(4), 5, false);
	append_compare(0xc7, Vgpr(4), 5, true);

	code.push_back(0x7c1d00f9u);
	code.push_back(0x86069201u);
	code.push_back(EncodeVopc(0x84, 249, 4));
	code.push_back(EncodeVopcSdwa(5, 19, 1));
	code.push_back(EncodeVopc(0x01, 250, 3));
	code.push_back(EncodeVop2Dpp(2));
	AppendVMovLiteral(&code, 6, 0xc0000000u);
	AppendVMovLiteral(&code, 7, 0x3f800000u);
	AppendVop3(&code, 0x04, 20, Vgpr(6), Vgpr(7), 0, 1);
	for (u32 i = 0; i < static_cast<u32>(results.size()); i++) {
		AppendStoreVgpr(&code, results[i], i);
	}
	AppendStoreSgpr(&code, 18, static_cast<u32>(results.size()));
	expected.push_back(1u);
	AppendStoreSgpr(&code, 19, static_cast<u32>(results.size() + 1u));
	expected.push_back(0u);
	AppendStoreSgpr(&code, 106, static_cast<u32>(results.size() + 2u));
	expected.push_back(1u);
	AppendStoreSgpr(&code, 20, static_cast<u32>(results.size() + 3u));
	expected.push_back(1u);
	AppendEnd(&code);

	return {"VectorCompareOps",
	        code,
	        {},
	        expected,
	        {O::VMovB32,    O::VCndmaskB32, O::VCmpFF32,   O::VCmpLtF32,  O::VCmpEqF32,
	         O::VCmpLeF32,  O::VCmpGtF32,   O::VCmpLgF32,  O::VCmpGeF32,  O::VCmpOF32,
	         O::VCmpUF32,   O::VCmpNgeF32,  O::VCmpNlgF32, O::VCmpNgtF32, O::VCmpNleF32,
	         O::VCmpNeqF32, O::VCmpNltF32,  O::VCmpTruF32, O::VCmpFI32,   O::VCmpLtI32,
	         O::VCmpEqI32,  O::VCmpLeI32,   O::VCmpGtI32,  O::VCmpNeI32,  O::VCmpGeI32,
	         O::VCmpTI32,   O::VCmpFU32,    O::VCmpLtU32,  O::VCmpEqU32,  O::VCmpLeU32,
	         O::VCmpGtU32,  O::VCmpNeU32,   O::VCmpGeU32,  O::VCmpTU32,   O::BufferStoreDword,
	         O::SEndpgm}};
}

TestCase VectorVop3CompareNeU64OnGpu() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	code.push_back(EncodeVop1(0x01, 1, InlineU32(1)));

	code.push_back(EncodeSop1(0x04, 106, 126)); // s_mov_b64 vcc, exec
	code.push_back(0xd4e5006au);
	code.push_back(0x0000d47eu); // v_cmp_ne_u64 vcc, exec, vcc
	code.push_back(EncodeVop2(0x01, 2, InlineU32(0), 1));
	AppendStoreVgpr(&code, 2, 0);

	AppendSMovLiteral(&code, 106, 0);
	AppendSMovLiteral(&code, 107, 0);
	code.push_back(0xd4e5006au);
	code.push_back(0x0000d47eu); // v_cmp_ne_u64 vcc, exec, vcc
	code.push_back(EncodeVop2(0x01, 3, InlineU32(0), 1));
	AppendStoreVgpr(&code, 3, 1);
	AppendEnd(&code);

	return {"VectorVop3CompareNeU64OnGpu",
	        code,
	        {},
	        {0, 1},
	        {O::VMovB32, O::SMovB64, O::VCmpNeU64, O::VCndmaskB32, O::SMovB32, O::BufferStoreDword,
	         O::SEndpgm}};
}

TestCase VectorVop3CompareEqI64OnGpu() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	code.push_back(EncodeVop1(0x01, 1, InlineU32(1)));
	code.push_back(EncodeSop1(0x04, 4, 126)); // s_mov_b64 s[4:5], exec

	code.push_back(0xd4a2006au);
	code.push_back(0x0000087eu); // v_cmp_eq_i64 vcc, exec, s[4:5]
	code.push_back(EncodeVop2(0x01, 2, InlineU32(0), 1));
	AppendStoreVgpr(&code, 2, 0);

	AppendSMovLiteral(&code, 4, 0);
	code.push_back(0xd4a2006au);
	code.push_back(0x0000087eu); // low dword mismatch
	code.push_back(EncodeVop2(0x01, 3, InlineU32(0), 1));
	AppendStoreVgpr(&code, 3, 1);
	AppendEnd(&code);

	return {"VectorVop3CompareEqI64OnGpu",
	        code,
	        {},
	        {1, 0},
	        {O::VMovB32, O::SMovB64, O::VCmpEqI64, O::VCndmaskB32, O::SMovB32,
	         O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorCompareClassF32() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	std::vector<u32> expected;
	AppendVMovU32(&code, 1, 1);

	u32  out          = 0;
	auto append_class = [&](u32 bits, u32 class_mask, bool match) {
		AppendVMovLiteral(&code, 0, bits);
		AppendVMovU32(&code, 2, class_mask);
		code.push_back(EncodeVopc(0x88, Vgpr(0), 2));
		code.push_back(EncodeVop2(0x01, 3, InlineU32(0), 1));
		AppendStoreVgpr(&code, 3, out++);
		expected.push_back(match ? 1u : 0u);
	};

	append_class(0x7fc00000u, 1u << 1u, true);
	append_class(0xff800000u, 1u << 2u, true);
	append_class(0xbf800000u, 1u << 3u, true);
	append_class(0x80000000u, 1u << 5u, true);
	append_class(0x00000000u, 1u << 6u, true);
	append_class(0x40000000u, 1u << 8u, true);
	append_class(0x7f800000u, 1u << 9u, true);
	append_class(0x40000000u, 1u << 3u, false);

	AppendVMovLiteral(&code, 0, 0x3f800000u);
	AppendVMovU32(&code, 2, 1u << 3u);
	AppendVop3(&code, 0x88, 20, Vgpr(0), Vgpr(2), 0, 0, 0, false, 0, 1);
	AppendStoreSgprPair(&code, 20, out);
	expected.push_back(1u);
	expected.push_back(0u);
	AppendEnd(&code);

	return {"VectorCompareClassF32",
	        code,
	        {},
	        expected,
	        {O::VMovB32, O::VCmpClassF32, O::VCndmaskB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorCompareF16Ops() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	std::vector<u32> expected;
	AppendVMovU32(&code, 1, 1);
	AppendVMovLiteral(&code, 2, 0x40003c00u); // low=1.0h, high=2.0h
	AppendVMovLiteral(&code, 3, 0x44004200u); // low=3.0h, high=4.0h
	AppendVMovLiteral(&code, 4, 0x00007e00u); // low=qNaN

	u32  dst            = 10;
	u32  out            = 0;
	auto append_compare = [&](u32 opcode, u32 src0, u32 src1, bool value) {
		code.push_back(EncodeVopc(opcode, src0, src1));
		code.push_back(EncodeVop2(0x01, dst, InlineU32(0), 1));
		AppendStoreVgpr(&code, dst, out++);
		expected.push_back(value ? 1u : 0u);
		dst++;
	};

	append_compare(0xc9, Vgpr(2), 3, true);
	append_compare(0xca, Vgpr(2), 2, true);
	append_compare(0xcb, Vgpr(2), 3, true);
	append_compare(0xcc, Vgpr(3), 2, true);
	append_compare(0xcd, Vgpr(2), 3, true);
	append_compare(0xce, Vgpr(3), 2, true);
	append_compare(0xed, Vgpr(4), 2, true);

	auto append_cmpx = [&](u32 opcode, u32 src0, u32 src1) {
		code.push_back(EncodeSMovB32(126, InlineU32(1)));
		code.push_back(EncodeVopc(opcode, src0, src1));
		AppendStoreVgpr(&code, 1, out++);
		expected.push_back(1u);
	};

	append_cmpx(0xd9, Vgpr(2), 3);
	append_cmpx(0xda, Vgpr(2), 2);
	append_cmpx(0xdb, Vgpr(2), 3);
	append_cmpx(0xdc, Vgpr(3), 2);
	append_cmpx(0xde, Vgpr(3), 2);
	append_cmpx(0xfd, Vgpr(4), 2);
	append_cmpx(0xfe, Vgpr(2), 2);
	AppendEnd(&code);

	return {"VectorCompareF16Ops",
	        code,
	        {},
	        expected,
	        {O::VMovB32, O::SMovB32, O::VCmpLtF16, O::VCmpEqF16, O::VCmpLeF16, O::VCmpGtF16,
	         O::VCmpLgF16, O::VCmpGeF16, O::VCmpNeqF16, O::VCmpxLtF16, O::VCmpxEqF16, O::VCmpxLeF16,
	         O::VCmpxGtF16, O::VCmpxGeF16, O::VCmpxNeqF16, O::VCmpxNltF16, O::VCndmaskB32,
	         O::BufferStoreDword, O::SEndpgm}};
}

TestCase Vop2SdwaCndmaskSourceModifier() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 6, 0x00000000u);
	AppendVMovLiteral(&code, 47, 0x12345678u);
	AppendVMovLiteral(&code, 53, 0x3f800000u);
	code.push_back(EncodeVopc(0xc7, Vgpr(6), 6)); // v_cmp_t_u32
	code.push_back(0x025e6af9u);
	code.push_back(0x16060635u);
	AppendStoreVgpr(&code, 47, 0);
	AppendEnd(&code);

	return {"Vop2SdwaCndmaskSourceModifier",
	        code,
	        {},
	        {0xbf800000u},
	        {O::VMovB32, O::VCmpTU32, O::VCndmaskB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase Vop2SdwaCndmaskFullDestinationWithSubDwordSource() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0xabcd1234u);
	AppendVMovLiteral(&code, 1, 0x55667788u);
	code.push_back(EncodeVopc(0xc0, Vgpr(0), 0)); // v_cmp_f_u32
	code.push_back(0x020e02f9u);
	code.push_back(0x06040600u);
	AppendStoreVgpr(&code, 7, 0);
	code.push_back(EncodeVopc(0xc7, Vgpr(0), 0)); // v_cmp_t_u32
	code.push_back(0x020e02f9u);
	code.push_back(0x06040600u);
	AppendStoreVgpr(&code, 7, 1);
	AppendEnd(&code);

	TestCase test;
	test.name           = "Vop2SdwaCndmaskFullDestinationWithSubDwordSource";
	test.code           = code;
	test.expected       = {0x00001234u, 0x55667788u};
	test.opcodes        = {O::VMovB32, O::VCmpFU32, O::VCmpTU32, O::VCndmaskB32,
	                       O::BufferStoreDword, O::SEndpgm};
	test.required_spirv = {"OpBitFieldUExtract", "OpSelect"};
	return test;
}

TestCase Vop3CndmaskUsesSgprMaskLaneBits() {
	using O = ShaderOpcode;

	std::vector<u32> code = {
	    EncodeVop1(0x01, 1, 0),
	    EncodeVop2(0x1a, 1, InlineU32(2), 1),
	    EncodeVop2(0x25, 1, Vgpr(0), 1),
	    EncodeVopc(0xc0, Vgpr(0), 0),
	};
	AppendSMovLiteral(&code, 4, 1);
	AppendSMovLiteral(&code, 5, 0);
	AppendVop3(&code, 0x101, 2, InlineU32(10), InlineU32(20), 4);
	code.push_back(EncodeVop2(0x1a, 3, InlineU32(2), 1));
	AppendBufferStoreDword(&code, 2, 3);
	AppendEnd(&code);

	TestCase test;
	test.name     = "Vop3CndmaskUsesSgprMaskLaneBits";
	test.code     = code;
	test.expected = {20, 10, 10, 10, 20, 10, 10, 10};
	test.opcodes  = {O::VMovB32, O::VLshlrevB32, O::VAddNcU32,        O::VCmpFU32,
	                 O::SMovB32, O::VCndmaskB32, O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0]     = 4;
	test.compute_info.threads_num[1]     = 1;
	test.compute_info.threads_num[2]     = 1;
	test.compute_info.group_id[0]        = true;
	test.compute_info.thread_ids_num     = 1;
	test.compute_info.workgroup_register = 0;
	test.has_compute_info                = true;
	test.dispatch_x                      = 2;
	return test;
}

TestCase Vop3CndmaskAllowsDataSourceModifier() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 7, 0x3f800000u);
	AppendSMovLiteral(&code, 34, 1);
	AppendSMovLiteral(&code, 35, 0);
	code.push_back(0xd5010004u);
	code.push_back(0x408a0f07u);
	AppendStoreVgpr(&code, 4, 0);
	AppendEnd(&code);

	return {"Vop3CndmaskAllowsDataSourceModifier",
	        code,
	        {},
	        {0xbf800000u},
	        {O::VMovB32, O::SMovB32, O::VCndmaskB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorCompareExecOps() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	code.push_back(EncodeVop1(0x01, 1, InlineU32(1)));
	AppendVMovLiteral(&code, 2, 0x40000000u);
	AppendVMovLiteral(&code, 3, 0x40400000u);
	code.push_back(EncodeVop1(0x01, 4, InlineU32(1)));
	AppendVMovLiteral(&code, 5, 0xffffffffu);

	u32  out         = 0;
	auto append_cmpx = [&](u32 opcode, u32 src0, u32 src1) {
		code.push_back(EncodeSMovB32(126, InlineU32(1)));
		code.push_back(EncodeVopc(opcode, src0, src1));
		AppendStoreVgpr(&code, 1, out++);
	};

	append_cmpx(0x11, Vgpr(2), 3);
	append_cmpx(0x12, Vgpr(3), 3);
	append_cmpx(0x13, Vgpr(2), 3);
	append_cmpx(0x14, Vgpr(3), 2);
	append_cmpx(0x15, Vgpr(2), 3);
	append_cmpx(0x16, Vgpr(3), 3);
	append_cmpx(0x19, Vgpr(2), 3);
	append_cmpx(0x1a, Vgpr(3), 3);
	append_cmpx(0x1b, Vgpr(2), 3);
	append_cmpx(0x1c, Vgpr(3), 2);
	append_cmpx(0x1d, Vgpr(2), 3);
	append_cmpx(0x1e, Vgpr(3), 3);
	append_cmpx(0x91, Vgpr(5), 4);
	append_cmpx(0x92, Vgpr(4), 4);
	append_cmpx(0x93, Vgpr(5), 4);
	append_cmpx(0x94, Vgpr(4), 5);
	append_cmpx(0x95, Vgpr(5), 4);
	append_cmpx(0x96, Vgpr(4), 4);
	append_cmpx(0xd1, Vgpr(4), 5);
	append_cmpx(0xd2, Vgpr(4), 4);
	append_cmpx(0xd3, Vgpr(4), 5);
	append_cmpx(0xd4, Vgpr(5), 4);
	append_cmpx(0xd5, Vgpr(4), 5);
	append_cmpx(0xd6, Vgpr(4), 4);
	AppendEnd(&code);

	return {"VectorCompareExecOps",
	        code,
	        {},
	        std::vector<u32>(out, 1),
	        {O::SMovB32,     O::VMovB32,          O::VCmpxLtF32,  O::VCmpxEqF32,  O::VCmpxLeF32,
	         O::VCmpxGtF32,  O::VCmpxLgF32,       O::VCmpxGeF32,  O::VCmpxNgeF32, O::VCmpxNlgF32,
	         O::VCmpxNgtF32, O::VCmpxNleF32,      O::VCmpxNeqF32, O::VCmpxNltF32, O::VCmpxLtI32,
	         O::VCmpxEqI32,  O::VCmpxLeI32,       O::VCmpxGtI32,  O::VCmpxNeI32,  O::VCmpxGeI32,
	         O::VCmpxLtU32,  O::VCmpxEqU32,       O::VCmpxLeU32,  O::VCmpxGtU32,  O::VCmpxNeU32,
	         O::VCmpxGeU32,  O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorVop3FloatCompareNegSourceModifier() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x3f800000u);
	AppendVMovU32(&code, 1, 0);
	AppendVop3(&code, 0x01, 20, Vgpr(0), Vgpr(1), 0, 0, 0, false, 0,
	           0x1); // -1.0 < 0.0
	AppendStoreSgprPair(&code, 20, 0);
	AppendEnd(&code);

	return {"VectorVop3FloatCompareNegSourceModifier",
	        code,
	        {},
	        {1, 0},
	        {O::VMovB32, O::VCmpLtF32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorVop3CmpxWritesExecMask() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 0, 2);
	AppendVMovU32(&code, 1, 1);
	AppendVMovU32(&code, 2, 0);
	AppendVMovU32(&code, 30, 0);
	AppendVop3(&code, 0xd1, 5, Vgpr(0), Vgpr(1)); // v_cmpx_lt_u32, false
	AppendVMovU32(&code, 2, 7);
	AppendBufferStoreDword(&code, 2, 30);
	AppendEnd(&code);

	return {"VectorVop3CmpxWritesExecMask",
	        code,
	        {0},
	        {0},
	        {O::VMovB32, O::VCmpxLtU32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorVopcSdwaCmpxWritesExecMask() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 0, 2);
	AppendVMovU32(&code, 1, 1);
	AppendVMovU32(&code, 2, 0);
	AppendVMovU32(&code, 30, 0);
	code.push_back(EncodeVopc(0xd1, 249, 1)); // v_cmpx_lt_u32.sdwa, false
	code.push_back(EncodeVopcSdwa(0));
	AppendVMovU32(&code, 2, 7);
	AppendBufferStoreDword(&code, 2, 30);
	AppendEnd(&code);

	return {"VectorVopcSdwaCmpxWritesExecMask",
	        code,
	        {0},
	        {0},
	        {O::VMovB32, O::VCmpxLtU32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorCompareInvertedMaskSelect() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x12345678u);
	code.push_back(EncodeVopc(0xc7, Vgpr(0), 0));      // v_cmp_t_u32 v0, v0
	code.push_back(EncodeSop1(0x08, 106, 106));        // s_not_b64 vcc, vcc
	code.push_back(EncodeVop2(0x01, 1, Vgpr(0), 128)); // v_cndmask_b32 v1, v0, 0
	AppendBufferStoreDword(&code, 1, 30);
	AppendEnd(&code);

	return {"VectorCompareInvertedMaskSelect",
	        code,
	        {},
	        {0x12345678u},
	        {O::VMovB32, O::VCmpTU32, O::SNotB64, O::VCndmaskB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase BranchSelect() {
	using O = ShaderOpcode;

	std::vector<u32> code = {
	    EncodeSMovB32(0, InlineU32(0)),
	    EncodeSopc(0x06, 0, InlineU32(0)),
	    EncodeSopp(0x05, 2),
	    EncodeSMovB32(1, InlineU32(1)),
	    EncodeSopp(0x02, 1),
	    EncodeSMovB32(1, InlineU32(7)),
	    EncodeSop2(0x0a, 2, 1, InlineU32(2)),
	    EncodeVop1(0x01, 0, 2),
	};
	AppendBufferStoreDword(&code, 0, 30);
	AppendEnd(&code);
	return {"BranchSelect",
	        code,
	        {},
	        {7},
	        {O::SMovB32, O::SCmpEqU32, O::SCbranchScc1, O::SBranch, O::SCselectB32, O::VMovB32,
	         O::BufferStoreDword, O::SEndpgm}};
}

TestCase SimpleLoop() {
	using O = ShaderOpcode;

	std::vector<u32> code = {
	    EncodeSMovB32(0, InlineU32(0)),
	    EncodeSopc(0x0a, 0, InlineU32(4)),
	    EncodeSopp(0x04, 2),
	    EncodeSop2(0x00, 0, 0, InlineU32(1)),
	    EncodeSopp(0x02, 0xfffcu),
	    EncodeVop1(0x01, 0, 0),
	};
	AppendBufferStoreDword(&code, 0, 30);
	AppendEnd(&code);
	return {"SimpleLoop",
	        code,
	        {},
	        {4},
	        {O::SMovB32, O::SCmpLtU32, O::SCbranchScc0, O::SAddU32, O::SBranch, O::VMovB32,
	         O::BufferStoreDword, O::SEndpgm}};
}

TestCase BranchVccnzUsesWholeMask() {
	using O = ShaderOpcode;

	std::vector<u32> code = {
	    EncodeVop1(0x01, 1, 0),
	    EncodeVop2(0x1a, 1, InlineU32(2), 1),
	    EncodeVop2(0x25, 1, Vgpr(0), 1),
	    EncodeVop2(0x1a, 3, InlineU32(2), 1),
	    EncodeVopc(0xc2, InlineU32(0), 0),
	    EncodeSopp(0x07, 2),
	    EncodeVop1(0x01, 2, InlineU32(11)),
	    EncodeSopp(0x02, 1),
	};
	AppendVMovU32(&code, 2, 42);
	AppendBufferStoreDword(&code, 2, 3);
	AppendEnd(&code);

	TestCase test;
	test.name     = "BranchVccnzUsesWholeMask";
	test.code     = code;
	test.expected = std::vector<u32>(8, 42);
	test.opcodes  = {O::VMovB32,       O::VLshlrevB32, O::VAddNcU32,        O::VCmpEqU32,
	                 O::SCbranchVccnz, O::SBranch,     O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0]     = 4;
	test.compute_info.threads_num[1]     = 1;
	test.compute_info.threads_num[2]     = 1;
	test.compute_info.group_id[0]        = true;
	test.compute_info.thread_ids_num     = 1;
	test.compute_info.workgroup_register = 0;
	test.has_compute_info                = true;
	test.dispatch_x                      = 2;
	return test;
}

TestCase ScalarMemoryLoadVariants() {
	using O = ShaderOpcode;

	struct Load {
		u32 opcode;
		u32 dst;
		u32 byte_offset;
		u32 dwords;
	};

	const Load s_loads[] = {
	    {0x00, 20, 0, 1},  {0x01, 21, 4, 2},   {0x02, 23, 12, 4},
	    {0x03, 27, 28, 8}, {0x04, 40, 60, 16},
	};
	const Load s_buffer_loads[] = {
	    {0x08, 56, 0, 1},  {0x09, 57, 4, 2},   {0x0a, 59, 12, 4},
	    {0x0b, 63, 28, 8}, {0x0c, 71, 60, 16},
	};

	std::vector<u32> code;
	code.push_back(EncodeSMovB32(0, InlineU32(0)));
	for (const auto& load: s_loads) {
		AppendSmemLoadOpcode(&code, load.opcode, load.dst, load.byte_offset);
	}
	for (const auto& load: s_buffer_loads) {
		AppendSmemLoadOpcode(&code, load.opcode, load.dst, load.byte_offset);
	}

	u32 out = 0;
	for (const auto& load: s_loads) {
		for (u32 i = 0; i < load.dwords; i++) {
			AppendStoreSgpr(&code, load.dst + i, out++);
		}
	}
	for (const auto& load: s_buffer_loads) {
		for (u32 i = 0; i < load.dwords; i++) {
			AppendStoreSgpr(&code, load.dst + i, out++);
		}
	}
	AppendEnd(&code);

	std::vector<u32> initial;
	for (u32 i = 0; i < 31u; i++) {
		initial.push_back(0x10000000u + i);
	}
	std::vector<u32> expected = initial;
	expected.insert(expected.end(), initial.begin(), initial.end());

	return {"ScalarMemoryLoadVariants",
	        code,
	        initial,
	        expected,
	        {O::SMovB32, O::SLoadDword, O::SLoadDwordx2, O::SLoadDwordx4, O::SLoadDwordx8,
	         O::SLoadDwordx16, O::SBufferLoadDword, O::SBufferLoadDwordx2, O::SBufferLoadDwordx4,
	         O::SBufferLoadDwordx8, O::SBufferLoadDwordx16, O::VMovB32, O::BufferStoreDword,
	         O::SEndpgm}};
}

TestCase ScalarLoadSignedImmediateOffsetAddsSoffset() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	code.push_back(EncodeSMovB32(0, InlineU32(8)));
	code.push_back(EncodeSMovB32(2, InlineU32(0)));
	code.push_back(EncodeSMovB32(3, InlineU32(0)));
	code.push_back(EncodeSmem0(0x00, 1, 1));
	code.push_back(EncodeSmem1(0x1ffffcu, 0));
	AppendStoreSgpr(&code, 1, 0);
	AppendEnd(&code);

	return {"ScalarLoadSignedImmediateOffsetAddsSoffset",
	        code,
	        {0x11111111u, 0x22222222u},
	        {0x22222222u, 0x22222222u},
	        {O::SMovB32, O::SLoadDword, O::BufferStoreDword, O::SEndpgm}};
}

TestCase BufferLoadStore() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendBufferLoadDword(&code, 0, 30);
	code.push_back(EncodeVop1(0x01, 31, InlineU32(4)));
	AppendBufferStoreDword(&code, 0, 31);
	AppendEnd(&code);
	return {"BufferLoadStore",
	        code,
	        {0x11223344u, 0},
	        {0x11223344u, 0x11223344u},
	        {O::BufferLoadDword, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase BufferLoadDwordOffenIdxenUsesVaddrPlusOneOffset() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendVMovU32(&code, 21, 8);
	code.push_back(EncodeMubuf0(0x0cu, 0, true, true));
	code.push_back(EncodeMubuf1(0, 0, 20));
	AppendStoreVgpr(&code, 0, 0);
	AppendEnd(&code);

	return {"BufferLoadDwordOffenIdxenUsesVaddrPlusOneOffset",
	        code,
	        {0x11111111u, 0x22222222u, 0x33333333u},
	        {0x33333333u, 0x22222222u, 0x33333333u},
	        {O::VMovB32, O::BufferLoadDword, O::BufferStoreDword, O::SEndpgm}};
}

TestCase BufferStoreDwordOffenIdxenUsesVaddrPlusOneOffset() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendVMovU32(&code, 21, 12);
	AppendVMovLiteral(&code, 0, 0xabcdef01u);
	code.push_back(EncodeMubuf0(0x1cu, 0, true, true));
	code.push_back(EncodeMubuf1(0, 0, 20));
	AppendEnd(&code);

	return {"BufferStoreDwordOffenIdxenUsesVaddrPlusOneOffset",
	        code,
	        {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u},
	        {0x11111111u, 0x22222222u, 0x33333333u, 0xabcdef01u},
	        {O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase BufferLoadDwordNoAddressFlagsIgnoresVaddr() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 8);
	code.push_back(EncodeMubuf0(0x0cu, 0, false, false));
	code.push_back(EncodeMubuf1(0, 0, 20));
	AppendStoreVgpr(&code, 0, 3);
	AppendEnd(&code);

	return {"BufferLoadDwordNoAddressFlagsIgnoresVaddr",
	        code,
	        {0x11111111u, 0x22222222u, 0x33333333u, 0},
	        {0x11111111u, 0x22222222u, 0x33333333u, 0x11111111u},
	        {O::VMovB32, O::BufferLoadDword, O::BufferStoreDword, O::SEndpgm}};
}

TestCase BufferLoadDwordIdxenUsesDescriptorStride() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 2);
	code.push_back(EncodeMubuf0(0x0cu, 0, true, false));
	code.push_back(EncodeMubuf1(0, 0, 20));
	AppendStoreVgpr(&code, 0, 0);
	AppendEnd(&code);

	TestCase test;
	test.name          = "BufferLoadDwordIdxenUsesDescriptorStride";
	test.code          = code;
	test.initial       = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u, 0x55555555u,
	                      0x66666666u, 0x77777777u, 0x88888888u, 0x99aabbccu};
	test.expected      = {0x99aabbccu, 0x22222222u, 0x33333333u, 0x44444444u, 0x55555555u,
	                      0x66666666u, 0x77777777u, 0x88888888u, 0x99aabbccu};
	test.opcodes       = {O::VMovB32, O::BufferLoadDword, O::BufferStoreDword, O::SEndpgm};
	test.user_data     = MakeStructuredStorageBufferData(16, 3);
	test.has_user_data = true;
	return test;
}

TestCase BufferStoreDwordIdxenUsesDescriptorStride() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 2);
	AppendVMovLiteral(&code, 0, 0xabcdef01u);
	code.push_back(EncodeMubuf0(0x1cu, 0, true, false));
	code.push_back(EncodeMubuf1(0, 0, 20));
	AppendEnd(&code);

	TestCase test;
	test.name          = "BufferStoreDwordIdxenUsesDescriptorStride";
	test.code          = code;
	test.initial       = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u, 0x55555555u,
	                      0x66666666u, 0x77777777u, 0x88888888u, 0x99aabbccu};
	test.expected      = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u, 0x55555555u,
	                      0x66666666u, 0x77777777u, 0x88888888u, 0xabcdef01u};
	test.opcodes       = {O::VMovB32, O::BufferStoreDword, O::SEndpgm};
	test.user_data     = MakeStructuredStorageBufferData(16, 3);
	test.has_user_data = true;
	return test;
}

TestCase BufferStoreDwordAppliesHostOffset() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0xabcdef01u);
	code.push_back(EncodeMubuf0(0x1cu, 0, false, false));
	code.push_back(EncodeMubuf1(0, 0, 20));
	AppendEnd(&code);

	TestCase test;
	test.name                        = "BufferStoreDwordAppliesHostOffset";
	test.code                        = code;
	test.initial                     = {0x11111111u, 0x22222222u, 0x33333333u, 0};
	test.expected                    = {0x11111111u, 0x22222222u, 0x33333333u, 0xabcdef01u};
	test.storage_buffer_range_dwords = 1;
	test.storage_buffer_offsets      = {12};
	test.opcodes                     = {O::VMovB32, O::BufferStoreDword, O::SEndpgm};
	test.user_data                   = MakeStructuredStorageBufferData(4, 1);
	test.has_user_data               = true;
	return test;
}

TestCase BufferOffsetsUsePackedLaneAndStorageFallback() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	for (u32 base: {0u, 4u}) {
		AppendSMovLiteral(&code, base, 0x1000u + base * 0x1000u);
		AppendSMovLiteral(&code, base + 1u, 4u << 16u);
		AppendSMovLiteral(&code, base + 2u, 1u);
		AppendSMovLiteral(&code, base + 3u, 1u << 24u);
	}
	AppendVMovLiteral(&code, 0, 0x12345678u);
	code.push_back(EncodeMubuf0(0x1cu, 0, false, false));
	code.push_back(EncodeMubuf1(0, 0, 20));
	AppendVMovLiteral(&code, 1, 0xabcdef01u);
	code.push_back(EncodeMubuf0(0x1cu, 0, false, false));
	code.push_back(EncodeMubuf1(1, 1, 20));
	AppendEnd(&code);

	TestCase test;
	test.name                        = "BufferOffsetsUsePackedLaneAndStorageFallback";
	test.code                        = std::move(code);
	test.initial                     = {0, 0, 0, 0};
	test.expected                    = {0x12345678u, 0, 0, 0xabcdef01u};
	test.storage_buffer_range_dwords = 1;
	test.storage_buffer_offsets      = {0, 12};
	test.force_shader_data_storage   = true;
	test.opcodes                     = {O::SMovB32, O::VMovB32, O::BufferStoreDword, O::SEndpgm};
	return test;
}

TestCase BufferLoadVariants() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendBufferLoadOpcode(&code, 0x08, 0, 20);
	AppendVMovU32(&code, 20, 2);
	AppendBufferLoadOpcode(&code, 0x09, 1, 20);
	AppendVMovU32(&code, 20, 0);
	AppendBufferLoadOpcode(&code, 0x0a, 2, 20);
	AppendVMovU32(&code, 20, 2);
	AppendBufferLoadOpcode(&code, 0x0b, 3, 20);
	AppendVMovU32(&code, 20, 8);
	AppendBufferLoadOpcode(&code, 0x0d, 4, 20);
	AppendBufferLoadOpcode(&code, 0x0f, 6, 20);
	AppendBufferLoadOpcode(&code, 0x0e, 9, 20);

	for (u32 i = 0; i < 13u; i++) {
		AppendStoreVgpr(&code, i, i);
	}
	AppendEnd(&code);

	return {"BufferLoadVariants",
	        code,
	        {0x80ff7f01u, 0x00008001u, 0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u},
	        {0x01u, 0xffffffffu, 0x7f01u, 0xffff80ffu, 0x11223344u, 0x55667788u, 0x11223344u,
	         0x55667788u, 0x99aabbccu, 0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u},
	        {O::BufferLoadUbyte, O::BufferLoadSbyte, O::BufferLoadUshort, O::BufferLoadSshort,
	         O::BufferLoadDwordx2, O::BufferLoadDwordx3, O::BufferLoadDwordx4, O::VMovB32,
	         O::BufferStoreDword, O::SEndpgm}};
}

TestCase BufferLoadDwordx4SnapshotsOverlappingAddress() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 21, 0);
	AppendVMovU32(&code, 22, 0);
	code.push_back(EncodeMubuf0(0x0eu, 0, true, true));
	code.push_back(EncodeMubuf1(21, 0, 21));
	for (u32 i = 0; i < 4; i++) {
		AppendStoreVgpr(&code, 21 + i, 4 + i);
	}
	AppendEnd(&code);

	TestCase test;
	test.name          = "BufferLoadDwordx4SnapshotsOverlappingAddress";
	test.code          = std::move(code);
	test.initial       = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u, 0, 0, 0, 0};
	test.expected      = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
	                      0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
	test.opcodes       = {O::VMovB32, O::BufferLoadDwordx4, O::BufferStoreDword, O::SEndpgm};
	test.user_data     = MakeStructuredStorageBufferData(16, 2);
	test.has_user_data = true;
	return test;
}

TestCase BufferLoadDwordx2SnapshotsOverlappingAddress() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 21, 0);
	AppendVMovU32(&code, 22, 0);
	code.push_back(EncodeMubuf0(0x0du, 0, true, true));
	code.push_back(EncodeMubuf1(21, 0, 21));
	for (u32 i = 0; i < 2; i++) {
		AppendStoreVgpr(&code, 21 + i, 2 + i);
	}
	AppendEnd(&code);

	TestCase test;
	test.name          = "BufferLoadDwordx2SnapshotsOverlappingAddress";
	test.code          = std::move(code);
	test.initial       = {0x11111111u, 0x22222222u, 0, 0};
	test.expected      = {0x11111111u, 0x22222222u, 0x11111111u, 0x22222222u};
	test.opcodes       = {O::VMovB32, O::BufferLoadDwordx2, O::BufferStoreDword, O::SEndpgm};
	test.user_data     = MakeStructuredStorageBufferData(8, 2);
	test.has_user_data = true;
	return test;
}

TestCase BufferLoadDwordx3SnapshotsOverlappingAddress() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 21, 0);
	AppendVMovU32(&code, 22, 0);
	code.push_back(EncodeMubuf0(0x0fu, 0, true, true));
	code.push_back(EncodeMubuf1(21, 0, 21));
	for (u32 i = 0; i < 3; i++) {
		AppendStoreVgpr(&code, 21 + i, 3 + i);
	}
	AppendEnd(&code);

	TestCase test;
	test.name      = "BufferLoadDwordx3SnapshotsOverlappingAddress";
	test.code      = std::move(code);
	test.initial   = {0x11111111u, 0x22222222u, 0x33333333u, 0, 0, 0};
	test.expected  = {0x11111111u, 0x22222222u, 0x33333333u, 0x11111111u, 0x22222222u, 0x33333333u};
	test.opcodes   = {O::VMovB32, O::BufferLoadDwordx3, O::BufferStoreDword, O::SEndpgm};
	test.user_data = MakeStructuredStorageBufferData(12, 2);
	test.has_user_data = true;
	return test;
}

TestCase BufferStoreVariants() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 1);
	AppendVMovU32(&code, 0, 0xaa);
	AppendBufferStoreOpcode(&code, 0x18, 0, 20);
	AppendVMovU32(&code, 20, 2);
	AppendVMovLiteral(&code, 1, 0x0000bbccu);
	AppendBufferStoreOpcode(&code, 0x1a, 1, 20);
	AppendVMovU32(&code, 20, 4);
	AppendVMovLiteral(&code, 2, 0x11111111u);
	AppendVMovLiteral(&code, 3, 0x22222222u);
	AppendBufferStoreOpcode(&code, 0x1d, 2, 20);
	AppendVMovU32(&code, 20, 12);
	AppendVMovLiteral(&code, 4, 0x33333333u);
	AppendVMovLiteral(&code, 5, 0x44444444u);
	AppendVMovLiteral(&code, 6, 0x55555555u);
	AppendBufferStoreOpcode(&code, 0x1f, 4, 20);
	AppendVMovU32(&code, 20, 24);
	AppendVMovLiteral(&code, 7, 0x66666666u);
	AppendVMovLiteral(&code, 8, 0x77777777u);
	AppendVMovLiteral(&code, 9, 0x88888888u);
	AppendVMovLiteral(&code, 10, 0x99999999u);
	AppendBufferStoreOpcode(&code, 0x1e, 7, 20);
	AppendEnd(&code);

	return {"BufferStoreVariants",
	        code,
	        std::vector<u32>(10, 0),
	        {0xbbccaa00u, 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u, 0x55555555u,
	         0x66666666u, 0x77777777u, 0x88888888u, 0x99999999u},
	        {O::VMovB32, O::BufferStoreByte, O::BufferStoreShort, O::BufferStoreDwordx2,
	         O::BufferStoreDwordx3, O::BufferStoreDwordx4, O::SEndpgm}};
}

TestCase BufferFormatVariants() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendBufferLoadOpcode(&code, 0x00, 0, 20);
	AppendBufferLoadOpcode(&code, 0x01, 1, 20);
	AppendBufferLoadOpcode(&code, 0x02, 3, 20);
	AppendBufferLoadOpcode(&code, 0x03, 6, 20);
	for (u32 i = 0; i < 10u; i++) {
		AppendStoreVgpr(&code, i, i);
	}
	AppendEnd(&code);

	TestCase load;
	load.name     = "BufferFormatLoadVariants";
	load.code     = code;
	load.initial  = {0x01020304u, 0x11121314u, 0x21222324u, 0x31323334u};
	load.expected = {0x01020304u, 0x01020304u, 0x11121314u, 0x01020304u, 0x11121314u,
	                 0x21222324u, 0x01020304u, 0x11121314u, 0x21222324u, 0x31323334u};
	load.opcodes  = {O::BufferLoadFormatX,
	                 O::BufferLoadFormatXy,
	                 O::BufferLoadFormatXyz,
	                 O::BufferLoadFormatXyzw,
	                 O::VMovB32,
	                 O::BufferStoreDword,
	                 O::SEndpgm};
	return load;
}

TestCase BufferLoadFormatXyzwSnapshotsOverlappingAddress() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 21, 0);
	AppendVMovU32(&code, 22, 0);
	code.push_back(EncodeMubuf0(0x03u, 0, true, true));
	code.push_back(EncodeMubuf1(21, 0, 21));
	for (u32 i = 0; i < 4; i++) {
		AppendStoreVgpr(&code, 21 + i, 4 + i);
	}
	AppendEnd(&code);

	TestCase test;
	test.name      = "BufferLoadFormatXyzwSnapshotsOverlappingAddress";
	test.code      = std::move(code);
	test.initial   = {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u, 0, 0, 0, 0};
	test.expected  = {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u,
	                  0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u};
	test.opcodes   = {O::VMovB32, O::BufferLoadFormatXyzw, O::BufferStoreDword, O::SEndpgm};
	test.user_data = MakeStructuredStorageBufferData(
	    16, 2, false, BufferFormat(Prospero::BufferFormat::k32_32_32_32Float));
	test.has_user_data = true;
	return test;
}

TestCase BufferLoadFormatXyzwInactiveExecPreservesOverlappingAddress() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 21, 0x11111111u);
	AppendVMovLiteral(&code, 22, 0x22222222u);
	AppendVMovLiteral(&code, 23, 0x33333333u);
	AppendVMovLiteral(&code, 24, 0x44444444u);
	code.push_back(EncodeSop1(0x04, 126, InlineU32(0)));
	code.push_back(EncodeMubuf0(0x03u, 0, true, true));
	code.push_back(EncodeMubuf1(21, 0, 21));
	code.push_back(EncodeSMovB32(126, InlineU32(1)));
	code.push_back(EncodeSMovB32(127, InlineU32(0)));
	for (u32 i = 0; i < 4; i++) {
		AppendStoreVgpr(&code, 21 + i, 4 + i);
	}
	AppendEnd(&code);

	TestCase test;
	test.name      = "BufferLoadFormatXyzwInactiveExecPreservesOverlappingAddress";
	test.code      = std::move(code);
	test.initial   = {0xaaaaaaaa, 0xbbbbbbbb, 0xcccccccc, 0xdddddddd, 0, 0, 0, 0};
	test.expected  = {0xaaaaaaaau, 0xbbbbbbbbu, 0xccccccccu, 0xddddddddu,
	                  0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
	test.opcodes   = {O::VMovB32, O::SMovB64,          O::BufferLoadFormatXyzw,
	                  O::SMovB32, O::BufferStoreDword, O::SEndpgm};
	test.user_data = MakeStructuredStorageBufferData(
	    16, 2, false, BufferFormat(Prospero::BufferFormat::k32_32_32_32Float));
	test.has_user_data = true;
	return test;
}

TestCase BufferFormatStoreVariants() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendVMovLiteral(&code, 0, 0x01020304u);
	AppendBufferStoreOpcode(&code, 0x04, 0, 20);
	AppendVMovU32(&code, 20, 4);
	AppendVMovLiteral(&code, 1, 0x11121314u);
	AppendVMovLiteral(&code, 2, 0x21222324u);
	AppendBufferStoreOpcode(&code, 0x05, 1, 20);
	AppendVMovU32(&code, 20, 12);
	AppendVMovLiteral(&code, 3, 0x31323334u);
	AppendVMovLiteral(&code, 4, 0x41424344u);
	AppendVMovLiteral(&code, 5, 0x51525354u);
	AppendBufferStoreOpcode(&code, 0x06, 3, 20);
	AppendVMovU32(&code, 20, 24);
	AppendVMovLiteral(&code, 6, 0x61626364u);
	AppendVMovLiteral(&code, 7, 0x71727374u);
	AppendVMovLiteral(&code, 8, 0x81828384u);
	AppendVMovLiteral(&code, 9, 0x91929394u);
	AppendBufferStoreOpcode(&code, 0x07, 6, 20);
	AppendEnd(&code);

	return {"BufferFormatStoreVariants",
	        code,
	        std::vector<u32>(10, 0),
	        {0x01020304u, 0x11121314u, 0x21222324u, 0x31323334u, 0x41424344u, 0x51525354u,
	         0x61626364u, 0x71727374u, 0x81828384u, 0x91929394u},
	        {O::VMovB32, O::BufferStoreFormatX, O::BufferStoreFormatXy, O::BufferStoreFormatXyz,
	         O::BufferStoreFormatXyzw, O::SEndpgm}};
}

TestCase BufferLoadFormatXResource8UintZeroExtendsByte() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	code.push_back(EncodeMubuf0(0x00u));
	code.push_back(EncodeMubuf1(0, 0, 20));
	AppendStoreVgpr(&code, 0, 1);
	AppendEnd(&code);

	TestCase test;
	test.name          = "BufferLoadFormatXResource8UintZeroExtendsByte";
	test.code          = code;
	test.initial       = {0x11223344u, 0};
	test.expected      = {0x11223344u, 0x00000044u};
	test.opcodes       = {O::VMovB32, O::BufferLoadFormatX, O::BufferStoreDword, O::SEndpgm};
	test.user_data     = MakeStructuredStorageBufferData(0, 8, false, 5);
	test.has_user_data = true;
	return test;
}

TestCase BufferStoreFormatXResource16UintWritesHalfword() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendVMovLiteral(&code, 0, 0x0000aaaau);
	code.push_back(EncodeMubuf0(0x04u));
	code.push_back(EncodeMubuf1(0, 0, 20));
	AppendEnd(&code);

	TestCase test;
	test.name          = "BufferStoreFormatXResource16UintWritesHalfword";
	test.code          = code;
	test.initial       = {0x11223344u};
	test.expected      = {0x1122aaaau};
	test.opcodes       = {O::VMovB32, O::BufferStoreFormatX, O::SEndpgm};
	test.user_data     = MakeStructuredStorageBufferData(0, 4, false, 11);
	test.has_user_data = true;
	return test;
}

TestCase BufferLoadFormatXyResource88UintExtractsBytes() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	code.push_back(EncodeMubuf0(0x01u));
	code.push_back(EncodeMubuf1(0, 0, 20));
	AppendStoreVgpr(&code, 0, 2);
	AppendStoreVgpr(&code, 1, 3);
	AppendEnd(&code);

	TestCase test;
	test.name          = "BufferLoadFormatXyResource8_8UintExtractsBytes";
	test.code          = code;
	test.initial       = {0x0000807fu, 0x55667788u, 0, 0};
	test.expected      = {0x0000807fu, 0x55667788u, 0x0000007fu, 0x00000080u};
	test.opcodes       = {O::VMovB32, O::BufferLoadFormatXy, O::BufferStoreDword, O::SEndpgm};
	test.user_data     = MakeStructuredStorageBufferData(0, 8, false, 18);
	test.has_user_data = true;
	return test;
}

TestCase BufferLoadFormatXyResource8888UnormConvertsFirstTwoComponents() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	code.push_back(EncodeMubuf0(0x01u));
	code.push_back(EncodeMubuf1(0, 0, 20));
	AppendStoreVgpr(&code, 0, 0);
	AppendStoreVgpr(&code, 1, 1);
	AppendEnd(&code);

	TestCase test;
	test.name          = "BufferLoadFormatXyResource8_8_8_8UnormConvertsFirstTwoComponents";
	test.code          = code;
	test.initial       = {0x44332211u, 0xdeadbeefu};
	test.expected      = {0x3d888889u, 0x3e088889u};
	test.opcodes       = {O::VMovB32, O::BufferLoadFormatXy, O::BufferStoreDword, O::SEndpgm};
	test.user_data     = MakeStructuredStorageBufferData(0, 8, false, 56);
	test.has_user_data = true;
	return test;
}

TestCase BufferStoreFormatXyResource88UintWritesBytes() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendVMovLiteral(&code, 0, 0x000000aau);
	AppendVMovLiteral(&code, 1, 0x000000bbu);
	code.push_back(EncodeMubuf0(0x05u));
	code.push_back(EncodeMubuf1(0, 0, 20));
	AppendEnd(&code);

	TestCase test;
	test.name          = "BufferStoreFormatXyResource8_8UintWritesBytes";
	test.code          = code;
	test.initial       = {0x11223344u, 0x55667788u};
	test.expected      = {0x1122bbaau, 0x55667788u};
	test.opcodes       = {O::VMovB32, O::BufferStoreFormatXy, O::SEndpgm};
	test.user_data     = MakeStructuredStorageBufferData(0, 8, false, 18);
	test.has_user_data = true;
	return test;
}

TestCase BufferStoreFormatXyResource32UintWritesOneDword() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendVMovLiteral(&code, 0, 0xabcdef01u);
	AppendVMovLiteral(&code, 1, 0x12345678u);
	code.push_back(EncodeMubuf0(0x05u));
	code.push_back(EncodeMubuf1(0, 0, 20));
	AppendEnd(&code);

	TestCase test;
	test.name          = "BufferStoreFormatXyResource32UintWritesOneDword";
	test.code          = code;
	test.initial       = {0x11111111u, 0x22222222u};
	test.expected      = {0xabcdef01u, 0x22222222u};
	test.opcodes       = {O::VMovB32, O::BufferStoreFormatXy, O::SEndpgm};
	test.user_data     = MakeStructuredStorageBufferData(0, 8, false, 20);
	test.has_user_data = true;
	return test;
}

TestCase BufferStoreFormatXyzResource3232UintWritesTwoDwords() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendVMovLiteral(&code, 0, 0xabcdef01u);
	AppendVMovLiteral(&code, 1, 0x12345678u);
	AppendVMovLiteral(&code, 2, 0x0badc0deu);
	code.push_back(EncodeMubuf0(0x06u));
	code.push_back(EncodeMubuf1(0, 0, 20));
	AppendEnd(&code);

	TestCase test;
	test.name          = "BufferStoreFormatXyzResource32_32UintWritesTwoDwords";
	test.code          = code;
	test.initial       = {0x11111111u, 0x22222222u, 0x33333333u};
	test.expected      = {0xabcdef01u, 0x12345678u, 0x33333333u};
	test.opcodes       = {O::VMovB32, O::BufferStoreFormatXyz, O::SEndpgm};
	test.user_data     = MakeStructuredStorageBufferData(0, 8, false, 62);
	test.has_user_data = true;
	return test;
}

TestCase BufferStoreFormatXyzwResource323232UintWritesThreeDwords() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendVMovLiteral(&code, 0, 0xabcdef01u);
	AppendVMovLiteral(&code, 1, 0x12345678u);
	AppendVMovLiteral(&code, 2, 0x0badc0deu);
	AppendVMovLiteral(&code, 3, 0xfeedfaceu);
	code.push_back(EncodeMubuf0(0x07u));
	code.push_back(EncodeMubuf1(0, 0, 20));
	AppendEnd(&code);

	TestCase test;
	test.name          = "BufferStoreFormatXyzwResource32_32_32UintWritesThreeDwords";
	test.code          = code;
	test.initial       = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
	test.expected      = {0xabcdef01u, 0x12345678u, 0x0badc0deu, 0x44444444u};
	test.opcodes       = {O::VMovB32, O::BufferStoreFormatXyzw, O::SEndpgm};
	test.user_data     = MakeStructuredStorageBufferData(0, 8, false, 72);
	test.has_user_data = true;
	return test;
}

TestCase BufferStoreFormatXyzResource8UintWritesOneByte() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendVMovLiteral(&code, 0, 0x000000aau);
	AppendVMovLiteral(&code, 1, 0x000000bbu);
	AppendVMovLiteral(&code, 2, 0x000000ccu);
	code.push_back(EncodeMubuf0(0x06u));
	code.push_back(EncodeMubuf1(0, 0, 20));
	AppendEnd(&code);

	TestCase test;
	test.name          = "BufferStoreFormatXyzResource8UintWritesOneByte";
	test.code          = code;
	test.initial       = {0x11223344u, 0x55667788u, 0x99aabbccu};
	test.expected      = {0x112233aau, 0x55667788u, 0x99aabbccu};
	test.opcodes       = {O::VMovB32, O::BufferStoreFormatXyz, O::SEndpgm};
	test.user_data     = MakeStructuredStorageBufferData(0, 8, false, 5);
	test.has_user_data = true;
	return test;
}

TestCase BufferStoreFormatXAddTidUsesLaneIndex() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x12345678u);
	code.push_back(EncodeMubuf0(0x04u, 0, false, false));
	code.push_back(EncodeMubuf1(0, 0, 20));
	AppendEnd(&code);

	TestCase test;
	test.name                        = "BufferStoreFormatXAddTidUsesLaneIndex";
	test.code                        = code;
	test.initial                     = std::vector<u32>(4, 0);
	test.expected                    = std::vector<u32>(4, 0x12345678u);
	test.opcodes                     = {O::VMovB32, O::BufferStoreFormatX, O::SEndpgm};
	test.compute_info.threads_num[0] = 4;
	test.compute_info.threads_num[1] = 1;
	test.compute_info.threads_num[2] = 1;
	test.compute_info.thread_ids_num = 1;
	test.has_compute_info            = true;
	test.user_data                   = MakeStructuredStorageBufferData(4, 4, true);
	test.has_user_data               = true;
	return test;
}

TestCase BufferStoreFormatXDropsOutOfRangeRecord() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 1);
	AppendVMovLiteral(&code, 0, 0xabcdef01u);
	code.push_back(EncodeMubuf0(0x04u, 0, true, false));
	code.push_back(EncodeMubuf1(0, 0, 20));
	AppendEnd(&code);

	TestCase test;
	test.name                        = "BufferStoreFormatXDropsOutOfRangeRecord";
	test.code                        = code;
	test.initial                     = {0x11111111u, 0x22222222u};
	test.expected                    = {0x11111111u, 0x22222222u};
	test.storage_buffer_range_dwords = 1;
	test.opcodes                     = {O::VMovB32, O::BufferStoreFormatX, O::SEndpgm};
	test.user_data                   = MakeStructuredStorageBufferData(4, 1, false, 20);
	test.has_user_data               = true;
	return test;
}

TestCase TBufferLoadVariants() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendTBufferLoadOpcode(&code, 0x00, 0, 20);
	AppendTBufferLoadOpcode(&code, 0x01, 1, 20);
	AppendTBufferLoadOpcode(&code, 0x02, 3, 20);
	AppendTBufferLoadOpcode(&code, 0x03, 6, 20);
	for (u32 i = 0; i < 10u; i++) {
		AppendStoreVgpr(&code, i, i);
	}
	AppendEnd(&code);

	return {"TBufferLoadVariants",
	        code,
	        {0x01020304u, 0x11121314u, 0x21222324u, 0x31323334u},
	        {0x01020304u, 0x01020304u, 0x11121314u, 0x01020304u, 0x11121314u, 0x21222324u,
	         0x01020304u, 0x11121314u, 0x21222324u, 0x31323334u},
	        {O::TBufferLoadFormatX, O::TBufferLoadFormatXy, O::TBufferLoadFormatXyz,
	         O::TBufferLoadFormatXyzw, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXyzwSnapshotsOverlappingAddress() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 21, 0);
	AppendVMovU32(&code, 22, 0);
	constexpr auto format = BufferFormat(Prospero::BufferFormat::k32_32_32_32Float);
	code.push_back(EncodeMtbuf0(0x03u, format & 0xfu, (format >> 4u) & 0x7u, 0, true, true));
	code.push_back(EncodeMtbuf1(0x03u, 21, 0, 21));
	for (u32 i = 0; i < 4; i++) {
		AppendStoreVgpr(&code, 21 + i, 4 + i);
	}
	AppendEnd(&code);

	TestCase test;
	test.name          = "TBufferLoadFormatXyzwSnapshotsOverlappingAddress";
	test.code          = std::move(code);
	test.initial       = {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u, 0, 0, 0, 0};
	test.expected      = {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u,
	                      0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u};
	test.opcodes       = {O::VMovB32, O::TBufferLoadFormatXyzw, O::BufferStoreDword, O::SEndpgm};
	test.user_data     = MakeStructuredStorageBufferData(16, 2);
	test.has_user_data = true;
	return test;
}

TestCase TBufferLoadFormatXyzwPackedSnapshotsOverlappingAddress() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 21, 0);
	AppendVMovU32(&code, 22, 0);
	constexpr auto format = BufferFormat(Prospero::BufferFormat::k8_8_8_8UInt);
	code.push_back(EncodeMtbuf0(0x03u, format & 0xfu, (format >> 4u) & 0x7u, 0, true, true));
	code.push_back(EncodeMtbuf1(0x03u, 21, 0, 21));
	for (u32 i = 0; i < 4; i++) {
		AppendStoreVgpr(&code, 21 + i, 4 + i);
	}
	AppendEnd(&code);

	TestCase test;
	test.name          = "TBufferLoadFormatXyzwPackedSnapshotsOverlappingAddress";
	test.code          = std::move(code);
	test.initial       = {0x44332211u, 0, 0, 0, 0, 0, 0, 0};
	test.expected      = {0x44332211u, 0, 0, 0, 0x11u, 0x22u, 0x33u, 0x44u};
	test.opcodes       = {O::VMovB32, O::TBufferLoadFormatXyzw, O::BufferStoreDword, O::SEndpgm};
	test.user_data     = MakeStructuredStorageBufferData(4, 8);
	test.has_user_data = true;
	return test;
}

TestCase TBufferStoreFormatX8UintWritesOneByte() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendVMovU32(&code, 0, 0xaa);
	code.push_back(EncodeMtbuf0(0x04, 5, 0));
	code.push_back(EncodeMtbuf1(0x04, 0, 0, 20));
	AppendEnd(&code);

	return {"TBufferStoreFormatX8UintWritesOneByte",
	        code,
	        {0x11223344u},
	        {0x112233aau},
	        {O::VMovB32, O::TBufferStoreFormatX, O::SEndpgm}};
}

TestCase TBufferLoadFormatX8UintZeroExtendsByte() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	code.push_back(EncodeMtbuf0(0x00, 5, 0));
	code.push_back(EncodeMtbuf1(0x00, 0, 0, 20));
	AppendStoreVgpr(&code, 0, 0);
	AppendEnd(&code);

	return {"TBufferLoadFormatX8UintZeroExtendsByte",
	        code,
	        {0x11223344u, 0},
	        {0x00000044u, 0},
	        {O::VMovB32, O::TBufferLoadFormatX, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatX8888UintExtractsFirstByte() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendTBufferLoadFormatOpcode(&code, 0x00, 0, 20, Prospero::BufferFormat::k8_8_8_8UInt);
	AppendStoreVgpr(&code, 0, 0);
	AppendEnd(&code);

	return {"TBufferLoadFormatX8_8_8_8UintExtractsFirstByte",
	        code,
	        {0x44332211u, 0},
	        {0x00000011u, 0},
	        {O::VMovB32, O::TBufferLoadFormatX, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXIdxenUsesDescriptorStride() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 2);
	code.push_back(EncodeMtbuf0(0x00, 5, 0, 0, true, false));
	code.push_back(EncodeMtbuf1(0x00, 0, 0, 20));
	AppendStoreVgpr(&code, 0, 0);
	AppendEnd(&code);

	TestCase test;
	test.name          = "TBufferLoadFormatXIdxenUsesDescriptorStride";
	test.code          = code;
	test.initial       = {0x01020304u, 0, 0, 0, 0, 0, 0, 0, 0x0000007eu};
	test.expected      = {0x0000007eu, 0, 0, 0, 0, 0, 0, 0, 0x0000007eu};
	test.opcodes       = {O::VMovB32, O::TBufferLoadFormatX, O::BufferStoreDword, O::SEndpgm};
	test.user_data     = MakeStructuredStorageBufferData(16, 3);
	test.has_user_data = true;
	return test;
}

TestCase TBufferLoadFormatX16FloatConvertsToFloat() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendTBufferLoadFormatOpcode(&code, 0x00, 0, 20, Prospero::BufferFormat::k16Float);
	AppendStoreVgpr(&code, 0, 0);
	AppendEnd(&code);

	return {"TBufferLoadFormatX16FloatConvertsToFloat",
	        code,
	        {0x00003c00u, 0},
	        {0x3f800000u, 0},
	        {O::VMovB32, O::TBufferLoadFormatX, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXSintSignExtendsSubDword() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	code.push_back(EncodeMtbuf0(0x00, 6, 0));
	code.push_back(EncodeMtbuf1(0x00, 0, 0, 20));
	AppendVMovU32(&code, 20, 4);
	code.push_back(EncodeMtbuf0(0x00, 12, 0));
	code.push_back(EncodeMtbuf1(0x00, 1, 0, 20));
	AppendStoreVgpr(&code, 0, 0);
	AppendStoreVgpr(&code, 1, 1);
	AppendEnd(&code);

	return {"TBufferLoadFormatXSintSignExtendsSubDword",
	        code,
	        {0x00000080u, 0x00008001u},
	        {0xffffff80u, 0xffff8001u},
	        {O::VMovB32, O::TBufferLoadFormatX, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferStoreFormatXSintWritesSubDword() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendVMovLiteral(&code, 0, 0xffffff80u);
	code.push_back(EncodeMtbuf0(0x04, 6, 0));
	code.push_back(EncodeMtbuf1(0x04, 0, 0, 20));
	AppendVMovU32(&code, 20, 4);
	AppendVMovLiteral(&code, 1, 0xffff8001u);
	code.push_back(EncodeMtbuf0(0x04, 12, 0));
	code.push_back(EncodeMtbuf1(0x04, 1, 0, 20));
	AppendEnd(&code);

	return {"TBufferStoreFormatXSintWritesSubDword",
	        code,
	        {0x11223344u, 0x55667788u},
	        {0x11223380u, 0x55668001u},
	        {O::VMovB32, O::TBufferStoreFormatX, O::SEndpgm}};
}

TestCase TBufferLoadFormatXy88IntegerComponents() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	code.push_back(EncodeMtbuf0(0x01, 2, 1));
	code.push_back(EncodeMtbuf1(0x01, 0, 0, 20));
	AppendVMovU32(&code, 20, 4);
	code.push_back(EncodeMtbuf0(0x01, 3, 1));
	code.push_back(EncodeMtbuf1(0x01, 2, 0, 20));
	for (u32 i = 0; i < 4; i++) {
		AppendStoreVgpr(&code, i, i);
	}
	AppendEnd(&code);

	return {"TBufferLoadFormatXy8_8IntegerComponents",
	        code,
	        {0x0000807fu, 0x00007f80u, 0, 0},
	        {0x0000007fu, 0x00000080u, 0xffffff80u, 0x0000007fu},
	        {O::VMovB32, O::TBufferLoadFormatXy, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferStoreFormatXy88IntegerComponents() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendVMovU32(&code, 0, 0xaa);
	AppendVMovU32(&code, 1, 0xbb);
	code.push_back(EncodeMtbuf0(0x05, 2, 1));
	code.push_back(EncodeMtbuf1(0x05, 0, 0, 20));
	AppendVMovU32(&code, 20, 4);
	AppendVMovLiteral(&code, 2, 0xffffff80u);
	AppendVMovU32(&code, 3, 0x7f);
	code.push_back(EncodeMtbuf0(0x05, 3, 1));
	code.push_back(EncodeMtbuf1(0x05, 2, 0, 20));
	AppendEnd(&code);

	return {"TBufferStoreFormatXy8_8IntegerComponents",
	        code,
	        {0x11223344u, 0x55667788u},
	        {0x1122bbaau, 0x55667f80u},
	        {O::VMovB32, O::TBufferStoreFormatXy, O::SEndpgm}};
}

TestCase TBufferLoadFormatXy1616IntegerComponents() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	code.push_back(EncodeMtbuf0(0x01, 11, 1));
	code.push_back(EncodeMtbuf1(0x01, 0, 0, 20));
	AppendVMovU32(&code, 20, 4);
	code.push_back(EncodeMtbuf0(0x01, 12, 1));
	code.push_back(EncodeMtbuf1(0x01, 2, 0, 20));
	for (u32 i = 0; i < 4; i++) {
		AppendStoreVgpr(&code, i, i);
	}
	AppendEnd(&code);

	return {"TBufferLoadFormatXy16_16IntegerComponents",
	        code,
	        {0x80017fffu, 0x7fff8000u, 0, 0},
	        {0x00007fffu, 0x00008001u, 0xffff8000u, 0x00007fffu},
	        {O::VMovB32, O::TBufferLoadFormatXy, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferStoreFormatXy1616IntegerComponents() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendVMovU32(&code, 0, 0xaaaa);
	AppendVMovU32(&code, 1, 0xbbbb);
	code.push_back(EncodeMtbuf0(0x05, 11, 1));
	code.push_back(EncodeMtbuf1(0x05, 0, 0, 20));
	AppendVMovU32(&code, 20, 4);
	AppendVMovLiteral(&code, 2, 0xffff8000u);
	AppendVMovU32(&code, 3, 0x7fff);
	code.push_back(EncodeMtbuf0(0x05, 12, 1));
	code.push_back(EncodeMtbuf1(0x05, 2, 0, 20));
	AppendEnd(&code);

	return {"TBufferStoreFormatXy16_16IntegerComponents",
	        code,
	        {0x11223344u, 0x55667788u},
	        {0xbbbbaaaau, 0x7fff8000u},
	        {O::VMovB32, O::TBufferStoreFormatXy, O::SEndpgm}};
}

TestCase TBufferLoadFormatXyz16161616UintLoadsHalfwords() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendTBufferLoadFormatOpcode(&code, 0x02, 0, 20, Prospero::BufferFormat::k16_16_16_16UInt);
	for (u32 i = 0; i < 3; i++) {
		AppendStoreVgpr(&code, i, i);
	}
	AppendEnd(&code);

	return {"TBufferLoadFormatXyz16_16_16_16UintLoadsHalfwords",
	        code,
	        {0x22221111u, 0x44443333u, 0},
	        {0x00001111u, 0x00002222u, 0x00003333u},
	        {O::VMovB32, O::TBufferLoadFormatXyz, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXy88UnormConvertsToFloat() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	code.push_back(EncodeMtbuf0(0x01, 14, 0));
	code.push_back(EncodeMtbuf1(0x01, 0, 0, 20));
	AppendStoreVgpr(&code, 0, 0);
	AppendStoreVgpr(&code, 1, 1);
	AppendEnd(&code);

	return {"TBufferLoadFormatXy8_8UnormConvertsToFloat",
	        code,
	        {0x0000ff80u, 0},
	        {0x3f008081u, 0x3f800000u},
	        {O::VMovB32, O::TBufferLoadFormatXy, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXy88SnormConvertsToFloat() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendTBufferLoadFormatOpcode(&code, 0x01, 0, 20, Prospero::BufferFormat::k8_8SNorm);
	AppendStoreVgpr(&code, 0, 0);
	AppendStoreVgpr(&code, 1, 1);
	AppendEnd(&code);

	return {"TBufferLoadFormatXy8_8SnormConvertsToFloat",
	        code,
	        {0x00007f80u, 0},
	        {0xbf800000u, 0x3f800000u},
	        {O::VMovB32, O::TBufferLoadFormatXy, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXy1616UnormConvertsToFloat() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	code.push_back(EncodeMtbuf0(0x01, 7, 1));
	code.push_back(EncodeMtbuf1(0x01, 0, 0, 20));
	AppendStoreVgpr(&code, 0, 0);
	AppendStoreVgpr(&code, 1, 1);
	AppendEnd(&code);

	return {"TBufferLoadFormatXy16_16UnormConvertsToFloat",
	        code,
	        {0xffff8000u, 0},
	        {0x3f000080u, 0x3f800000u},
	        {O::VMovB32, O::TBufferLoadFormatXy, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXy8888UnormConvertsFirstTwoComponents() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	code.push_back(EncodeMtbuf0(0x01, 8, 3));
	code.push_back(EncodeMtbuf1(0x01, 0, 0, 20));
	AppendStoreVgpr(&code, 0, 0);
	AppendStoreVgpr(&code, 1, 1);
	AppendEnd(&code);

	return {"TBufferLoadFormatXy8_8_8_8UnormConvertsFirstTwoComponents",
	        code,
	        {0x44332211u, 0xdeadbeefu},
	        {0x3d888889u, 0x3e088889u},
	        {O::VMovB32, O::TBufferLoadFormatXy, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXyzw8888UintExtractsBytes() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendTBufferLoadFormatOpcode(&code, 0x03, 0, 20, Prospero::BufferFormat::k8_8_8_8UInt);
	for (u32 i = 0; i < 4; i++) {
		AppendStoreVgpr(&code, i, i);
	}
	AppendEnd(&code);

	return {"TBufferLoadFormatXyzw8_8_8_8UintExtractsBytes",
	        code,
	        {0x44332211u, 0xaaaaaaaau, 0xbbbbbbbbu, 0xccccccccu},
	        {0x00000011u, 0x00000022u, 0x00000033u, 0x00000044u},
	        {O::VMovB32, O::TBufferLoadFormatXyzw, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXyzw1010102SnormConvertsToFloat() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendTBufferLoadFormatOpcode(&code, 0x03, 0, 20, Prospero::BufferFormat::k10_10_10_2SNorm);
	for (u32 i = 0; i < 4; i++) {
		AppendStoreVgpr(&code, i, i);
	}
	AppendEnd(&code);

	return {"TBufferLoadFormatXyzw10_10_10_2SnormConvertsToFloat",
	        code,
	        {0x800801ffu, 0, 0, 0},
	        {0x3f800000u, 0xbf800000u, 0, 0xbf800000u},
	        {O::VMovB32, O::TBufferLoadFormatXyzw, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXyz111110FloatUnpacks() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendTBufferLoadFormatOpcode(&code, 0x02, 0, 20, Prospero::BufferFormat::k11_11_10Float);
	for (u32 i = 0; i < 3; i++) {
		AppendStoreVgpr(&code, i, i);
	}
	AppendEnd(&code);

	return {"TBufferLoadFormatXyz11_11_10FloatUnpacks",
	        code,
	        {0x781e03c0u, 0, 0},
	        {0x3f800000u, 0x3f800000u, 0x3f800000u},
	        {O::VMovB32, O::TBufferLoadFormatXyz, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXyzw3232FloatZerosMissingComponents() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	code.push_back(EncodeMtbuf0(0x03, 0, 4));
	code.push_back(EncodeMtbuf1(0x03, 0, 0, 20));
	for (u32 i = 0; i < 4; i++) {
		AppendStoreVgpr(&code, i, i);
	}
	AppendEnd(&code);

	return {"TBufferLoadFormatXyzw32_32FloatZerosMissingComponents",
	        code,
	        {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u},
	        {0x11111111u, 0x22222222u, 0, 0},
	        {O::VMovB32, O::TBufferLoadFormatXyzw, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferStoreFormatXyzw3232FloatWritesOnlyPresentComponents() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendVMovLiteral(&code, 0, 0x3f800000u);
	AppendVMovLiteral(&code, 1, 0x40000000u);
	AppendVMovLiteral(&code, 2, 0x40400000u);
	AppendVMovLiteral(&code, 3, 0x40800000u);
	code.push_back(EncodeMtbuf0(0x07, 0, 4));
	code.push_back(EncodeMtbuf1(0x07, 0, 0, 20));
	AppendEnd(&code);

	return {"TBufferStoreFormatXyzw32_32FloatWritesOnlyPresentComponents",
	        code,
	        {0xaaaaaaaau, 0xbbbbbbbbu, 0xccccccccu, 0xddddddddu},
	        {0x3f800000u, 0x40000000u, 0xccccccccu, 0xddddddddu},
	        {O::VMovB32, O::TBufferStoreFormatXyzw, O::SEndpgm}};
}

TestCase TBufferStoreVariants() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendVMovLiteral(&code, 0, 0x01020304u);
	AppendTBufferStoreOpcode(&code, 0x04, 0, 20);
	AppendVMovU32(&code, 20, 4);
	AppendVMovLiteral(&code, 1, 0x11121314u);
	AppendVMovLiteral(&code, 2, 0x21222324u);
	AppendTBufferStoreOpcode(&code, 0x05, 1, 20);
	AppendVMovU32(&code, 20, 12);
	AppendVMovLiteral(&code, 3, 0x31323334u);
	AppendVMovLiteral(&code, 4, 0x41424344u);
	AppendVMovLiteral(&code, 5, 0x51525354u);
	AppendTBufferStoreOpcode(&code, 0x06, 3, 20);
	AppendVMovU32(&code, 20, 24);
	AppendVMovLiteral(&code, 6, 0x61626364u);
	AppendVMovLiteral(&code, 7, 0x71727374u);
	AppendVMovLiteral(&code, 8, 0x81828384u);
	AppendVMovLiteral(&code, 9, 0x91929394u);
	AppendTBufferStoreOpcode(&code, 0x07, 6, 20);
	AppendEnd(&code);

	return {"TBufferStoreVariants",
	        code,
	        std::vector<u32>(10, 0),
	        {0x01020304u, 0x11121314u, 0x21222324u, 0x31323334u, 0x41424344u, 0x51525354u,
	         0x61626364u, 0x71727374u, 0x81828384u, 0x91929394u},
	        {O::VMovB32, O::TBufferStoreFormatX, O::TBufferStoreFormatXy, O::TBufferStoreFormatXyz,
	         O::TBufferStoreFormatXyzw, O::SEndpgm}};
}

TestCase FlatLoadVariants() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	code.push_back(EncodeFlat0(0x08, 0, 0));
	code.push_back(EncodeFlat1(0, 0x7d, 0, 20));
	code.push_back(EncodeFlat0(0x09, 0, 2));
	code.push_back(EncodeFlat1(1, 0x7d, 0, 20));
	code.push_back(EncodeFlat0(0x0a, 0, 0));
	code.push_back(EncodeFlat1(2, 0x7d, 0, 20));
	code.push_back(EncodeFlat0(0x0b, 0, 2));
	code.push_back(EncodeFlat1(3, 0x7d, 0, 20));
	code.push_back(EncodeFlat0(0x0c, 0, 8));
	code.push_back(EncodeFlat1(13, 0x7d, 0, 20));
	code.push_back(EncodeFlat0(0x0d, 0, 8));
	code.push_back(EncodeFlat1(4, 0x7d, 0, 20));
	code.push_back(EncodeFlat0(0x0f, 0, 8));
	code.push_back(EncodeFlat1(6, 0x7d, 0, 20));
	code.push_back(EncodeFlat0(0x0e, 0, 8));
	code.push_back(EncodeFlat1(9, 0x7d, 0, 20));

	for (u32 i = 0; i < 14u; i++) {
		AppendStoreVgpr(&code, i, i);
	}
	AppendEnd(&code);

	TestCase test {"FlatLoadVariants",
	               code,
	               {0x80ff7f01u, 0x00008001u, 0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u},
	               {0x01u, 0xffffffffu, 0x7f01u, 0xffff80ffu, 0x11223344u, 0x55667788u, 0x11223344u,
	                0x55667788u, 0x99aabbccu, 0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u,
	                0x11223344u},
	               {O::VMovB32, O::FlatLoadUbyte, O::FlatLoadSbyte, O::FlatLoadUshort,
	                O::FlatLoadSshort, O::FlatLoadDword, O::FlatLoadDwordx2, O::FlatLoadDwordx3,
	                O::FlatLoadDwordx4, O::BufferStoreDword, O::SEndpgm}};
	test.flat_memory_base = 0;
	return test;
}

TestCase BranchVccnzUsesCarryProducedWholeMask() {
	using O = ShaderOpcode;

	std::vector<u32> code = {
	    EncodeVop1(0x01, 1, 0),          EncodeVop2(0x1a, 1, InlineU32(2), 1),
	    EncodeVop2(0x25, 1, Vgpr(0), 1), EncodeVop2(0x1a, 3, InlineU32(2), 1),
	    EncodeVopc(0xc0, Vgpr(0), 0),
	};
	AppendVMovLiteral(&code, 2, 0xffffffffu);
	code.push_back(EncodeVop2(0x28, 5, Vgpr(2), 1));
	code.push_back(EncodeSopp(0x07, 2));
	code.push_back(EncodeVop1(0x01, 2, InlineU32(11)));
	code.push_back(EncodeSopp(0x02, 1));
	AppendVMovU32(&code, 2, 42);
	AppendBufferStoreDword(&code, 2, 3);
	AppendEnd(&code);

	TestCase test;
	test.name     = "BranchVccnzUsesCarryProducedWholeMask";
	test.code     = code;
	test.expected = std::vector<u32>(8, 42);
	test.opcodes = {O::VMovB32,       O::VLshlrevB32, O::VAddNcU32,        O::VCmpFU32, O::VAddcU32,
	                O::SCbranchVccnz, O::SBranch,     O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0]     = 4;
	test.compute_info.threads_num[1]     = 1;
	test.compute_info.threads_num[2]     = 1;
	test.compute_info.group_id[0]        = true;
	test.compute_info.thread_ids_num     = 1;
	test.compute_info.workgroup_register = 0;
	test.has_compute_info                = true;
	test.dispatch_x                      = 2;
	return test;
}

TestCase ScalarLoadAlignsComponentsAndMasksAddress() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendSMovLiteral(&code, 0, 1u);
	AppendSMovLiteral(&code, 2, 3u);
	AppendSMovLiteral(&code, 3, 0xffff0000u);
	code.push_back(EncodeSmem0(0x00, 1, 1));
	code.push_back(EncodeSmem1(3, 0));
	AppendStoreSgpr(&code, 1, 0);
	AppendEnd(&code);

	return {"ScalarLoadAlignsComponentsAndMasksAddress",
	        code,
	        {0x11111111u, 0x22222222u},
	        {0x11111111u, 0x22222222u},
	        {O::SMovB32, O::SLoadDword, O::BufferStoreDword, O::SEndpgm}};
}

TestCase FlatVirtualAddressRebasesGuestAllocation() {
	using O = ShaderOpcode;

	constexpr uint64_t GuestBase = 0x0000000110000000ull;
	std::vector<u32>   code;
	AppendVMovLiteral(&code, 20, static_cast<u32>(GuestBase + 4u));
	AppendVMovLiteral(&code, 21, static_cast<u32>(GuestBase >> 32u));
	code.push_back(EncodeFlat0(0x0c, 0, 0));
	code.push_back(EncodeFlat1(0, 0x7d, 0, 20));
	code.push_back(EncodeFlat0(0x0c, 2, 0));
	code.push_back(EncodeFlat1(1, 0x7d, 0, 20));
	AppendVMovLiteral(&code, 21, static_cast<u32>((GuestBase >> 32u) + 1u));
	code.push_back(EncodeFlat0(0x0c, 0, 0));
	code.push_back(EncodeFlat1(2, 0x7d, 0, 20));
	AppendStoreVgpr(&code, 0, 0);
	AppendStoreVgpr(&code, 1, 1);
	AppendStoreVgpr(&code, 2, 2);
	AppendEnd(&code);

	TestCase test;
	test.name             = "FlatVirtualAddressRebasesGuestAllocation";
	test.code             = std::move(code);
	test.initial          = {0, 0x12345678u};
	test.expected         = {0x12345678u, 0x12345678u, 0};
	test.flat_memory_base = GuestBase;
	test.opcodes          = {O::VMovB32, O::FlatLoadDword, O::BufferStoreDword, O::SEndpgm};
	return test;
}

TestCase GlobalSignedImmediateRebasesBeforeSaddr() {
	using O = ShaderOpcode;

	constexpr uint64_t GuestBase = 0x0000000110000000ull;
	std::vector<u32>   code;
	AppendSMovLiteral(&code, 0, static_cast<u32>(GuestBase));
	AppendSMovLiteral(&code, 1, static_cast<u32>(GuestBase >> 32u));
	AppendVMovU32(&code, 20, 8);
	code.push_back(EncodeFlat0(0x0c, 2, 0xffcu));
	code.push_back(EncodeFlat1(0, 0, 0, 20));
	AppendStoreVgpr(&code, 0, 0);
	AppendEnd(&code);

	return {"GlobalSignedImmediateRebasesBeforeSaddr",
	        code,
	        {0x11111111u, 0x22222222u, 0x12345678u},
	        {0x12345678u},
	        {O::SMovB32, O::VMovB32, O::FlatLoadDword, O::BufferStoreDword, O::SEndpgm}};
}

TestCase FlatSegmentIgnoresSaddrAndMasksOffsetMsb() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	code.push_back(EncodeSMovB32(0, InlineU32(4)));
	AppendVMovU32(&code, 20, 0);
	code.push_back(EncodeFlat0(0x0c, 0, 4));
	code.push_back(EncodeFlat1(0, 0, 0, 20));
	AppendStoreVgpr(&code, 0, 0);
	AppendVMovU32(&code, 20, 0);
	code.push_back(EncodeFlat0(0x0c, 0, 0x804));
	code.push_back(EncodeFlat1(1, 0x7d, 0, 20));
	AppendStoreVgpr(&code, 1, 1);
	AppendEnd(&code);

	std::vector<u32> initial(514, 0);
	initial[1]   = 0x11111111u;
	initial[2]   = 0x22222222u;
	initial[513] = 0x33333333u;

	TestCase test;
	test.name     = "FlatSegmentIgnoresSaddrAndMasksOffsetMsb";
	test.code     = code;
	test.initial  = initial;
	test.expected = {0x11111111u, 0x11111111u};
	test.opcodes  = {O::SMovB32, O::VMovB32, O::FlatLoadDword, O::BufferStoreDword, O::SEndpgm};
	test.flat_memory_base = 0;
	return test;
}

TestCase FlatStoreVariants() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 1);
	AppendVMovU32(&code, 0, 0xaa);
	code.push_back(EncodeFlat0(0x18, 0, 0));
	code.push_back(EncodeFlat1(0, 0x7d, 0, 20));
	AppendVMovU32(&code, 20, 2);
	AppendVMovLiteral(&code, 1, 0x0000bbccu);
	code.push_back(EncodeFlat0(0x1a, 0, 0));
	code.push_back(EncodeFlat1(0, 0x7d, 1, 20));
	AppendVMovU32(&code, 20, 4);
	AppendVMovLiteral(&code, 2, 0x11111111u);
	AppendVMovLiteral(&code, 3, 0x22222222u);
	code.push_back(EncodeFlat0(0x1d, 0, 0));
	code.push_back(EncodeFlat1(0, 0x7d, 2, 20));
	AppendVMovU32(&code, 20, 12);
	AppendVMovLiteral(&code, 4, 0x33333333u);
	AppendVMovLiteral(&code, 5, 0x44444444u);
	AppendVMovLiteral(&code, 6, 0x55555555u);
	code.push_back(EncodeFlat0(0x1f, 0, 0));
	code.push_back(EncodeFlat1(0, 0x7d, 4, 20));
	AppendVMovU32(&code, 20, 24);
	AppendVMovLiteral(&code, 7, 0x66666666u);
	AppendVMovLiteral(&code, 8, 0x77777777u);
	AppendVMovLiteral(&code, 9, 0x88888888u);
	AppendVMovLiteral(&code, 10, 0x99999999u);
	code.push_back(EncodeFlat0(0x1e, 0, 0));
	code.push_back(EncodeFlat1(0, 0x7d, 7, 20));
	AppendVMovU32(&code, 20, 0);
	code.push_back(EncodeFlat0(0x1c, 0, 40));
	code.push_back(EncodeFlat1(0, 0x7d, 2, 20));
	AppendEnd(&code);

	TestCase test {"FlatStoreVariants",
	               code,
	               std::vector<u32>(12, 0),
	               {0xbbccaa00u, 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u, 0x55555555u,
	                0x66666666u, 0x77777777u, 0x88888888u, 0x99999999u, 0x11111111u, 0},
	               {O::VMovB32, O::FlatStoreByte, O::FlatStoreShort, O::FlatStoreDword,
	                O::FlatStoreDwordx2, O::FlatStoreDwordx3, O::FlatStoreDwordx4, O::SEndpgm}};
	test.flat_memory_base = 0;
	return test;
}

TestCase DsReadWriteVariants() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 1, 0);
	AppendVMovLiteral(&code, 2, 0x11223344u);
	code.push_back(EncodeDs0(0x0d, 0));
	code.push_back(EncodeDs1(0, 2, 1));
	code.push_back(EncodeDs0(0x36, 0));
	code.push_back(EncodeDs1(3, 0, 1));
	AppendVMovU32(&code, 4, 0xaa);
	code.push_back(EncodeDs0(0x1e, 4));
	code.push_back(EncodeDs1(0, 4, 1));
	code.push_back(EncodeDs0(0x3a, 4));
	code.push_back(EncodeDs1(5, 0, 1));
	code.push_back(EncodeDs0(0x39, 4));
	code.push_back(EncodeDs1(6, 0, 1));
	AppendVMovLiteral(&code, 7, 0x000080ffu);
	code.push_back(EncodeDs0(0x1f, 8));
	code.push_back(EncodeDs1(0, 7, 1));
	code.push_back(EncodeDs0(0x3c, 8));
	code.push_back(EncodeDs1(8, 0, 1));
	code.push_back(EncodeDs0(0x3b, 8));
	code.push_back(EncodeDs1(9, 0, 1));
	AppendVMovLiteral(&code, 10, 0x10101010u);
	AppendVMovLiteral(&code, 11, 0x11111111u);
	code.push_back(EncodeDs0(0x4d, 12));
	code.push_back(EncodeDs1(0, 10, 1));
	code.push_back(EncodeDs0(0x76, 12));
	code.push_back(EncodeDs1(14, 0, 1));
	AppendVMovLiteral(&code, 16, 0x20202020u);
	AppendVMovLiteral(&code, 17, 0x21212121u);
	AppendVMovLiteral(&code, 18, 0x22222222u);
	code.push_back(EncodeDs0(0xde, 20));
	code.push_back(EncodeDs1(0, 16, 1));
	code.push_back(EncodeDs0(0xfe, 20));
	code.push_back(EncodeDs1(19, 0, 1));
	AppendVMovLiteral(&code, 22, 0x30303030u);
	AppendVMovLiteral(&code, 23, 0x31313131u);
	AppendVMovLiteral(&code, 24, 0x32323232u);
	AppendVMovLiteral(&code, 25, 0x33333333u);
	code.push_back(EncodeDs0(0xdf, 32));
	code.push_back(EncodeDs1(0, 22, 1));
	code.push_back(EncodeDs0(0xff, 32));
	code.push_back(EncodeDs1(26, 0, 1));

	const u32 results[] = {3, 5, 6, 8, 9, 14, 15, 19, 20, 21, 26, 27, 28, 29};
	for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
		AppendStoreVgpr(&code, results[i], i);
	}
	AppendEnd(&code);

	return {"DsReadWriteVariants",
	        code,
	        std::vector<u32>(14, 0),
	        {0x11223344u, 0xaau, 0xffffffaau, 0x80ffu, 0xffff80ffu, 0x10101010u, 0x11111111u,
	         0x20202020u, 0x21212121u, 0x22222222u, 0x30303030u, 0x31313131u, 0x32323232u,
	         0x33333333u},
	        {O::VMovB32, O::DsWriteB32, O::DsReadB32, O::DsWriteByte, O::DsReadUbyte,
	         O::DsReadSbyte, O::DsWriteShort, O::DsReadUshort, O::DsReadSshort, O::DsWriteB64,
	         O::DsReadB64, O::DsWriteB96, O::DsReadB96, O::DsWriteB128, O::DsReadB128,
	         O::BufferStoreDword, O::SEndpgm}};
}

TestCase DsReadWrite2Variants() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 1, 0);
	AppendVMovLiteral(&code, 2, 0x11111111u);
	AppendVMovLiteral(&code, 3, 0x22222222u);
	code.push_back(EncodeDs0(0x0e, (3u << 8u) | 1u));
	code.push_back(EncodeDs1Ex(0, 3, 2, 1));
	code.push_back(EncodeDs0(0x37, (3u << 8u) | 1u));
	code.push_back(EncodeDs1Ex(4, 0, 0, 1));
	AppendVMovLiteral(&code, 6, 0x33333333u);
	AppendVMovLiteral(&code, 7, 0x44444444u);
	AppendVMovLiteral(&code, 8, 0x55555555u);
	AppendVMovLiteral(&code, 9, 0x66666666u);
	code.push_back(EncodeDs0(0x4d, 32));
	code.push_back(EncodeDs1(0, 6, 1));
	code.push_back(EncodeDs0(0x4d, 48));
	code.push_back(EncodeDs1(0, 8, 1));
	code.push_back(EncodeDs0(0x77, (6u << 8u) | 4u));
	code.push_back(EncodeDs1Ex(10, 0, 0, 1));
	AppendVMovLiteral(&code, 14, 0x77777777u);
	AppendVMovLiteral(&code, 15, 0x88888888u);
	code.push_back(EncodeDs0(0x0f, (2u << 8u) | 1u));
	code.push_back(EncodeDs1Ex(0, 15, 14, 1));
	code.push_back(EncodeDs0(0x38, (2u << 8u) | 1u));
	code.push_back(EncodeDs1Ex(16, 0, 0, 1));
	AppendVMovLiteral(&code, 18, 0x99999999u);
	AppendVMovLiteral(&code, 19, 0xaaaaaaaau);
	AppendVMovLiteral(&code, 20, 0xbbbbbbbbu);
	AppendVMovLiteral(&code, 21, 0xccccccccu);
	code.push_back(EncodeDs0(0x4e, (10u << 8u) | 8u));
	code.push_back(EncodeDs1Ex(0, 20, 18, 1));
	code.push_back(EncodeDs0(0x77, (10u << 8u) | 8u));
	code.push_back(EncodeDs1Ex(22, 0, 0, 1));
	AppendVMovLiteral(&code, 26, 0xddddddddu);
	AppendVMovLiteral(&code, 27, 0xeeeeeeeeu);
	AppendVMovLiteral(&code, 28, 0xf0f0f0f0u);
	AppendVMovLiteral(&code, 29, 0x12345678u);
	code.push_back(EncodeDs0(0x4f, (2u << 8u) | 1u));
	code.push_back(EncodeDs1Ex(0, 28, 26, 1));
	code.push_back(EncodeDs0(0x78, (2u << 8u) | 1u));
	code.push_back(EncodeDs1Ex(34, 0, 0, 1));

	const u32 results[] = {4, 5, 10, 11, 12, 13, 22, 23, 24, 25, 16, 17, 34, 35, 36, 37};
	for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
		AppendStoreVgpr(&code, results[i], i);
	}
	AppendEnd(&code);

	return {"DsReadWrite2Variants",
	        code,
	        std::vector<u32>(16, 0),
	        {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u, 0x55555555u, 0x66666666u,
	         0x99999999u, 0xaaaaaaaau, 0xbbbbbbbbu, 0xccccccccu, 0x77777777u, 0x88888888u,
	         0xddddddddu, 0xeeeeeeeeu, 0xf0f0f0f0u, 0x12345678u},
	        {O::VMovB32, O::DsWrite2B32, O::DsRead2B32, O::DsWriteB64, O::DsRead2B64,
	         O::DsWrite2B64, O::DsWrite2St64B32, O::DsWrite2St64B64, O::DsRead2St64B64,
	         O::BufferStoreDword, O::SEndpgm}};
}

TestCase DsAtomicNoReturnVariants() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 1, 0);
	const u32 initial[] = {10, 10, 0xfffffff0u, 0xfffffff0u, 10, 10, 0xf0f0u, 0xf000u, 0xf00fu};
	const u32 values[]  = {5, 3, 5, 5, 5, 20, 0x0ff0u, 0x0f00u, 0x00ffu};
	const u32 ops[]     = {0x00, 0x01, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b};
	for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
		AppendVMovLiteral(&code, 2, initial[i]);
		code.push_back(EncodeDs0(0x0d, i * 4u));
		code.push_back(EncodeDs1(0, 2, 1));
		AppendVMovLiteral(&code, 3, values[i]);
		code.push_back(EncodeDs0(ops[i], i * 4u));
		code.push_back(EncodeDs1(0, 3, 1));
		code.push_back(EncodeDs0(0x36, i * 4u));
		code.push_back(EncodeDs1(10u + i, 0, 1));
	}
	for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
		AppendStoreVgpr(&code, 10u + i, i);
	}
	AppendEnd(&code);

	return {"DsAtomicNoReturnVariants",
	        code,
	        std::vector<u32>(9, 0),
	        {15, 7, 0xfffffff0u, 5, 5, 20, 0x00f0u, 0xff00u, 0xf0f0u},
	        {O::VMovB32, O::DsWriteB32, O::DsAddU32, O::DsSubU32, O::DsMinI32, O::DsMaxI32,
	         O::DsMinU32, O::DsMaxU32, O::DsAndB32, O::DsOrB32, O::DsXorB32, O::DsReadB32,
	         O::BufferStoreDword, O::SEndpgm}};
}

TestCase DsAtomicReturnVariants() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 1, 0);
	const u32 initial[] = {10, 10, 0xfffffff0u, 0xfffffff0u, 10, 10, 0xf0f0u, 0xf000u, 0xf00fu, 10};
	const u32 values[]  = {5, 3, 5, 5, 5, 20, 0x0ff0u, 0x0f00u, 0x00ffu, 99};
	const u32 ops[]     = {0x20, 0x21, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2d};
	for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
		AppendVMovLiteral(&code, 2, initial[i]);
		code.push_back(EncodeDs0(0x0d, i * 4u));
		code.push_back(EncodeDs1(0, 2, 1));
		AppendVMovLiteral(&code, 3, values[i]);
		code.push_back(EncodeDs0(ops[i], i * 4u));
		code.push_back(EncodeDs1(10u + i, 3, 1));
		code.push_back(EncodeDs0(0x36, i * 4u));
		code.push_back(EncodeDs1(20u + i, 0, 1));
	}
	for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
		AppendStoreVgpr(&code, 10u + i, i);
	}
	for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
		AppendStoreVgpr(&code, 20u + i, i + 10u);
	}
	AppendEnd(&code);

	return {"DsAtomicReturnVariants",
	        code,
	        std::vector<u32>(20, 0),
	        {10, 10, 0xfffffff0u, 0xfffffff0u, 10, 10, 0xf0f0u, 0xf000u, 0xf00fu, 10,
	         15, 7,  0xfffffff0u, 5,           5,  20, 0x00f0u, 0xff00u, 0xf0f0u, 99},
	        {O::VMovB32, O::DsWriteB32, O::DsAddRtnU32, O::DsSubRtnU32, O::DsMinRtnI32,
	         O::DsMaxRtnI32, O::DsMinRtnU32, O::DsMaxRtnU32, O::DsAndRtnB32, O::DsOrRtnB32,
	         O::DsXorRtnB32, O::DsWrxchgRtnB32, O::DsReadB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase DsMiscVariants() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 1, 0);
	AppendVMovLiteral(&code, 2, 0x40800000u);
	code.push_back(EncodeDs0(0x0d, 0));
	code.push_back(EncodeDs1(0, 2, 1));
	AppendVMovLiteral(&code, 3, 0x3f800000u);
	code.push_back(EncodeDs0(0x0d, 4));
	code.push_back(EncodeDs1(0, 3, 1));
	AppendVMovLiteral(&code, 4, 0x40000000u);
	code.push_back(EncodeDs0(0x12, 0));
	code.push_back(EncodeDs1(0, 4, 1));
	AppendVMovLiteral(&code, 5, 0x40400000u);
	code.push_back(EncodeDs0(0x13, 4));
	code.push_back(EncodeDs1(0, 5, 1));
	code.push_back(EncodeDs0(0x36, 0));
	code.push_back(EncodeDs1(6, 0, 1));
	code.push_back(EncodeDs0(0x36, 4));
	code.push_back(EncodeDs1(7, 0, 1));
	AppendVMovLiteral(&code, 8, 0x12345678u);
	code.push_back(EncodeDs0(0x35, 0x001f));
	code.push_back(EncodeDs1(9, 0, 8));
	code.push_back(EncodeSMovB32(124, InlineU32(0)));
	AppendVMovLiteral(&code, 10, 0xabcdef01u);
	code.push_back(EncodeDs0(0xb0, 8));
	code.push_back(EncodeDs1(0, 10, 0));
	code.push_back(EncodeDs0(0xb1, 8));
	code.push_back(EncodeDs1(11, 0, 0));

	const u32 results[] = {6, 7, 9, 11};
	for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
		AppendStoreVgpr(&code, results[i], i);
	}
	AppendEnd(&code);

	TestCase test;
	test.name     = "DsMiscVariants";
	test.code     = code;
	test.initial  = std::vector<u32>(4, 0);
	test.expected = {0x40000000u, 0x40400000u, 0x12345678u, 0xabcdef01u};
	test.opcodes  = {O::VMovB32,         O::SMovB32,          O::DsWriteB32,   O::DsMinF32,
	                 O::DsMaxF32,        O::DsReadB32,        O::DsSwizzleB32, O::DsWriteAddtidB32,
	                 O::DsReadAddtidB32, O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0] = 1;
	test.compute_info.threads_num[1] = 1;
	test.compute_info.threads_num[2] = 1;
	test.has_compute_info            = true;
	return test;
}

TestCase DsFloatMinMaxUsesSeparateCompareOperand() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 1, 0);
	AppendVMovLiteral(&code, 2, 0x40800000u);
	code.push_back(EncodeDs0(0x0d, 0));
	code.push_back(EncodeDs1(0, 2, 1));
	AppendVMovLiteral(&code, 3, 0x40800000u);
	code.push_back(EncodeDs0(0x0d, 4));
	code.push_back(EncodeDs1(0, 3, 1));
	AppendVMovLiteral(&code, 4, 0x41100000u);
	AppendVMovLiteral(&code, 5, 0x40000000u);
	code.push_back(EncodeDs0(0x12, 0));
	code.push_back(EncodeDs1Ex(0, 5, 4, 1));
	AppendVMovLiteral(&code, 6, 0x3f800000u);
	AppendVMovLiteral(&code, 7, 0x40400000u);
	code.push_back(EncodeDs0(0x13, 4));
	code.push_back(EncodeDs1Ex(0, 7, 6, 1));
	code.push_back(EncodeDs0(0x36, 0));
	code.push_back(EncodeDs1(8, 0, 1));
	code.push_back(EncodeDs0(0x36, 4));
	code.push_back(EncodeDs1(9, 0, 1));
	AppendStoreVgpr(&code, 8, 0);
	AppendStoreVgpr(&code, 9, 1);
	AppendEnd(&code);

	TestCase test;
	test.name                        = "DsFloatMinMaxUsesSeparateCompareOperand";
	test.code                        = code;
	test.initial                     = std::vector<u32>(2, 0);
	test.expected                    = {0x41100000u, 0x3f800000u};
	test.opcodes                     = {O::VMovB32,   O::DsWriteB32,       O::DsMinF32, O::DsMaxF32,
	                                    O::DsReadB32, O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0] = 1;
	test.compute_info.threads_num[1] = 1;
	test.compute_info.threads_num[2] = 1;
	test.has_compute_info            = true;
	return test;
}

TestCase DsSwizzleInvalidSourceLaneZero() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 1, 100);
	code.push_back(EncodeDs0(0x35, 0x00e0));
	code.push_back(EncodeDs1(2, 0, 1));
	code.push_back(EncodeVop2(0x1a, 3, InlineU32(2), 0));
	AppendBufferStoreDword(&code, 2, 3);
	AppendEnd(&code);

	TestCase test;
	test.name     = "DsSwizzleInvalidSourceLaneZero";
	test.code     = code;
	test.expected = {0, 0, 0, 0};
	test.opcodes  = {O::VMovB32, O::DsSwizzleB32, O::VLshlrevB32, O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0] = 4;
	test.compute_info.threads_num[1] = 1;
	test.compute_info.threads_num[2] = 1;
	test.compute_info.thread_ids_num = 1;
	test.has_compute_info            = true;
	return test;
}

TestCase BufferAtomicVariants() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	const u32        values[] = {100, 5, 3, 5, 5, 5, 20, 0x0ff0u, 0x0f00u, 0x00ffu};
	const u32        ops[]    = {0x30, 0x32, 0x33, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b};
	for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
		AppendVMovU32(&code, 20, i * 4u);
		AppendVMovU32(&code, i, values[i]);
		AppendBufferStoreOpcode(&code, ops[i], i, 20, true);
	}
	for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
		AppendStoreVgpr(&code, i, i + 10u);
	}
	AppendEnd(&code);

	return {"BufferAtomicVariants",
	        code,
	        {10, 10, 10, 0xfffffff0u, 10, 0xfffffff0u, 10, 0xf0f0u, 0xf000u, 0xf00fu,
	         0,  0,  0,  0,           0,  0,           0,  0,       0,       0},
	        {100, 15, 7,  0xfffffff0u, 5,  5,           20, 0x00f0u, 0xff00u, 0xf0f0u,
	         10,  10, 10, 0xfffffff0u, 10, 0xfffffff0u, 10, 0xf0f0u, 0xf000u, 0xf00fu},
	        {O::VMovB32, O::BufferAtomicSwap, O::BufferAtomicAdd, O::BufferAtomicSub,
	         O::BufferAtomicSMin, O::BufferAtomicUMin, O::BufferAtomicSMax, O::BufferAtomicUMax,
	         O::BufferAtomicAnd, O::BufferAtomicOr, O::BufferAtomicXor, O::BufferStoreDword,
	         O::SEndpgm}};
}

TestCase BufferAtomicGlc0DoesNotReturnOldValue() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendVMovU32(&code, 0, 5);
	AppendBufferStoreOpcode(&code, 0x32, 0, 20);
	AppendStoreVgpr(&code, 0, 1);
	AppendEnd(&code);

	return {"BufferAtomicGlc0DoesNotReturnOldValue",
	        code,
	        {10, 0},
	        {15, 5},
	        {O::VMovB32, O::BufferAtomicAdd, O::BufferStoreDword, O::SEndpgm}};
}

TestCase BufferAtomicFMinExactRawGlcModes() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x40000000u); // 2.0
	code.push_back(0xe0fc0000u);
	code.push_back(0x80010000u); // exact failing buffer_atomic_fmin, GLC=0
	AppendStoreVgpr(&code, 0, 1);
	AppendVMovLiteral(&code, 0, 0x3f800000u); // 1.0
	code.push_back(0xe0fc4000u);
	code.push_back(0x80010000u); // same instruction with GLC=1
	AppendStoreVgpr(&code, 0, 2);
	AppendEnd(&code);

	TestCase test;
	test.name     = "BufferAtomicFMinExactRawGlcModes";
	test.code     = code;
	test.initial  = {0x40800000u, 0, 0}; // 4.0
	test.expected = {0x3f800000u, 0x40000000u, 0x40000000u};
	test.opcodes  = {O::VMovB32, O::BufferAtomicFMin, O::BufferStoreDword, O::SEndpgm};
	const auto descriptor =
	    MakeStructuredStorageBufferData(0, static_cast<u32>(test.initial.size() * sizeof(u32)));
	std::copy_n(descriptor.begin(), 4, test.user_data.begin() + 4);
	test.user_data[50] = 1u << 20u;
	test.has_user_data = true;
	return test;
}

TestCase BufferAtomicFMinSpecialValues() {
	using O = ShaderOpcode;

	const u32 values[] = {
	    0x40000000u, // 2.0
	    0x7f800000u, // +infinity
	    0xff800000u, // -infinity
	    0x3f800000u, // 1.0
	    0x7fc00000u, // quiet NaN
	    0x00000000u, // +0.0
	    0x80000000u, // -0.0
	    0x00000000u, // +0.0
	    0x80000001u, // smallest negative denorm
	};
	std::vector<u32> code;
	for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
		AppendVMovU32(&code, 20, i * 4u);
		AppendVMovLiteral(&code, i, values[i]);
		AppendBufferStoreOpcode(&code, 0x3f, i, 20, true);
	}
	for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
		AppendStoreVgpr(&code, i, i + static_cast<u32>(std::size(values)));
	}
	AppendEnd(&code);

	return {"BufferAtomicFMinSpecialValues",
	        code,
	        {0x40800000u, 0xbf800000u, 0x7f800000u, 0x7fc00000u, 0x3f800000u,
	         0x80000000u, 0x00000000u, 0x00000001u, 0x00000000u, 0, 0, 0, 0, 0, 0, 0, 0, 0},
	        {0x40000000u, 0xbf800000u, 0xff800000u, 0x7fc00000u, 0x3f800000u,
	         0x80000000u, 0x00000000u, 0x00000000u, 0x80000001u, 0x40800000u,
	         0xbf800000u, 0x7f800000u, 0x7fc00000u, 0x3f800000u, 0x80000000u,
	         0x00000000u, 0x00000001u, 0x00000000u},
	        {O::VMovB32, O::BufferAtomicFMin, O::BufferStoreDword, O::SEndpgm}};
}

TestCase BufferAtomicFMinContendedWorkgroup() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	code.push_back(EncodeVop1(0x06, 1, Vgpr(0))); // v_cvt_f32_u32 v1, thread_id.x
	AppendVMovU32(&code, 20, 0);
	AppendBufferStoreOpcode(&code, 0x3f, 1, 20);
	AppendEnd(&code);

	TestCase test;
	test.name     = "BufferAtomicFMinContendedWorkgroup";
	test.code     = code;
	test.initial  = {0x42c80000u}; // 100.0
	test.expected = {0x00000000u}; // min(100.0, 0.0 .. 63.0)
	test.opcodes  = {O::VCvtF32U32, O::VMovB32, O::BufferAtomicFMin, O::SEndpgm};
	test.compute_info.threads_num[0] = 64;
	test.compute_info.threads_num[1] = 1;
	test.compute_info.threads_num[2] = 1;
	test.compute_info.thread_ids_num = 1;
	test.has_compute_info            = true;
	return test;
}

TestCase BufferAtomicFMaxExactRawGlcModes() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 3, 0x40000000u); // 2.0
	code.push_back(0xe100000cu);
	code.push_back(0x80010300u); // exact failing buffer_atomic_fmax v3, offset 12, GLC=0
	AppendStoreVgpr(&code, 3, 4);
	AppendVMovLiteral(&code, 0, 0x40800000u); // 4.0
	code.push_back(0xe100400cu);
	code.push_back(0x80010000u); // same address with GLC=1
	AppendStoreVgpr(&code, 0, 5);
	AppendEnd(&code);

	TestCase test;
	test.name     = "BufferAtomicFMaxExactRawGlcModes";
	test.code     = code;
	test.initial  = {0, 0, 0, 0x3f800000u, 0, 0}; // memory[3] = 1.0
	test.expected = {0, 0, 0, 0x40800000u, 0x40000000u, 0x40000000u};
	test.opcodes  = {O::VMovB32, O::BufferAtomicFMax, O::BufferStoreDword, O::SEndpgm};
	const auto descriptor =
	    MakeStructuredStorageBufferData(0, static_cast<u32>(test.initial.size() * sizeof(u32)));
	std::copy_n(descriptor.begin(), 4, test.user_data.begin() + 4);
	test.user_data[50] = 1u << 20u;
	test.has_user_data = true;
	return test;
}

TestCase BufferAtomicFMaxSpecialValues() {
	using O = ShaderOpcode;

	const u32 values[] = {
	    0x40000000u, // 2.0, finite update
	    0x40000000u, // 2.0, finite no-update
	    0x7f800000u, // incoming +infinity
	    0xff800000u, // incoming -infinity
	    0x3f800000u, // finite against old +infinity
	    0x3f800000u, // finite against old -infinity
	    0x3f800000u, // finite against old quiet NaN
	    0x7fcabcdeu, // incoming quiet NaN
	    0x3f800000u, // finite against old signaling NaN
	    0x7faabcdeu, // incoming signaling NaN
	    0x00000000u, // +0.0 against old -0.0
	    0x80000000u, // -0.0 against old +0.0
	    0x80000000u, // -0.0 against a negative denorm
	    0x80000001u, // negative denorm against -0.0
	    0x00000001u, // positive denorm against +0.0
	    0x00000000u, // +0.0 against a positive denorm
	};
	std::vector<u32> code;
	for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
		AppendVMovU32(&code, 20, i * 4u);
		AppendVMovLiteral(&code, i, values[i]);
		AppendBufferStoreOpcode(&code, 0x40, i, 20, true);
	}
	for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
		AppendStoreVgpr(&code, i, i + static_cast<u32>(std::size(values)));
	}
	AppendEnd(&code);

	return {"BufferAtomicFMaxSpecialValues",
	        code,
	        {0x3f800000u, 0x40800000u, 0xbf800000u, 0x3f800000u, 0x7f800000u,
	         0xff800000u, 0x7fc12345u, 0x3f800000u, 0x7fa54321u, 0x3f800000u,
	         0x80000000u, 0x00000000u, 0x80000001u, 0x80000000u, 0x00000000u,
	         0x00000001u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
	        {0x40000000u, 0x40800000u, 0x7f800000u, 0x3f800000u, 0x7f800000u,
	         0x3f800000u, 0x7fc12345u, 0x3f800000u, 0x7fa54321u, 0x3f800000u,
	         0x80000000u, 0x00000000u, 0x80000000u, 0x80000000u, 0x00000001u,
	         0x00000001u, 0x3f800000u, 0x40800000u, 0xbf800000u, 0x3f800000u,
	         0x7f800000u, 0xff800000u, 0x7fc12345u, 0x3f800000u, 0x7fa54321u,
	         0x3f800000u, 0x80000000u, 0x00000000u, 0x80000001u, 0x80000000u,
	         0x00000000u, 0x00000001u},
	        {O::VMovB32, O::BufferAtomicFMax, O::BufferStoreDword, O::SEndpgm}};
}

TestCase BufferAtomicFMaxContendedWorkgroup() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	code.push_back(EncodeVop1(0x06, 1, Vgpr(0))); // v_cvt_f32_u32 v1, thread_id.x
	AppendVMovU32(&code, 20, 0);
	AppendBufferStoreOpcode(&code, 0x40, 1, 20);
	AppendEnd(&code);

	TestCase test;
	test.name     = "BufferAtomicFMaxContendedWorkgroup";
	test.code     = code;
	test.initial  = {0xc2c80000u}; // -100.0
	test.expected = {0x427c0000u}; // max(-100.0, 0.0 .. 63.0)
	test.opcodes  = {O::VCvtF32U32, O::VMovB32, O::BufferAtomicFMax, O::SEndpgm};
	test.compute_info.threads_num[0] = 64;
	test.compute_info.threads_num[1] = 1;
	test.compute_info.threads_num[2] = 1;
	test.compute_info.thread_ids_num = 1;
	test.has_compute_info            = true;
	return test;
}

std::vector<u32> MakeRgbaImage(u32 width, u32 height, u32 value = 0) {
	return std::vector<u32>(static_cast<size_t>(width) * height * 4u, value);
}

void SetRgbaPixel(std::vector<u32>* image, u32 width, u32 x, u32 y, u32 r, u32 g, u32 b, u32 a) {
	const auto base    = static_cast<size_t>((y * width + x) * 4u);
	(*image)[base + 0] = r;
	(*image)[base + 1] = g;
	(*image)[base + 2] = b;
	(*image)[base + 3] = a;
}

TestCase ImageLoadVariants() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 2);
	AppendVMovU32(&code, 21, 1);
	AppendVMovU32(&code, 22, 0);
	AppendVMovU32(&code, 23, 0);
	code.push_back(EncodeMimg0(0x00, 0xf));
	code.push_back(EncodeMimg1(0, 20));
	code.push_back(EncodeMimg0(0x01, 0xf));
	code.push_back(EncodeMimg1(4, 20));
	AppendVMovU32(&code, 24, 0);
	code.push_back(EncodeMimg0(0x0e, 0x1));
	code.push_back(EncodeMimg1(8, 24));
	for (u32 i = 0; i < 9u; i++) {
		AppendStoreVgpr(&code, i, i);
	}
	AppendEnd(&code);

	auto image = MakeRgbaImage(4, 4);
	SetRgbaPixel(&image, 4, 2, 1, 0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u);

	TestCase test;
	test.name               = "ImageLoadVariants";
	test.code               = code;
	test.expected           = {0x3f800000u, 0x40000000u, 0x40400000u,
	                           0x40800000u, 0x3f800000u, 0x40000000u,
	                           0x40400000u, 0x40800000u, 4};
	test.opcodes            = {O::VMovB32,         O::ImageLoad,        O::ImageLoadMip,
	                           O::ImageGetResinfo, O::BufferStoreDword, O::SEndpgm};
	test.sampled_image_rgba = image;
	return test;
}

TestCase DsAppendConsumeUsesEncodedLdsSelector() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendSMovLiteral(&code, 124, 0x0000ffffu);
	AppendVMovU32(&code, 1, 0);
	AppendVMovU32(&code, 2, 10);
	code.push_back(EncodeDs0(0x0d, 0));
	code.push_back(EncodeDs1(0, 2, 1));
	code.push_back(EncodeDs0(0x3e, 0));
	code.push_back(EncodeDs1(3, 0, 0));
	code.push_back(EncodeDs0(0x3d, 0));
	code.push_back(EncodeDs1(4, 0, 0));
	AppendSMovLiteral(&code, 124, 0);
	code.push_back(EncodeDs0(0x3e, 0));
	code.push_back(EncodeDs1(5, 0, 0));
	code.push_back(EncodeDs0(0x36, 0));
	code.push_back(EncodeDs1(6, 0, 1));
	AppendStoreVgpr(&code, 3, 0);
	AppendStoreVgpr(&code, 4, 1);
	AppendStoreVgpr(&code, 5, 2);
	AppendStoreVgpr(&code, 6, 3);
	AppendEnd(&code);

	return {"DsAppendConsumeLdsSelector",
	        code,
	        {},
	        {10, 74, 0, 10},
	        {O::SMovB32, O::VMovB32, O::DsWriteB32, O::DsReadB32, O::DsAppend, O::DsConsume,
	         O::BufferStoreDword, O::SEndpgm}};
}

TestCase DsAppendUsesEncodedGdsSelector() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendSMovLiteral(&code, 124, 0x00000008u);
	code.push_back(EncodeDs0(0x3e, 0, true));
	code.push_back(EncodeDs1(0, 0, 0));
	code.push_back(EncodeDs0(0x3d, 0, true));
	code.push_back(EncodeDs1(1, 0, 0));
	code.push_back(EncodeDs0(0x3e, 4, true));
	code.push_back(EncodeDs1(2, 0, 0));
	code.push_back(EncodeDs0(0x3d, 4, true));
	code.push_back(EncodeDs1(3, 0, 0));
	AppendSMovLiteral(&code, 124, 0x00080008u);
	code.push_back(EncodeDs0(0x3e, 4, true));
	code.push_back(EncodeDs1(4, 0, 0));
	code.push_back(EncodeDs0(0x3d, 4, true));
	code.push_back(EncodeDs1(5, 0, 0));
	AppendStoreVgpr(&code, 0, 0);
	AppendStoreVgpr(&code, 1, 1);
	AppendStoreVgpr(&code, 2, 2);
	AppendStoreVgpr(&code, 3, 3);
	AppendStoreVgpr(&code, 4, 4);
	AppendStoreVgpr(&code, 5, 5);
	AppendEnd(&code);

	TestCase test {"DsAppendGdsSelector",
	               code,
	               {},
	               {10, 74, 20, 84, 40, 104},
	               {O::SMovB32, O::DsAppend, O::DsConsume, O::BufferStoreDword, O::SEndpgm}};
	test.gds_initial  = {10, 20, 30, 40};
	test.expected_gds = {10, 20, 30, 40};
	return test;
}

TestCase DsGdsSubdwordAndAtomicWrites() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	code.push_back(EncodeVop1(0x01, 1, Vgpr(0)));
	code.push_back(EncodeVop2(0x25, 2, InlineU32(1), 0));
	code.push_back(EncodeDs0(0x1e, 0, true));
	code.push_back(EncodeDs1(0, 2, 1));
	code.push_back(EncodeVop2(0x1a, 8, InlineU32(1), 0));
	code.push_back(EncodeVop2(0x25, 8, InlineU32(12), 8));
	code.push_back(EncodeDs0(0x1f, 0, true));
	code.push_back(EncodeDs1(0, 2, 8));
	AppendVMovU32(&code, 3, 4);
	AppendVMovU32(&code, 4, 1);
	code.push_back(EncodeDs0(0x00, 0, true));
	code.push_back(EncodeDs1(0, 4, 3));
	AppendVMovU32(&code, 5, 8);
	AppendVMovLiteral(&code, 6, 0x40a00000u);
	AppendVMovLiteral(&code, 7, 0x40a00000u);
	code.push_back(EncodeDs0(0x12, 0, true));
	code.push_back(EncodeDs1Ex(0, 7, 6, 5));
	AppendEnd(&code);

	TestCase test;
	test.name    = "DsGdsSubdwordAndAtomics";
	test.code    = code;
	test.opcodes = {O::VMovB32,      O::VAddNcU32, O::VLshlrevB32, O::DsWriteByte,
	                O::DsWriteShort, O::DsAddU32,  O::DsMinF32,    O::SEndpgm};
	test.compute_info.threads_num[0] = 4;
	test.compute_info.threads_num[1] = 1;
	test.compute_info.threads_num[2] = 1;
	test.compute_info.thread_ids_num = 1;
	test.has_compute_info            = true;
	test.gds_initial                 = {0, 0, 0x42c80000u, 0, 0};
	test.expected_gds                = {0x04030201u, 4, 0x40a00000u, 0x00020001u, 0x00040003u};
	return test;
}

TestCase ImageLoadR32UintUsesIntegerSampledImage() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 2);
	AppendVMovU32(&code, 21, 1);
	code.push_back(EncodeMimg0(0x00, 0x1));
	code.push_back(EncodeMimg1(0, 20));
	AppendStoreVgpr(&code, 0, 0);
	AppendEnd(&code);

	TestCase test;
	test.name     = "ImageLoadR32UintUsesIntegerSampledImage";
	test.code     = code;
	test.expected = {0xdeadbeefu};
	test.opcodes  = {O::VMovB32, O::ImageLoad, O::BufferStoreDword, O::SEndpgm};
	test.sampled_image_rgba.resize(16);
	test.sampled_image_rgba[6]          = 0xdeadbeefu;
	test.sampled_image_format           = vk::Format::eR32Uint;
	test.sampled_image_dwords_per_pixel = 1;
	test.user_data                      = MakeSampledTextureData(Prospero::BufferFormat::k32UInt);
	test.has_user_data                  = true;
	test.required_spirv                 = {"sampled_uint_2d"};
	return test;
}

TestCase ImageLoad1DUsesScalarCoordinate() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 2);
	code.push_back(EncodeMimg0(0x00, 0x1, 0, false, 0));
	code.push_back(EncodeMimg1(0, 20));
	AppendStoreVgpr(&code, 0, 0);
	AppendEnd(&code);

	TestCase test;
	test.name                 = "ImageLoad1DUsesScalarCoordinate";
	test.code                 = code;
	test.expected             = {0xdeadbeefu};
	test.opcodes              = {O::VMovB32, O::ImageLoad, O::BufferStoreDword, O::SEndpgm};
	test.image_width          = 4;
	test.image_height         = 1;
	test.sampled_image_rgba   = {0, 0, 0xdeadbeefu, 0};
	test.sampled_image_format = vk::Format::eR32Uint;
	test.sampled_image_dwords_per_pixel = 1;
	test.sampled_image_type             = vk::ImageType::e1D;
	test.sampled_image_view_type        = vk::ImageViewType::e1D;
	test.user_data                      = MakeSampledTextureData(Prospero::BufferFormat::k32UInt);
	test.user_data[3]   = static_cast<uint32_t>(Prospero::ImageType::kColor1D) << 28u;
	test.has_user_data  = true;
	test.required_spirv = {"sampled_uint_1d"};
	return test;
}

TestCase ImageLoad1DArrayUsesLayerCoordinate() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 2);
	AppendVMovU32(&code, 21, 1);
	code.push_back(EncodeMimg0(0x00, 0x1, 0, false, 4));
	code.push_back(EncodeMimg1(0, 20));
	AppendStoreVgpr(&code, 0, 0);
	AppendEnd(&code);

	TestCase test;
	test.name                 = "ImageLoad1DArrayUsesLayerCoordinate";
	test.code                 = code;
	test.expected             = {0xcafebabeu};
	test.opcodes              = {O::VMovB32, O::ImageLoad, O::BufferStoreDword, O::SEndpgm};
	test.image_width          = 4;
	test.image_height         = 1;
	test.sampled_image_rgba   = {0, 0, 0, 0, 0, 0, 0xcafebabeu, 0};
	test.sampled_image_format = vk::Format::eR32Uint;
	test.sampled_image_dwords_per_pixel = 1;
	test.sampled_image_type             = vk::ImageType::e1D;
	test.sampled_image_view_type        = vk::ImageViewType::e1DArray;
	test.sampled_image_layers           = 2;
	test.user_data                      = MakeSampledTextureData(Prospero::BufferFormat::k32UInt);
	test.user_data[3]   = static_cast<uint32_t>(Prospero::ImageType::kColor1DArray) << 28u;
	test.has_user_data  = true;
	test.required_spirv = {"sampled_uint_1d_array"};
	return test;
}

TestCase ImageLoad1DArrayDescriptorUsesSelectedLayer() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 2);
	code.push_back(EncodeMimg0(0x00, 0x1, 0, false, 0));
	code.push_back(EncodeMimg1(0, 20));
	AppendStoreVgpr(&code, 0, 0);
	AppendEnd(&code);

	TestCase test;
	test.name               = "ImageLoad1DArrayDescriptorUsesSelectedLayer";
	test.code               = code;
	test.expected           = {0xcafebabeu};
	test.opcodes            = {O::VMovB32, O::ImageLoad, O::BufferStoreDword, O::SEndpgm};
	test.image_width        = 4;
	test.image_height       = 1;
	test.sampled_image_rgba = {
	    0, 0, 0xdeadbeefu, 0, 0, 0, 0xcafebabeu, 0,
	};
	test.sampled_image_format           = vk::Format::eR32Uint;
	test.sampled_image_dwords_per_pixel = 1;
	test.sampled_image_type             = vk::ImageType::e1D;
	test.sampled_image_view_type        = vk::ImageViewType::e1D;
	test.sampled_image_layers           = 2;
	test.sampled_image_view_base_layer  = 1;
	test.sampled_image_view_layers      = 1;
	test.user_data                      = MakeSampledTextureData(Prospero::BufferFormat::k32UInt);
	test.user_data[3]   = static_cast<uint32_t>(Prospero::ImageType::kColor1DArray) << 28u;
	test.user_data[4]   = 1u | (1u << 16u);
	test.has_user_data  = true;
	test.required_spirv = {"sampled_uint_1d"};
	return test;
}

TestCase ImageLoadMipUsesVaddr2Lod2D() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 1);
	AppendVMovU32(&code, 21, 1);
	AppendVMovU32(&code, 22, 1);
	AppendVMovU32(&code, 23, 0);
	code.push_back(EncodeMimg0(0x01, 0x1));
	code.push_back(EncodeMimg1(0, 20));
	AppendStoreVgpr(&code, 0, 0);
	AppendEnd(&code);

	auto base = MakeRgbaImage(4, 4);
	SetRgbaPixel(&base, 4, 1, 1, 0x3f800000u, 0, 0, 0);
	auto mip1 = MakeRgbaImage(2, 2);
	SetRgbaPixel(&mip1, 2, 1, 1, 0x40000000u, 0, 0, 0);

	TestCase test;
	test.name                    = "ImageLoadMipUsesVaddr2Lod2D";
	test.code                    = code;
	test.expected                = {0x40000000u};
	test.opcodes                 = {O::VMovB32, O::ImageLoadMip, O::BufferStoreDword, O::SEndpgm};
	test.sampled_image_rgba_mips = {base, mip1};
	return test;
}

TestCase ImageLoadMipNsaUsesSelectedAddressVgprs() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 1);
	AppendVMovU32(&code, 21, 0);
	AppendVMovU32(&code, 22, 0);
	AppendVMovU32(&code, 30, 1);
	AppendVMovU32(&code, 31, 1);
	code.push_back(EncodeMimg0(0x01, 0x1, 1));
	code.push_back(EncodeMimg1(0, 20));
	code.push_back((30u << 0u) | (31u << 8u));
	AppendStoreVgpr(&code, 0, 0);
	AppendEnd(&code);

	auto base = MakeRgbaImage(4, 4);
	SetRgbaPixel(&base, 4, 1, 0, 0x3f800000u, 0, 0, 0);
	auto mip1 = MakeRgbaImage(2, 2);
	SetRgbaPixel(&mip1, 2, 1, 1, 0x40a00000u, 0, 0, 0);

	TestCase test;
	test.name                    = "ImageLoadMipNsaUsesSelectedAddressVgprs";
	test.code                    = code;
	test.expected                = {0x40a00000u};
	test.opcodes                 = {O::VMovB32, O::ImageLoadMip, O::BufferStoreDword, O::SEndpgm};
	test.sampled_image_rgba_mips = {base, mip1};
	return test;
}

TestCase ImageLoadA16UintCoordsOnGpu() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 20, 0x00010002u); // x=2, y=1 packed as u16.
	AppendVMovU32(&code, 21, 0);
	code.push_back(EncodeMimg0(0x00, 0xf));
	code.push_back(EncodeMimg1(0, 20, 0, 0, true));
	for (u32 i = 0; i < 4u; i++) {
		AppendStoreVgpr(&code, i, i);
	}
	AppendEnd(&code);

	auto image = MakeRgbaImage(4, 4);
	SetRgbaPixel(&image, 4, 2, 1, 0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u);

	TestCase test;
	test.name               = "ImageLoadA16UintCoordsOnGpu";
	test.code               = code;
	test.expected           = {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u};
	test.opcodes            = {O::VMovB32, O::ImageLoad, O::BufferStoreDword, O::SEndpgm};
	test.sampled_image_rgba = image;
	test.required_spirv     = {"OpShiftRightLogical", "OpBitwiseAnd"};
	test.forbidden_spirv    = {"UnpackHalf2x16"};
	return test;
}

TestCase ImageGetResinfoDmaskWidthHeight() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 24, 0);
	code.push_back(EncodeMimg0(0x0e, 0x3));
	code.push_back(EncodeMimg1(0, 24));
	AppendStoreVgpr(&code, 0, 0);
	AppendStoreVgpr(&code, 1, 1);
	AppendEnd(&code);

	TestCase test;
	test.name               = "ImageGetResinfoDmaskWidthHeight";
	test.code               = code;
	test.expected           = {4, 2};
	test.opcodes            = {O::VMovB32, O::ImageGetResinfo, O::BufferStoreDword, O::SEndpgm};
	test.image_width        = 4;
	test.image_height       = 2;
	test.sampled_image_rgba = MakeRgbaImage(test.image_width, test.image_height);
	return test;
}

TestCase ImageGetResinfoDmaskMipLevels() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 24, 0);
	code.push_back(EncodeMimg0(0x0e, 0x8));
	code.push_back(EncodeMimg1(0, 24));
	AppendStoreVgpr(&code, 0, 0);
	AppendEnd(&code);

	TestCase test;
	test.name     = "ImageGetResinfoDmaskMipLevels";
	test.code     = code;
	test.expected = {2};
	test.opcodes  = {O::VMovB32, O::ImageGetResinfo, O::BufferStoreDword, O::SEndpgm};
	auto base     = MakeRgbaImage(4, 4);
	auto mip1     = MakeRgbaImage(2, 2);
	test.sampled_image_rgba_mips = {base, mip1};
	return test;
}

SkippedCase ImageStoreMipWritesExplicitMip2D() {
	return {"ImageStoreMipWritesExplicitMip2D",
	        "requires per-mip storage-image view descriptors; Vulkan "
	        "OpImageWrite cannot take Lod"};
}

TestCase ImageSampleAndGather() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 20, 0x3f200000u);
	AppendVMovLiteral(&code, 21, 0x3ec00000u);
	AppendVMovU32(&code, 22, 0);
	code.push_back(EncodeMimg0(0x20, 0xf));
	code.push_back(EncodeMimg1(0, 20));
	code.push_back(EncodeMimg0(0x47, 0x1));
	code.push_back(EncodeMimg1(4, 20));
	AppendVMovU32(&code, 24, 0);
	AppendVMovLiteral(&code, 25, 0x3f200000u);
	AppendVMovLiteral(&code, 26, 0x3ec00000u);
	AppendVMovU32(&code, 27, 0);
	code.push_back(EncodeMimg0(0x57, 0x1));
	code.push_back(EncodeMimg1(8, 24));
	code.push_back(EncodeMimg0(0x60, 0x1));
	code.push_back(EncodeMimg1(12, 20));
	for (u32 i = 0; i < 13u; i++) {
		AppendStoreVgpr(&code, i, i);
	}
	AppendEnd(&code);

	auto image = MakeRgbaImage(4, 4);
	for (u32 y = 0; y < 4u; y++) {
		for (u32 x = 0; x < 4u; x++) {
			SetRgbaPixel(&image, 4, x, y, 0x3f800000u, 0, 0, 0);
		}
	}
	SetRgbaPixel(&image, 4, 2, 1, 0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u);

	TestCase test;
	test.name     = "ImageSampleAndGather";
	test.code     = code;
	test.expected = {0x3f800000u,
	                 0x40000000u,
	                 0x40400000u,
	                 0x40800000u,
	                 0x3f800000u,
	                 0x3f800000u,
	                 0x3f800000u,
	                 0x3f800000u,
	                 0x3f800000u,
	                 0x3f800000u,
	                 0x3f800000u,
	                 0x3f800000u,
	                 0};
	test.opcodes  = {O::VMovB32,         O::ImageSample,      O::ImageGetLod, O::ImageGather4Lz,
	                 O::ImageGather4LzO, O::BufferStoreDword, O::SEndpgm};
	test.sampled_image_rgba = image;
	return test;
}

TestCase ImageSampleA16SamplerCoordsOnGpu() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 20, 0x36003900u); // x=0.625, y=0.375 packed as f16.
	AppendVMovU32(&code, 21, 0);
	code.push_back(EncodeMimg0(0x20, 0xf));
	code.push_back(EncodeMimg1(0, 20, 0, 0, true));
	for (u32 i = 0; i < 4u; i++) {
		AppendStoreVgpr(&code, i, i);
	}
	AppendEnd(&code);

	auto image = MakeRgbaImage(4, 4);
	SetRgbaPixel(&image, 4, 2, 1, 0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u);

	TestCase test;
	test.name               = "ImageSampleA16SamplerCoordsOnGpu";
	test.code               = code;
	test.expected           = {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u};
	test.opcodes            = {O::VMovB32, O::ImageSample, O::BufferStoreDword, O::SEndpgm};
	test.sampled_image_rgba = image;
	test.required_spirv     = {"UnpackHalf2x16"};
	return test;
}

TestCase ImageSampleOpcodeAliasUsesNormalCoords() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 2, 0x3f000000u);
	AppendVMovLiteral(&code, 3, 0x3f000000u);
	code.push_back(0xf0800109u); // observed image_sample_a v6, v2, s0, s24
	code.push_back(0x00c00602u);
	AppendEnd(&code);

	TestCase test;
	test.name            = "ImageSampleOpcodeAliasUsesNormalCoords";
	test.code            = code;
	test.opcodes         = {O::VMovB32, O::ImageSample, O::SEndpgm};
	test.required_spirv  = {"OpImageSampleExplicitLod"};
	test.forbidden_spirv = {"UnpackHalf2x16"};
	test.compile_only    = true;
	return test;
}

TestCase ImageSampleA16OffsetKeepsTexelOffset32BitOnGpu() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20,
	              1); // Non-constant +1 X offset is not a SPIR-V ConstOffset.
	AppendVMovLiteral(&code, 21, 0x36003900u); // x=0.625, y=0.375 packed as f16.
	AppendVMovU32(&code, 22, 0);
	code.push_back(EncodeMimg0(0x30, 0xf));
	code.push_back(EncodeMimg1(0, 20, 0, 0, true));
	for (u32 i = 0; i < 4u; i++) {
		AppendStoreVgpr(&code, i, i);
	}
	AppendEnd(&code);

	auto image = MakeRgbaImage(4, 4);
	SetRgbaPixel(&image, 4, 2, 1, 0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u);

	TestCase test;
	test.name               = "ImageSampleA16OffsetKeepsTexelOffset32BitOnGpu";
	test.code               = code;
	test.expected           = {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u};
	test.opcodes            = {O::VMovB32, O::ImageSample, O::BufferStoreDword, O::SEndpgm};
	test.sampled_image_rgba = image;
	test.required_spirv     = {"UnpackHalf2x16"};
	test.forbidden_spirv    = {"OpBitFieldSExtract"};
	return test;
}

TestCase ImageSampleA16CompareBiasRdna2AddressOrder() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 20, 0x00003800u); // bias=0.5 as low f16.
	AppendVMovLiteral(&code, 21, 0x3f000000u); // PCF reference stays 32-bit.
	AppendVMovLiteral(&code, 22, 0x36003900u); // x=0.625, y=0.375 packed as f16.
	code.push_back(EncodeMimg0(0x2d, 0x1));
	code.push_back(EncodeMimg1(0, 20, 0, 0, true));
	AppendEnd(&code);

	TestCase test;
	test.name           = "ImageSampleA16CompareBiasRdna2AddressOrder";
	test.code           = code;
	test.opcodes        = {O::VMovB32, O::ImageSample, O::SEndpgm};
	test.required_spirv = {"OpImageSampleDrefExplicitLod", "UnpackHalf2x16"};
	test.compile_only   = true;
	return test;
}

TestCase ImageGatherCompareOpcodes() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendVMovLiteral(&code, 21, 0x3f000000u);
	AppendVMovLiteral(&code, 22, 0x3f200000u);
	AppendVMovLiteral(&code, 23, 0x3ec00000u);
	AppendVMovLiteral(&code, 24, 0x3f000000u);
	AppendVMovLiteral(&code, 25, 0x3f200000u);
	AppendVMovLiteral(&code, 26, 0x3ec00000u);
	AppendVMovLiteral(&code, 28, 0x3f000000u);
	AppendVMovLiteral(&code, 29, 0x3f200000u);
	AppendVMovLiteral(&code, 30, 0x3ec00000u);
	AppendVMovU32(&code, 32, 0);
	AppendVMovLiteral(&code, 33, 0x3f000000u);
	AppendVMovLiteral(&code, 34, 0x3f200000u);
	AppendVMovLiteral(&code, 35, 0x3ec00000u);
	code.push_back(EncodeMimg0(0x48, 0x1));
	code.push_back(EncodeMimg1(4, 24));
	code.push_back(EncodeMimg0(0x4f, 0x1));
	code.push_back(EncodeMimg1(8, 28));
	code.push_back(EncodeMimg0(0x58, 0x1));
	code.push_back(EncodeMimg1(0, 20));
	code.push_back(EncodeMimg0(0x5f, 0x1));
	code.push_back(EncodeMimg1(12, 32));
	AppendEnd(&code);

	TestCase test;
	test.name           = "ImageGatherCompareOpcodes";
	test.code           = code;
	test.opcodes        = {O::VMovB32,        O::ImageGather4C,    O::ImageGather4CLz,
	                       O::ImageGather4CO, O::ImageGather4CLzO, O::SEndpgm};
	test.required_spirv = {"OpImageDrefGather", "OpBitFieldSExtract"};
	test.compile_only   = true;
	return test;
}

TestCase ImageStoreVariants() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 1);
	AppendVMovU32(&code, 21, 2);
	AppendVMovU32(&code, 22, 0);
	AppendVMovLiteral(&code, 0, 0x3f800000u);
	AppendVMovLiteral(&code, 1, 0x40000000u);
	AppendVMovLiteral(&code, 2, 0x40400000u);
	AppendVMovLiteral(&code, 3, 0x40800000u);
	code.push_back(EncodeMimg0(0x08, 0xf));
	code.push_back(EncodeMimg1(0, 20));
	AppendVMovU32(&code, 24, 3);
	AppendVMovU32(&code, 25, 2);
	AppendVMovU32(&code, 26, 0);
	AppendVMovU32(&code, 27, 0);
	AppendVMovLiteral(&code, 4, 0x40a00000u);
	AppendVMovLiteral(&code, 5, 0x40c00000u);
	AppendVMovLiteral(&code, 6, 0x40e00000u);
	AppendVMovLiteral(&code, 7, 0x41000000u);
	code.push_back(EncodeMimg0(0x09, 0xf));
	code.push_back(EncodeMimg1(4, 24));
	AppendVMovLiteral(&code, 8, 0x12345678u);
	AppendStoreVgpr(&code, 8, 0);
	AppendEnd(&code);

	auto expected_image = MakeRgbaImage(4, 4);
	SetRgbaPixel(&expected_image, 4, 1, 2, 0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u);
	SetRgbaPixel(&expected_image, 4, 3, 2, 0x40a00000u, 0x40c00000u, 0x40e00000u, 0x41000000u);

	TestCase test;
	test.name     = "ImageStoreVariants";
	test.code     = code;
	test.expected = {0x12345678u};
	test.opcodes  = {O::VMovB32, O::ImageStore, O::ImageStoreMip, O::BufferStoreDword, O::SEndpgm};
	test.storage_image_rgba          = MakeRgbaImage(4, 4);
	test.storage_image_r32ui         = std::vector<u32>(16, 0);
	test.expected_storage_image_rgba = expected_image;
	return test;
}

TestCase ImageStoreRgbOneUsesInverseSwizzle() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 1);
	AppendVMovU32(&code, 21, 2);
	AppendVMovU32(&code, 22, 0);
	AppendVMovLiteral(&code, 0, 0x3f800000u);
	AppendVMovLiteral(&code, 1, 0x3f008081u);
	AppendVMovLiteral(&code, 2, 0x3e808081u);
	AppendVMovLiteral(&code, 3, 0x3f40c0c1u);
	code.push_back(EncodeMimg0(0x08, 0xf));
	code.push_back(EncodeMimg1(0, 20));
	AppendEnd(&code);

	TestCase test;
	test.name          = "ImageStoreRgbOneUsesInverseSwizzle";
	test.code          = code;
	test.opcodes       = {O::VMovB32, O::ImageStore, O::SEndpgm};
	test.has_user_data = true;
	test.user_data[0]  = 0x1000u;
	test.user_data[1]  = static_cast<uint32_t>(Prospero::BufferFormat::k8_8_8_8UNorm) << 20u;
	test.image_descriptor_swizzle               = DstSel(4, 5, 6, 1);
	test.storage_image_format                   = vk::Format::eR8G8B8A8Unorm;
	test.storage_image_dwords_per_pixel         = 1;
	test.storage_image_rgba                     = std::vector<u32>(16, 0);
	test.expected_storage_image_rgba            = std::vector<u32>(16, 0);
	test.expected_storage_image_rgba[2 * 4 + 1] = 0x004080ffu;
	return test;
}

TestCase ImageStoreDuplicateSelectorUsesInverseSwizzle() {
	auto test                                   = ImageStoreRgbOneUsesInverseSwizzle();
	test.name                                   = "ImageStoreDuplicateSelectorUsesInverseSwizzle";
	test.image_descriptor_swizzle               = DstSel(4, 4, 6, 7);
	test.expected_storage_image_rgba[2 * 4 + 1] = 0xc04000ffu;
	return test;
}

TestCase ImageStoreBgraUsesInverseSwizzle() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 1);
	AppendVMovU32(&code, 21, 2);
	AppendVMovU32(&code, 22, 0);
	AppendVMovLiteral(&code, 0, 0x3f800000u);
	AppendVMovLiteral(&code, 1, 0x3f008081u); // 128/255
	AppendVMovLiteral(&code, 2, 0x3e808081u); // 64/255
	AppendVMovLiteral(&code, 3, 0x3f3fbfc0u); // 191/255
	code.push_back(EncodeMimg0(0x08, 0xf));
	code.push_back(EncodeMimg1(0, 20));
	AppendEnd(&code);

	TestCase test;
	test.name          = "ImageStoreBgraUsesInverseSwizzle";
	test.code          = code;
	test.opcodes       = {O::VMovB32, O::ImageStore, O::SEndpgm};
	test.has_user_data = true;
	test.user_data[0]  = 0x1000u;
	test.user_data[1]  = static_cast<uint32_t>(Prospero::BufferFormat::k8_8_8_8UNorm) << 20u;
	test.image_descriptor_swizzle               = DstSel(6, 5, 4, 7);
	test.storage_image_format                   = vk::Format::eR8G8B8A8Unorm;
	test.storage_image_dwords_per_pixel         = 1;
	test.storage_image_rgba                     = std::vector<u32>(16, 0);
	test.expected_storage_image_rgba            = std::vector<u32>(16, 0);
	test.expected_storage_image_rgba[2 * 4 + 1] = 0xbfff8040u;
	return test;
}

TestCase ImageStoreYzwxUsesInverseSwizzle() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 1);
	AppendVMovU32(&code, 21, 2);
	AppendVMovU32(&code, 22, 0);
	AppendVMovLiteral(&code, 0, 0x3f800000u);
	AppendVMovLiteral(&code, 1, 0x3f000000u);
	AppendVMovLiteral(&code, 2, 0x3e800000u);
	AppendVMovLiteral(&code, 3, 0x3f400000u);
	code.push_back(EncodeMimg0(0x08, 0xf));
	code.push_back(EncodeMimg1(0, 20));
	AppendEnd(&code);

	TestCase test;
	test.name          = "ImageStoreYzwxUsesInverseSwizzle";
	test.code          = code;
	test.opcodes       = {O::VMovB32, O::ImageStore, O::SEndpgm};
	test.user_data     = MakeStorageTextureData(Prospero::BufferFormat::k32_32_32_32Float);
	test.has_user_data = true;
	test.image_descriptor_swizzle    = DstSel(5, 6, 7, 4);
	test.storage_image_rgba          = MakeRgbaImage(4, 4);
	test.expected_storage_image_rgba = MakeRgbaImage(4, 4);
	SetRgbaPixel(&test.expected_storage_image_rgba, 4, 1, 2, 0x3f400000u, 0x3f800000u, 0x3f000000u,
	             0x3e800000u);
	return test;
}

TestCase ImageStoreR32FloatUsesFormatlessStorageImage() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 2);
	AppendVMovU32(&code, 21, 1);
	AppendVMovU32(&code, 22, 0);
	AppendVMovLiteral(&code, 0, 0x3f800000u);
	code.push_back(EncodeMimg0(0x08, 0x1));
	code.push_back(EncodeMimg1(0, 20));
	AppendEnd(&code);

	std::vector<u32> expected_image(16, 0);
	expected_image[1 * 4 + 2] = 0x3f800000u;

	TestCase test;
	test.name                           = "ImageStoreR32FloatUsesFormatlessStorageImage";
	test.code                           = code;
	test.opcodes                        = {O::VMovB32, O::ImageStore, O::SEndpgm};
	test.storage_image_format           = vk::Format::eR32Sfloat;
	test.storage_image_dwords_per_pixel = 1;
	test.storage_image_rgba             = std::vector<u32>(16, 0);
	test.expected_storage_image_rgba    = expected_image;
	test.required_spirv                 = {"OpCapability StorageImageReadWithoutFormat",
	                                       "OpCapability StorageImageWriteWithoutFormat"};
	test.forbidden_spirv                = {"Rgba32f"};
	return test;
}

TestCase ImageStoreR32SintUsesRawUintView() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 2);
	AppendVMovU32(&code, 21, 1);
	AppendVMovU32(&code, 22, 0);
	AppendVMovLiteral(&code, 0, 0x80000001u);
	code.push_back(EncodeMimg0(0x08, 0x1));
	code.push_back(EncodeMimg1(0, 20));
	AppendEnd(&code);

	std::vector<u32> expected_image(16, 0);
	expected_image[1 * 4 + 2] = 0x80000001u;

	TestCase test;
	test.name                         = "ImageStoreR32SintUsesRawUintView";
	test.code                         = code;
	test.opcodes                      = {O::VMovB32, O::ImageStore, O::SEndpgm};
	test.user_data                    = MakeStorageTextureData(Prospero::BufferFormat::k32SInt);
	test.has_user_data                = true;
	test.storage_image_r32ui          = std::vector<u32>(16, 0);
	test.expected_storage_image_r32ui = expected_image;
	test.required_spirv               = {"storage_uint_2d"};
	return test;
}

TestCase ImageStoreR32UintUsesUintStorageImage() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 2);
	AppendVMovU32(&code, 21, 1);
	AppendVMovU32(&code, 22, 0);
	AppendVMovLiteral(&code, 0, 0x12345678u);
	code.push_back(EncodeMimg0(0x08, 0x1));
	code.push_back(EncodeMimg1(0, 20));
	AppendEnd(&code);

	std::vector<u32> expected_image(16, 0);
	expected_image[1 * 4 + 2] = 0x12345678u;

	TestCase test;
	test.name                         = "ImageStoreR32UintUsesUintStorageImage";
	test.code                         = code;
	test.opcodes                      = {O::VMovB32, O::ImageStore, O::SEndpgm};
	test.user_data                    = MakeStorageTextureData(Prospero::BufferFormat::k32UInt);
	test.has_user_data                = true;
	test.storage_image_rgba           = MakeRgbaImage(4, 4);
	test.storage_image_r32ui          = std::vector<u32>(16, 0);
	test.expected_storage_image_r32ui = expected_image;
	test.required_spirv               = {"R32ui", "storage_uint_2d"};
	return test;
}

TestCase ComputeTgSizeSgprUsesWaveMetadata() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendStoreSgpr(&code, 2, 0);
	AppendEnd(&code);

	TestCase test;
	test.name                            = "ComputeTgSizeSgprUsesWaveMetadata";
	test.code                            = code;
	test.opcodes                         = {O::VMovB32, O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0]     = 4;
	test.compute_info.threads_num[1]     = 4;
	test.compute_info.threads_num[2]     = 16;
	test.compute_info.group_id[0]        = true;
	test.compute_info.group_id[1]        = true;
	test.compute_info.wave_size          = 32;
	test.compute_info.thread_ids_num     = 3;
	test.compute_info.workgroup_register = 0;
	test.compute_info.tg_size_en         = true;
	test.has_compute_info                = true;
	test.compile_only                    = true;
	test.required_spirv                  = {"OpUDiv", "OpShiftLeftLogical", "2147483648"};
	return test;
}

TestCase ImageAtomicVariants() {
	using O = ShaderOpcode;

	const u32 initial[] = {10, 10, 0xf0f0u, 0xf000u, 0xf00fu};
	const u32 values[]  = {5, 5, 0x0ff0u, 0x0f00u, 0x00ffu};
	const u32 ops[]     = {0x11, 0x15, 0x18, 0x19, 0x1a};

	std::vector<u32> code;
	for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
		AppendVMovU32(&code, 20, i & 3u);
		AppendVMovU32(&code, 21, i >> 2u);
		AppendVMovU32(&code, 22, 0);
		AppendVMovLiteral(&code, 0, values[i]);
		code.push_back(EncodeMimg0(ops[i], 0x1, 0, true));
		code.push_back(EncodeMimg1(0, 20));
		AppendStoreVgpr(&code, 0, i);
	}
	AppendEnd(&code);

	TestCase test;
	test.name     = "ImageAtomicVariants";
	test.code     = code;
	test.expected = {10, 10, 0xf0f0u, 0xf000u, 0xf00fu};
	test.opcodes  = {O::VMovB32,       O::ImageAtomicAdd, O::ImageAtomicUMin,  O::ImageAtomicAnd,
	                 O::ImageAtomicOr, O::ImageAtomicXor, O::BufferStoreDword, O::SEndpgm};
	test.storage_image_rgba  = MakeRgbaImage(4, 4);
	test.storage_image_r32ui = std::vector<u32>(16, 0);
	for (u32 i = 0; i < static_cast<u32>(std::size(initial)); i++) {
		test.storage_image_r32ui[i] = initial[i];
	}
	test.expected_storage_image_r32ui    = std::vector<u32>(16, 0);
	test.expected_storage_image_r32ui[0] = 15;
	test.expected_storage_image_r32ui[1] = 5;
	test.expected_storage_image_r32ui[2] = 0x00f0u;
	test.expected_storage_image_r32ui[3] = 0xff00u;
	test.expected_storage_image_r32ui[4] = 0xf0f0u;
	return test;
}

TestCase ImageAtomicGlc0DoesNotReturnOldValue() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovU32(&code, 20, 0);
	AppendVMovU32(&code, 21, 0);
	AppendVMovU32(&code, 22, 0);
	AppendVMovU32(&code, 0, 5);
	code.push_back(EncodeMimg0(0x11, 0x1));
	code.push_back(EncodeMimg1(0, 20));
	AppendStoreVgpr(&code, 0, 0);
	AppendEnd(&code);

	TestCase test;
	test.name                   = "ImageAtomicGlc0DoesNotReturnOldValue";
	test.code                   = code;
	test.expected               = {5};
	test.opcodes                = {O::VMovB32, O::ImageAtomicAdd, O::BufferStoreDword, O::SEndpgm};
	test.storage_image_rgba     = MakeRgbaImage(4, 4);
	test.storage_image_r32ui    = std::vector<u32>(16, 0);
	test.storage_image_r32ui[0] = 10;
	test.expected_storage_image_r32ui    = std::vector<u32>(16, 0);
	test.expected_storage_image_r32ui[0] = 15;
	return test;
}

GraphicsCase GraphicsInterpolationExport() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	code.push_back(EncodeVintrp(0x00, 0, 0, 0, 0));
	code.push_back(EncodeVintrp(0x01, 0, 0, 0, 0));
	code.push_back(EncodeVintrp(0x02, 1, 0, 1, 2));
	AppendVMovLiteral(&code, 2, 0x3f400000u);
	AppendVMovLiteral(&code, 3, 0x3f800000u);
	code.push_back(EncodeExp0(0x00, 0xf));
	code.push_back(EncodeExp1(0, 1, 2, 3));
	AppendEnd(&code);

	return {"GraphicsInterpolationExport",
	        code,
	        {0x3e800000u, 0x3f000000u, 0x3f400000u, 0x3f800000u},
	        {O::VInterpP1F32, O::VInterpP2F32, O::VInterpMovF32, O::VMovB32, O::Exp, O::SEndpgm}};
}

GraphicsCase GraphicsFlatInterpolatorExport() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	code.push_back(EncodeVintrp(0x02, 0, 0, 0, 2));
	AppendVMovLiteral(&code, 1, 0x00000000u);
	AppendVMovLiteral(&code, 2, 0x00000000u);
	AppendVMovLiteral(&code, 3, 0x3f800000u);
	code.push_back(EncodeExp0(0x00, 0xf));
	code.push_back(EncodeExp1(0, 1, 2, 3));
	AppendEnd(&code);

	GraphicsCase test;
	test.name                        = "GraphicsFlatInterpolatorExport";
	test.fragment_code               = code;
	test.expected_pixel              = {0x3e800000u, 0x00000000u, 0x00000000u, 0x3f800000u};
	test.opcodes                     = {O::VInterpMovF32, O::VMovB32, O::Exp, O::SEndpgm};
	test.pixel_interpolator_settings = {0x00000400u};
	test.vertices                    = {
	    0xbf800000u, 0xbf800000u, 0x3e800000u, 0x00000000u, 0x00000000u, 0x3f800000u,
	    0x40400000u, 0xbf800000u, 0x3f400000u, 0x00000000u, 0x00000000u, 0x3f800000u,
	    0xbf800000u, 0x40400000u, 0x3e800000u, 0x00000000u, 0x00000000u, 0x3f800000u,
	};
	return test;
}

GraphicsCase GraphicsDsAddtidScratchExport() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	code.push_back(EncodeSMovB32(124, InlineU32(0)));
	AppendVMovLiteral(&code, 0, 0x3f000000u);
	code.push_back(EncodeDs0(0xb0, 0));
	code.push_back(EncodeDs1(0, 0, 0));
	code.push_back(EncodeDs0(0xb1, 0));
	code.push_back(EncodeDs1(4, 0, 0));
	code.push_back(EncodeExp0(0x00, 0xf));
	code.push_back(EncodeExp1(4, 4, 4, 4));
	AppendEnd(&code);

	return {"GraphicsDsAddtidScratchExport",
	        code,
	        {0x3f000000u, 0x3f000000u, 0x3f000000u, 0x3f000000u},
	        {O::SMovB32, O::VMovB32, O::DsWriteAddtidB32, O::DsReadAddtidB32, O::Exp, O::SEndpgm}};
}

GraphicsCase GraphicsDirectSgprPushConstantExport() {
	using O = ShaderOpcode;

	const std::vector<u32> values = {0x3e800000u, 0x3f000000u, 0x3f400000u, 0x3f800000u};

	std::vector<u32> code;
	code.push_back(EncodeVop1(0x01, 0, 0));
	code.push_back(EncodeVop1(0x01, 1, 1));
	code.push_back(EncodeVop1(0x01, 2, 2));
	code.push_back(EncodeVop1(0x01, 3, 3));
	code.push_back(EncodeExp0(0x00, 0xf));
	code.push_back(EncodeExp1(0, 1, 2, 3));
	AppendEnd(&code);

	GraphicsCase test;
	test.name           = "GraphicsDirectSgprPushConstantExport";
	test.fragment_code  = code;
	test.expected_pixel = values;
	test.opcodes        = {O::VMovB32, O::Exp, O::SEndpgm};

	for (size_t i = 0; i < values.size(); i++) {
		test.user_data[i] = values[i];
	}
	test.has_user_data  = true;
	test.push_constants = values;
	return test;
}

GraphicsCase GraphicsInlineSrtScalarPromotionExport() {
	using O = ShaderOpcode;

	const std::vector<u32> values = {0x00000000u, 0x00000000u, 0x00000000u, 0x3f800000u};

	std::vector<u32> code;
	AppendVop3(&code, 0x12fu, 0, 0, 1);
	AppendVop3(&code, 0x12fu, 1, 2, 3);
	code.push_back(EncodeExp0(0x00, 0xf, true, true, true));
	code.push_back(EncodeExp1(0, 1, 0, 0));
	AppendEnd(&code);

	GraphicsCase test;
	test.name           = "GraphicsInlineSrtScalarPromotionExport";
	test.fragment_code  = code;
	test.expected_pixel = values;
	test.opcodes        = {O::VCvtPkrtzF16F32, O::Exp, O::SEndpgm};

	for (size_t i = 0; i < values.size(); i++) {
		test.user_data[i] = values[i];
	}
	test.has_user_data  = true;
	test.push_constants = values;
	return test;
}

GraphicsCase GraphicsNullVmExportDiscardsInactiveExec() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x3f000000u);
	AppendVMovLiteral(&code, 1, 0x3f400000u);
	AppendVMovLiteral(&code, 2, 0x3f800000u);
	AppendVMovLiteral(&code, 3, 0x40000000u);
	code.push_back(EncodeExp0(0x00, 0xf, false));
	code.push_back(EncodeExp1(0, 1, 2, 3));
	code.push_back(EncodeSop1(0x04, 126, InlineU32(0)));
	code.push_back(EncodeExp0(0x09, 0x0, true, false, true));
	code.push_back(EncodeExp1(0, 0, 0, 0));
	AppendEnd(&code);

	return {"GraphicsNullVmExportDiscardsInactiveExec",
	        code,
	        {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
	        {O::VMovB32, O::Exp, O::SMovB64, O::SEndpgm}};
}

GraphicsCase GraphicsMrt0OffVmExportDiscardsInactiveExec() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x3f000000u);
	AppendVMovLiteral(&code, 1, 0x3f400000u);
	AppendVMovLiteral(&code, 2, 0x3f800000u);
	AppendVMovLiteral(&code, 3, 0x40000000u);
	code.push_back(EncodeExp0(0x00, 0xf, false));
	code.push_back(EncodeExp1(0, 1, 2, 3));
	code.push_back(EncodeSop1(0x04, 126, InlineU32(0)));
	code.push_back(EncodeExp0(0x00, 0x0, true, true, true));
	code.push_back(EncodeExp1(0, 0, 0, 0));
	AppendEnd(&code);

	return {"GraphicsMrt0OffVmExportDiscardsInactiveExec",
	        code,
	        {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
	        {O::VMovB32, O::Exp, O::SMovB64, O::SEndpgm}};
}

GraphicsCase GraphicsFinalVmExportSupersedesEarlierVmMask() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x3f000000u);
	AppendVMovLiteral(&code, 1, 0x3f400000u);
	AppendVMovLiteral(&code, 2, 0x3f800000u);
	AppendVMovLiteral(&code, 3, 0x40000000u);
	code.push_back(EncodeSop1(0x04, 126, InlineU32(0)));
	code.push_back(EncodeExp0(0x00, 0xf, false, false, true));
	code.push_back(EncodeExp1(0, 1, 2, 3));
	code.push_back(EncodeSMovB32(126, 193u));
	code.push_back(EncodeSMovB32(127, 193u));
	AppendVMovLiteral(&code, 4, 0x40400000u);
	AppendVMovLiteral(&code, 5, 0x40800000u);
	AppendVMovLiteral(&code, 6, 0x40a00000u);
	AppendVMovLiteral(&code, 7, 0x40c00000u);
	code.push_back(EncodeExp0(0x00, 0xf, true, false, true));
	code.push_back(EncodeExp1(4, 5, 6, 7));
	AppendEnd(&code);

	return {"GraphicsFinalVmExportSupersedesEarlierVmMask",
	        code,
	        {0x40400000u, 0x40800000u, 0x40a00000u, 0x40c00000u},
	        {O::VMovB32, O::SMovB64, O::Exp, O::SMovB32, O::SEndpgm}};
}

GraphicsCase GraphicsBranchPathFinalVmExportDiscardsInactiveExec() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	AppendVMovLiteral(&code, 0, 0x3f000000u);
	AppendVMovLiteral(&code, 1, 0x3f400000u);
	AppendVMovLiteral(&code, 2, 0x3f800000u);
	AppendVMovLiteral(&code, 3, 0x40000000u);
	code.push_back(EncodeExp0(0x00, 0xf, false));
	code.push_back(EncodeExp1(0, 1, 2, 3));
	code.push_back(EncodeSop1(0x04, 126, InlineU32(0)));
	code.push_back(EncodeExp0(0x09, 0x0, true, false, true));
	code.push_back(EncodeExp1(0, 0, 0, 0));
	code.push_back(EncodeSopc(0x06, InlineU32(0), InlineU32(1)));
	const auto branch_index = code.size();
	code.push_back(0);
	AppendEnd(&code);
	const auto later_export_index = code.size();
	code[branch_index] = EncodeSopp(0x05, static_cast<u32>(later_export_index - branch_index - 1u));
	code.push_back(EncodeSMovB32(126, 193u));
	code.push_back(EncodeSMovB32(127, 193u));
	AppendVMovLiteral(&code, 4, 0x40400000u);
	AppendVMovLiteral(&code, 5, 0x40800000u);
	AppendVMovLiteral(&code, 6, 0x40a00000u);
	AppendVMovLiteral(&code, 7, 0x40c00000u);
	code.push_back(EncodeExp0(0x00, 0xf, true, false, true));
	code.push_back(EncodeExp1(4, 5, 6, 7));
	AppendEnd(&code);

	return {
	    "GraphicsBranchPathFinalVmExportDiscardsInactiveExec",
	    code,
	    {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
	    {O::VMovB32, O::Exp, O::SMovB64, O::SCmpEqU32, O::SCbranchScc1, O::SMovB32, O::SEndpgm}};
}

TestCase MultipleWorkitemsGlobalId() {
	using O = ShaderOpcode;

	std::vector<u32> code = {
	    EncodeVop1(0x01, 1, 0),
	    EncodeVop2(0x1a, 1, InlineU32(2), 1),
	    EncodeVop2(0x25, 1, Vgpr(0), 1),
	    EncodeVop2(0x25, 2, InlineU32(64), 1),
	    EncodeVop2(0x1a, 3, InlineU32(2), 1),
	};
	AppendBufferStoreDword(&code, 2, 3);
	AppendEnd(&code);

	TestCase test;
	test.name     = "MultipleWorkitemsGlobalId";
	test.code     = code;
	test.expected = {64, 65, 66, 67, 68, 69, 70, 71};
	test.opcodes  = {O::VMovB32, O::VLshlrevB32, O::VAddNcU32, O::BufferStoreDword, O::SEndpgm};
	test.compute_info.threads_num[0]     = 4;
	test.compute_info.threads_num[1]     = 1;
	test.compute_info.threads_num[2]     = 1;
	test.compute_info.group_id[0]        = true;
	test.compute_info.thread_ids_num     = 1;
	test.compute_info.workgroup_register = 0;
	test.has_compute_info                = true;
	test.dispatch_x                      = 2;
	return test;
}

TestCase DispatcherIrreducibleControlFlow() {
	using O = ShaderOpcode;

	std::vector<u32> code;
	code.push_back(EncodeSopp(0x05, 5)); // entry -> B, fallthrough A
	AppendVMovU32(&code, 0, 7);
	AppendStoreVgpr(&code, 0, 0);
	code.push_back(EncodeSopp(0x04, 1));       // A exits while SCC is initially zero
	code.push_back(EncodeSopp(0x02, 0xfffau)); // B -> A
	AppendEnd(&code);

	TestCase test;
	test.name     = "DispatcherIrreducibleControlFlow";
	test.code     = code;
	test.expected = {7};
	test.opcodes  = {O::SCbranchScc1, O::VMovB32, O::BufferStoreDword,
	                 O::SCbranchScc0, O::SBranch, O::SEndpgm};
	return test;
}

std::vector<TestCase> MakeCases() {
	std::vector<TestCase> cases;
	cases.reserve(128);
	auto AddCase = [&cases](TestCase (*factory)()) { cases.push_back(factory()); };

	AddCase(IntegerAddSubMul);
	AddCase(BitwiseOps);
	AddCase(Shifts);
	AddCase(ExactPushConstantExtent);
	AddCase(ScalarShiftCountsMaskLowBits);
	AddCase(Rdna2ScalarOpcodes);
	AddCase(ScalarExtendedArithmetic);
	AddCase(ScalarArithmeticSccCarryBorrowOverflow);
	AddCase(ScalarMinMaxSccComparisonEdges);
	AddCase(ScalarAbsI32UpdatesScc);
	AddCase(ScalarShiftLeftAddSccCarryEdges);
	AddCase(ScalarCompareOps);
	AddCase(ScalarShiftAddAndMaskOps);
	AddCase(ScalarNotB64UpdatesScc);
	AddCase(ScalarFlbitI32B64Gpu);
	AddCase(ScalarSaveExecOps);
	AddCase(ScalarOrn2SaveexecUsesSourceOrNotExec);
	AddCase(ScalarGetpcWritesNextInstructionPc);
	AddCase(ScalarBitfieldPack);
	AddCase(ScalarBrevB32PreservesScc);
	AddCase(BitfieldExtractWidthPastEndEdges);
	AddCase(Scalar64BitOps);
	AddCase(ScalarAndn2B64SccBranch);
	AddCase(ScalarLiteral);
	AddCase(VectorMoves);
	AddCase(VectorVop3MoveAppliesFloatSourceModifiers);
	AddCase(VectorIntegerOps);
	AddCase(Vop2SdwaSubNcExactByte2Destination);
	AddCase(Vop2SdwaSubNcPreservesByteAndWordDestinations);
	AddCase(Vop2SdwaMinU32PreservesWordDestination);
	AddCase(VectorShiftCountsMaskLowBits);
	AddCase(VectorVop3IntegerOps);
	AddCase(VectorBfeI32ArithmeticShiftMasksField);
	AddCase(VectorCarryAndBitCountOps);
	AddCase(VectorMbcntUsesThreadMask);
	AddCase(VectorAddcWritesPerLaneCarryOut);
	AddCase(VectorAddcUsesPerLaneCarryIn);
	AddCase(VectorVop3BCarryOutWritesSgprMask);
	AddCase(VectorVop3BCarryOutUsesEncodedSdst);
	AddCase(VectorVop3BSubCoU32UsesRdna2Opcode310);
	AddCase(VectorMadU64U32UnsignedCarryOut);
	AddCase(VectorLaneAndPackedOps);
	AddCase(CvtPkU8F32PacksSelectedByte);
	AddCase(CvtPkrtzF16F32SubnormalRoundsTowardZero);
	AddCase(PackedMinMaxF16NanAndSignedZeroEdges);
	AddCase(VectorMinMaxF16Ops);
	AddCase(VectorCvtU16F16Sdwa);
	AddCase(VectorMinMaxMed3F16Ops);
	AddCase(VectorSpecialF16Ops);
	AddCase(VectorWritelaneIgnoresExecMask);
	AddCase(VectorReadlaneFromInactiveWrittenLane);
	AddCase(VectorLaneWave32RuntimeSelectorWraps);
	AddCase(VectorPermlanex16);
	AddCase(VectorPermlane16FetchInactiveZero);
	AddCase(VectorPermlane16FetchInactiveFi);
	AddCase(VectorDppQuadPermuteReverse);
	AddCase(VectorDppBankMaskPreservesDestination);
	AddCase(VectorDppBoundsControlZeroPreservesDestination);
	AddCase(Vop3LdexpSourceModifier);
	AddCase(Vop1MoveRelSource);
	AddCase(Vop1MoveRelDestination);
	AddCase(VectorFloatSpecialOps);
	AddCase(MadMixF16LiteralHalfSourceUsesOpsel);
	AddCase(MadMixF16NegHiIsAbsAndNegIsIndependent);
	AddCase(VectorVop3FmaF16UsesRdna2Opcode34b);
	AddCase(VectorFloatArithmeticOps);
	AddCase(VectorMinMaxF32NanAndSignedZeroEdges);
	AddCase(VectorMed3F32NanUsesMin3Path);
	AddCase(VectorFloatConversionOps);
	AddCase(CvtF32ToIntSaturatesNaNAndOutOfRange);
	AddCase(VectorSpecialF32FlushesDenormalInputs);
	AddCase(VectorSinCosMaxFiniteSpecialCases);
	AddCase(VectorCompareOps);
	AddCase(VectorVop3CompareEqI64OnGpu);
	AddCase(VectorVop3CompareNeU64OnGpu);
	AddCase(VectorCompareClassF32);
	AddCase(VectorCompareF16Ops);
	AddCase(Vop2SdwaCndmaskSourceModifier);
	AddCase(Vop2SdwaCndmaskFullDestinationWithSubDwordSource);
	AddCase(Vop3CndmaskUsesSgprMaskLaneBits);
	AddCase(Vop3CndmaskAllowsDataSourceModifier);
	AddCase(VectorCompareExecOps);
	AddCase(VectorVop3FloatCompareNegSourceModifier);
	AddCase(VectorVop3CmpxWritesExecMask);
	AddCase(VectorVopcSdwaCmpxWritesExecMask);
	AddCase(VectorCompareInvertedMaskSelect);
	AddCase(BranchSelect);
	AddCase(SimpleLoop);
	AddCase(BranchVccnzUsesWholeMask);
	AddCase(BranchVccnzUsesCarryProducedWholeMask);
	AddCase(ScalarMemoryLoadVariants);
	AddCase(ScalarLoadSignedImmediateOffsetAddsSoffset);
	AddCase(ScalarLoadAlignsComponentsAndMasksAddress);
	AddCase(BufferLoadStore);
	AddCase(BufferLoadDwordOffenIdxenUsesVaddrPlusOneOffset);
	AddCase(BufferStoreDwordOffenIdxenUsesVaddrPlusOneOffset);
	AddCase(BufferLoadDwordNoAddressFlagsIgnoresVaddr);
	AddCase(BufferLoadDwordIdxenUsesDescriptorStride);
	AddCase(BufferStoreDwordIdxenUsesDescriptorStride);
	AddCase(BufferStoreDwordAppliesHostOffset);
	AddCase(BufferOffsetsUsePackedLaneAndStorageFallback);
	AddCase(BufferLoadVariants);
	AddCase(BufferLoadDwordx2SnapshotsOverlappingAddress);
	AddCase(BufferLoadDwordx3SnapshotsOverlappingAddress);
	AddCase(BufferLoadDwordx4SnapshotsOverlappingAddress);
	AddCase(BufferStoreVariants);
	AddCase(BufferFormatVariants);
	AddCase(BufferLoadFormatXyzwSnapshotsOverlappingAddress);
	AddCase(BufferLoadFormatXyzwInactiveExecPreservesOverlappingAddress);
	AddCase(BufferFormatStoreVariants);
	AddCase(BufferStoreFormatXResource16UintWritesHalfword);
	AddCase(BufferLoadFormatXResource8UintZeroExtendsByte);
	AddCase(BufferLoadFormatXyResource88UintExtractsBytes);
	AddCase(BufferLoadFormatXyResource8888UnormConvertsFirstTwoComponents);
	AddCase(BufferStoreFormatXyResource88UintWritesBytes);
	AddCase(BufferStoreFormatXyzResource3232UintWritesTwoDwords);
	AddCase(BufferStoreFormatXyzwResource323232UintWritesThreeDwords);
	AddCase(BufferStoreFormatXyResource32UintWritesOneDword);
	AddCase(BufferStoreFormatXyzResource8UintWritesOneByte);
	AddCase(BufferStoreFormatXAddTidUsesLaneIndex);
	AddCase(BufferStoreFormatXDropsOutOfRangeRecord);
	AddCase(TBufferLoadVariants);
	AddCase(TBufferLoadFormatXyzwSnapshotsOverlappingAddress);
	AddCase(TBufferLoadFormatXyzwPackedSnapshotsOverlappingAddress);
	AddCase(TBufferLoadFormatX8UintZeroExtendsByte);
	AddCase(TBufferLoadFormatX8888UintExtractsFirstByte);
	AddCase(TBufferLoadFormatXIdxenUsesDescriptorStride);
	AddCase(TBufferLoadFormatX16FloatConvertsToFloat);
	AddCase(TBufferStoreFormatX8UintWritesOneByte);
	AddCase(TBufferStoreFormatXSintWritesSubDword);
	AddCase(TBufferLoadFormatXSintSignExtendsSubDword);
	AddCase(TBufferLoadFormatXy1616IntegerComponents);
	AddCase(TBufferStoreFormatXy1616IntegerComponents);
	AddCase(TBufferLoadFormatXyz16161616UintLoadsHalfwords);
	AddCase(TBufferLoadFormatXy1616UnormConvertsToFloat);
	AddCase(TBufferLoadFormatXy88UnormConvertsToFloat);
	AddCase(TBufferLoadFormatXy88SnormConvertsToFloat);
	AddCase(TBufferLoadFormatXy8888UnormConvertsFirstTwoComponents);
	AddCase(TBufferLoadFormatXyzw8888UintExtractsBytes);
	AddCase(TBufferLoadFormatXyzw1010102SnormConvertsToFloat);
	AddCase(TBufferLoadFormatXyz111110FloatUnpacks);
	AddCase(TBufferLoadFormatXyzw3232FloatZerosMissingComponents);
	AddCase(TBufferStoreFormatXyzw3232FloatWritesOnlyPresentComponents);
	AddCase(TBufferStoreFormatXy88IntegerComponents);
	AddCase(TBufferLoadFormatXy88IntegerComponents);
	AddCase(TBufferStoreVariants);
	AddCase(FlatLoadVariants);
	AddCase(FlatVirtualAddressRebasesGuestAllocation);
	AddCase(GlobalSignedImmediateRebasesBeforeSaddr);
	AddCase(FlatSegmentIgnoresSaddrAndMasksOffsetMsb);
	AddCase(FlatStoreVariants);
	AddCase(DsReadWriteVariants);
	AddCase(DsAppendConsumeUsesEncodedLdsSelector);
	AddCase(DsAppendUsesEncodedGdsSelector);
	AddCase(DsGdsSubdwordAndAtomicWrites);
	AddCase(DsReadWrite2Variants);
	AddCase(DsAtomicNoReturnVariants);
	AddCase(DsAtomicReturnVariants);
	AddCase(DsMiscVariants);
	AddCase(DsFloatMinMaxUsesSeparateCompareOperand);
	AddCase(DsSwizzleInvalidSourceLaneZero);
	AddCase(BufferAtomicVariants);
	AddCase(BufferAtomicGlc0DoesNotReturnOldValue);
	AddCase(BufferAtomicFMinExactRawGlcModes);
	AddCase(BufferAtomicFMinSpecialValues);
	AddCase(BufferAtomicFMinContendedWorkgroup);
	AddCase(BufferAtomicFMaxExactRawGlcModes);
	AddCase(BufferAtomicFMaxSpecialValues);
	AddCase(BufferAtomicFMaxContendedWorkgroup);
	AddCase(ImageLoadVariants);
	AddCase(ImageLoadR32UintUsesIntegerSampledImage);
	AddCase(ImageLoad1DUsesScalarCoordinate);
	AddCase(ImageLoad1DArrayUsesLayerCoordinate);
	AddCase(ImageLoad1DArrayDescriptorUsesSelectedLayer);
	AddCase(ImageLoadMipUsesVaddr2Lod2D);
	AddCase(ImageLoadMipNsaUsesSelectedAddressVgprs);
	AddCase(ImageLoadA16UintCoordsOnGpu);
	AddCase(ImageGetResinfoDmaskWidthHeight);
	AddCase(ImageGetResinfoDmaskMipLevels);
	AddCase(ImageSampleAndGather);
	AddCase(ImageSampleA16SamplerCoordsOnGpu);
	AddCase(ImageSampleOpcodeAliasUsesNormalCoords);
	AddCase(ImageSampleA16OffsetKeepsTexelOffset32BitOnGpu);
	AddCase(ImageSampleA16CompareBiasRdna2AddressOrder);
	AddCase(ImageGatherCompareOpcodes);
	AddCase(ImageStoreVariants);
	AddCase(ImageStoreRgbOneUsesInverseSwizzle);
	AddCase(ImageStoreDuplicateSelectorUsesInverseSwizzle);
	AddCase(ImageStoreBgraUsesInverseSwizzle);
	AddCase(ImageStoreYzwxUsesInverseSwizzle);
	AddCase(ImageStoreR32FloatUsesFormatlessStorageImage);
	AddCase(ImageStoreR32SintUsesRawUintView);
	AddCase(ImageStoreR32UintUsesUintStorageImage);
	AddCase(ComputeTgSizeSgprUsesWaveMetadata);
	AddCase(ImageAtomicVariants);
	AddCase(ImageAtomicGlc0DoesNotReturnOldValue);
	AddCase(MultipleWorkitemsGlobalId);
	AddCase(DispatcherIrreducibleControlFlow);

	return cases;
}

std::vector<GraphicsCase> MakeGraphicsCases() {
	return {
	    GraphicsInterpolationExport(),
	    GraphicsFlatInterpolatorExport(),
	    GraphicsDsAddtidScratchExport(),
	    GraphicsDirectSgprPushConstantExport(),
	    GraphicsInlineSrtScalarPromotionExport(),
	    GraphicsNullVmExportDiscardsInactiveExec(),
	    GraphicsMrt0OffVmExportDiscardsInactiveExec(),
	    GraphicsFinalVmExportSupersedesEarlierVmMask(),
	    GraphicsBranchPathFinalVmExportDiscardsInactiveExec(),
	};
}

std::vector<SkippedCase> MakeSkippedCases() {
	return {ImageStoreMipWritesExplicitMip2D()};
}

void CheckPs5GameExampleImageClearRuntimeShape() {
	const auto MakeCode = [] {
		std::vector<u32> code;
		AppendVop3(&code, 0x347u, 4, 8, InlineU32(6), Vgpr(0));
		for (u32 i = 0; i < 4; i++) {
			code.push_back(EncodeVop1(0x01u, i, i + 4u));
		}
		code.push_back(EncodeMubuf0(0x07u, 0, true, false));
		code.push_back(EncodeMubuf1(0, 0, 4));
		AppendEnd(&code);
		return code;
	};
	std::array<u32, 8> user_data {};
	user_data[0] = 0x00010000u;
	user_data[1] = 16u << 16u;
	user_data[2] = 64;
	user_data[3] = (static_cast<uint32_t>(Prospero::BufferFormat::k32_32_32_32UInt) << 12u) | 4u |
	               (5u << 3u) | (6u << 6u) | (7u << 9u);
	std::fill(user_data.begin() + 4, user_data.end(), 0xff000000u);
	ShaderComputeInputInfo compute {};
	compute.threads_num[0]             = 64;
	compute.threads_num[1]             = 1;
	compute.threads_num[2]             = 1;
	compute.dispatch_threads_num[0]    = 64;
	compute.dispatch_threads_num[1]    = 1;
	compute.dispatch_threads_num[2]    = 1;
	compute.group_id[0]                = true;
	compute.dispatch_thread_dimensions = true;
	compute.wave_size                  = 32;
	compute.thread_ids_num             = 1;
	compute.workgroup_register         = 8;

	const auto Compile = [&](const char* stage, const std::vector<u32>& code) {
		ShaderRecompiler::CompileOptions options;
		options.stage              = ShaderType::Compute;
		options.wave_size          = 32;
		options.user_data_base     = 0;
		options.user_data_count    = static_cast<u32>(user_data.size());
		options.user_data          = user_data.data();
		options.compute_input_info = &compute;
		options.dump_ir            = false;
		ShaderRecompiler::CompileResult result;
		std::string                     error;
		Require("Ps5GameExampleImageClear", stage,
		        ShaderRecompiler::TryRecompile(code, options, result, &error), error);
		ValidateSpirv("Ps5GameExampleImageClear", result.spirv);
		return result;
	};

	const auto code     = MakeCode();
	auto       positive = Compile("exact Prospero kernel", code);

	compute.stage.program = std::make_shared<const ShaderRecompiler::IR::Program>(positive.program);
	compute.stage.resources =
	    std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(positive.resources);
	ShaderBufferResource descriptor {};
	u32                  packed_clear = 0;
	uint64_t             size         = 0;
	Require("Ps5GameExampleImageClear", "runtime shape",
	        ResolveComputeImageClear(compute, 64, 1, 1, 0x61u, descriptor, packed_clear, size) &&
	            descriptor.Base48() == 0x10000u && size == 64u * 16u && packed_clear == 0xff000000u,
	        "exact Prospero runtime binding did not resolve to a complete clear");

	auto non_repeated = positive.resources;
	non_repeated.user_data[7] ^= 1u;
	compute.stage.resources =
	    std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(non_repeated);
	Require("Ps5GameExampleImageClear", "non-repeated clear",
	        !ResolveComputeImageClear(compute, 64, 1, 1, 0x61u, descriptor, packed_clear, size),
	        "non-uniform uint4 data was replaced with a color clear");
	compute.stage.resources =
	    std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(positive.resources);
	compute.dispatch_threads_num[0] = 32;
	Require("Ps5GameExampleImageClear", "partial dispatch",
	        !ResolveComputeImageClear(compute, 32, 1, 1, 0x61u, descriptor, packed_clear, size),
	        "partial buffer coverage was classified as a complete clear");
	std::printf("[host]    %-32s ok\n", "Ps5GameExampleImageClear");
}

void CheckEmbeddedFetchVertexOffset() {
	const auto MakeFetch = [](std::initializer_list<std::pair<u32, u32>> adds,
	                          std::optional<std::pair<u32, u32>>         late_add = {},
	                          u32 accumulator_vgpr = 0, bool ngg_sad = false) {
		std::vector<u32> code;
		code.push_back(EncodeSMovB32(0, InlineU32(0)));
		code.push_back(EncodeSmem0(0x02u, 20, 4));
		code.push_back(EncodeSmem1(0));
		const auto AppendOffsets = [&]() {
			for (const auto [sgpr, index_vgpr]: adds) {
				if (ngg_sad) {
					AppendVop3(&code, 0x15du, accumulator_vgpr, sgpr, InlineU32(0),
					           Vgpr(index_vgpr));
				} else {
					AppendVop3B(&code, 0x30fu, accumulator_vgpr, 0, sgpr, Vgpr(index_vgpr));
				}
			}
		};
		if (ngg_sad) {
			AppendOffsets();
		}
		code.push_back(EncodeVop2(0x01u, 0, Vgpr(8), 5));
		if (!ngg_sad) {
			AppendOffsets();
		}
		code.push_back(EncodeMubuf0(0x03u, 0, true));
		code.push_back(EncodeMubuf1(9, 5, 0));
		if (late_add.has_value()) {
			AppendVop3B(&code, 0x30fu, 0, 0, late_add->first, Vgpr(late_add->second));
		}
		AppendEnd(&code);
		return code;
	};

	const auto Compile = [&](const char* name, const std::vector<u32>& code, u32 slot10) {
		std::array<u32, 11> user_data {};
		user_data[10] = slot10;
		ShaderVertexInputInfo vertex;
		vertex.fetch_embedded                 = true;
		vertex.fetch_buffer_reg               = 0;
		vertex.fetch_attrib_reg               = 2;
		vertex.resources_num                  = 1;
		vertex.resources_dst[0].attr_id       = 0;
		vertex.resources_dst[0].registers_num = 4;

		ShaderRecompiler::CompileOptions options;
		options.stage             = ShaderType::Vertex;
		options.user_data_base    = 8;
		options.user_data_count   = static_cast<u32>(user_data.size());
		options.user_data         = user_data.data();
		options.vertex_input_info = &vertex;

		ShaderRecompiler::CompileResult result;
		std::string                     error;
		Require(name, "compile", ShaderRecompiler::TryRecompile(code, options, result, &error),
		        error);
		Require(name, "fetch rewrite", vertex.resource_fetch_components[0] == 4,
		        "encoded fetch sequence was not recognized and rewritten");
		return result;
	};

	const auto Resolve = [](const ShaderRecompiler::CompileResult& result, u32 index_offset) {
		ShaderVertexInputInfo vertex;
		vertex.fetch_embedded = true;
		vertex.stage.program =
		    std::make_shared<const ShaderRecompiler::IR::Program>(result.program);
		vertex.stage.resources =
		    std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(result.resources);
		return ResolveVertexOffset(index_offset, vertex);
	};

	const auto valid = Compile("EmbeddedFetchVertexOffset", MakeFetch({{18, 0}}), 7);
	Require("EmbeddedFetchVertexOffset", "parse",
	        valid.program.info.vertex_offset_sgpr == 18 && Resolve(valid, 0) == 7 &&
	            Resolve(valid, 5) == 5,
	        "canonical fetch offset or register index-offset precedence is wrong");

	const auto ngg_code = MakeFetch({{18, 5}}, {}, 5, true);
	Require("EmbeddedFetchNggVertexOffset", "encoding",
	        ngg_code[3] == 0xd55d0005u && ngg_code[4] == 0x04150012u,
	        "test does not encode the PS5 V_SAD_U32 vertex-offset prolog");
	const auto ngg = Compile("EmbeddedFetchNggVertexOffset", ngg_code, 8);
	Require("EmbeddedFetchNggVertexOffset", "parse",
	        ngg.program.info.vertex_offset_sgpr == 18 && Resolve(ngg, 0) == 8 &&
	            Resolve(ngg, 5) == 5,
	        "PS5 NGG vertex-index offset or register index-offset precedence is "
	        "wrong");

	const auto pointer = 0x5b7c5100u;
	const auto late =
	    Compile("EmbeddedFetchLateOffset", MakeFetch({}, std::pair<u32, u32> {18, 0}), pointer);
	const auto conflict =
	    Compile("EmbeddedFetchConflictingOffset", MakeFetch({{17, 0}, {18, 0}}), pointer);
	const auto malformed = Compile("EmbeddedFetchMalformedOffset", MakeFetch({{18, 1}}), pointer);
	const auto outside   = Compile("EmbeddedFetchOutsideOffset", MakeFetch({{19, 0}}), pointer);
	for (const auto* result: {&late, &conflict, &malformed, &outside}) {
		Require("EmbeddedFetchVertexOffset", "fail closed",
		        result->program.info.vertex_offset_sgpr == -1 && Resolve(*result, 0) == 0,
		        "non-prolog, conflicting, malformed, or out-of-window add was "
		        "classified");
	}
	std::printf("[host]    %-32s ok\n", "EmbeddedFetchVertexOffset");
}

[[noreturn]] void RunReverseRenderTargetDeathCase() {
	(void)TextureGetRenderTargetFormat(Prospero::ChannelLayout::k16_16_16_16,
	                                   Prospero::ChannelType::kSrgb,
	                                   Prospero::ChannelOrder::kStandard);
	std::_Exit(0x7f);
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
void CheckRenderTargetFormatContract() {
	const auto r8_uint =
	    TextureGetRenderTargetFormat(Prospero::ChannelLayout::k8,
	                                 Prospero::ChannelType::kUInt,
	                                 Prospero::ChannelOrder::kStandard);
	Require("RenderTargetFormat", "R8 uint",
	        r8_uint.format == vk::Format::eR8Uint && r8_uint.bytes_per_element == 1u &&
	            r8_uint.export_mapping.IsIdentity(),
	        "R8 uint render-target tuple was rejected");

	const auto rgb565 = TextureGetRenderTargetFormat(Prospero::ChannelLayout::k5_6_5,
	                                                 Prospero::ChannelType::kUNorm,
	                                                 Prospero::ChannelOrder::kStandard);
	Require("RenderTargetFormat", "RGB565 UNorm",
	        rgb565.format == vk::Format::eB5G6R5UnormPack16 && rgb565.bytes_per_element == 2u &&
	            rgb565.export_mapping.IsIdentity(),
	        "RGB565 UNorm render-target tuple was rejected");

	const auto uint_format = TextureGetRenderTargetFormat(
	    Prospero::ChannelLayout::k16_16_16_16, Prospero::ChannelType::kUInt,
	    Prospero::ChannelOrder::kStandard);
	Require("RenderTargetFormat", "RGBA16 uint",
	        uint_format.format == vk::Format::eR16G16B16A16Uint &&
	            uint_format.bytes_per_element == 8u && uint_format.export_mapping.IsIdentity(),
	        "RGBA16 uint render-target tuple was rejected");

	const auto format = TextureGetRenderTargetFormat(
	    Prospero::ChannelLayout::k16_16_16_16, Prospero::ChannelType::kFloat,
	    Prospero::ChannelOrder::kReversed);
	Require("ReverseRenderTarget", "exact format",
	        format.format == vk::Format::eR16G16B16A16Sfloat && format.bytes_per_element == 8u &&
	            format.export_mapping == Prospero::ColorMappingAbgr,
	        "exact reverse RGBA16F render-target tuple was rejected");
	Require("ReverseRenderTarget", "write masks",
	        format.export_mapping.ApplyMask(0x1u) == 0x8u &&
	            format.export_mapping.ApplyMask(0x2u) == 0x4u &&
	            format.export_mapping.ApplyMask(0x4u) == 0x2u &&
	            format.export_mapping.ApplyMask(0x8u) == 0x1u &&
	            format.export_mapping.ApplyMask(0xfu) == 0xfu,
	        "reverse RGBA16F component mask was not mapped exactly once");

	const auto rg32 = TextureGetRenderTargetFormat(Prospero::ChannelLayout::k32_32,
	                                               Prospero::ChannelType::kFloat,
	                                               Prospero::ChannelOrder::kReversed);
	Require("ReverseRenderTarget", "GR32 float",
	        rg32.format == vk::Format::eR32G32Sfloat && rg32.bytes_per_element == 8u &&
	            rg32.export_mapping == Prospero::ColorMappingGr &&
	            rg32.export_mapping.ApplyMask(0x1u) == 0x2u &&
	            rg32.export_mapping.ApplyMask(0x2u) == 0x1u &&
	            rg32.export_mapping.ApplyMask(0x3u) == 0x3u &&
	            rg32.export_mapping.ApplyMask(0xfu) == 0xfu,
	        "reverse GR32F render-target export or write-mask mapping is incorrect");

	const auto argb = TextureGetRenderTargetFormat(
	    Prospero::ChannelLayout::k16_16_16_16, Prospero::ChannelType::kFloat,
	    Prospero::ChannelOrder::kAltReversed);
	Require("ReverseRenderTarget", "ARGB float",
	        argb.format == vk::Format::eR16G16B16A16Sfloat &&
	            argb.export_mapping == Prospero::ColorMappingArgb &&
	            argb.export_mapping.ApplyMask(0x1u) == 0x2u &&
	            argb.export_mapping.ApplyMask(0x2u) == 0x4u &&
	            argb.export_mapping.ApplyMask(0x4u) == 0x8u &&
	            argb.export_mapping.ApplyMask(0x8u) == 0x1u,
	        "alternate-reversed render-target mapping did not invert its ARGB cycle");

	char path[MAX_PATH] {};
	Require("ReverseRenderTarget", "host", GetModuleFileNameA(nullptr, path, MAX_PATH) != 0,
	        "GetModuleFileName failed");
	std::string       command = std::string("\"") + path + "\" --reverse-rt-death";
	std::vector<char> mutable_command(command.begin(), command.end());
	mutable_command.push_back('\0');
	STARTUPINFOA        startup {sizeof(startup)};
	PROCESS_INFORMATION process {};
	Require("ReverseRenderTarget", "host",
	        CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
	                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != 0,
	        "CreateProcess failed");
	Require("ReverseRenderTarget", "host",
	        WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0,
	        "unsupported adjacent render-target tuple timed out");
	DWORD      exit_code = 0;
	const bool exited    = GetExitCodeProcess(process.hProcess, &exit_code) != 0;
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	Require("ReverseRenderTarget", "hard failure", exited && exit_code == 321,
	        "invalid render-target tuple did not retain the fatal guard");
	std::printf("[host]    %-32s ok\n", "RenderTargetFormat");
}
#endif

[[noreturn]] void RunImageViewDeathCase(const char* kind) {
	if (std::strcmp(kind, "sampled-invalid-selector") == 0) {
		(void)SelectSampledColorView(vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Unorm,
		                             DstSel(4, 5, 6, 2));
	} else if (std::strcmp(kind, "sampled-incompatible-format") == 0) {
		(void)SelectSampledColorView(vk::Format::eR8Unorm, vk::Format::eR16Unorm,
		                             DstSel(4, 0, 0, 1));
	} else if (std::strcmp(kind, "sampled-invalid-high") == 0) {
		(void)SelectSampledColorView(vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Unorm,
		                             DstSel(7, 6, 5, 3));
	} else if (std::strcmp(kind, "sampled-depth-format") == 0) {
		(void)SelectSampledDepthView(vk::Format::eD24UnormS8Uint, vk::Format::eR32Sfloat,
		                             DstSel(4, 4, 4, 4));
	} else if (std::strcmp(kind, "sampled-depth-swizzle") == 0) {
		(void)SelectSampledDepthView(vk::Format::eD32SfloatS8Uint, vk::Format::eR32Sfloat,
		                             DstSel(4, 5, 6, 7));
	} else if (std::strcmp(kind, "storage-incompatible-format") == 0) {
		ValidateStorageColorView(vk::Format::eR8G8B8A8Srgb, vk::Format::eR16G16B16A16Unorm,
		                         DstSel(4, 5, 6, 7));
	} else if (std::strcmp(kind, "volume-mip-count") == 0 ||
	           std::strcmp(kind, "volume-slice-range") == 0) {
		VulkanHarness    vulkan;
		auto&            graphics = vulkan.RuntimeContext();
		CommandScheduler scheduler(vulkan.RuntimeRenderer(), graphics);
		ImageInfo        volume_info {};
		volume_info.pixel_format    = vk::Format::eR8G8B8A8Unorm;
		volume_info.guest_format    = Prospero::BufferFormat::k8_8_8_8UNorm;
		volume_info.type            = Prospero::ImageType::kColor3D;
		volume_info.extent          = {8, 8, 4};
		volume_info.resources       = {2, 1};
		volume_info.pitch           = 8;
		volume_info.bytes_per_block = 4;
		volume_info.samples         = 1;
		volume_info.tile_mode       = Prospero::TileMode::kLinear;
		Libs::Graphics::Image volume(graphics, scheduler, volume_info);
		ImageViewInfo         view {};
		view.format      = volume_info.pixel_format;
		view.type        = vk::ImageViewType::e2DArray;
		view.aspect      = vk::ImageAspectFlagBits::eColor;
		view.base_level  = 1;
		view.level_count = 1;
		view.layer_count = 1;
		view.usage       = vk::ImageUsageFlagBits::eSampled;
		if (std::strcmp(kind, "volume-mip-count") == 0) {
			view.base_level  = 0;
			view.level_count = 2;
		} else {
			view.base_layer  = 1;
			view.layer_count = 2;
		}
		(void)volume.FindView(view);
	} else {
		ShaderRecompiler::IR::ImageResource resource {};
		resource.kind      = ShaderRecompiler::IR::ResourceKind::StorageImage;
		resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
		resource.written   = true;
		if (std::strcmp(kind, "storage-kind") == 0) {
			resource.kind = ShaderRecompiler::IR::ResourceKind::Image;
		} else if (std::strcmp(kind, "storage-no-write") == 0) {
			resource.written = false;
		} else if (std::strcmp(kind, "storage-nonuint-atomic") == 0) {
			resource.atomic = true;
		} else if (std::strcmp(kind, "storage-compare") == 0) {
			resource.depth_compare = true;
		} else if (std::strcmp(kind, "storage-mip") == 0) {
			resource.mip_mode = ShaderRecompiler::IR::ImageMipMode::DynamicStorage;
		} else if (std::strcmp(kind, "storage-dimension") == 0) {
			resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Unknown;
		} else {
			std::_Exit(0x7e);
		}
		ValidateStorageImageResource(resource);
	}
	std::_Exit(0x7f);
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
void CheckSampledColorViews() {
	Require("SampledColorViews", "identity",
	        SelectSampledColorView(vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Unorm,
	                               DstSel(4, 5, 6, 7)) == DstSel(4, 5, 6, 7),
	        "RGBA did not select the identity view");
	ShaderRecompiler::IR::ImageResource cube_resource {};
	cube_resource.kind      = ShaderRecompiler::IR::ResourceKind::Image;
	cube_resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2DArray;
	cube_resource.read      = true;
	const auto cube_view =
	    ResolveTargetTextureView(cube_resource, Prospero::ImageType::kCube, 0, 6);
	Require("SampledColorViews", "PPSA17337 cubemap render target",
	        cube_view.type == vk::ImageViewType::e2DArray && cube_view.base_layer == 0 &&
	            cube_view.layer_count == 6,
	        "captured six-face cubemap did not resolve to a 2D-array view");
	const auto cube_array_view =
	    ResolveTargetTextureView(cube_resource, Prospero::ImageType::kCube, 6, 18);
	Require("SampledColorViews", "cubemap array subview",
	        cube_array_view.type == vk::ImageViewType::e2DArray &&
	            cube_array_view.base_layer == 6 && cube_array_view.layer_count == 12,
	        "nonzero-base multi-cube view did not preserve whole face groups");
	auto non_array_cube_resource      = cube_resource;
	non_array_cube_resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
	Require(
	    "SampledColorViews", "cubemap hard guards",
	    ResolveTargetTextureView(non_array_cube_resource, Prospero::ImageType::kCube, 0, 6).type ==
	            static_cast<vk::ImageViewType>(VK_IMAGE_VIEW_TYPE_MAX_ENUM) &&
	        ResolveTargetTextureView(cube_resource, Prospero::ImageType::kCube, 0, 7).type ==
	            static_cast<vk::ImageViewType>(VK_IMAGE_VIEW_TYPE_MAX_ENUM) &&
	        ResolveTargetTextureView(cube_resource, Prospero::ImageType::kCube, 1, 6).type ==
	            static_cast<vk::ImageViewType>(VK_IMAGE_VIEW_TYPE_MAX_ENUM) &&
	        ResolveTargetTextureView(cube_resource, Prospero::ImageType::kCube, 6, 6).type ==
	            static_cast<vk::ImageViewType>(VK_IMAGE_VIEW_TYPE_MAX_ENUM),
	    "non-array or partial cubemap views were accepted");
	uint32_t valid_swizzles = 0;
	for (uint32_t swizzle = 0; swizzle <= 0xfffu; swizzle++) {
		bool expected = true;
		for (uint32_t channel = 0; channel < 4; channel++) {
			const auto selector = GetDstSel(swizzle, channel);
			if (selector == 2 || selector == 3) {
				expected = false;
			}
		}
		const bool valid     = IsValidImageSwizzle(swizzle);
		const bool supported = IsSupportedSampledColorView(vk::Format::eR8G8B8A8Unorm,
		                                                   vk::Format::eR8G8B8A8Unorm, swizzle);
		Require("SampledColorViews", "exhaustive read swizzle domain",
		        valid == expected && supported == expected,
		        "sampled swizzle validator disagreed with the PS5 selector domain");
		valid_swizzles += valid;
	}
	Require("SampledColorViews", "all PS5 read swizzles",
	        valid_swizzles == 1296 && !IsValidImageSwizzle(DstSel(4, 5, 6, 2)) &&
	            !IsValidImageSwizzle(DstSel(4, 5, 6, 3)) && !IsValidImageSwizzle(0x1000),
	        "valid PS5 sampled mappings were rejected or reserved selectors were "
	        "admitted");
	const auto arbitrary  = DstSel(5, 1, 7, 0);
	const auto components = TextureGetComponentMapping(arbitrary);
	Require("SampledColorViews", "generic Vulkan component mapping",
	        SelectSampledColorView(vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Unorm,
	                               arbitrary) == arbitrary &&
	            components.r == vk::ComponentSwizzle::eG &&
	            components.g == vk::ComponentSwizzle::eOne &&
	            components.b == vk::ComponentSwizzle::eA &&
	            components.a == vk::ComponentSwizzle::eZero,
	        "arbitrary valid sampled mapping did not use the generic view path");
	Require("SampledColorViews", "R8 R001",
	        SelectSampledColorView(vk::Format::eR8Unorm, vk::Format::eR8Unorm,
	                               DstSel(4, 0, 0, 1)) == DstSel(4, 0, 0, 1),
	        "R8 did not select its R001 view");
	Require("SampledColorViews", "R8 000R",
	        SelectSampledColorView(vk::Format::eR8Unorm, vk::Format::eR8Unorm,
	                               DstSel(0, 0, 0, 4)) == DstSel(0, 0, 0, 4),
	        "R8 did not select its 000R component-mapped view");
	Require("SampledColorViews", "mutable R8 uint/unorm 000R",
	        SelectSampledColorView(vk::Format::eR8Uint, vk::Format::eR8Unorm, DstSel(0, 0, 0, 4)) ==
	            DstSel(0, 0, 0, 4),
	        "compatible R8 integer target sampled view was rejected");
	Require("SampledColorViews", "R16G16 RG01",
	        SelectSampledColorView(vk::Format::eR16G16Sfloat, vk::Format::eR16G16Sfloat,
	                               DstSel(4, 5, 0, 1)) == DstSel(4, 5, 0, 1),
	        "R16G16 did not select its RG01 view");
	Require("SampledColorViews", "alpha one",
	        SelectSampledColorView(vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Unorm,
	                               DstSel(4, 5, 6, 1)) == DstSel(4, 5, 6, 1),
	        "RGB1 did not select the alpha-one view");
	Require("SampledColorViews", "mutable BGRA target",
	        SelectSampledColorView(vk::Format::eB8G8R8A8Unorm, vk::Format::eR8G8B8A8Unorm,
	                               DstSel(6, 5, 4, 7)) == DstSel(6, 5, 4, 7),
	        "BGRA target did not select the exact RGBA/BGRA mutable view");
	Require("SampledColorViews", "mutable sRGB view of UNORM BGRA target",
	        SelectSampledColorView(vk::Format::eB8G8R8A8Unorm, vk::Format::eR8G8B8A8Srgb,
	                               DstSel(6, 5, 4, 7)) == DstSel(6, 5, 4, 7),
	        "UNORM BGRA target did not select its compatible sRGB RGBA sampled view");
	Require("SampledColorViews", "mutable sRGB BGRA target",
	        SelectSampledColorView(vk::Format::eB8G8R8A8Srgb, vk::Format::eR8G8B8A8Srgb,
	                               DstSel(6, 5, 4, 7)) == DstSel(6, 5, 4, 7),
	        "sRGB BGRA target did not select its matching mutable RGBA view");
	constexpr auto colorspace_swizzle = DstSel(5, 1, 7, 0);
	Require("SampledColorViews", "mutable sRGB/UNORM views",
	        SelectSampledColorView(vk::Format::eR8G8B8A8Srgb, vk::Format::eR8G8B8A8Unorm,
	                               DstSel(4, 5, 6, 7)) == DstSel(4, 5, 6, 7) &&
	            SelectSampledColorView(vk::Format::eB8G8R8A8Unorm, vk::Format::eB8G8R8A8Srgb,
	                                   colorspace_swizzle) == colorspace_swizzle,
	        "same-order mutable sRGB/UNORM sampled views were rejected");
	Require("SampledColorViews", "mutable packed RGB10 view",
	        SelectSampledColorView(vk::Format::eA2R10G10B10UnormPack32,
	                               vk::Format::eA2B10G10R10UnormPack32,
	                               DstSel(6, 5, 4, 7)) == DstSel(6, 5, 4, 7),
	        "packed RGB10 target did not select its matching mutable "
	        "channel-order view");
	Require("SampledColorViews", "reverse RGBA16F sampled view",
	        SelectSampledColorView(vk::Format::eR16G16B16A16Sfloat, vk::Format::eR16G16B16A16Sfloat,
	                               DstSel(7, 6, 5, 4)) == DstSel(7, 6, 5, 4),
	        "reverse RGBA16F target did not select its reciprocal ABGR sampled view");
	Require("SampledColorViews", "mutable integer-class views",
	        SelectSampledColorView(vk::Format::eR16G16B16A16Sfloat, vk::Format::eR16G16B16A16Uint,
	                               DstSel(4, 5, 6, 7)) == DstSel(4, 5, 6, 7) &&
	            SelectSampledColorView(vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Uint,
	                                   DstSel(4, 5, 6, 7)) == DstSel(4, 5, 6, 7) &&
	            SelectSampledColorView(vk::Format::eR8G8B8A8Uint, vk::Format::eR8G8B8A8Unorm,
	                                   DstSel(4, 5, 6, 7)) == DstSel(4, 5, 6, 7),
	        "compatible integer render-target sampled view was rejected");
	Require("SampledColorViews", "D32 depth target",
	        SelectSampledDepthView(vk::Format::eD32SfloatS8Uint, vk::Format::eR32Sfloat,
	                               DstSel(4, 4, 4, 4)) == DstSel(4, 4, 4, 4),
	        "D32 depth target did not select its depth-aspect view");
	Require("SampledColorViews", "D16 R000 depth target",
	        SelectSampledDepthView(vk::Format::eD16Unorm, vk::Format::eR16Unorm,
	                               DstSel(4, 0, 0, 0)) == DstSel(4, 0, 0, 0),
	        "D16 depth target did not select its R000 depth-aspect view");
	Require("SampledColorViews", "D16S8 R001 depth target",
	        SelectSampledDepthView(vk::Format::eD16UnormS8Uint, vk::Format::eR16Unorm,
	                               DstSel(4, 0, 0, 1)) == DstSel(4, 0, 0, 1),
	        "D16S8 depth target did not select its R001 depth-aspect view");
	Require("SampledColorViews", "promoted D24S8 R001 depth target",
	        SelectSampledDepthView(vk::Format::eD24UnormS8Uint, vk::Format::eR16Unorm,
	                               DstSel(4, 0, 0, 1)) == DstSel(4, 0, 0, 1),
	        "D24S8 host fallback did not preserve the guest R16 depth view");
	Require("SampledColorViews", "promoted D32S8 R001 depth target",
	        SelectSampledDepthView(vk::Format::eD32SfloatS8Uint, vk::Format::eR16Unorm,
	                               DstSel(4, 0, 0, 1)) == DstSel(4, 0, 0, 1),
	        "D32S8 host fallback did not preserve the guest R16 depth view");
	Require("SampledColorViews", "D32S8 R001 depth target",
	        SelectSampledDepthView(vk::Format::eD32SfloatS8Uint, vk::Format::eR32Sfloat,
	                               DstSel(4, 0, 0, 1)) == DstSel(4, 0, 0, 1),
	        "D32S8 depth target did not select its R001 depth-aspect view");
	ShaderRecompiler::IR::ImageResource storage_resource {};
	storage_resource.kind      = ShaderRecompiler::IR::ResourceKind::StorageImage;
	storage_resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
	storage_resource.written   = true;
	Require("SampledColorViews", "storage resource",
	        IsSupportedStorageImageResource(storage_resource),
	        "exact storage resource contract was rejected");
	storage_resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim3D;
	storage_resource.read      = true;
	Require("SampledColorViews", "read-write 3D storage resource",
	        IsSupportedStorageImageResource(storage_resource),
	        "basic read-write 3D storage resource was rejected");
	storage_resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2DArray;
	storage_resource.read      = false;
	Require("SampledColorViews", "write-only 2D-array storage resource",
	        IsSupportedStorageImageResource(storage_resource),
	        "basic write-only 2D-array storage resource was rejected");
	storage_resource.kind = ShaderRecompiler::IR::ResourceKind::StorageImageUint;
	Require("SampledColorViews", "write-only uint 2D-array storage resource",
	        IsSupportedStorageImageResource(storage_resource),
	        "basic write-only uint 2D-array storage resource was rejected");
	storage_resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
	storage_resource.read      = true;
	storage_resource.atomic    = true;
	Require("SampledColorViews", "atomic uint 2D storage resource",
	        IsSupportedStorageImageResource(storage_resource),
	        "atomic uint storage resource was rejected");

	char path[MAX_PATH] {};
	Require("SampledColorViews", "host", GetModuleFileNameA(nullptr, path, MAX_PATH) != 0,
	        "GetModuleFileName failed");
	for (const char* kind:
	     {"sampled-invalid-selector", "sampled-incompatible-format", "sampled-invalid-high",
	      "sampled-depth-format", "sampled-depth-swizzle", "storage-incompatible-format",
	      "storage-kind", "storage-no-write", "storage-nonuint-atomic", "storage-compare",
	      "storage-mip", "storage-dimension", "volume-mip-count", "volume-slice-range"}) {
		std::string       command = std::string("\"") + path + "\" --image-view-death " + kind;
		std::vector<char> mutable_command(command.begin(), command.end());
		mutable_command.push_back('\0');
		STARTUPINFOA        startup {sizeof(startup)};
		PROCESS_INFORMATION process {};
		Require("SampledColorViews", "host",
		        CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
		                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != 0,
		        "CreateProcess failed");
		Require("SampledColorViews", "host",
		        WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0,
		        "unsupported view death case timed out");
		DWORD      exit_code = 0;
		const bool exited    = GetExitCodeProcess(process.hProcess, &exit_code) != 0;
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		Require("SampledColorViews", "host", exited && exit_code == 321,
		        std::string(kind) + " component mapping did not report a fatal error");
	}
	std::printf("[host]    %-32s ok\n", "SampledColorRenderTargetViews");
}
#endif

void CheckSampledDepthResource() {
	ShaderRecompiler::IR::ImageResource resource {};
	resource.kind          = ShaderRecompiler::IR::ResourceKind::Image;
	resource.dimension     = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
	resource.read          = true;
	resource.depth_compare = true;
	Require("SampledDepthResource", "comparison read", IsSupportedSampledDepthResource(resource),
	        "basic comparison-sampled depth resource was rejected");

	resource.depth_compare = false;
	Require("SampledDepthResource", "ordinary read", IsSupportedSampledDepthResource(resource),
	        "basic non-comparison depth read was rejected");

	const auto basic = resource;
	resource.read    = false;
	Require("SampledDepthResource", "read required", !IsSupportedSampledDepthResource(resource),
	        "non-reading depth resource was accepted");
	resource         = basic;
	resource.written = true;
	Require("SampledDepthResource", "write rejected", !IsSupportedSampledDepthResource(resource),
	        "writable depth resource was accepted");
	resource        = basic;
	resource.atomic = true;
	Require("SampledDepthResource", "atomic rejected", !IsSupportedSampledDepthResource(resource),
	        "atomic depth resource was accepted");
	resource          = basic;
	resource.mip_mode = ShaderRecompiler::IR::ImageMipMode::DynamicStorage;
	Require("SampledDepthResource", "dynamic mip rejected",
	        !IsSupportedSampledDepthResource(resource),
	        "dynamic-storage mip depth resource was accepted");
	resource           = basic;
	resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2DArray;
	Require("SampledDepthResource", "singleton array accepted",
	        IsSupportedSampledDepthResource(resource), "array depth resource was rejected");
	const auto singleton_array_view =
	    ResolveTargetTextureView(resource, Prospero::ImageType::kColor2DArray, 0, 1);
	Require("SampledDepthResource", "singleton array view",
	        singleton_array_view.type == vk::ImageViewType::e2DArray &&
	            singleton_array_view.base_layer == 0 && singleton_array_view.layer_count == 1,
	        "singleton depth array did not preserve the shader array view type");
	Require("SampledDepthResource", "array type mismatch rejected",
	        ResolveTargetTextureView(resource, Prospero::ImageType::kColor2D, 0, 1).type ==
	            static_cast<vk::ImageViewType>(VK_IMAGE_VIEW_TYPE_MAX_ENUM),
	        "array shader resource accepted a non-array descriptor view");
	resource      = basic;
	resource.kind = ShaderRecompiler::IR::ResourceKind::ImageUint;
	Require("SampledDepthResource", "integer rejected", !IsSupportedSampledDepthResource(resource),
	        "integer depth resource was accepted");
	Require("SampledDepthResource", "integer reinterpret resource",
	        IsSupportedSampledDepthUintResource(resource),
	        "read-only uint depth reinterpretation resource was rejected");
	std::printf("[host]    %-32s ok\n", "SampledDepthResource");
}

void CheckSampledVideoOutView(RenderContext& renderer) {
	auto&                               context = renderer.GetGraphics();
	CommandScheduler                    scheduler(renderer, context);
	ShaderRecompiler::IR::ImageResource resource {};
	resource.kind      = ShaderRecompiler::IR::ResourceKind::Image;
	resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
	resource.read      = true;

	ShaderTextureResource descriptor {};
	descriptor.fields[3] = static_cast<uint32_t>(Prospero::ImageType::kColor2D) << 28u;
	ImageInfo info {};
	info.pixel_format    = vk::Format::eR8G8B8A8Unorm;
	info.guest_format    = Prospero::BufferFormat::k8_8_8_8UNorm;
	info.type            = Prospero::ImageType::kColor2D;
	info.extent          = {1, 1, 1};
	info.resources       = {1, 1};
	info.pitch           = 1;
	info.bytes_per_block = 4;
	info.samples         = 1;
	info.tile_mode       = Prospero::TileMode::kLinear;
	info.mip_layout[0]   = {0, 4, 1, 1};
	Image image(context, scheduler, info);
	image.usage.video_out = true;
	Require("SampledVideoOutView", "basic 2D",
	        IsSupportedSampledVideoOutView(resource, descriptor, image),
	        "basic 2D video-out view was rejected");

	const auto basic_resource = resource;
	resource.dimension        = ShaderRecompiler::Decoder::ImageDimension::Dim2DArray;
	const bool rejects_array_resource =
	    !IsSupportedSampledVideoOutView(resource, descriptor, image);
	resource             = basic_resource;
	descriptor.fields[3] = static_cast<uint32_t>(Prospero::ImageType::kColor2DArray) << 28u;
	const bool rejects_array_descriptor =
	    !IsSupportedSampledVideoOutView(resource, descriptor, image);
	descriptor.fields[3]             = static_cast<uint32_t>(Prospero::ImageType::kColor2D) << 28u;
	descriptor.fields[4]             = 1u << 16u;
	const bool rejects_base_layer    = !IsSupportedSampledVideoOutView(resource, descriptor, image);
	descriptor.fields[4]             = 1u;
	const bool rejects_layer_count   = !IsSupportedSampledVideoOutView(resource, descriptor, image);
	descriptor.fields[4]             = 0;
	image.info.resources.layers      = 2;
	const bool rejects_layered_image = !IsSupportedSampledVideoOutView(resource, descriptor, image);
	Require("SampledVideoOutView", "array hard failures",
	        rejects_array_resource && rejects_array_descriptor && rejects_base_layer &&
	            rejects_layer_count && rejects_layered_image,
	        "unsupported layered video-out view was accepted");
	std::printf("[host]    %-32s ok\n", "SampledVideoOutView");
}

void CheckImageTransitionState(RenderContext& renderer) {
	constexpr const char* name    = "ImageTransitionState";
	auto&                 context = renderer.GetGraphics();
	CommandScheduler      scheduler(renderer, context);
	const auto            MakeInfo = [](vk::Format format, uint32_t levels, uint32_t layers) {
		ImageInfo info {};
		info.pixel_format    = format;
		info.guest_format    = Prospero::BufferFormat::k8UNorm;
		info.type            = Prospero::ImageType::kColor2D;
		info.extent          = {4, 4, 1};
		info.resources       = {levels, layers};
		info.pitch           = 4;
		info.bytes_per_block = 1;
		info.samples         = 1;
		info.tile_mode       = Prospero::TileMode::kLinear;
		return info;
	};
	const auto graphics_stage =
	    vk::PipelineStageFlagBits2::eAllGraphics | vk::PipelineStageFlagBits2::eComputeShader;
	constexpr uint64_t copy_capacity = 128ull << 20;
	Require(name, "buffered-copy capacity bands",
	        ImageTestAccess::CopyRows(32ull << 10, 4096, copy_capacity) == 4096 &&
	            ImageTestAccess::CopyRows(32ull << 10, 4097, copy_capacity) == 4096 &&
	            ImageTestAccess::CopyRows(copy_capacity + 4, 1, copy_capacity) == 0,
	        "buffered image copy does not split at the fixed scratch capacity");

	Image image(context, scheduler, MakeInfo(vk::Format::eR8Unorm, 2, 3));
	auto  barriers = image.GetBarriers(vk::ImageLayout::eShaderReadOnlyOptimal,
	                                   vk::AccessFlagBits2::eShaderRead, graphics_stage, {});
	Require(name, "initial full state",
	        barriers.size() == 1 &&
	            barriers[0].srcStageMask == vk::PipelineStageFlagBits2::eAllCommands &&
	            barriers[0].srcAccessMask == vk::AccessFlagBits2::eNone &&
	            barriers[0].dstStageMask == graphics_stage &&
	            barriers[0].dstAccessMask == vk::AccessFlagBits2::eShaderRead &&
	            barriers[0].oldLayout == vk::ImageLayout::eUndefined &&
	            barriers[0].newLayout == vk::ImageLayout::eShaderReadOnlyOptimal &&
	            barriers[0].subresourceRange.aspectMask == vk::ImageAspectFlagBits::eColor &&
	            barriers[0].subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS &&
	            barriers[0].subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS &&
	            image.backing.state.layout == vk::ImageLayout::eShaderReadOnlyOptimal &&
	            image.backing.state.access_mask == vk::AccessFlagBits2::eShaderRead &&
	            image.backing.state.pl_stage == graphics_stage,
	        "initial transition did not record or retain pinned state");
	Require(name, "repeat read elision",
	        image
	            .GetBarriers(vk::ImageLayout::eShaderReadOnlyOptimal,
	                         vk::AccessFlagBits2::eShaderRead, graphics_stage, {})
	            .empty(),
	        "identical read-only transition emitted a barrier");
	barriers = image.GetBarriers(
	    vk::ImageLayout::eShaderReadOnlyOptimal,
	    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, graphics_stage, {});
	Require(name, "access-only transition",
	        barriers.size() == 1 && barriers[0].oldLayout == barriers[0].newLayout &&
	            barriers[0].srcAccessMask == vk::AccessFlagBits2::eShaderRead,
	        "same-layout access change was lost");

	const auto CheckRepeatedWrite = [&](vk::AccessFlagBits2 access, const char* label) {
		image.backing.state = {};
		image.backing.subresource_states.clear();
		(void)image.GetBarriers(vk::ImageLayout::eGeneral, access, graphics_stage, {});
		Require(name, label,
		        image.GetBarriers(vk::ImageLayout::eGeneral, access, graphics_stage, {}).size() ==
		            1,
		        "pinned repeated-write dependency was elided");
	};
	CheckRepeatedWrite(vk::AccessFlagBits2::eTransferWrite, "repeat transfer write");
	CheckRepeatedWrite(vk::AccessFlagBits2::eShaderWrite, "repeat shader write");
	CheckRepeatedWrite(vk::AccessFlagBits2::eMemoryWrite, "repeat memory write");
	image.backing.state = {};
	image.backing.subresource_states.clear();
	const auto attachment_access =
	    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
	(void)image.GetBarriers(vk::ImageLayout::eColorAttachmentOptimal, attachment_access,
	                        graphics_stage, {});
	Require(name, "pinned attachment-write quirk",
	        image
	            .GetBarriers(vk::ImageLayout::eColorAttachmentOptimal, attachment_access,
	                         graphics_stage, {})
	            .empty(),
	        "attachment writes diverged from the pinned repeated-write set");

	image.backing.state = {};
	image.backing.subresource_states.clear();
	(void)image.GetBarriers(vk::ImageLayout::eShaderReadOnlyOptimal,
	                        vk::AccessFlagBits2::eShaderRead, graphics_stage, {});
	constexpr ImageSubresourceRange mip1_layer0 {1, 1, 0, 1};
	barriers = image.GetBarriers(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderWrite,
	                             graphics_stage, mip1_layer0);
	Require(
	    name, "first partial",
	    barriers.size() == 1 && barriers[0].subresourceRange.baseMipLevel == 1 &&
	        barriers[0].subresourceRange.baseArrayLayer == 0 &&
	        image.backing.subresource_states.size() == 6 &&
	        image.backing.subresource_states[3].layout == vk::ImageLayout::eGeneral &&
	        image.backing.subresource_states[3].access_mask == vk::AccessFlagBits2::eShaderWrite &&
	        image.backing.subresource_states[1].layout == vk::ImageLayout::eShaderReadOnlyOptimal,
	    "partial state did not use mip-major indexing");
	constexpr ImageSubresourceRange mip0_layer1 {0, 1, 1, 1};
	barriers =
	    image.GetBarriers(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
	                      vk::PipelineStageFlagBits2::eTransfer, mip0_layer1);
	Require(name, "second partial",
	        barriers.size() == 1 && barriers[0].subresourceRange.baseMipLevel == 0 &&
	            barriers[0].subresourceRange.baseArrayLayer == 1 &&
	            image.backing.subresource_states[1].layout ==
	                vk::ImageLayout::eTransferDstOptimal &&
	            image.backing.subresource_states[1].access_mask ==
	                vk::AccessFlagBits2::eTransferWrite &&
	            image.backing.subresource_states[3].layout == vk::ImageLayout::eGeneral,
	        "second partial transition changed the wrong cell");
	const ImageSubresourceRange explicit_full {0, 2, 0, 3};
	barriers = image.GetBarriers(vk::ImageLayout::eShaderReadOnlyOptimal,
	                             vk::AccessFlagBits2::eShaderRead, graphics_stage, explicit_full);
	bool saw_mip0_layer1 = false;
	bool saw_mip1_layer0 = false;
	for (const auto& barrier: barriers) {
		saw_mip0_layer1 |= barrier.subresourceRange.baseMipLevel == 0 &&
		                   barrier.subresourceRange.baseArrayLayer == 1 &&
		                   barrier.oldLayout == vk::ImageLayout::eTransferDstOptimal &&
		                   barrier.srcStageMask == vk::PipelineStageFlagBits2::eTransfer;
		saw_mip1_layer0 |= barrier.subresourceRange.baseMipLevel == 1 &&
		                   barrier.subresourceRange.baseArrayLayer == 0 &&
		                   barrier.oldLayout == vk::ImageLayout::eGeneral &&
		                   barrier.srcAccessMask == vk::AccessFlagBits2::eShaderWrite;
	}
	Require(name, "partial normalization",
	        barriers.size() == 2 && saw_mip0_layer1 && saw_mip1_layer0 &&
	            image.backing.subresource_states.empty() &&
	            image.backing.state.layout == vk::ImageLayout::eShaderReadOnlyOptimal &&
	            image.backing.state.access_mask == vk::AccessFlagBits2::eShaderRead &&
	            image.backing.state.pl_stage == graphics_stage,
	        "full transition did not reconcile and collapse split state");
	barriers =
	    image.GetBarriers(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
	                      vk::PipelineStageFlagBits2::eTransfer, {});
	Require(name, "normalized full transition",
	        barriers.size() == 1 &&
	            barriers[0].subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS &&
	            barriers[0].subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS &&
	            barriers[0].srcStageMask == graphics_stage &&
	            barriers[0].dstStageMask == vk::PipelineStageFlagBits2::eTransfer,
	        "normalized state did not return to one full barrier");

	auto depth_info            = MakeInfo(vk::Format::eD32SfloatS8Uint, 1, 1);
	depth_info.guest_format    = Prospero::BufferFormat::k32Float;
	depth_info.bytes_per_block = 4;
	depth_info.tile_mode       = Prospero::TileMode::kDepth;
	Image depth(context, scheduler, depth_info);
	barriers = depth.GetBarriers(vk::ImageLayout::eDepthStencilReadOnlyOptimal,
	                             vk::AccessFlagBits2::eShaderRead, graphics_stage, {});
	Require(name, "depth stencil aspects",
	        barriers.size() == 1 &&
	            barriers[0].subresourceRange.aspectMask ==
	                (vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil),
	        "depth/stencil transition did not use the full image aspect");

	HW::Context    registers {};
	HW::UserConfig user_config {};
	HW::Shader     shaders {};
	scheduler.Begin(registers, user_config, shaders);
	image.backing.state = {};
	image.backing.subresource_states.clear();
	image.Transit(vk::ImageLayout::eGeneral,
	              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {},
	              scheduler.Current().Handle());
	Require(name, "mixed access stages",
	        image.backing.state.pl_stage ==
	            (graphics_stage | vk::PipelineStageFlagBits2::eTransfer),
	        "mixed shader/transfer access omitted a required pipeline stage");

	auto source_info                  = depth_info;
	source_info.extent                = {2, 2, 1};
	source_info.pitch                 = 2;
	source_info.stencil               = {0x1000, 4};
	auto destination_info             = depth_info;
	destination_info.resources.levels = 2;
	destination_info.stencil          = {0x2000, 16};
	Image        source(context, scheduler, source_info);
	Image        destination(context, scheduler, destination_info);
	StreamBuffer upload(context, scheduler, MemoryUsage::Upload, 4096);
	StreamBuffer download(context, scheduler, MemoryUsage::Download, 4096);
	const auto [upload_data, upload_offset] = upload.Map(128, 4);
	Require(name, "depth/stencil upload map", upload_data != nullptr,
	        "combined depth/stencil upload allocation failed");
	constexpr std::array<uint32_t, 4> expected_depth {0x3e800000u, 0x3f000000u, 0x3f400000u,
	                                                  0x3f800000u};
	constexpr std::array<uint8_t, 4>  expected_stencil {0x12, 0x34, 0x56, 0x78};
	std::memcpy(upload_data, expected_depth.data(), sizeof(expected_depth));
	std::memcpy(upload_data + 64, expected_stencil.data(), sizeof(expected_stencil));
	upload.Commit();
	std::array<vk::BufferImageCopy, 2> source_copies {};
	source_copies[0].bufferOffset     = upload_offset;
	source_copies[0].imageSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1};
	source_copies[0].imageExtent      = {2, 2, 1};
	source_copies[1].bufferOffset     = upload_offset + 64;
	source_copies[1].imageSubresource = {vk::ImageAspectFlagBits::eStencil, 0, 0, 1};
	source_copies[1].imageExtent      = {2, 2, 1};
	source.Upload(source_copies, upload.Handle(), upload_offset, 128);
	destination.CopyMip(source, 1, 0);

	const auto [download_data, download_offset] = download.Map(128, 4);
	Require(name, "depth/stencil download map", download_data != nullptr,
	        "combined depth/stencil download allocation failed");
	download.Commit();
	auto destination_copies                         = source_copies;
	destination_copies[0].bufferOffset              = download_offset;
	destination_copies[0].imageSubresource.mipLevel = 1;
	destination_copies[1].bufferOffset              = download_offset + 64;
	destination_copies[1].imageSubresource.mipLevel = 1;
	destination.Download(destination_copies, download.Handle(), download_offset, 128);

	auto buffered_info          = MakeInfo(vk::Format::eR8Unorm, 1, 1);
	buffered_info.extent        = {8, 4, 1};
	buffered_info.pitch         = 8;
	buffered_info.mip_layout[0] = {0, 32, 8, 4};
	Image                             buffered_source(context, scheduler, buffered_info);
	Image                             buffered_destination(context, scheduler, buffered_info);
	constexpr std::array<uint8_t, 32> buffered_expected {
	    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa,
	    0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x10, 0x21, 0x32, 0x43, 0x54, 0x65,
	    0x76, 0x87, 0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f};
	const auto [buffered_upload_data, buffered_upload_offset] =
	    upload.Map(buffered_expected.size(), 4);
	Require(name, "buffered-copy upload map", buffered_upload_data != nullptr,
	        "buffered-copy source allocation failed");
	std::memcpy(buffered_upload_data, buffered_expected.data(), buffered_expected.size());
	upload.Commit();
	vk::BufferImageCopy buffered_region {};
	buffered_region.bufferOffset      = buffered_upload_offset;
	buffered_region.bufferRowLength   = buffered_info.pitch;
	buffered_region.bufferImageHeight = buffered_info.extent.height;
	buffered_region.imageSubresource  = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	buffered_region.imageExtent       = buffered_info.extent;
	buffered_source.Upload(std::span {&buffered_region, 1}, upload.Handle(), buffered_upload_offset,
	                       buffered_expected.size());
	Buffer copy_scratch(context, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
	buffered_destination.CopyImageWithBuffer(buffered_source, copy_scratch);
	const auto [buffered_download_data, buffered_download_offset] =
	    download.Map(buffered_expected.size(), 4);
	Require(name, "buffered-copy download map", buffered_download_data != nullptr,
	        "buffered-copy destination allocation failed");
	download.Commit();
	buffered_region.bufferOffset = buffered_download_offset;
	buffered_destination.Download(std::span {&buffered_region, 1}, download.Handle(),
	                              buffered_download_offset, buffered_expected.size());
	scheduler.Finish();
	download.Invalidate(download_offset, 128);
	download.Invalidate(buffered_download_offset, buffered_expected.size());
	Require(
	    name, "depth/stencil mip contents",
	    std::memcmp(download_data, expected_depth.data(), sizeof(expected_depth)) == 0 &&
	        std::memcmp(download_data + 64, expected_stencil.data(), sizeof(expected_stencil)) == 0,
	    "combined depth/stencil mip copy lost an aspect");
	Require(name, "buffered-copy chunk contents",
	        std::memcmp(buffered_download_data, buffered_expected.data(),
	                    buffered_expected.size()) == 0,
	        "real buffered image copy lost a row across its scratch boundary");

	std::printf("[host]    %-32s ok\n", name);
}

void CheckSampledDepthDescriptor(RenderContext& renderer) {
	auto&            context = renderer.GetGraphics();
	CommandScheduler scheduler(renderer, context);
	const auto make_info = [](uint32_t width, uint32_t height, uint32_t pitch, uint32_t layers,
	                          vk::Format format, Prospero::ImageType type, uint32_t samples = 1) {
		ImageInfo info {};
		info.pixel_format    = format;
		info.guest_format    = Prospero::BufferFormat::k32Float;
		info.type            = type;
		info.extent          = {width, height, 1};
		info.resources       = {1, layers};
		info.pitch           = pitch;
		info.bytes_per_block = 4;
		info.samples         = samples;
		info.tile_mode       = Prospero::TileMode::kDepth;
		info.mip_layout[0] = {0, static_cast<uint64_t>(pitch) * height * layers * 4, pitch, height};
		return info;
	};

	ShaderTextureResource descriptor {{0x00eb0900u, 0xc1600000u, 0x00bcc14fu, 0x91800924u,
	                                   0x00000000u, 0x00700000u, 0x00000000u, 0x00000000u}};
	Image                 image(
	    context, scheduler,
	    make_info(1344, 756, 1408, 1, vk::Format::eD32SfloatS8Uint, Prospero::ImageType::kColor2D));
	image.usage.depth_target = true;
	Require("SampledDepthDescriptor", "normalized padded pitch",
	        descriptor.Width5() + 1u == image.info.extent.width &&
	            descriptor.Height5() + 1u == image.info.extent.height &&
	            IsSupportedDepthTargetDescriptor(descriptor, image),
	        "normalized depth image rejected a valid padded descriptor");

	const ShaderTextureResource disabled_sampler_tweaks {{
	    0x05135600u,
	    0xc1600000u,
	    0x010dc1dfu,
	    0x91800924u,
	    0x00000000u,
	    0x00000000u,
	    0x00000000u,
	    0x00000000u,
	}};
	Require("SampledDepthDescriptor", "disabled sampler tweaks",
	        disabled_sampler_tweaks.PerfMod5() == 0 &&
	            IsSupportedDepthTextureEncoding(disabled_sampler_tweaks, image),
	        "valid sampler modulation factor zero was rejected");

	const ShaderTextureResource uncompressed_msaa {{
	    0x00705d00u,
	    0xc1600000u,
	    0x010dc1dfu,
	    0xe1810924u,
	    0x00000000u,
	    0x00700010u,
	    0x00000000u,
	    0x00000000u,
	}};
	auto                        msaa_info =
	    make_info(1920, 1080, 1920, 1, vk::Format::eD32Sfloat, Prospero::ImageType::kColor2D, 2);
	msaa_info.mip_layout[0] = {0, 0x010e0000, 1920, 1152};
	Image msaa_image(context, scheduler, msaa_info);
	msaa_image.usage.depth_target = true;
	Require("SampledDepthDescriptor", "uncompressed 2x MSAA depth",
	        IsSupportedDepthTargetDescriptor(uncompressed_msaa, msaa_image) &&
	            IsSupportedDepthTextureEncoding(uncompressed_msaa, msaa_image),
	        "valid uncompressed MSAA depth descriptor required an HTILE "
	        "compatibility flag");

	descriptor.fields[3] = (descriptor.fields[3] & ~(0xfu << 28u)) |
	                       (static_cast<uint32_t>(Prospero::ImageType::kColor2DArray) << 28u);
	Require("SampledDepthDescriptor", "singleton array descriptor",
	        IsSupportedDepthTargetDescriptor(descriptor, image),
	        "normalized singleton-array depth view was rejected");
	descriptor.fields[3] = (descriptor.fields[3] & ~(0xfu << 28u)) |
	                       (static_cast<uint32_t>(Prospero::ImageType::kColor2D) << 28u);

	ShaderTextureResource cube_descriptor {{0x01267d00u, 0xc0700000u, 0x00ffc0ffu, 0xb1800924u,
	                                        0x00000005u, 0x00700000u, 0x00000000u, 0x00000000u}};
	Image                 cube_image(
	    context, scheduler,
	    make_info(1024, 1024, 1024, 6, vk::Format::eD32Sfloat, Prospero::ImageType::kColor2D));
	cube_image.usage.depth_target = true;
	ShaderRecompiler::IR::ImageResource cube_resource {};
	cube_resource.kind          = ShaderRecompiler::IR::ResourceKind::Image;
	cube_resource.dimension     = ShaderRecompiler::Decoder::ImageDimension::Dim2DArray;
	cube_resource.read          = true;
	cube_resource.depth_compare = true;
	const auto cube_view = ResolveTargetTextureView(cube_resource, Prospero::ImageType::kCube, 0,
	                                                cube_image.info.resources.layers);
	Require("SampledDepthDescriptor", "normalized depth cube",
	        IsSupportedDepthTargetDescriptor(cube_descriptor, cube_image) &&
	            IsSupportedDepthTextureEncoding(cube_descriptor, cube_image) &&
	            cube_view.type == vk::ImageViewType::e2DArray && cube_view.layer_count == 6,
	        "normalized depth cube did not preserve its six-face view");

	constexpr uint64_t          captured_htile_address = 0x106d48000ull;
	const ShaderTextureResource compressed_descriptor {{
	    0x0104d500u,
	    0xc1600000u,
	    0x021bc3bfu,
	    0x91800924u,
	    0x00000000u,
	    0x00700000u,
	    0x80280000u,
	    0x000106d4u,
	}};
	image.info.metadata.range = {captured_htile_address, 0x1000};
	image.info.metadata.kind  = ImageMetadataKind::Htile;
	auto mismatched_metadata  = compressed_descriptor;
	mismatched_metadata.fields[7] ^= 1u;
	auto mismatched_metadata_low = compressed_descriptor;
	mismatched_metadata_low.fields[6] ^= 1u << 24u;
	auto dcc_only_control = compressed_descriptor;
	dcc_only_control.fields[6] |= 1u << 22u;
	const bool accepts_compressed  = IsSupportedDepthTextureEncoding(compressed_descriptor, image);
	image.info.metadata.kind       = ImageMetadataKind::Dcc;
	const bool rejects_dcc         = !IsSupportedDepthTextureEncoding(compressed_descriptor, image);
	image.info.metadata.kind       = ImageMetadataKind::None;
	const bool rejects_none        = !IsSupportedDepthTextureEncoding(compressed_descriptor, image);
	image.info.metadata.kind       = ImageMetadataKind::Htile;
	image.info.metadata.range.size = 0;
	const bool rejects_empty       = !IsSupportedDepthTextureEncoding(compressed_descriptor, image);
	image.info.metadata.range.size = TRACKER_ADDRESS_SIZE - captured_htile_address + 1u;
	const bool rejects_overflow    = !IsSupportedDepthTextureEncoding(compressed_descriptor, image);
	image.info.metadata.range.size = 0x1000;
	Require("SampledDepthDescriptor", "compressed HTILE descriptor",
	        accepts_compressed && !IsSupportedDepthTextureEncoding(mismatched_metadata, image) &&
	            !IsSupportedDepthTextureEncoding(mismatched_metadata_low, image) &&
	            !IsSupportedDepthTextureEncoding(dcc_only_control, image),
	        "compressed sampled depth did not require its exact tracked HTILE");
	Require("SampledDepthDescriptor", "tracked HTILE state",
	        rejects_dcc && rejects_none && rejects_empty && rejects_overflow,
	        "compressed sampled depth accepted invalid tracked metadata");

	auto partial_cube      = cube_descriptor;
	partial_cube.fields[4] = 4;
	auto based_cube        = cube_descriptor;
	based_cube.fields[4] |= 1u << 16u;
	auto reserved_cube = cube_descriptor;
	reserved_cube.fields[4] |= 1u << 13u;
	auto non_square_cube      = cube_descriptor;
	non_square_cube.fields[2] = (non_square_cube.fields[2] & ~(0x3fffu << 14u)) | (511u << 14u);
	image.info.pitch          = 1344;
	const bool rejects_pitch  = !IsSupportedDepthTargetDescriptor(descriptor, image);
	image.info.pitch          = 1408;
	Require("SampledDepthDescriptor", "normalized hard guards",
	        rejects_pitch && !IsSupportedDepthTargetDescriptor(partial_cube, cube_image) &&
	            !IsSupportedDepthTargetDescriptor(based_cube, cube_image) &&
	            !IsSupportedDepthTextureEncoding(reserved_cube, cube_image) &&
	            !IsSupportedDepthTargetDescriptor(non_square_cube, cube_image),
	        "normalized depth descriptor accepted an incompatible image view");
	std::printf("[host]    %-32s ok\n", "SampledDepthDescriptor");
}

ShaderRecompiler::IR::ImageResource BasicStorageTextureResource() {
	ShaderRecompiler::IR::ImageResource resource {};
	resource.kind      = ShaderRecompiler::IR::ResourceKind::StorageImage;
	resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim3D;
	resource.read      = true;
	resource.written   = true;
	return resource;
}

ShaderTextureResource BasicStorageTextureDescriptor() {
	return {{0x00785d00u, 0x04700000u, 0x00080008u, 0xa1b00facu, 0x00000020u, 0x00700000u,
	         0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource BasicLinearStorageTextureResource() {
	auto resource      = BasicStorageTextureResource();
	resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
	resource.read      = false;
	return resource;
}

ShaderTextureResource BasicLinearStorageTextureDescriptor() {
	return {{0x04bcc401u, 0xc3800000u, 0x021bc3bfu, 0x900003acu, 0x00000000u, 0x00700000u,
	         0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource BasicBgraStorageTextureResource() {
	auto resource      = BasicStorageTextureResource();
	resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
	resource.read      = false;
	return resource;
}

ShaderTextureResource BasicBgraStorageTextureDescriptor() {
	return {{0x007c6500u, 0xc3800000u, 0x010dc1dfu, 0x91b00f2eu, 0x00000000u, 0x00700000u,
	         0x00000000u, 0x00000000u}};
}

ShaderTextureResource Ppsa06228R11G11B10StorageTextureDescriptor() {
	return {{0x10c6b500u, 0xc2400000u, 0x010dc1dfu, 0x91b003acu, 0x00000000u, 0x00700000u,
	         0x00000000u, 0x00000000u}};
}

ShaderTextureResource Ppsa01530MaxMipStorageTextureDescriptor() {
	return {{0x04a42900u, 0xc3e00000u, 0x000fc00fu, 0x91b0022cu, 0x00000000u, 0x00700050u,
	         0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource Ppsa01530MaxMipStorageTextureResource() {
	auto resource = BasicBgraStorageTextureResource();
	resource.kind = ShaderRecompiler::IR::ResourceKind::StorageImageUint;
	return resource;
}

ShaderTextureResource Ppsa02527R16FloatStorageTextureDescriptor() {
	return {{0x00ce3500u, 0xc0d00000u, 0x010dc1dfu, 0x91b00204u, 0x00000000u, 0x00700000u,
	         0x00000000u, 0x00000000u}};
}

ShaderTextureResource Ppsa02527R32FloatStorageTextureDescriptor() {
	return {{0x00cea900u, 0xc1600000u, 0x0086c0efu, 0x91b00204u, 0x00000000u, 0x00700000u,
	         0x00000000u, 0x00000000u}};
}

ShaderTextureResource Ppsa02527R8UnormStorageTextureDescriptor() {
	return {{0x00c7d500u, 0xc0100000u, 0x0086c0efu, 0x91b00204u, 0x00000000u, 0x00700000u,
	         0x00000000u, 0x00000000u}};
}

ShaderTextureResource BasicYzwxStorageTextureDescriptor() {
	return {{0x00627801u, 0xc4d00000u, 0x0001c001u, 0x900009f5u, 0x00000000u, 0x00700000u,
	         0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource BasicArrayStorageTextureResource() {
	auto resource      = BasicLinearStorageTextureResource();
	resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2DArray;
	return resource;
}

ShaderTextureResource BasicArrayStorageTextureDescriptor() {
	return {{0x20179000u, 0x03800000u, 0x00000000u, 0xd1b00f2eu, 0x00000000u, 0x00700000u,
	         0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource BasicUintArrayStorageTextureResource() {
	auto resource = BasicArrayStorageTextureResource();
	resource.kind = ShaderRecompiler::IR::ResourceKind::StorageImageUint;
	return resource;
}

ShaderTextureResource BasicUintArrayStorageTextureDescriptor() {
	return {{0x20179200u, 0x01400000u, 0x00000000u, 0xd1b00204u, 0x00000000u, 0x00700000u,
	         0x00000000u, 0x00000000u}};
}

ShaderTextureResource Standard4KBUintArrayStorageTextureDescriptor() {
	return {{0x006c6600u, 0x01400000u, 0x00000000u, 0xd0500204u, 0x00000000u, 0x00700000u,
	         0x00000000u, 0x00000000u}};
}

ShaderTextureResource Standard64KBStorageTextureDescriptor() {
	return {{0x011fab00u, 0xc3800000u, 0x0003c003u, 0xd0900facu, 0x00000000u, 0x00700000u,
	         0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource Ppsa14053DepthTileStorageTextureResource() {
	return BasicUintArrayStorageTextureResource();
}

ShaderTextureResource Ppsa14053DepthTileStorageTextureDescriptor() {
	return {{0x20144c00u, 0x00500000u, 0x00000000u, 0xd1800204u, 0x00000000u, 0x00700000u,
	         0x00000000u, 0x00000000u}};
}

ShaderTextureResource Ppsa10112D16StorageTextureDescriptor() {
	return {{0x205b9000u, 0xc0700000u, 0x0021803bu, 0xd1800204u, 0x00000000u, 0x00700000u,
	         0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource BasicUintVolumeStorageTextureResource() {
	auto resource = BasicStorageTextureResource();
	resource.kind = ShaderRecompiler::IR::ResourceKind::StorageImageUint;
	resource.read = false;
	return resource;
}

ShaderTextureResource BasicUintVolumeStorageTextureDescriptor() {
	return {{0x20180600u, 0xc0b00000u, 0x0003c003u, 0xa0000004u, 0x0000000fu, 0x00700000u,
	         0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource AtomicStorageTextureResource() {
	auto resource   = BasicLinearStorageTextureResource();
	resource.kind   = ShaderRecompiler::IR::ResourceKind::StorageImageUint;
	resource.read   = true;
	resource.atomic = true;
	return resource;
}

ShaderTextureResource AtomicStorageTextureDescriptor() {
	return {{0x304bb700u, 0xc1400000u, 0x0000001fu, 0x91b00204u, 0x00000000u, 0x00700000u,
	         0x00000000u, 0x00000000u}};
}

[[noreturn]] void RunStorageTextureDescriptorDeathCase(const char* kind) {
	auto resource   = BasicStorageTextureResource();
	auto descriptor = BasicStorageTextureDescriptor();
	if (std::strcmp(kind, "linear-rgb1-read") == 0) {
		resource      = BasicLinearStorageTextureResource();
		descriptor    = BasicLinearStorageTextureDescriptor();
		resource.read = true;
	} else if (std::strcmp(kind, "bgra-read") == 0) {
		resource      = BasicBgraStorageTextureResource();
		descriptor    = BasicBgraStorageTextureDescriptor();
		resource.read = true;
	} else if (std::strcmp(kind, "r16-float-read") == 0) {
		resource      = BasicBgraStorageTextureResource();
		descriptor    = Ppsa02527R16FloatStorageTextureDescriptor();
		resource.read = true;
	} else if (std::strcmp(kind, "r8-unorm-read") == 0) {
		resource      = BasicBgraStorageTextureResource();
		descriptor    = Ppsa02527R8UnormStorageTextureDescriptor();
		resource.read = true;
	} else if (std::strcmp(kind, "yzwx-read") == 0) {
		resource      = BasicLinearStorageTextureResource();
		descriptor    = BasicYzwxStorageTextureDescriptor();
		resource.read = true;
	} else if (std::strcmp(kind, "reserved-swizzle") == 0) {
		resource             = BasicLinearStorageTextureResource();
		descriptor           = BasicLinearStorageTextureDescriptor();
		descriptor.fields[3] = (descriptor.fields[3] & ~0xfffu) | DstSel(4, 5, 6, 2);
	} else if (std::strcmp(kind, "resource") == 0) {
		resource.written = false;
	} else if (std::strcmp(kind, "type") == 0) {
		descriptor.fields[3] = (descriptor.fields[3] & 0x0fffffffu) |
		                       (static_cast<uint32_t>(Prospero::ImageType::kColor2D) << 28u);
	} else if (std::strcmp(kind, "standard256b-volume") == 0) {
		descriptor.fields[3] = (descriptor.fields[3] & ~(0x1fu << 20u)) |
		                       (static_cast<uint32_t>(Prospero::TileMode::kStandard256B) << 20u);
	} else if (std::strcmp(kind, "mip") == 0) {
		descriptor.fields[3] |= 1u << 16u;
	} else if (std::strcmp(kind, "swizzle") == 0) {
		descriptor.fields[3] = (descriptor.fields[3] & ~0xfffu) | DstSel(4, 5, 6, 1);
	} else if (std::strcmp(kind, "array-base-out-of-range") == 0) {
		resource   = BasicArrayStorageTextureResource();
		descriptor = BasicArrayStorageTextureDescriptor();
		descriptor.fields[4] |= 1u << 16u;
	} else if (std::strcmp(kind, "reserved") == 0) {
		descriptor.fields[1] |= 1u << 29u;
	} else if (std::strcmp(kind, "uint-format") == 0) {
		descriptor.fields[1] =
		    (descriptor.fields[1] & ~0x1ff00000u) |
		    (static_cast<uint32_t>(Prospero::BufferFormat::k8_8_8_8UInt) << 20u);
	} else if (std::strcmp(kind, "uint-resource-float-format") == 0) {
		resource   = BasicUintArrayStorageTextureResource();
		descriptor = BasicArrayStorageTextureDescriptor();
	} else if (std::strcmp(kind, "atomic-format") == 0) {
		resource             = AtomicStorageTextureResource();
		descriptor           = AtomicStorageTextureDescriptor();
		descriptor.fields[1] = (descriptor.fields[1] & ~0x1ff00000u) |
		                       (static_cast<uint32_t>(Prospero::BufferFormat::k8UInt) << 20u);
	} else if (std::strcmp(kind, "depth-tile-read") == 0) {
		resource      = Ppsa14053DepthTileStorageTextureResource();
		descriptor    = Ppsa14053DepthTileStorageTextureDescriptor();
		resource.read = true;
	} else if (std::strcmp(kind, "depth-tile-extent") == 0) {
		resource             = Ppsa14053DepthTileStorageTextureResource();
		descriptor           = Ppsa14053DepthTileStorageTextureDescriptor();
		resource.dimension   = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
		descriptor.fields[3] = (descriptor.fields[3] & 0x0fffffffu) |
		                       (static_cast<uint32_t>(Prospero::ImageType::kColor2D) << 28u);
		descriptor.fields[4] |= 1u;
	} else if (std::strcmp(kind, "depth-tile-fmask") == 0) {
		resource   = BasicArrayStorageTextureResource();
		descriptor = Ppsa10112D16StorageTextureDescriptor();
		descriptor.fields[1] =
		    (descriptor.fields[1] & ~(0x1ffu << 20u)) |
		    (static_cast<uint32_t>(Prospero::BufferFormat::kFmask8_S4_F4) << 20u);
	} else {
		std::_Exit(0x7e);
	}
	ValidateStorageTexture(resource, descriptor, 0x10000);
	std::_Exit(0x7f);
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
void CheckBasicStorageTextureDescriptor() {
	const auto descriptor = BasicStorageTextureDescriptor();
	Require("BasicStorageTexture", "descriptor",
	        descriptor.Base40() == 0x785d0000ull && descriptor.Width5() + 1u == 33 &&
	            descriptor.Height5() + 1u == 33 && descriptor.Depth() + 1u == 33,
	        "basic 3D storage descriptor fixture is malformed");
	ValidateStorageTexture(BasicStorageTextureResource(), descriptor, 0x10000);

	const ShaderTextureResource extended {{0x204aca00u, 0xc4700000u, 0x000fc00fu, 0xa1b00facu,
	                                       0x0000003fu, 0x00700000u, 0x006b0000u, 0x00204b0au}};
	auto                        extended_resource = BasicStorageTextureResource();
	extended_resource.read                        = false;
	Require("BasicStorageTexture", "extended descriptor",
	        extended.fields[6] != 0 && extended.fields[7] != 0,
	        "extended 3D storage descriptor fixture is malformed");
	ValidateStorageTexture(extended_resource, extended, 0x400000);

	const auto linear = BasicLinearStorageTextureDescriptor();
	Require("BasicStorageTexture", "linear descriptor",
	        linear.Base40() == 0x4bcc40100ull && linear.Width5() + 1u == 3840 &&
	            linear.Height5() + 1u == 2160 && linear.Depth() + 1u == 1 &&
	            linear.Format() == Prospero::BufferFormat::k8_8_8_8UNorm &&
	            linear.TileMode() == Prospero::TileMode::kLinear,
	        "PPSA07429 linear 2D storage descriptor fixture is malformed");
	ValidateStorageTexture(BasicLinearStorageTextureResource(), linear, 0x1fa4000);

	const auto bgra = BasicBgraStorageTextureDescriptor();
	Require("BasicStorageTexture", "BGRA descriptor",
	        bgra.Base40() == 0x7c650000ull && bgra.Width5() + 1u == 1920 &&
	            bgra.Height5() + 1u == 1080 && bgra.Depth() + 1u == 1 &&
	            bgra.Format() == Prospero::BufferFormat::k8_8_8_8UNorm &&
	            bgra.TileMode() == Prospero::TileMode::kRenderTarget &&
	            bgra.DstSelXYZW() == DstSel(6, 5, 4, 7),
	        "PPSA02604 BGRA 2D storage descriptor fixture is malformed");
	ValidateStorageTexture(BasicBgraStorageTextureResource(), bgra, 0x870000);

	const auto r11g11b10 = Ppsa06228R11G11B10StorageTextureDescriptor();
	Require("BasicStorageTexture", "PPSA06228 R11G11B10 descriptor",
	        r11g11b10.Base40() == 0x10c6b50000ull && r11g11b10.Width5() + 1u == 1920 &&
	            r11g11b10.Height5() + 1u == 1080 && r11g11b10.Depth() + 1u == 1 &&
	            r11g11b10.Format() ==
	                Prospero::BufferFormat::k11_11_10Float &&
	            r11g11b10.TileMode() == Prospero::TileMode::kRenderTarget &&
	            r11g11b10.DstSelXYZW() == DstSel(4, 5, 6, 1),
	        "PPSA06228 R11G11B10 storage descriptor fixture is malformed");
	ValidateStorageTexture(BasicBgraStorageTextureResource(), r11g11b10, 0x870000);
	ValidateStorageColorView(vk::Format::eB8G8R8A8Unorm, vk::Format::eB10G11R11UfloatPack32,
	                         r11g11b10.DstSelXYZW());

	const auto max_mip = Ppsa01530MaxMipStorageTextureDescriptor();
	Require("BasicStorageTexture", "PPSA01530 max-mip descriptor",
	        max_mip.Base40() == 0x4a4290000ull && max_mip.Width5() + 1u == 64 &&
	            max_mip.Height5() + 1u == 64 && max_mip.Depth() + 1u == 1 &&
	            max_mip.BaseLevel() == 0 && max_mip.LastLevel() == 0 && max_mip.MaxMip() == 5 &&
	            max_mip.Format() == Prospero::BufferFormat::k32_32UInt &&
	            max_mip.TileMode() == Prospero::TileMode::kRenderTarget &&
	            max_mip.DstSelXYZW() == DstSel(4, 5, 0, 1),
	        "PPSA01530 max-mip storage descriptor fixture is malformed");
	ValidateStorageTexture(Ppsa01530MaxMipStorageTextureResource(), max_mip, 0x20000);
	auto mip_one = max_mip;
	mip_one.fields[3] |= (1u << 12u) | (1u << 16u);
	Require("BasicStorageTexture", "PPSA01530 mip-one descriptor",
	        mip_one.BaseLevel() == 1 && mip_one.LastLevel() == 1 && mip_one.MaxMip() == 5,
	        "PPSA01530 mip-one storage descriptor fixture is malformed");
	ValidateStorageTexture(Ppsa01530MaxMipStorageTextureResource(), mip_one, 0x20000);

	const auto r16_float = Ppsa02527R16FloatStorageTextureDescriptor();
	Require("BasicStorageTexture", "PPSA02527 R16F descriptor",
	        r16_float.Base40() == 0xce350000ull && r16_float.Width5() + 1u == 1920 &&
	            r16_float.Height5() + 1u == 1080 && r16_float.Depth() + 1u == 1 &&
	            r16_float.Format() == Prospero::BufferFormat::k16Float &&
	            r16_float.TileMode() == Prospero::TileMode::kRenderTarget &&
	            r16_float.DstSelXYZW() == DstSel(4, 0, 0, 1),
	        "PPSA02527 R16F 2D storage descriptor fixture is malformed");
	ValidateStorageTexture(BasicBgraStorageTextureResource(), r16_float, 0x480000);

	const auto r32_float = Ppsa02527R32FloatStorageTextureDescriptor();
	Require("BasicStorageTexture", "PPSA02527 R32F descriptor",
	        r32_float.Base40() == 0xcea90000ull && r32_float.Width5() + 1u == 960 &&
	            r32_float.Height5() + 1u == 540 && r32_float.Depth() + 1u == 1 &&
	            r32_float.Format() == Prospero::BufferFormat::k32Float &&
	            r32_float.TileMode() == Prospero::TileMode::kRenderTarget &&
	            r32_float.DstSelXYZW() == DstSel(4, 0, 0, 1),
	        "PPSA02527 R32F 2D storage descriptor fixture is malformed");
	ValidateStorageTexture(BasicBgraStorageTextureResource(), r32_float, 0x280000);

	const auto r8_unorm = Ppsa02527R8UnormStorageTextureDescriptor();
	Require("BasicStorageTexture", "PPSA02527 R8 UNORM descriptor",
	        r8_unorm.Base40() == 0xc7d50000ull && r8_unorm.Width5() + 1u == 960 &&
	            r8_unorm.Height5() + 1u == 540 && r8_unorm.Depth() + 1u == 1 &&
	            r8_unorm.Format() == Prospero::BufferFormat::k8UNorm &&
	            r8_unorm.TileMode() == Prospero::TileMode::kRenderTarget &&
	            r8_unorm.DstSelXYZW() == DstSel(4, 0, 0, 1),
	        "PPSA02527 R8 UNORM 2D storage descriptor fixture is malformed");
	ValidateStorageTexture(BasicBgraStorageTextureResource(), r8_unorm, 0xc0000);

	const auto yzwx = BasicYzwxStorageTextureDescriptor();
	Require("BasicStorageTexture", "YZWX descriptor",
	        yzwx.Base40() == 0x62780100ull && yzwx.Width5() + 1u == 8 && yzwx.Height5() + 1u == 8 &&
	            yzwx.Depth() + 1u == 1 &&
	            yzwx.Format() ==
	                Prospero::BufferFormat::k32_32_32_32Float &&
	            yzwx.TileMode() == Prospero::TileMode::kLinear &&
	            yzwx.DstSelXYZW() == DstSel(5, 6, 7, 4),
	        "PPSA04181 linear YZWX storage descriptor fixture is malformed");
	ValidateStorageTexture(BasicLinearStorageTextureResource(), yzwx, 0x800);
	auto all_swizzles = yzwx;
	all_swizzles.fields[1] =
	    (all_swizzles.fields[1] & ~0x1ff00000u) |
	    (static_cast<uint32_t>(Prospero::BufferFormat::k16_16_16_16Float) << 20u);
	uint32_t valid_storage_swizzles = 0;
	for (uint32_t swizzle = 0; swizzle <= 0xfffu; swizzle++) {
		if (!IsValidImageSwizzle(swizzle)) {
			continue;
		}
		all_swizzles.fields[3] = (all_swizzles.fields[3] & ~0xfffu) | swizzle;
		ValidateStorageTexture(BasicLinearStorageTextureResource(), all_swizzles, 0x800);
		ValidateStorageColorView(vk::Format::eR16G16B16A16Sfloat, vk::Format::eR16G16B16A16Sfloat,
		                         swizzle);
		ValidateStorageColorView(vk::Format::eR8G8B8A8Srgb, vk::Format::eR8G8B8A8Unorm, swizzle);
		ValidateStorageColorView(vk::Format::eB8G8R8A8Srgb, vk::Format::eR8G8B8A8Unorm, swizzle);
		valid_storage_swizzles++;
	}
	Require("BasicStorageTexture", "all write swizzles", valid_storage_swizzles == 1296,
	        "valid write-only storage image mappings were rejected");

	const ShaderTextureResource cube {{0x025bca00u, 0xc4700000u, 0x003fc03fu, 0xb1b00facu,
	                                     0x00000005u, 0x00700080u, 0x00000000u, 0x00000000u}};
	auto                        cube_resource = BasicArrayStorageTextureResource();
	cube_resource.cube                        = true;
	Require("BasicStorageTexture", "PPSA07429 cube descriptor",
	        cube.Base40() == 0x25bca0000ull && cube.Width5() + 1u == 256 &&
	            cube.Height5() + 1u == 256 && cube.Depth() + 1u == 6 &&
	            cube.BaseArray5() == 0 && cube.MaxMip() == 8 &&
	            cube.Type() == Prospero::ImageType::kCube &&
	            cube.Format() == Prospero::BufferFormat::k16_16_16_16Float &&
	            cube.TileMode() == Prospero::TileMode::kRenderTarget &&
	            cube.DstSelXYZW() == DstSel(4, 5, 6, 7),
	        "PPSA07429 cube storage descriptor fixture is malformed");
	ValidateStorageTexture(cube_resource, cube, 0x420000);

	const auto array = BasicArrayStorageTextureDescriptor();
	Require("BasicStorageTexture", "2D-array descriptor",
	        array.Base40() == 0x2017900000ull && array.Width5() + 1u == 1 &&
	            array.Height5() + 1u == 1 && array.Depth() + 1u == 1 && array.BaseArray5() == 0 &&
	            array.Type() == Prospero::ImageType::kColor2DArray &&
	            array.Format() == Prospero::BufferFormat::k8_8_8_8UNorm &&
	            array.TileMode() == Prospero::TileMode::kRenderTarget &&
	            array.DstSelXYZW() == DstSel(6, 5, 4, 7),
	        "PPSA21268 2D-array storage descriptor fixture is malformed");
	ValidateStorageTexture(BasicArrayStorageTextureResource(), array, 0x10000);
	const ShaderTextureResource mip_array {{0x20268d00u, 0xc4700000u, 0x001fc01fu,
	                                        0xd1b11facu, 0x00000000u, 0x00700070u,
	                                        0x00000000u, 0x00000000u}};
	Require("BasicStorageTexture", "PPSA14457 mip-one 2D-array descriptor",
	        mip_array.BaseLevel() == 1 && mip_array.LastLevel() == 1 &&
	            mip_array.MaxMip() == 7 &&
	            mip_array.Type() == Prospero::ImageType::kColor2DArray &&
	            mip_array.Depth() == 0 && mip_array.BaseArray5() == 0,
	        "PPSA14457 mip-one 2D-array storage descriptor fixture is malformed");
	ValidateStorageTexture(BasicArrayStorageTextureResource(), mip_array, 0x30000);

	const auto uint_array = BasicUintArrayStorageTextureDescriptor();
	Require("BasicStorageTexture", "uint 2D-array descriptor",
	        uint_array.Base40() == 0x2017920000ull && uint_array.Width5() + 1u == 1 &&
	            uint_array.Height5() + 1u == 1 && uint_array.Depth() + 1u == 1 &&
	            uint_array.BaseArray5() == 0 &&
	            uint_array.Type() == Prospero::ImageType::kColor2DArray &&
	            uint_array.Format() == Prospero::BufferFormat::k32UInt &&
	            uint_array.TileMode() ==
	                Prospero::TileMode::kRenderTarget &&
	            uint_array.DstSelXYZW() == DstSel(4, 0, 0, 1),
	        "PPSA21268 uint 2D-array storage descriptor fixture is malformed");
	ValidateStorageTexture(BasicUintArrayStorageTextureResource(), uint_array, 0x10000);

	const ShaderTextureResource standard256b {{
	    0x204e4900u,
	    0x43c00000u,
	    0x00004000u,
	    0x90100facu,
	    0x00000000u,
	    0x00700000u,
	    0x00000000u,
	    0x00000000u,
	}};
	auto standard256b_resource = BasicBgraStorageTextureResource();
	standard256b_resource.kind = ShaderRecompiler::IR::ResourceKind::StorageImageUint;
	TileSizeAlign standard256b_size {};
	TileGetTextureTotalSize(standard256b.Format(), standard256b.Width5() + 1u,
	                        standard256b.Height5() + 1u, standard256b.Depth() + 1u,
	                        1, standard256b.TileMode(), false,
	                        standard256b_size);
	Require("BasicStorageTexture", "Standard256B uint 2D descriptor",
	        standard256b.Width5() + 1u == 2 && standard256b.Height5() + 1u == 2 &&
	            standard256b.Depth() + 1u == 1 &&
	            standard256b.Type() == Prospero::ImageType::kColor2D &&
	            standard256b.Format() ==
	                Prospero::BufferFormat::k8_8_8_8UInt &&
	            standard256b.TileMode() ==
	                Prospero::TileMode::kStandard256B &&
	            standard256b.DstSelXYZW() == DstSel(4, 5, 6, 7) &&
	            standard256b_size.size == 0x100 && standard256b_size.align == 0x100,
	        "captured PPSA08511 Standard256B storage descriptor is malformed");
	ValidateStorageTexture(standard256b_resource, standard256b, standard256b_size.size);

	const auto standard4kb_array = Standard4KBUintArrayStorageTextureDescriptor();
	TileSizeAlign standard4kb_size {};
	TileGetTextureTotalSize(standard4kb_array.Format(), 1, 1, 1, 1,
	                        standard4kb_array.TileMode(), false, standard4kb_size);
	Require(
	    "BasicStorageTexture", "Standard4KB uint 2D-array descriptor",
	    standard4kb_array.Type() == Prospero::ImageType::kColor2DArray &&
	        standard4kb_array.Format() == Prospero::BufferFormat::k32UInt &&
	        standard4kb_array.TileMode() ==
	            Prospero::TileMode::kStandard4KB &&
	        standard4kb_array.DstSelXYZW() == DstSel(4, 0, 0, 1) &&
	        standard4kb_size.size == 0x1000 && standard4kb_size.align == 0x1000,
	    "captured Standard4KB uint 2D-array descriptor is malformed");
	ValidateStorageTexture(BasicUintArrayStorageTextureResource(), standard4kb_array,
	                       standard4kb_size.size);

	auto based_standard4kb_array       = standard4kb_array;
	based_standard4kb_array.fields[0]  = 0x006c6800u;
	based_standard4kb_array.fields[4]  = 0x00010001u;
	TileSizeAlign based_standard4kb_size {};
	TileGetTextureTotalSize(based_standard4kb_array.Format(), 1, 1,
	                        based_standard4kb_array.Depth() + 1u, 1,
	                        based_standard4kb_array.TileMode(), false, based_standard4kb_size);
	Require("BasicStorageTexture", "based Standard4KB array view",
	        based_standard4kb_array.Base40() == 0x6c680000ull &&
	            based_standard4kb_array.BaseArray5() == 1 && based_standard4kb_array.Depth() == 1 &&
	            based_standard4kb_size.size == 0x2000 && based_standard4kb_size.align == 0x1000,
	        "captured based Standard4KB array view is malformed");
	ValidateStorageTexture(BasicUintArrayStorageTextureResource(), based_standard4kb_array,
	                       based_standard4kb_size.size);

	const auto standard64kb       = Standard64KBStorageTextureDescriptor();
	TileSizeAlign standard64kb_size {};
	TileGetTextureTotalSize(standard64kb.Format(), standard64kb.Width5() + 1u,
	                        standard64kb.Height5() + 1u, standard64kb.Depth() + 1u,
	                        1, standard64kb.TileMode(), false,
	                        standard64kb_size);
	Require("BasicStorageTexture", "Standard64KB 2D descriptor",
	        standard64kb.Type() == Prospero::ImageType::kColor2DArray &&
	            standard64kb.Format() ==
	                Prospero::BufferFormat::k8_8_8_8UNorm &&
	            standard64kb.TileMode() ==
	                Prospero::TileMode::kStandard64KB &&
	            standard64kb.DstSelXYZW() == DstSel(4, 5, 6, 7) &&
	            standard64kb_size.size == 0x10000 && standard64kb_size.align == 0x10000,
	        "captured Standard64KB storage descriptor is malformed");
	ValidateStorageTexture(BasicBgraStorageTextureResource(), standard64kb, standard64kb_size.size);

	const auto uint_volume = BasicUintVolumeStorageTextureDescriptor();
	Require("BasicStorageTexture", "uint 3D descriptor",
	        uint_volume.Base40() == 0x2018060000ull && uint_volume.Width5() + 1u == 16 &&
	            uint_volume.Height5() + 1u == 16 && uint_volume.Depth() + 1u == 16 &&
	            uint_volume.BaseArray5() == 0 &&
	            uint_volume.Type() == Prospero::ImageType::kColor3D &&
	            uint_volume.Format() == Prospero::BufferFormat::k16UInt &&
	            uint_volume.TileMode() == Prospero::TileMode::kLinear &&
	            uint_volume.DstSelXYZW() == DstSel(4, 0, 0, 0),
	        "PPSA21268 uint 3D storage descriptor fixture is malformed");
	ValidateStorageTexture(BasicUintVolumeStorageTextureResource(), uint_volume, 0x10000);

	const ShaderTextureResource tiled_uint_volume {{0x1ac0e530u, 0xc0b00000u, 0x0003c003u,
	                                                0xa0500004u, 0x0000000fu, 0x00700000u,
	                                                0x00000000u, 0x00000000u}};
	Require("BasicStorageTexture", "tiled uint 3D descriptor",
	        tiled_uint_volume.Width5() + 1u == 16 && tiled_uint_volume.Height5() + 1u == 16 &&
	            tiled_uint_volume.Depth() + 1u == 16 &&
	            tiled_uint_volume.Type() == Prospero::ImageType::kColor3D &&
	            tiled_uint_volume.TileMode() ==
	                Prospero::TileMode::kStandard4KB,
	        "captured tiled uint 3D storage descriptor fixture is malformed");
	ValidateStorageTexture(BasicUintVolumeStorageTextureResource(), tiled_uint_volume, 0x2000);

	const auto depth_tile = Ppsa14053DepthTileStorageTextureDescriptor();
	Require("BasicStorageTexture", "PPSA14053 depth-tile descriptor",
	        depth_tile.Base40() == 0x20144c0000ull && depth_tile.Width5() + 1u == 1 &&
	            depth_tile.Height5() + 1u == 1 && depth_tile.Depth() + 1u == 1 &&
	            depth_tile.BaseArray5() == 0 &&
	            depth_tile.Type() == Prospero::ImageType::kColor2DArray &&
	            depth_tile.Format() == Prospero::BufferFormat::k8UInt &&
	            depth_tile.TileMode() == Prospero::TileMode::kDepth &&
	            depth_tile.DstSelXYZW() == DstSel(4, 0, 0, 1),
	        "PPSA14053 write-only depth-tile storage descriptor fixture is "
	        "malformed");
	ValidateStorageTexture(Ppsa14053DepthTileStorageTextureResource(), depth_tile, 0x10000);
	const auto d16_depth_tile = Ppsa10112D16StorageTextureDescriptor();
	Require(
	    "BasicStorageTexture", "PPSA10112 D16 depth-tile descriptor",
	    d16_depth_tile.Base40() == 0x205b900000ull && d16_depth_tile.Width5() + 1u == 240 &&
	        d16_depth_tile.Height5() + 1u == 135 && d16_depth_tile.Depth() + 1u == 1 &&
	        d16_depth_tile.Type() == Prospero::ImageType::kColor2DArray &&
	        d16_depth_tile.Format() == Prospero::BufferFormat::k16UNorm &&
	        d16_depth_tile.TileMode() == Prospero::TileMode::kDepth &&
	        d16_depth_tile.DstSelXYZW() == DstSel(4, 0, 0, 1),
	    "PPSA10112 writable D16 depth-plane descriptor fixture is malformed");
	const auto d16_pitch = TileGetTexturePitch(
	    d16_depth_tile.Format(), d16_depth_tile.Width5() + 1u, d16_depth_tile.TileMode());
	TileSizeAlign d16_size {};
	TileGetTextureTotalSize(d16_depth_tile.Format(), d16_depth_tile.Width5() + 1u,
	                        d16_depth_tile.Height5() + 1u, d16_depth_tile.Depth() + 1u, 1,
	                        d16_depth_tile.TileMode(), false, d16_size);
	Require("BasicStorageTexture", "PPSA10112 D16 depth-tile footprint",
	        d16_pitch == 256 && d16_size.size == 0x20000 && d16_size.align == 0x10000,
	        "PPSA10112 writable D16 depth-plane footprint is incorrect");
	ValidateStorageTexture(BasicArrayStorageTextureResource(), d16_depth_tile, d16_size.size);
	auto depth_tile_r32          = depth_tile;
	depth_tile_r32.fields[1]     = (depth_tile_r32.fields[1] & ~(0x1ffu << 20u)) |
	                               (static_cast<uint32_t>(Prospero::BufferFormat::k32UInt) << 20u);
	depth_tile_r32.fields[3]     = (depth_tile_r32.fields[3] & ~(0xfu << 28u)) |
	                               (static_cast<uint32_t>(Prospero::ImageType::kColor2D) << 28u);
	auto depth_tile_r32_resource = Ppsa14053DepthTileStorageTextureResource();
	depth_tile_r32_resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
	ValidateStorageTexture(depth_tile_r32_resource, depth_tile_r32, 0x10000);
	Require("BasicStorageTexture", "R32_UINT replicated write mapping",
	        IsValidImageSwizzle(DstSel(4, 4, 4, 4)),
	        "single-channel replicated destination selection was rejected");

	const auto atomic = AtomicStorageTextureDescriptor();
	Require("BasicStorageTexture", "atomic R32_UINT descriptor",
	        atomic.Width5() + 1u == 128 && atomic.Height5() + 1u == 1 && atomic.Depth() + 1u == 1 &&
	            atomic.Type() == Prospero::ImageType::kColor2D &&
	            atomic.Format() == Prospero::BufferFormat::k32UInt &&
	            atomic.DstSelXYZW() == DstSel(4, 0, 0, 1),
	        "PPSA22102 image-atomic descriptor fixture is malformed");
	ValidateStorageTexture(AtomicStorageTextureResource(), atomic, 0x10000);

	char path[MAX_PATH] {};
	Require("BasicStorageTexture", "host", GetModuleFileNameA(nullptr, path, MAX_PATH) != 0,
	        "GetModuleFileName failed");
	for (const char* kind: {"resource",
	                        "type",
	                        "standard256b-volume",
	                        "mip",
	                        "swizzle",
	                        "linear-rgb1-read",
	                        "bgra-read",
	                        "r16-float-read",
	                        "r8-unorm-read",
	                        "yzwx-read",
	                        "reserved-swizzle",
	                        "array-base-out-of-range",
	                        "reserved",
	                        "uint-format",
	                        "uint-resource-float-format",
	                        "atomic-format",
	                        "depth-tile-read",
	                        "depth-tile-extent",
	                        "depth-tile-fmask"}) {
		std::string command =
		    std::string("\"") + path + "\" --storage-texture-descriptor-death " + kind;
		std::vector<char> mutable_command(command.begin(), command.end());
		mutable_command.push_back('\0');
		STARTUPINFOA        startup {sizeof(startup)};
		PROCESS_INFORMATION process {};
		Require("BasicStorageTexture", "host",
		        CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
		                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != 0,
		        "CreateProcess failed");
		Require("BasicStorageTexture", "host",
		        WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0,
		        "descriptor death case timed out");
		DWORD      exit_code = 0;
		const bool exited    = GetExitCodeProcess(process.hProcess, &exit_code) != 0;
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		Require("BasicStorageTexture", "host", exited && exit_code == 321,
		        std::string(kind) + " storage descriptor did not report a fatal error");
	}
	std::printf("[host]    %-32s ok\n", "BasicStorageTextureDescriptor");
}
#endif

void CheckStorageTextureLinearUploadLayout() {
	constexpr auto format = Prospero::BufferFormat::k8_8_8_8UNorm;
	constexpr uint32_t width  = 3840;
	constexpr uint32_t height = 2160;
	constexpr uint32_t depth  = 1;
	constexpr auto tile   = Prospero::TileMode::kLinear;
	const auto         pitch  = TileGetTexturePitch(format, width, tile);
	TileSizeAlign      total {};
	TileGetTextureTotalSize(format, width, height, depth, 1, tile, false, total);
	const auto layout =
	    TextureCalcUploadLayout(format, width, height, 1, depth, tile, total.size, true,
	                            false, "StorageTextureLinearTest");
	const auto regions = TextureBuildImageCopies(layout);
	Require("StorageTextureLinearUpload", "layout",
	        pitch == width && total.size == 0x1fa4000 && total.align == 256 &&
	            layout.surface.description.tile_mode == tile && layout.pitch == width &&
	            layout.slice_stride == total.size &&
	            regions.size() == 1 && regions[0].bufferOffset == 0 &&
	            regions[0].imageExtent.width == width && regions[0].imageExtent.height == height &&
	            regions[0].bufferRowLength == width,
	        "linear RGBA8 storage upload lost Prospero pitch or allocation size");
	std::printf("[host]    %-32s ok\n", "StorageTextureLinearUpload");
}

void CheckStorageTextureDepthTileUploadLayout() {
	constexpr auto format = Prospero::BufferFormat::k8UInt;
	constexpr uint32_t width  = 1;
	constexpr uint32_t height = 1;
	constexpr uint32_t depth  = 1;
	constexpr auto tile   = Prospero::TileMode::kDepth;
	const auto         pitch  = TileGetTexturePitch(format, width, tile);
	TileSizeAlign      slice {};
	TileSizeAlign      total {};
	TileSizeOffset     level {};
	TilePaddedSize     padded {};
	TileGetTextureSize(format, width, height, 1, tile, &slice, &level, &padded);
	TileGetTextureTotalSize(format, width, height, depth, 1, tile, false, total);
	const auto layout =
	    TextureCalcUploadLayout(format, width, height, 1, depth, tile, total.size, true,
	                            false, "StorageTextureDepthTileTest");
	const auto regions = TextureBuildImageCopies(layout);
	Require("StorageTextureDepthTileUpload", "PPSA14053 layout",
	        pitch == 256 && padded.width == 256 && padded.height == 256 && slice.size == 0x10000 &&
	            slice.align == 0x10000 && level.size == slice.size && level.offset == 0 &&
	            total.size == slice.size && total.align == slice.align &&
	            layout.surface.description.tile_mode == tile &&
	            layout.surface.texture.block.family == TileBlockFamily::Depth64KB &&
	            layout.pitch == pitch &&
	            layout.slice_stride == pitch && layout.source_slice_stride == total.size &&
	            layout.mips[0].size == pitch &&
	            layout.surface.mips[0].size == total.size && regions.size() == 1 &&
	            regions[0].bufferOffset == 0 && regions[0].imageExtent.width == width &&
	            regions[0].imageExtent.height == height && regions[0].bufferRowLength == pitch,
	        "1x1 R8_UINT depth tile lost its 64 KiB source footprint");
	std::printf("[host]    %-32s ok\n", "StorageTextureDepthTileUpload");
}
void CheckStorageImageSwizzleSpecializationId() {
	std::array<u32, 1>    code {};
	HW::ComputeShaderInfo regs {};
	regs.cs_regs.data_addr = reinterpret_cast<uint64_t>(code.data());

	ShaderRecompiler::IR::Program identity_program;
	identity_program.binding_layout_complete = true;
	ShaderRecompiler::IR::ImageResource image;
	image.kind      = ShaderRecompiler::IR::ResourceKind::StorageImage;
	image.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
	identity_program.info.images.push_back(image);
	auto rgb1_program                           = identity_program;
	rgb1_program.info.images[0].storage_swizzle = DstSel(4, 5, 6, 1);

	const auto resources = std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>();
	ShaderComputeInputInfo identity_info;
	identity_info.stage.program =
	    std::make_shared<const ShaderRecompiler::IR::Program>(identity_program);
	identity_info.stage.resources = resources;
	auto rgb1_info                = identity_info;
	rgb1_info.stage.program = std::make_shared<const ShaderRecompiler::IR::Program>(rgb1_program);

	const auto identity_id = ShaderGetIdCS(regs, identity_info, true);
	const auto rgb1_id     = ShaderGetIdCS(regs, rgb1_info, true);
	Require("StorageImageSwizzleSpecializationId", "pipeline cache key",
	        identity_id != rgb1_id && identity_id.ids.size() == rgb1_id.ids.size(),
	        "storage swizzle-specialized SPIR-V variants share a pipeline ID");
	std::printf("[host]    %-32s ok\n", "StorageImageSwizzlePipelineId");
}

void CheckStorageTextureVolumeUploadLayout() {
	constexpr auto format = Prospero::BufferFormat::k16_16_16_16Float;
	constexpr uint32_t width  = 33;
	constexpr uint32_t height = 33;
	constexpr uint32_t depth  = 33;
	const auto         pitch  = TileGetTexturePitch(
	    format, width, Prospero::TileMode::kRenderTarget);
	TileSizeAlign total {};
	TileGetTextureTotalSize(format, width, height, depth, 1,
	                        Prospero::TileMode::kRenderTarget, true, total);
	const auto layout =
	    TextureCalcUploadLayout(format, width, height, 1, depth,
	                            Prospero::TileMode::kRenderTarget,
	                            total.size, true, true, "StorageTextureVolumeTest");
	const auto regions = TextureBuildImageCopies(layout);
	Require("StorageTextureVolumeUpload", "layout",
	        pitch == 128 && total.size == 0x210000 && layout.slice_stride == 0x2208 &&
	            layout.source_slice_stride == 0 && layout.mips[0].size == 0x2208 &&
	            layout.surface.mips[0].size == 0x10000 && regions.size() == depth,
	        "3D render-target upload did not preserve its compact linear layout");

	std::vector<GpuTileInfo> infos;
	Require("StorageTextureVolumeUpload", "GPU records",
	        TextureBuildGpuTileInfos(total.size, regions, layout, 1, infos) &&
	            infos.size() == depth,
	        "3D render-target GPU records were not built");
	for (const uint32_t z: {0u, 1u, depth - 1u}) {
		Require("StorageTextureVolumeUpload", "slice offsets",
		        infos[z].linear_offset == static_cast<uint64_t>(z) * 0x2208 &&
		            infos[z].tiled_offset == static_cast<uint64_t>(z) * 0x10000 &&
		            infos[z].surface_z == z && infos[z].pitch == width,
		        "volume slice lost its linear stride, block slice, or Z swizzle");
	}
	std::printf("[host]    %-32s ok\n", "StorageTextureVolumeUpload");
}

void CheckStorageTextureVolumeMipRegions() {
	constexpr auto format = Prospero::BufferFormat::k8_8_8_8UNorm;
	constexpr uint32_t width  = 8;
	constexpr uint32_t height = 4;
	constexpr uint32_t depth  = 5;
	constexpr uint32_t levels = 3;
	constexpr auto tile   = Prospero::TileMode::kLinear;
	TileSizeAlign      total {};
	TileGetTextureTotalSize(format, width, height, depth, levels, tile, true, total);
	const auto layout =
	    TextureCalcUploadLayout(format, width, height, levels, depth, tile, total.size, true,
	                            true, "StorageTextureVolumeMipTest");
	const auto copies = TextureBuildImageCopies(layout);

	bool   valid = copies.size() == 8;
	size_t index = 0;
	for (uint32_t level = 0; level < levels && valid; level++) {
		const uint32_t mip_depth  = std::max(depth >> level, 1u);
		const uint32_t mip_width  = std::max(width >> level, 1u);
		const uint32_t mip_height = std::max(height >> level, 1u);
		for (uint32_t z = 0; z < mip_depth; z++, index++) {
			const auto& copy = copies[index];
			valid &=
			    copy.imageSubresource.mipLevel == level &&
			    copy.imageOffset.z == static_cast<int>(z) && copy.imageExtent.width == mip_width &&
			    copy.imageExtent.height == mip_height &&
			    copy.bufferOffset == layout.mips[level].offset + z * layout.slice_stride &&
			    copy.bufferRowLength == layout.mips[level].row_length &&
			    copy.bufferImageHeight == layout.mips[level].image_height;
		}
	}
	valid &= index == copies.size();
	Require("StorageTextureVolumeMipRegions", "per-mip depth", valid,
	        "direction-neutral image copies did not shrink depth or preserve "
	        "Vulkan Z coordinates");
	std::printf("[host]    %-32s ok\n", "StorageTextureVolumeMipRegions");
}
void CheckStandard64RenderTargetTileRoundTrip() {
	constexpr auto format = Prospero::BufferFormat::k32Float;
	constexpr auto tile   = Prospero::TileMode::kStandard64KB;

	constexpr uint32_t observed_width  = 3840;
	constexpr uint32_t observed_height = 2160;
	const auto         observed_pitch  = TileGetTexturePitch(format, observed_width, tile);
	TileSizeAlign      observed {};
	TileGetTextureSize(format, observed_width, observed_height, 1, tile, &observed,
	                   nullptr, nullptr);
	Require("Standard64RenderTarget", "observed layout",
	        observed_pitch == 3840 && observed.size == 0x1fe0000 && observed.align == 0x10000,
	        "PPSA02721 Standard64KB render-target footprint changed");

	constexpr uint32_t width  = 257;
	constexpr uint32_t height = 131;
	const auto         pitch  = TileGetTexturePitch(format, width, tile);
	TileSizeAlign      storage {};
	TileGetTextureSize(format, width, height, 1, tile, &storage, nullptr, nullptr);
	Require("Standard64RenderTarget", "partial layout",
	        pitch == 384 && storage.size == 0x60000 && storage.align == 0x10000,
	        "partial Standard64KB footprint was not padded in 128x128 blocks");

	ImageInfo info {};
	info.data            = {0x10000, storage.size};
	info.pixel_format    = vk::Format::eR8G8B8A8Unorm;
	info.guest_format    = format;
	info.type            = Prospero::ImageType::kColor2D;
	info.extent          = {width, height, 1};
	info.resources       = {1, 1};
	info.pitch           = pitch;
	info.bytes_per_block = 4;
	info.samples         = 1;
	info.tile_mode       = tile;
	info.mip_layout[0]   = {0, storage.size, pitch, height};
	Require("Standard64RenderTarget", "support boundary",
	        IsSupportedStandard64RenderTarget(info) && IsTiledRenderTarget(info),
	        "exact Standard64KB render target was not classified as tiled");
	Require("Standard64RenderTarget", "display tile boundary",
	        IsSupportedDisplayRenderTargetTileMode(
	            Prospero::TileMode::kRenderTarget) &&
	            !IsSupportedDisplayRenderTargetTileMode(tile),
	        "Standard64KB render target could alias a mode-27 display image");
	auto unsupported = info;
	unsupported.data.address += 4;
	Require("Standard64RenderTarget", "address guard",
	        !IsSupportedStandard64RenderTarget(unsupported),
	        "unaligned Standard64KB backing was accepted");
	unsupported                 = info;
	unsupported.bytes_per_block = 8;
	Require("Standard64RenderTarget", "element guard",
	        !IsSupportedStandard64RenderTarget(unsupported),
	        "unimplemented Standard64KB element size was accepted");
	unsupported = info;
	unsupported.pitch += 128;
	Require("Standard64RenderTarget", "pitch guard",
	        !IsSupportedStandard64RenderTarget(unsupported),
	        "non-minimal Standard64KB pitch was accepted");
	unsupported = info;
	unsupported.data.size += 0x10000;
	Require("Standard64RenderTarget", "size guard", !IsSupportedStandard64RenderTarget(unsupported),
	        "non-exact Standard64KB allocation was accepted");
	unsupported                  = info;
	unsupported.resources.levels = 2;
	Require("Standard64RenderTarget", "mip guard", !IsSupportedStandard64RenderTarget(unsupported),
	        "unimplemented Standard64KB mip chain was accepted");
	unsupported                  = info;
	unsupported.resources.layers = 2;
	Require("Standard64RenderTarget", "layer guard",
	        !IsSupportedStandard64RenderTarget(unsupported),
	        "unimplemented Standard64KB array was accepted");

	std::printf("[host]    %-32s ok\n", "Standard64RenderTarget");
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
void CheckStorageTextureGpuOwnedRebindState() {
	constexpr uintptr_t base         = 0x0000000200200000ull;
	constexpr uint64_t  size         = 0x10000;
	const auto          guest_memory = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    base, size, Common::VirtualMemory::Mode::ReadWrite, "storage_texture_gpu_owned_rebind",
	    true);
	auto* memory = reinterpret_cast<uint8_t*>(guest_memory);
	Require("StorageTextureGpuOwnedRebind", "allocation", guest_memory == base,
	        "fixed guest-owner allocation failed");
	PageManager   page_manager;
	MemoryTracker tracker(page_manager);
	page_manager.OnGpuMap(base, size);
	tracker.ForEachUploadRange(
	    base, size, true, [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
	uint64_t                 readable = 0;
	uint64_t                 mapped   = 0;
	MEMORY_BASIC_INFORMATION protection {};
	Require("StorageTextureGpuOwnedRebind", "owned",
	        tracker.IsRegionGpuModified(base, size) &&
	            (!HostMemoryQueryReadable(base, size, readable) || readable < size) &&
	            HostMemoryQueryRange(base, size, HostMemoryAccess::Mapped, mapped) &&
	            mapped == size && VirtualQuery(memory, &protection, sizeof(protection)) != 0 &&
	            protection.Protect == PAGE_NOACCESS,
	        "GPU-owned storage pages remained host-readable or lost tracker "
	        "identity");

	tracker.UnmarkRegionAsGpuModified(base, size);
	readable = 0;
	Require("StorageTextureGpuOwnedRebind", "clean readback",
	        !tracker.IsRegionGpuModified(base, size) && !tracker.IsRegionCpuModified(base, size) &&
	            HostMemoryQueryReadable(base, size, readable) && readable == size,
	        "clean storage readback did not publish readable coherent backing");
	tracker.MarkRegionAsGpuModified(base, size);
	readable = 0;
	Require("StorageTextureGpuOwnedRebind", "clean reclaim",
	        tracker.IsRegionGpuModified(base, size) && !tracker.IsRegionCpuModified(base, size) &&
	            (!HostMemoryQueryReadable(base, size, readable) || readable < size),
	        "clean storage rebind did not reclaim GPU ownership without an upload");

	tracker.UnmarkRegionAsGpuModified(base, size);
	tracker.MarkRegionAsCpuModified(base, size);
	uint32_t dirty_ranges  = 0;
	bool     upload_called = false;
	tracker.ForEachUploadRange(
	    base, size, true, [&](uint64_t, uint64_t) noexcept { dirty_ranges++; },
	    [&]() noexcept { upload_called = true; });
	readable = 0;
	Require("StorageTextureGpuOwnedRebind", "dirty refresh",
	        dirty_ranges == 1 && upload_called && tracker.IsRegionGpuModified(base, size) &&
	            !tracker.IsRegionCpuModified(base, size) &&
	            (!HostMemoryQueryReadable(base, size, readable) || readable < size),
	        "CPU-dirty storage rebind did not refresh once and reclaim GPU "
	        "ownership");

	tracker.UnmarkRegionAsGpuModified(base, size);
	tracker.UntrackMemory(base, size);
	page_manager.OnGpuUnmap(base, size);
	Require("StorageTextureGpuOwnedRebind", "free",
	        Libs::LibKernel::Memory::FreeGuestMemory(base, size), "guest-owner free failed");
	std::printf("[host]    %-32s ok\n", "StorageTextureGpuOwnedRebind");
}
#endif

void CheckNativeMsaaState() {
	Require("NativeMsaaState", "sample encoding",
	        render_sample_count(0) == 1 && render_sample_count(1) == 2 &&
	            render_sample_count(2) == 4 && render_sample_count(3) == 8 &&
	            render_sample_count(4) == 0,
	        "PS5 sample encodings were not mapped exactly");
	Require("NativeMsaaState", "Vulkan sample mapping",
	        vulkan_sample_count(1) == vk::SampleCountFlagBits::e1 &&
	            vulkan_sample_count(2) == vk::SampleCountFlagBits::e2 &&
	            vulkan_sample_count(4) == vk::SampleCountFlagBits::e4 &&
	            vulkan_sample_count(8) == vk::SampleCountFlagBits::e8 &&
	            vulkan_sample_count(3) == vk::SampleCountFlagBits {},
	        "native sample counts were not mapped exactly to Vulkan");

	TileSizeAlign color {};
	const auto    color_pitch = TileGetRenderTargetPitch(1920, 8, 3);
	Require("NativeMsaaState", "8x color footprint",
	        color_pitch == 1920 && TileGetRenderTargetSize(1920, 1080, color_pitch, 8, color, 3) &&
	            color.align == 0x10000 && color.size == 0x07f80000,
	        "8x R16G16B16A16 color footprint was not preserved");

	TileSizeAlign depth {};
	TileSizeAlign stencil {};
	TileSizeAlign htile {};
	Require("NativeMsaaState", "8x depth/stencil footprint",
	        TileGetDepthPitch(1920, 4, 3) == 1920 &&
	            TileGetDepthSize(1920, 1080, 0,
	                             Prospero::DepthFormat::kZ32F,
	                             Prospero::StencilFormat::k8UInt, true,
	                             stencil, htile, depth, 3) &&
	            depth.align == 0x10000 && depth.size == 0x03fc0000 && stencil.align == 0x10000 &&
	            stencil.size == 0x010e0000 && htile.align == 0x8000 && htile.size == 0x00030000,
	        "8x depth/stencil or fragment-independent HTile footprint regressed");
	std::printf("[host]    %-32s ok\n", "NativeMsaaState");
}

void CheckDepthHtileStencilCompatibility() {
	Require("DepthHtileStencilCompatibility", "disabled acceleration",
	        depth_htile_stencil_acceleration_compatible(false, false, true),
	        "disabled Hi-Stencil state was rejected");
	Require("DepthHtileStencilCompatibility", "PS5 stencil plus HTile",
	        depth_htile_stencil_acceleration_compatible(true, true, false),
	        "valid PS5 Hi-Stencil attachment was rejected");
	Require("DepthHtileStencilCompatibility", "missing stencil plane",
	        !depth_htile_stencil_acceleration_compatible(false, true, false),
	        "Hi-Stencil without a stencil plane was silently admitted");
	Require("DepthHtileStencilCompatibility", "missing HTile metadata",
	        !depth_htile_stencil_acceleration_compatible(true, false, false),
	        "Hi-Stencil without HTile metadata was silently admitted");
	std::printf("[host]    %-32s ok\n", "DepthHtileStencilCompatibility");
}

void CheckPs5DepthRegisterDecoding() {
	constexpr uint32_t captured_z       = 0x22900803u;
	constexpr uint32_t captured_stencil = 0x00100801u;
	const auto         z                = HW::DepthZInfo::Decode(captured_z);
	const auto         stencil          = HW::DepthStencilInfo::Decode(captured_stencil);
	Require("Ps5DepthRegisterDecoding", "captured texture-compatible state",
	        z.format == Prospero::DepthFormat::kZ32F &&
	            z.num_samples == 0 && z.htile_acceleration && !z.expclear_enabled &&
	            !z.partially_resident && z.max_mip_level == 0 &&
	            z.z_compare_base == Prospero::ZCompareBase::kZMin &&
	            z.texture_compatibility == Prospero::TextureCompatiblePlaneCompression::kEnable &&
	            z.HasValidTextureCompatibility() &&
	            stencil.format == Prospero::StencilFormat::k8UInt &&
	            !stencil.htile_stencil_disabled && !stencil.expclear_enabled &&
	            !stencil.partially_resident &&
	            stencil.texture_compatibility == Prospero::TextureCompatibleStencil::kEnable &&
	            stencil.HasValidTextureCompatibility(),
	        "valid PS5 depth/stencil aggregate encodings were not decoded exactly");

	const auto malformed_z       = HW::DepthZInfo::Decode(0x00000803u);
	const auto malformed_stencil = HW::DepthStencilInfo::Decode(0x00100001u);
	Require("Ps5DepthRegisterDecoding", "malformed aggregate state",
	        !malformed_z.HasValidTextureCompatibility() &&
	            !malformed_stencil.HasValidTextureCompatibility(),
	        "partial texture-compatible aggregate encodings were accepted");
	std::printf("[host]    %-32s ok\n", "Ps5DepthRegisterDecoding");
}

void CheckStencilAttachmentAccess() {
	PipelineStencilStaticState  state {vk::StencilOp::eKeep, vk::StencilOp::eKeep,
	                                   vk::StencilOp::eKeep, vk::CompareOp::eAlways};
	PipelineStencilDynamicState dynamic {0xff, 0xff, 0};
	Require("StencilAttachmentAccess", "always keep",
	        !stencil_face_accesses_attachment(state, dynamic),
	        "ALWAYS/KEEP state was classified as stencil access");
	state.compareOp = vk::CompareOp::eEqual;
	Require("StencilAttachmentAccess", "compare reads",
	        stencil_face_accesses_attachment(state, dynamic),
	        "real stencil comparison was classified as no access");
	state.compareOp = vk::CompareOp::eAlways;
	state.passOp    = vk::StencilOp::eZero;
	Require("StencilAttachmentAccess", "write operation",
	        stencil_face_accesses_attachment(state, dynamic),
	        "write-capable stencil operation was classified as no access");
	dynamic.writeMask = 0;
	Require("StencilAttachmentAccess", "masked write",
	        !stencil_face_accesses_attachment(state, dynamic),
	        "fully masked stencil write was classified as access");
	std::printf("[host]    %-32s ok\n", "StencilAttachmentAccess");
}

void CheckDepthAttachmentWrites() {
	RenderDepthInfo target {};
	target.format             = vk::Format::eD32SfloatS8Uint;
	target.depth_write_enable = true;
	Require("DepthAttachmentWrites", "disabled depth test", !target.AttachmentWriteAspects(),
	        "disabled depth testing claimed a depth write");

	target.depth_test_enable = true;
	target.depth_compare_op  = vk::CompareOp::eLess;
	Require("DepthAttachmentWrites", "depth write",
	        target.AttachmentWriteAspects() == vk::ImageAspectFlagBits::eDepth,
	        "write-enabled depth testing did not claim depth");

	target.depth_test_enable       = false;
	target.depth_write_enable      = false;
	target.depth_load_clear_enable = true;
	Require("DepthAttachmentWrites", "depth clear",
	        target.AttachmentWriteAspects() == vk::ImageAspectFlagBits::eDepth,
	        "depth attachment clear did not claim depth");

	target.depth_load_clear_enable = false;
	target.stencil_test_enable     = true;
	target.stencil_dynamic_front   = {0xff, 0xff, 0};
	target.stencil_dynamic_back    = target.stencil_dynamic_front;
	target.stencil_static_front = {vk::StencilOp::eKeep, vk::StencilOp::eKeep, vk::StencilOp::eKeep,
	                               vk::CompareOp::eAlways};
	target.stencil_static_back  = target.stencil_static_front;
	Require("DepthAttachmentWrites", "stencil keep", !target.AttachmentWriteAspects(),
	        "KEEP-only stencil state claimed a stencil write");

	target.stencil_static_front.failOp = vk::StencilOp::eZero;
	Require("DepthAttachmentWrites", "unreachable stencil fail", !target.AttachmentWriteAspects(),
	        "ALWAYS comparison claimed an unreachable fail operation");

	target.stencil_static_front.failOp = vk::StencilOp::eKeep;
	target.stencil_static_front.passOp = vk::StencilOp::eReplace;
	Require("DepthAttachmentWrites", "stencil pass write",
	        target.AttachmentWriteAspects() == vk::ImageAspectFlagBits::eStencil,
	        "write-capable stencil pass did not claim stencil");

	target.stencil_dynamic_front.writeMask = 0;
	Require("DepthAttachmentWrites", "back-face keep", !target.AttachmentWriteAspects(),
	        "masked front write or KEEP-only back face claimed stencil");

	target.stencil_test_enable  = false;
	target.stencil_clear_enable = true;
	Require("DepthAttachmentWrites", "stencil clear",
	        target.AttachmentWriteAspects() == vk::ImageAspectFlagBits::eStencil,
	        "stencil attachment clear did not claim stencil");

	target.format = vk::Format::eD32Sfloat;
	Require("DepthAttachmentWrites", "missing stencil aspect", !target.AttachmentWriteAspects(),
	        "depth-only format claimed a stencil write");
	std::printf("[host]    %-32s ok\n", "DepthAttachmentWrites");
}

void CheckDynamicRenderingState() {
	RenderState first {};
	first.width                             = 64;
	first.height                            = 32;
	first.num_layers                        = 2;
	first.num_color_attachments             = 1;
	first.color_attachments[0].image_layout = vk::ImageLayout::eColorAttachmentOptimal;
	first.color_attachments[0].clear_value  = {1, 2, 3, 4};
	first.color_attachments[0].is_clear     = true;
	auto changed                            = first;
	Require("DynamicRenderingState", "identical state", first == changed,
	        "fieldwise RenderState identity rejected an identical scope");
	changed.color_attachments[0].is_clear = false;
	Require("DynamicRenderingState", "clear identity", first != changed,
	        "clear/load state did not participate in rendering identity");
	changed            = first;
	changed.num_layers = 1;
	Require("DynamicRenderingState", "layer identity", first != changed,
	        "layer count did not participate in rendering identity");
	changed                                   = first;
	changed.color_attachments[0].image_layout = vk::ImageLayout::eGeneral;
	Require("DynamicRenderingState", "layout identity", first != changed,
	        "attachment layout did not participate in rendering identity");

	PipelineRenderingState rgba {};
	rgba.color_count            = 1;
	rgba.color_formats[0]       = vk::Format::eR8G8B8A8Unorm;
	auto uint_color             = rgba;
	uint_color.color_formats[0] = vk::Format::eR8G8B8A8Uint;
	Require("DynamicRenderingState", "pipeline color format identity", rgba != uint_color,
	        "distinct dynamic-rendering formats can alias one pipeline key");
	auto depth           = rgba;
	depth.depth_format   = vk::Format::eD32SfloatS8Uint;
	depth.stencil_format = vk::Format::eD32SfloatS8Uint;
	Require("DynamicRenderingState", "pipeline depth/stencil identity", rgba != depth,
	        "depth/stencil formats did not participate in pipeline identity");

	RenderDepthInfo attachment {};
	attachment.format = vk::Format::eD32SfloatS8Uint;
	Require("DynamicRenderingState", "read-only depth/stencil layout",
	        depth_attachment_layout(attachment) == vk::ImageLayout::eDepthStencilReadOnlyOptimal,
	        "fully read-only depth/stencil used a writable layout");
	attachment.depth_load_clear_enable = true;
	Require("DynamicRenderingState", "depth-write stencil-read layout",
	        depth_attachment_layout(attachment) ==
	            vk::ImageLayout::eDepthAttachmentStencilReadOnlyOptimal,
	        "depth-only writes did not retain read-only stencil");
	attachment.depth_load_clear_enable = false;
	attachment.stencil_clear_enable    = true;
	Require("DynamicRenderingState", "depth-read stencil-write layout",
	        depth_attachment_layout(attachment) ==
	            vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal,
	        "stencil-only writes did not retain read-only depth");
	attachment.depth_load_clear_enable = true;
	Require("DynamicRenderingState", "writable depth/stencil layout",
	        depth_attachment_layout(attachment) == vk::ImageLayout::eDepthStencilAttachmentOptimal,
	        "combined depth/stencil writes did not use the writable layout");
	std::printf("[host]    %-32s ok\n", "DynamicRenderingState");
}

void CheckDepthTargetFootprints() {
	TileSizeAlign stencil {};
	TileSizeAlign htile {};
	TileSizeAlign depth {};
	struct AttachmentFormatCase {
		const char*             name;
		Prospero::DepthFormat   depth_format;
		Prospero::StencilFormat stencil_format;
		vk::Format              expected;
	};
	constexpr AttachmentFormatCase attachment_cases[] = {
	    {"Z16", Prospero::DepthFormat::kZ16,
	     Prospero::StencilFormat::kInvalid, vk::Format::eD16Unorm},
	    {"Z16S8", Prospero::DepthFormat::kZ16,
	     Prospero::StencilFormat::k8UInt, vk::Format::eD16UnormS8Uint},
	    {"Z32", Prospero::DepthFormat::kZ32F,
	     Prospero::StencilFormat::kInvalid, vk::Format::eD32Sfloat},
	    {"Z32S8", Prospero::DepthFormat::kZ32F,
	     Prospero::StencilFormat::k8UInt, vk::Format::eD32SfloatS8Uint},
	    {"invalid depth", static_cast<Prospero::DepthFormat>(2u),
	     Prospero::StencilFormat::k8UInt,
	     vk::Format::eUndefined},
	    {"invalid stencil", Prospero::DepthFormat::kZ16,
	     static_cast<Prospero::StencilFormat>(2u),
	     vk::Format::eUndefined},
	};
	for (const auto& test: attachment_cases) {
		Require("DepthTargetFootprints", test.name,
		        DepthAttachmentFormat(test.depth_format, test.stencil_format) == test.expected,
		        "PS5 depth/stencil attachment mapping changed");
	}
	Require("DepthTargetFootprints", "1920x1080 Z16S8 without HTile",
	        TileGetDepthSize(1920, 1080, 0, Prospero::DepthFormat::kZ16,
	                         Prospero::StencilFormat::k8UInt, false,
	                         stencil, htile, depth) &&
	            depth.size == 0x480000 && depth.align == 0x10000 && stencil.size == 0x280000 &&
	            stencil.align == 0x10000 && htile.size == 0,
	        "captured Z16S8 footprint disagrees with Prospero block rules");
	struct TargetFormatCase {
		const char*            name;
		vk::Format             host_format;
		Prospero::BufferFormat guest_format;
		uint32_t               bytes_per_element;
		bool                   stencil;
		bool                   supported;
		bool                   readback;
	};
	constexpr TargetFormatCase target_cases[] = {
	    {"D16", vk::Format::eD16Unorm, Prospero::BufferFormat::k16UNorm, 2,
	     false, true, true},
	    {"D16S8", vk::Format::eD16UnormS8Uint,
	     Prospero::BufferFormat::k16UNorm, 2, true, true, true},
	    {"D16 via D24S8", vk::Format::eD24UnormS8Uint,
	     Prospero::BufferFormat::k16UNorm, 2, true, true, true},
	    {"D16 via D32S8", vk::Format::eD32SfloatS8Uint,
	     Prospero::BufferFormat::k16UNorm, 2, true, true, true},
	    {"D32", vk::Format::eD32Sfloat, Prospero::BufferFormat::k32Float, 4,
	     false, true, true},
	    {"D32S8", vk::Format::eD32SfloatS8Uint,
	     Prospero::BufferFormat::k32Float, 4, true, true, true},
	    {"D16 plus stencil mismatch", vk::Format::eD16Unorm,
	     Prospero::BufferFormat::k16UNorm, 2, true, false, false},
	    {"fallback without stencil", vk::Format::eD24UnormS8Uint,
	     Prospero::BufferFormat::k16UNorm, 2, false, false, false},
	    {"D32 guest via D24", vk::Format::eD24UnormS8Uint,
	     Prospero::BufferFormat::k32Float, 4, true, false, false},
	    {"D16 byte mismatch", vk::Format::eD16Unorm,
	     Prospero::BufferFormat::k16UNorm, 4, false, false, false},
	};
	for (const auto& test: target_cases) {
		ImageInfo target {};
		target.pixel_format    = test.host_format;
		target.guest_format    = test.guest_format;
		target.bytes_per_block = test.bytes_per_element;
		target.stencil         = test.stencil ? GuestRange {0x10000, 0x10000} : GuestRange {};
		Require("DepthTargetFootprints", test.name,
		        IsSupportedDepthTargetFormat(target) == test.supported &&
		            IsSupportedDepthPlaneReadback(target) == test.readback,
		        "host/guest depth format or exact readback policy changed");
	}
	ImageInfo compressed_stencil {};
	compressed_stencil.pixel_format    = vk::Format::eD32SfloatS8Uint;
	compressed_stencil.guest_format    = Prospero::BufferFormat::k32Float;
	compressed_stencil.bytes_per_block = 4;
	compressed_stencil.stencil         = {0x10000, 0x10000};
	compressed_stencil.metadata.stencil_compressed = true;
	Require("DepthTargetFootprints", "compressed stencil depth-plane readback",
	        IsSupportedDepthTargetFormat(compressed_stencil) &&
	            IsSupportedDepthPlaneReadback(compressed_stencil),
	        "stencil metadata incorrectly blocked independent depth-plane "
	        "readback");
	Require("DepthTargetFootprints", "lexicographic subresource comparison",
	        ImageSubresources {2, 1} > ImageSubresources {1, 4} &&
	            ImageSubresources {1, 4} < ImageSubresources {2, 1},
	        "subresource ordering diverged from required overlap semantics");
	struct TransferPlaneCase {
		const char* name;
		vk::Format  attachment;
		vk::Format  transfer;
		uint32_t    bytes;
	};
	constexpr TransferPlaneCase transfer_cases[] = {
	    {"D16S8 transfer", vk::Format::eD16UnormS8Uint, vk::Format::eD16Unorm, 2},
	    {"D24S8 transfer", vk::Format::eD24UnormS8Uint, vk::Format::eX8D24UnormPack32, 4},
	    {"D32S8 transfer", vk::Format::eD32SfloatS8Uint, vk::Format::eD32Sfloat, 4},
	    {"invalid transfer", vk::Format::eR16Unorm, vk::Format::eUndefined, 0},
	};
	for (const auto& test: transfer_cases) {
		Require("DepthTargetFootprints", test.name,
		        DepthAspectTransferFormat(test.attachment) == test.transfer &&
		            DepthAspectTransferBytes(test.attachment) == test.bytes,
		        "combined depth transfer-plane layout changed");
	}
	struct PromotionCase {
		const char* name;
		uint16_t    source;
		uint32_t    d24;
		uint32_t    d32;
	};
	constexpr PromotionCase promotion_cases[] = {
	    {"zero", 0, 0, 0},
	    {"midpoint", 0x8000, 0x00800080, 0x3f000080},
	    {"maximum", 0xffff, 0x00ffffff, 0x3f800000},
	};
	for (const auto& test: promotion_cases) {
		Require("DepthTargetFootprints", test.name,
		        EncodeD16AsD24(test.source) == test.d24 && EncodeD16AsD32(test.source) == test.d32,
		        "D16 host promotion changed the represented depth value");
	}
	Require("DepthTargetFootprints", "640x360 Z32S8 without HTile",
	        TileGetDepthSize(640, 360, 0, Prospero::DepthFormat::kZ32F,
	                         Prospero::StencilFormat::k8UInt, false,
	                         stencil, htile, depth),
	        "valid non-HTile depth/stencil footprint was rejected");
	Require("DepthTargetFootprints", "640x360 Prospero block sizes",
	        depth.size == 0xf0000 && depth.align == 0x10000 && stencil.size == 0x60000 &&
	            stencil.align == 0x10000 && htile.size == 0 && htile.align == 0,
	        "non-HTile depth/stencil footprint disagrees with Prospero block rules");

	const auto depth_pitch =
	    TileGetTexturePitch(Prospero::BufferFormat::k32Float, 640,
	                        Prospero::TileMode::kDepth);
	TileSizeAlign texture_depth {};
	TileGetTextureTotalSize(Prospero::BufferFormat::k32Float, 640, 360, 1,
	                        1, Prospero::TileMode::kDepth,
	                        false, texture_depth);
	Require("DepthTargetFootprints", "640x360 generic depth tile",
	        depth_pitch == 640 && texture_depth.size == 0xf0000 && texture_depth.align == 0x10000,
	        "generic depth texture sizing bypassed 64 KiB block padding");

	Require("DepthTargetFootprints", "960x540 Z32S8 with HTile",
	        TileGetDepthSize(960, 540, 0, Prospero::DepthFormat::kZ32F,
	                         Prospero::StencilFormat::k8UInt, true, stencil,
	                         htile, depth) &&
	            depth.size == 0x280000 && depth.align == 0x10000 && stencil.size == 0xc0000 &&
	            stencil.align == 0x10000 && htile.size == 0x10000 && htile.align == 0x8000,
	        "generic Prospero HTile block calculation rejected the title footprint");
	Require("DepthTargetFootprints", "known HTile extent",
	        TileGetDepthSize(1280, 720, 0, Prospero::DepthFormat::kZ32F,
	                         Prospero::StencilFormat::k8UInt, true, stencil,
	                         htile, depth) &&
	            depth.size == 0x3c0000 && stencil.size == 0xf0000 && htile.size == 0x20000,
	        "validated 1280x720 HTile footprint regressed");
	Require("DepthTargetFootprints", "PPSA06228 3840x2160 Z32S8 with HTile",
	        TileGetDepthSize(3840, 2160, 0, Prospero::DepthFormat::kZ32F,
	                         Prospero::StencilFormat::k8UInt, true, stencil,
	                         htile, depth) &&
	            depth.size == 0x1fe0000 && depth.align == 0x10000 && stencil.size == 0x870000 &&
	            stencil.align == 0x10000 && htile.size == 0xa0000 && htile.align == 0x8000,
	        "captured 3840x2160 depth/stencil/HTile footprint disagrees with "
	        "Prospero rules");
	Require("DepthTargetFootprints", "invalid depth format",
	        !TileGetDepthSize(960, 540, 0, static_cast<Prospero::DepthFormat>(2u),
	                          Prospero::StencilFormat::k8UInt, true,
	                          stencil, htile, depth),
	        "unsupported depth format was silently admitted");
	Require("DepthTargetFootprints", "invalid stencil format",
	        !TileGetDepthSize(960, 540, 0, Prospero::DepthFormat::kZ32F,
	                          static_cast<Prospero::StencilFormat>(2u),
	                          true, stencil, htile, depth),
	        "unsupported stencil format was silently admitted");
	Require("DepthTargetFootprints", "invalid extent",
	        !TileGetDepthSize(0, 540, 0, Prospero::DepthFormat::kZ32F,
	                          Prospero::StencilFormat::k8UInt, true,
	                          stencil, htile, depth) &&
	            !TileGetDepthSize(16385, 540, 0,
	                              Prospero::DepthFormat::kZ32F,
	                              Prospero::StencilFormat::k8UInt, true,
	                              stencil, htile, depth),
	        "invalid HTile extent was silently admitted");
	std::printf("[host]    %-32s ok\n", "DepthTargetFootprints");
}

struct FenceLifetimeProbe {
	explicit FenceLifetimeProbe(bool* destroyed): destroyed(destroyed) {
		if (destroyed == nullptr || *destroyed) {
			EXIT("fence-lifetime probe has invalid construction state\n");
		}
	}
	~FenceLifetimeProbe() { *destroyed = true; }

	bool* destroyed = nullptr;
};

void CheckSharedFenceResourceLifetime() {
	bool                  destroyed = false;
	auto                  image     = std::make_shared<FenceLifetimeProbe>(&destroyed);
	FenceResourceRetainer first;
	FenceResourceRetainer second;
	first.Retain(image);
	second.Retain(image);
	first.Retain(image);
	image.reset();
	Require("SharedFenceResourceLifetime", "retained",
	        !destroyed && !first.Empty() && !second.Empty(),
	        "cache removal destroyed an image retained by command buffers");
	first.ReleaseAfterFence();
	Require("SharedFenceResourceLifetime", "first fence",
	        !destroyed && first.Empty() && !second.Empty(),
	        "first command-buffer fence destroyed another buffer's image");
	second.ReleaseAfterFence();
	Require("SharedFenceResourceLifetime", "last fence", destroyed && second.Empty(),
	        "last referencing command-buffer fence did not destroy the image");
	std::printf("[host]    %-32s ok\n", "SharedFenceResourceLifetime");
}

void CheckEmbeddedFetchLaneSpill() {
	std::vector<u32> code;
	code.push_back(EncodeSMovB32(0, InlineU32(0)));
	code.push_back(EncodeSmem0(0x02u, 20, 4));
	code.push_back(EncodeSmem1(0));                 // s_load_dwordx4 s[20:23], s[8:9]
	AppendVop3(&code, 0x361u, 6, 20, InlineU32(0)); // v_writelane_b32 v6, s20, 0
	code.push_back(EncodeSMovB32(20, InlineU32(0)));
	AppendVop3(&code, 0x360u, 20, Vgpr(6),
	           InlineU32(0)); // v_readlane_b32 s20, v6, 0
	code.push_back(EncodeVop2(0x01u, 0, Vgpr(8), 5));
	code.push_back(EncodeMubuf0(0x03u, 0, true));
	code.push_back(EncodeMubuf1(9, 5, 0));
	AppendEnd(&code);

	std::array<u32, 11>   user_data {};
	ShaderVertexInputInfo vertex;
	vertex.fetch_embedded                 = true;
	vertex.fetch_buffer_reg               = 0;
	vertex.resources_num                  = 1;
	vertex.resources_dst[0].attr_id       = 0;
	vertex.resources_dst[0].registers_num = 4;

	ShaderRecompiler::CompileOptions options;
	options.stage             = ShaderType::Vertex;
	options.user_data_base    = 8;
	options.user_data_count   = static_cast<u32>(user_data.size());
	options.user_data         = user_data.data();
	options.vertex_input_info = &vertex;

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	Require("EmbeddedFetchLaneSpill", "compile",
	        ShaderRecompiler::TryRecompile(code, options, result, &error), error);
	Require("EmbeddedFetchLaneSpill", "fetch rewrite", vertex.resource_fetch_components[0] == 4,
	        "lane-spilled fetch descriptor was not recognized and rewritten");
	std::printf("[host]    %-32s ok\n", "EmbeddedFetchLaneSpill");
}

void CheckReferenceClockScale() {
	uint64_t value = 0;
	Require("ReferenceClockScale", "zero",
	        Sync::ScaleReferenceClock(0, 3000000000ull, value) && value == 0,
	        "zero host tick did not produce a zero GPU clock");
	Require("ReferenceClockScale", "fractional second",
	        Sync::ScaleReferenceClock(1500000000ull, 3000000000ull, value) && value == 50000000ull,
	        "host half-second did not scale to 50,000,000 ticks");
	Require("ReferenceClockScale", "whole and fractional",
	        Sync::ScaleReferenceClock(3750000000ull, 3000000000ull, value) && value == 125000000ull,
	        "host 1.25 seconds did not scale to 125,000,000 ticks");
	Require("ReferenceClockScale", "monotonic floor",
	        Sync::ScaleReferenceClock(3750000001ull, 3000000000ull, value) && value == 125000000ull,
	        "sub-reference-tick increment did not use a monotonic floor");
	Require("ReferenceClockScale", "guards",
	        !Sync::ScaleReferenceClock(1, 0, value) &&
	            !Sync::ScaleReferenceClock(UINT64_MAX, 1, value),
	        "invalid frequency or overflow was accepted");
	std::printf("[host]    %-32s ok\n", "ReferenceClockScale");
}

void CheckClipControlDepthClipState() {
	HW::ClipControl clip;
	Require("ClipControlDepthClipState", "default",
	        clip.IsZClipModeRepresentable() && clip.IsZClipEnabled(),
	        "default paired Z clipping was not enabled");

	clip.min_z_clip_disable = true;
	Require("ClipControlDepthClipState", "asymmetric near", !clip.IsZClipModeRepresentable(),
	        "asymmetric near-plane state was accepted");

	clip.min_z_clip_disable = false;
	clip.max_z_clip_disable = true;
	Require("ClipControlDepthClipState", "asymmetric far", !clip.IsZClipModeRepresentable(),
	        "asymmetric far-plane state was accepted");

	clip.min_z_clip_disable = true;
	Require("ClipControlDepthClipState", "both disabled",
	        clip.IsZClipModeRepresentable() && !clip.IsZClipEnabled(),
	        "paired Z-clip disable was not represented");
	std::printf("[host]    %-32s ok\n", "ClipControlDepthClipState");
}

void CheckVulkan13FeatureRequirements() {
	const auto features = WindowContext::RequiredVulkan13Features();
	Require("Vulkan13FeatureRequirements", "production requirements",
	        features.sType == vk::StructureType::ePhysicalDeviceVulkan13Features &&
	            features.pNext == nullptr && features.dynamicRendering == VK_TRUE &&
	            features.synchronization2 == VK_TRUE,
	        "production device creation did not require dynamic rendering and "
	        "synchronization2 from one feature declaration");
	std::printf("[host]    %-32s ok\n", "Vulkan13FeatureRequirements");
}

void CheckPm4AcquireMemNoOp(RenderContext& renderer) {
	GraphicsInitJmpTables();
	CommandProcessor              processor(renderer);
	const std::array<uint32_t, 6> standard_payload {0x00400000u, 1u, 0u, 0u, 0u, 10u};
	const std::array<uint32_t, 7> custom_payload {
	    0xdeadbeefu, 0xffffffffu, 0x80000000u, 0x13579bdfu, 0x2468ace0u, 0xaaaaaaaau, 0x55555555u};
	const auto standard_before = standard_payload;
	const auto custom_before   = custom_payload;
	Require("Pm4AcquireMemNoOp", "recognized packets",
	        CpOpAcquireMem(processor, 0xC0055800u, standard_payload.data(), 0, 0) == 6 &&
	            CpOpAcquireMem(processor, 0xc0061050u, custom_payload.data(), 0, 0) == 7 &&
	            standard_payload == standard_before && custom_payload == custom_before,
	        "ACQUIRE_MEM did not consume both pinned packet forms as "
	        "side-effect-free "
	        "no-ops");
	std::printf("[host]    %-32s ok\n", "Pm4AcquireMemNoOp");
}

void CheckPm4StencilInfoValueLane(RenderContext& renderer) {
	CommandProcessor                  processor(renderer);
	constexpr std::array<uint32_t, 2> payload {0x00100801u, 0x28000000u};
	const auto                        consumed =
	    HwCtxSetStencilInfo(processor, 0xC0016900u, Pm4::DB_STENCIL_INFO, payload.data(), 1);
	const auto& stencil = processor.GetCtx().GetDepthStencilInfo();
	Require("Pm4StencilInfoValueLane", "standalone register value",
	        consumed == 1 &&
	            stencil.format == Prospero::StencilFormat::k8UInt &&
	            stencil.texture_compatibility == Prospero::TextureCompatibleStencil::kEnable &&
	            !stencil.expclear_enabled && !stencil.htile_stencil_disabled,
	        "standalone DB_STENCIL_INFO did not decode its sole payload value");
	std::printf("[host]    %-32s ok\n", "Pm4StencilInfoValueLane");
}

void CheckPm4PolygonOffsetRegisters(RenderContext& renderer) {
	GraphicsInitJmpTables();
	CommandProcessor processor(renderer);
	const auto&      defaults = processor.GetCtx().GetPolyOffset();
	Require("Pm4PolygonOffset", "format defaults",
	        defaults.neg_num_db_bits == -23 && defaults.db_is_float_fmt,
	        "polygon offset Z format did not default to D32F");

	constexpr std::array<uint32_t, 6> payload {
	    0x000000f0u, std::bit_cast<uint32_t>(2.5f), std::bit_cast<uint32_t>(16.0f),
	    std::bit_cast<uint32_t>(-3.25f), std::bit_cast<uint32_t>(32.0f),
	    std::bit_cast<uint32_t>(4.5f)};
	const auto consumed = HwCtxSetPolyOffsetRegisters(
	    processor, KYTY_PM4(8, Pm4::IT_SET_CONTEXT_REG, Pm4::R_ZERO),
	    Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL, payload.data(), 0);
	const auto& direct = processor.GetCtx().GetPolyOffset();
	Require("Pm4PolygonOffset", "direct registers",
	        consumed == payload.size() && direct.neg_num_db_bits == -16 &&
	            !direct.db_is_float_fmt && direct.clamp == 2.5f && direct.front_scale == 16.0f &&
	            direct.front_offset == -3.25f && direct.back_scale == 32.0f &&
	            direct.back_offset == 4.5f,
	        "direct polygon offset register packet was decoded incorrectly");

	g_hw_ctx_indirect_func[Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL](
	    processor, Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL, 0x000001e9u);
	g_hw_ctx_indirect_func[Pm4::PA_SU_POLY_OFFSET_FRONT_SCALE](
	    processor, Pm4::PA_SU_POLY_OFFSET_FRONT_SCALE, std::bit_cast<uint32_t>(24.0f));
	const auto& indirect = processor.GetCtx().GetPolyOffset();
	Require("Pm4PolygonOffset", "indirect registers",
	        indirect.neg_num_db_bits == -23 && indirect.db_is_float_fmt &&
	            indirect.front_scale == 24.0f && indirect.front_offset == -3.25f,
	        "indirect polygon offset register write was decoded incorrectly");
	std::printf("[host]    %-32s ok\n", "Pm4PolygonOffset");
}

void CheckPm4ContextStateOperations(RenderContext& renderer) {
	GraphicsInitJmpTables();
	CommandProcessor processor(renderer);
	constexpr auto primitive_type = static_cast<Prospero::PrimitiveType>(0x35u);
	struct AgcCommandBufferLayout {
		using Callback = KYTY_SYSV_ABI bool (*)(Gen5::CommandBuffer*, uint32_t, void*);

		uint32_t* bottom;
		uint32_t* top;
		uint32_t* cursor_up;
		uint32_t* cursor_down;
		Callback  callback;
		void*     user_data;
		uint32_t  reserved_dw;
	};
	static_assert(offsetof(AgcCommandBufferLayout, cursor_up) == 0x10);
	static_assert(offsetof(AgcCommandBufferLayout, reserved_dw) == 0x30);

	HW::RenderControl original_control {};
	original_control.depth_clear_enable   = true;
	original_control.stencil_clear_enable = true;
	processor.GetCtx().SetRenderControl(original_control);
	processor.GetCtx().SetRenderTargetMask(0x1234abcd);
	processor.GetUcfg().SetPrimitiveType(primitive_type);
	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_LO_PS](processor, Pm4::SPI_SHADER_PGM_LO_PS,
	                                                 0x00123456);
	const auto shader_address = processor.GetShCtx().GetPs().ps_regs.data_addr;
	const auto invoke = [&](ContextStateOperation operation) {
		std::array<uint32_t, 32> packet {};
		std::array<uint32_t, 6>  segment_sizes {};
		AgcCommandBufferLayout   dcb {packet.data(), packet.data() + packet.size(), packet.data(),
		                              packet.data() + packet.size(), nullptr, nullptr, 0};
		const auto operation_value = static_cast<uint32_t>(operation);
		size_t     segment_count  = 0;
		switch (operation) {
			case ContextStateOperation::Clear: segment_sizes = {5}; segment_count = 1; break;
			case ContextStateOperation::Push:
				segment_sizes = {5, 8, 9, 3, 2};
				segment_count = 5;
				break;
			case ContextStateOperation::Pop:
				segment_sizes = {3, 5, 8, 9, 2};
				segment_count = 5;
				break;
			case ContextStateOperation::PushClear:
				segment_sizes = {5, 8, 9, 3, 2, 5};
				segment_count = 6;
				break;
		}
		const auto size_dw =
		    static_cast<size_t>(Gen5::GraphicsDcbContextStateOpGetSize(operation_value) / 4);
		auto* emitted = Gen5::GraphicsDcbContextStateOp(
		    reinterpret_cast<Gen5::CommandBuffer*>(&dcb), operation_value);
		bool   packet_matches = true;
		size_t segment_offset = 0;
		for (size_t i = 0; i < segment_count; i++) {
			const auto segment_size = segment_sizes[i];
			const auto selector = (i == 0 ? Pm4::R_CONTEXT_STATE : Pm4::R_ZERO);
			packet_matches &=
			    packet[segment_offset] == KYTY_PM4(segment_size, Pm4::IT_NOP, selector);
			for (size_t j = 1; j < segment_size; j++) {
				packet_matches &= packet[segment_offset + j] ==
				                  (i == 0 && j == 1 ? operation_value : 0u);
			}
			segment_offset += segment_size;
		}
		Require("Pm4ContextState", "HLE packet",
		        emitted == packet.data() && dcb.cursor_up == packet.data() + size_dw &&
		            segment_offset == size_dw && packet_matches,
		        "context-state HLE packet does not match its real allocation size");
		Pm4Execution execution;
		return processor.Process(execution, packet.data(), size_dw);
	};

	Require("Pm4ContextState", "push-clear",
	        invoke(ContextStateOperation::PushClear) == Pm4ProcessResult::Complete &&
	            !processor.GetCtx().GetRenderControl().depth_clear_enable &&
	            !processor.GetCtx().GetRenderControl().stencil_clear_enable &&
	            processor.GetCtx().GetRenderTargetMask() == HW::Context {}.GetRenderTargetMask() &&
	            processor.GetUcfg().GetPrimType() == primitive_type &&
	            processor.GetShCtx().GetPs().ps_regs.data_addr == shader_address,
	        "push-clear did not save and clear only Cx state");

	processor.GetCtx().SetRenderTargetMask(0x55aa55aa);
	Require("Pm4ContextState", "pop",
	        invoke(ContextStateOperation::Pop) == Pm4ProcessResult::Complete &&
	            processor.GetCtx().GetRenderControl().depth_clear_enable &&
	            processor.GetCtx().GetRenderControl().stencil_clear_enable &&
	            processor.GetCtx().GetRenderTargetMask() == 0x1234abcd,
	        "pop did not restore the complete saved Cx state");

	Require("Pm4ContextState", "push",
	        invoke(ContextStateOperation::Push) == Pm4ProcessResult::Complete &&
	            processor.GetCtx().GetRenderTargetMask() == 0x1234abcd,
	        "push changed live Cx state");
	processor.GetCtx().SetRenderTargetMask(0x01020304);
        Require("Pm4ContextState", "push-pop restore",
                invoke(ContextStateOperation::Pop) ==
                        Pm4ProcessResult::Complete &&
                    processor.GetCtx().GetRenderTargetMask() == 0x1234abcd,
                "ordinary push/pop did not restore Cx state");

        Require("Pm4ContextState", "clear",
                invoke(ContextStateOperation::Clear) ==
                        Pm4ProcessResult::Complete &&
                    !processor.GetCtx().GetRenderControl().depth_clear_enable &&
                    processor.GetCtx().GetRenderTargetMask() ==
                        HW::Context{}.GetRenderTargetMask() &&
		            processor.GetUcfg().GetPrimType() == primitive_type &&
                    processor.GetShCtx().GetPs().ps_regs.data_addr ==
                        shader_address,
                "clear reset state outside the Cx register domain");

        processor.GetCtx().SetRenderTargetMask(0x0badc0de);
        Require("Pm4ContextState", "push before clear-state packet",
                invoke(ContextStateOperation::Push) ==
                    Pm4ProcessResult::Complete,
                "push before CLEAR_STATE failed");
        processor.GetCtx().SetRenderTargetMask(0xfeedface);
        std::array<uint32_t, 2> clear_state_packet{0xc0001200, 0};
        Pm4Execution clear_state_execution;
        Require("Pm4ContextState", "clear-state packet",
                processor.Process(
                    clear_state_execution, clear_state_packet.data(),
                    clear_state_packet.size()) == Pm4ProcessResult::Complete &&
                    processor.GetCtx().GetRenderTargetMask() ==
                        HW::Context{}.GetRenderTargetMask(),
                "CLEAR_STATE did not clear the current Cx state");
        Require("Pm4ContextState", "pop after clear-state packet",
                invoke(ContextStateOperation::Pop) ==
                        Pm4ProcessResult::Complete &&
                    processor.GetCtx().GetRenderTargetMask() == 0x0badc0de,
                "CLEAR_STATE discarded the pushed Cx state");

        processor.GetCtx().SetRenderTargetMask(0xabcdef01);
        Require("Pm4ContextState", "push before processor reset",
                invoke(ContextStateOperation::Push) ==
                    Pm4ProcessResult::Complete,
                "push before reset failed");
        processor.Reset();
        processor.GetCtx().SetRenderTargetMask(0x76543210);
        Require("Pm4ContextState", "processor reset discards push",
                invoke(ContextStateOperation::Push) ==
                        Pm4ProcessResult::Complete &&
                    invoke(ContextStateOperation::Pop) ==
                        Pm4ProcessResult::Complete &&
                    processor.GetCtx().GetRenderTargetMask() == 0x76543210,
                "processor reset retained an invalid saved Cx state");

        Require("Pm4ContextState", "HLE packet size",
                Gen5::GraphicsDcbContextStateOpGetSize(0) == 20 &&
                    Gen5::GraphicsDcbContextStateOpGetSize(1) == 108 &&
                    Gen5::GraphicsDcbContextStateOpGetSize(2) == 108 &&
                    Gen5::GraphicsDcbContextStateOpGetSize(3) == 128 &&
                    Gen5::GraphicsDcbContextStateOpGetSize(4) == 0,
                "context-state HLE sizes do not match libSceAgc");
        std::printf("[host]    %-32s ok\n", "Pm4ContextState");
}

void CheckPm4WaitResume(RenderContext& renderer) {
	GraphicsInitJmpTables();
	CommandProcessor processor(renderer);

	uint32_t   label             = 0;
	uint32_t   prefix            = 0;
	uint32_t   child_observation = UINT32_MAX;
	uint32_t   suffix            = 0;
	const auto address = [](const void* value) { return reinterpret_cast<uint64_t>(value); };

	std::array<uint32_t, 12> child {};
	child[0]  = KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0);
	child[1]  = 0;
	child[2]  = static_cast<uint32_t>(address(&child_observation));
	child[3]  = static_cast<uint32_t>(address(&child_observation) >> 32u);
	child[4]  = 0;
	child[5]  = KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_WAIT_MEM_32);
	child[6]  = static_cast<uint32_t>(address(&label));
	child[7]  = static_cast<uint32_t>(address(&label) >> 32u);
	child[8]  = UINT32_MAX;
	child[9]  = 1;
	child[10] = 0x10u | 3u;

	std::array<uint32_t, 4> nested {};
	nested[0] = KYTY_PM4(4, Pm4::IT_INDIRECT_BUFFER, 0);
	nested[1] = static_cast<uint32_t>(address(child.data()));
	nested[2] = static_cast<uint32_t>(address(child.data()) >> 32u);
	nested[3] = 0x0f200000u | static_cast<uint32_t>(child.size());

	std::array<uint32_t, 14> commands {};
	commands[0]  = KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0);
	commands[1]  = 0;
	commands[2]  = static_cast<uint32_t>(address(&prefix));
	commands[3]  = static_cast<uint32_t>(address(&prefix) >> 32u);
	commands[4]  = 11;
	commands[5]  = KYTY_PM4(4, Pm4::IT_INDIRECT_BUFFER, 0);
	commands[6]  = static_cast<uint32_t>(address(nested.data()));
	commands[7]  = static_cast<uint32_t>(address(nested.data()) >> 32u);
	commands[8]  = 0x0f200000u | static_cast<uint32_t>(nested.size());
	commands[9]  = KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0);
	commands[10] = 0;
	commands[11] = static_cast<uint32_t>(address(&suffix));
	commands[12] = static_cast<uint32_t>(address(&suffix) >> 32u);
	commands[13] = 22;

	Pm4Execution execution;
	Require("Pm4WaitResume", "suspend",
	        processor.Process(execution, commands.data(), commands.size()) ==
	                Pm4ProcessResult::Blocked &&
	            prefix == 11 && child_observation == 0 && suffix == 0,
	        "blocked indirect wait did not preserve its command position");

	label    = 1;
	child[4] = 1;
	Require("Pm4WaitResume", "resume",
	        processor.Process(execution, commands.data(), commands.size()) ==
	                Pm4ProcessResult::Complete &&
	            prefix == 11 && child_observation == 0 && suffix == 22,
	        "resumed indirect wait replayed a child or did not finish its parent");
	std::printf("[host]    %-32s ok\n", "Pm4WaitResume");
}

void CheckPm4CeCompletion(RenderContext& renderer) {
	GraphicsInitJmpTables();
	CommandProcessor processor(renderer);
	uint32_t         suffix  = 0;
	const auto       address = reinterpret_cast<uint64_t>(&suffix);

	std::array<uint32_t, 9> commands {};
	commands[0] = 0xc0008600u;
	commands[1] = 1;
	commands[2] = 0xc0008500u;
	commands[3] = 0;
	commands[4] = KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0);
	commands[5] = 0;
	commands[6] = static_cast<uint32_t>(address);
	commands[7] = static_cast<uint32_t>(address >> 32u);
	commands[8] = 33;

	processor.ResetDeCe();
	Pm4Execution execution;
	Require("Pm4CeCompletion", "wait",
	        processor.Process(execution, commands.data(), commands.size()) ==
	                Pm4ProcessResult::Blocked &&
	            suffix == 0,
	        "DE did not wait for an active CE stream");

	std::array<uint32_t, 2> ce_increment {0xc0008400u, 1};
	Pm4Execution            ce_execution;
	Require("Pm4CeCompletion", "CE increment",
	        processor.Process(ce_execution, ce_increment.data(), ce_increment.size()) ==
	            Pm4ProcessResult::Complete,
	        "CE counter increment did not complete");
	Require("Pm4CeCompletion", "counter gate",
	        processor.Process(execution, commands.data(), commands.size()) ==
	                Pm4ProcessResult::Complete &&
	            suffix == 33,
	        "DE did not resume after CE advanced or failed to increment its counter");

	suffix = 0;
	Pm4Execution balanced_execution;
	Require("Pm4CeCompletion", "balanced wait",
	        processor.Process(balanced_execution, commands.data(), commands.size()) ==
	                Pm4ProcessResult::Blocked &&
	            suffix == 0,
	        "balanced CE/DE counters did not gate the next DE packet");
	processor.SetCeComplete(true);
	Require("Pm4CeCompletion", "stream complete",
	        processor.Process(balanced_execution, commands.data(), commands.size()) ==
	                Pm4ProcessResult::Complete &&
	            suffix == 33,
	        "DE remained blocked after the CE stream completed");
	std::printf("[host]    %-32s ok\n", "Pm4CeCompletion");
}

} // namespace
} // namespace Libs::Graphics

int main(int argc, char** argv) {
	using namespace Libs::Graphics;

	std::setvbuf(stdout, nullptr, _IONBF, 0);
	EnsureConfigInitialized();
	CheckLeastRecentlyUsedCacheOrdering();
	if (argc == 2 && std::strcmp(argv[1], "--clip-control-only") == 0) {
		CheckClipControlDepthClipState();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--reference-clock-only") == 0) {
		CheckReferenceClockScale();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--scheduler-only") == 0) {
		VulkanHarness vulkan;
		vulkan.CheckSchedulerTimeline();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--mapped-range-only") == 0) {
		VulkanHarness vulkan;
		vulkan.CheckGpuMappedRangeLifecycle();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--stream-buffer-only") == 0) {
		VulkanHarness vulkan;
		vulkan.CheckStreamBufferRing();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--gpu-tiler-only") == 0) {
		VulkanHarness vulkan;
		vulkan.CheckGpuTilerCpuParity();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--gpu-command-lane-only") == 0) {
		VulkanHarness vulkan;
		vulkan.CheckGpuCommandLane();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--context-state-only") == 0) {
		VulkanHarness vulkan;
		CheckPm4PolygonOffsetRegisters(vulkan.RuntimeRenderer());
		CheckPm4ContextStateOperations(vulkan.RuntimeRenderer());
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--image-overlap-only") == 0) {
		CheckDepthAttachmentWrites();
		CheckDynamicRenderingState();
		VulkanHarness vulkan;
		vulkan.CheckRenderExecutorColorVolumeDiscovery();
		vulkan.CheckRenderExecutorStencilBindingDiscovery();
		vulkan.CheckUnifiedTextureCacheFlow();
		vulkan.CheckBgra16Readback();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--htile-clear-only") == 0) {
		VulkanHarness vulkan;
		vulkan.CheckUnifiedTextureCacheFlow();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--layered-image-only") == 0) {
		VulkanHarness vulkan;
		vulkan.CheckUnifiedImageViewCache();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--image-view-cache-only") == 0) {
		VulkanHarness vulkan;
		vulkan.CheckUnifiedImageViewCache();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--storage-sampled-only") == 0) {
		VulkanHarness vulkan;
		vulkan.CheckUnifiedImageViewCache();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--depth-readback-only") == 0) {
		CheckDepthTargetFootprints();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--buffer-cache-range-only") == 0) {
		VulkanHarness vulkan;
		vulkan.CheckUnifiedTextureCacheFlow();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--buffer-cache-gc-only") == 0) {
		VulkanHarness vulkan;
		vulkan.CheckBufferCacheDirtyGarbageCollection();
		return 0;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (argc == 2 && std::strcmp(argv[1], "--reverse-rt-death") == 0) {
		RunReverseRenderTargetDeathCase();
	}
	if (argc == 2 && std::strcmp(argv[1], "--reverse-rt-only") == 0) {
		CheckRenderTargetFormatContract();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--standard64-rt-only") == 0) {
		CheckStandard64RenderTargetTileRoundTrip();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--image-view-only") == 0) {
		VulkanHarness vulkan;
		CheckSampledColorViews();
		CheckSampledVideoOutView(vulkan.RuntimeRenderer());
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--image-transition-only") == 0) {
		VulkanHarness vulkan;
		CheckImageTransitionState(vulkan.RuntimeRenderer());
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--sampled-depth-resource-only") == 0) {
		VulkanHarness vulkan;
		CheckSampledDepthResource();
		CheckSampledDepthDescriptor(vulkan.RuntimeRenderer());
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--storage-bgra-only") == 0) {
		CheckSampledColorViews();
		CheckBasicStorageTextureDescriptor();
		VulkanHarness vulkan;
		vulkan.CheckUnifiedImageViewCache();
		RunCase(&vulkan, ImageStoreBgraUsesInverseSwizzle());
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--storage-yzwx-only") == 0) {
		CheckSampledColorViews();
		CheckBasicStorageTextureDescriptor();
		CheckStorageTextureGpuOwnedRebindState();
		VulkanHarness vulkan;
		RunCase(&vulkan, ImageStoreYzwxUsesInverseSwizzle());
		return 0;
	}
	if (argc == 3 && std::strcmp(argv[1], "--image-view-death") == 0) {
		RunImageViewDeathCase(argv[2]);
	}
	if (argc == 3 && std::strcmp(argv[1], "--storage-texture-descriptor-death") == 0) {
		RunStorageTextureDescriptorDeathCase(argv[2]);
	}
	if (argc != 1) {
		std::fprintf(stderr, "unknown test selector: %s\n", argv[1]);
		return 2;
	}
	VulkanHarness vulkan;
	CheckRenderTargetFormatContract();
	CheckSampledColorViews();
	CheckSampledVideoOutView(vulkan.RuntimeRenderer());
	CheckImageTransitionState(vulkan.RuntimeRenderer());
	CheckSampledDepthResource();
	CheckSampledDepthDescriptor(vulkan.RuntimeRenderer());
	CheckBasicStorageTextureDescriptor();
	CheckStorageTextureLinearUploadLayout();
	CheckStorageTextureDepthTileUploadLayout();
	CheckStorageImageSwizzleSpecializationId();
	CheckStandard64RenderTargetTileRoundTrip();
	CheckStorageTextureVolumeUploadLayout();
	CheckStorageTextureVolumeMipRegions();
	CheckStorageTextureGpuOwnedRebindState();
	CheckNativeMsaaState();
	CheckPs5DepthRegisterDecoding();
	CheckDepthHtileStencilCompatibility();
	CheckStencilAttachmentAccess();
	CheckDepthAttachmentWrites();
	CheckDynamicRenderingState();
	CheckDepthTargetFootprints();
	CheckSharedFenceResourceLifetime();
#else
	if (argc != 1) {
		std::fprintf(stderr, "unknown test selector: %s\n", argv[1]);
		return 2;
	}
	VulkanHarness vulkan;
#endif
	CheckClipControlDepthClipState();
	CheckReferenceClockScale();
	CheckVulkan13FeatureRequirements();
	CheckPm4AcquireMemNoOp(vulkan.RuntimeRenderer());
	CheckPm4StencilInfoValueLane(vulkan.RuntimeRenderer());
	CheckPm4PolygonOffsetRegisters(vulkan.RuntimeRenderer());
	CheckPm4ContextStateOperations(vulkan.RuntimeRenderer());
	CheckPm4WaitResume(vulkan.RuntimeRenderer());
	CheckPm4CeCompletion(vulkan.RuntimeRenderer());
	CheckEmbeddedFetchVertexOffset();
	CheckEmbeddedFetchLaneSpill();
	CheckRectListShaders();
	CheckPs5GameExampleImageClearRuntimeShape();
	vulkan.CheckSchedulerTimeline();
	vulkan.CheckGpuMappedRangeLifecycle();
	vulkan.CheckStreamBufferRing();
	vulkan.CheckCommandPoolGrowth();
	vulkan.CheckGpuTilerCpuParity();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	vulkan.CheckRenderExecutorColorVolumeDiscovery();
	vulkan.CheckRenderExecutorStencilBindingDiscovery();
	vulkan.CheckUnifiedTextureCacheFlow();
	vulkan.CheckBgra16Readback();
	vulkan.CheckBufferCacheDirtyGarbageCollection();
#endif
	vulkan.CheckUnifiedImageViewCache();
	const auto tests          = MakeCases();
	const auto graphics_tests = MakeGraphicsCases();
	CheckOpcodeCoverage(tests, graphics_tests);
	for (const auto& test: tests) {
		RunCase(&vulkan, test);
	}
	for (const auto& test: MakeSkippedCases()) {
		std::printf("[skip]    %-32s %s\n", test.name, test.reason);
	}
	for (const auto& test: graphics_tests) {
		RunGraphicsCase(&vulkan, test);
	}
	vulkan.CheckGpuCommandLane();
	std::printf("ShaderRecompilerComputeTests: all cases passed\n");
	return 0;
}
